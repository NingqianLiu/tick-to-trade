// Puts a whole day of market data on the wire.
//
// This is the other half of a run: the trader is the consumer, this is the producer.
//
// It does three things: read the ITCH file, gather messages into packets by exactly the
// rule the offline check uses, and hand each packet to the card at the moment the replay
// says it is due.
//
//   sender <itch-file> --intf enp129s0f0 --dst-ip 239.9.9.1 --dst-port 26477
//          [--src-ip A.B.C.D] [--src-port N] [--dst-port-b N]
//          [--max-messages N] [--frames-node N] [--drift-out FILE]
//
// Two paces. A measurement window, the warm up before it and the tail after it are
// replayed at the day's real spacing; everywhere else a packet goes out every
// ITCH_GAP_NS nanoseconds - ten microseconds by default, some eighty or ninety times
// faster than the day really was. That covers 97.7% of the packets in a day, and not one
// of them produces a sample.
//
// One thing that does not need worrying about, because the hardware holds it: however
// fast this sends, it cannot outrun the wire. Once the transmit ring is full the handover
// returns -EAGAIN, and a descriptor only comes back when its bytes are really on the
// wire. That back pressure comes with the card and it is the only rate limiting this
// program needs - there is no rate limiting code here. So "the producer might overwhelm
// the consumer" is not a real worry: it physically cannot exceed 10 Gb/s.

// sched_getcpu and sched_setaffinity, to pin the sending thread. It sends hundreds of
// thousands of packets a second, and being moved once by the scheduler leaves a visible
// gap.
#include <sched.h>

// The ef_vi declarations. What is used here is ef_event, ef_vi_transmit_init,
// ef_vi_transmit_push and ef_vi_transmit_unbundle - the whole send path.
#include <etherfabric/ef_vi.h>

// PRIu64 and friends: the report at the end is full of 64 bit counts.
#include <cinttypes>
#include <cstdio>
// strtoull and exit, for the command line.
#include <cstdlib>
// memcpy to move a message into a frame untouched, strcmp to recognise a flag.
#include <cstring>
// The session name and the file path.
#include <string>
// The directory of built frames.
#include <vector>

// hist::Hist. What is measured here is not latency but how far the moment a packet was
// due is from the moment it really went - whether the replay kept its own schedule. A
// large number here means the replay was not faithful.
#include "common/hist.hpp"
// huge::map, for the few gigabytes of frame storage. Huge pages do the same job here as
// in the trader: fewer address translations.
#include "common/huge.hpp"
// cfg::kMaxFrameBytes and cfg::kMaxPacketPayload - the limits the gathering obeys. Both
// ends have to use the same ones, which is why this header is shared with the trader
// rather than copied.
#include "common/settings.hpp"
// tsc::now and tsc::ticks_per_ns. The send loop keeps time by spinning to a moment.
#include "common/tsc.hpp"
// win::Params, win::Tracker and win::note_session - the same header the trader includes.
// Each end works the windows out for itself and they cannot disagree, so no handshake is
// needed.
#include "common/window.hpp"
// io::SeqReader, for reading the tens of gigabytes of ITCH.
#include "io/seq_reader.hpp"
// itch::for_each_message, to cut a stretch of bytes into messages by the length table.
#include "itch/framing.hpp"
// ef::Vi and ef::Frames: opening an interface and getting memory the card can read.
#include "net/ef.hpp"
// eth::write and eth::Endpoint, for the forty two bytes of Ethernet, IP and UDP headers
// at the front of a frame.
#include "net/eth.hpp"
// mold::Packer, which gathers a MoldUDP64 packet and writes its twenty byte header.
#include "net/mold.hpp"
// pace::Schedule, which turns a packet into the nanosecond it should leave at.
#include "net/pace.hpp"
// pkt::Packing, which decides when a packet is finished. The offline check uses the same
// rule, so both sides cut packets identically.
#include "net/pack.hpp"

namespace {

// The whole file is read into memory rather than read as it is sent.
//
// Why: one read that really reaches the disk stops the send loop for as long as it takes,
// and if that happens inside a window the spacing between two packets is no longer the
// exchange's - which is the only reason that stretch exists. The price is a couple of
// minutes before the run and tens of gigabytes of memory, and it is worth it.
constexpr std::size_t kReadBuffer = 0;
// 2048 bytes a slot.
//
// Every frame is built long before it is needed and never touched again. At the moment it
// is due, only its address is handed to the card - not one byte is written. That is what
// lets this program keep time: there is no work left to do at the moment itself.
//
// The size follows the frame limit: once that went back to the 1500 bytes the link
// allows, a slot of 512 no longer held one.
constexpr std::size_t kSlotBytes = 2048;
// A slot has to hold the largest frame the gathering rule can produce, or the card would
// split one across two slots and the receiving side has no code for that.
static_assert(cfg::kMaxFrameBytes <= kSlotBytes);
// How many built frames can be held at once. Two gigabytes by default, roughly two
// million packets of lead.
//
// The storage is a ring: frames are written ahead of the sending point and the space
// behind it is reused. Being a ring is what lets the same code replay a few hundred
// windows or a whole day, without ever holding a day of frames at once - which would be
// tens of gigabytes.
constexpr std::size_t kDefaultArenaMb = 2048;
// The transmit ring is deep enough that handing a frame over never blocks.
constexpr int kTxRing = 2047;
constexpr std::uint64_t kMaxInFlight = kTxRing - 8;
// A packet takes about a microsecond to build. Below this much time left, the
// wait is left alone rather than risk being inside a build when the moment
// arrives.
constexpr std::uint64_t kBuildGuardNs = 3000;

struct Options {
    const char* itch = nullptr;
    const char* intf = "enp129s0f0";
    std::uint32_t src_ip = eth::ipv4(10, 9, 9, 1);
    std::uint32_t dst_ip = eth::ipv4(239, 9, 9, 1);
    // Our own port. Multicast has no return path, so this only goes into the header and
    // nobody filters on it.
    std::uint16_t src_port = 40000;
    // The destination port of the first feed. The trader's filter is built from this
    // number, so the two have to agree.
    std::uint16_t dst_port = 26477;
    // The exchange sends the same data twice down two paths. Here both copies
    // go out the one port, differing only in where they are addressed.
    std::uint16_t dst_port_b = 0;
    // Stop after the first N messages; zero replays the whole file.
    //
    // This is the quickest setting there is: forty one windows after the open, a run of
    // two minutes forty five. The count was chosen against the original file, so on the
    // thinned one it cuts in the wrong place and the median comes out at eight
    // milliseconds.
    std::uint64_t max_messages = 0;
    std::uint32_t speed = pace::kUnitSpeed;
    // Stop once the last measurement window has closed instead of replaying the rest of
    // the file, which produces no samples at all and is pure waiting.
    bool stop_after_window = false;
    // Which node the frames the card reads are kept on. Below zero means the
    // one this process is running on. They belong next to the card, which is
    // not always where the process is: putting the two on different nodes is
    // how the question "is it the card they are fighting over, or the memory
    // controller" gets asked.
    int frames_node = -1;
    // Where to write the drift of each window. The summary can only say how far off the
    // worst was; this file says which windows it was.
    const char* drift_out = nullptr;   // a row per window, for finding where it comes from
    // The ten byte MoldUDP64 session name, fixed because both ends only ever use this
    // one. The receiver checks it to be sure it is watching the same replay.
    std::string session = "ITCHBENCH0";
};

// Turns "239.9.9.1" into an integer.
//
// Stricter than the trader's version, because an address here is typed on a command line
// by a person while the trader's usually comes from a script.
bool parse_ip(const char* s, std::uint32_t* out) {
    unsigned a, b, c, d;
    // Fewer than four parts means it is not an address.
    if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    // sscanf happily reads "999", so the range has to be checked here.
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = eth::ipv4(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
                     static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(d));
    return true;
}

// The command line, in three groups:
//   what to send   the first positional argument (the ITCH file) and --max-messages
//   where to send  --intf, --src-ip, --dst-ip, --src-port, --dst-port and --dst-port-b,
//                  where the destination has to match the trader's or its filter receives
//                  nothing
//   diagnostics    --drift-out and --frames-node
bool parse(int argc, char** argv, Options* o) {
    // The ITCH file is positional; everything after it is a flag.
    o->itch = argv[1];
    for (int i = 2; i < argc; ++i) {
        const bool has = i + 1 < argc;
        const char* a = argv[i];
        if (std::strcmp(a, "--intf") == 0 && has) {
            o->intf = argv[++i];
        } else if (std::strcmp(a, "--src-ip") == 0 && has) {
            if (!parse_ip(argv[++i], &o->src_ip)) return false;
        // The multicast address of the feed. It has to match the trader's --dst-ip, or
        // its filter receives nothing and neither side complains.
        } else if (std::strcmp(a, "--dst-ip") == 0 && has) {
            if (!parse_ip(argv[++i], &o->dst_ip)) return false;
        // Our own port, which only goes into the header.
        } else if (std::strcmp(a, "--src-port") == 0 && has) {
            o->src_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        // The first feed's destination port, which has to match the trader's as well.
        } else if (std::strcmp(a, "--dst-port") == 0 && has) {
            o->dst_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        // The second feed. Given one, the same bytes go out twice, which is what an
        // exchange does - the consumer removes the duplicates by sequence number. Without
        // it only one feed goes out, which is not what a real market looks like.
        } else if (std::strcmp(a, "--stop-after-window") == 0) {
            o->stop_after_window = true;
        } else if (std::strcmp(a, "--dst-port-b") == 0 && has) {
            o->dst_port_b = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--speed") == 0 && has) {
            o->speed = pace::speed_from_text(argv[++i]);
            if (o->speed == 0) {
                std::fprintf(stderr, "--speed wants something like 10x, not %s\n", argv[i]);
                return false;
            }
        // Only the first N messages - see the note in Options above.
        } else if (std::strcmp(a, "--max-messages") == 0 && has) {
            o->max_messages = std::strtoull(argv[++i], nullptr, 10);
        // Which half of the machine's memory the frame storage sits on. It is a
        // diagnostic: putting the memory and the card on different halves on purpose is
        // how to ask whether two processes are fighting over the card or over the memory
        // controller.
        } else if (std::strcmp(a, "--frames-node") == 0 && has) {
            o->frames_node = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--drift-out") == 0 && has) {
            o->drift_out = argv[++i];
        } else {
            // An unknown flag fails rather than being ignored: ignoring it would let a run
            // proceed quietly with a configuration nobody asked for.
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return false;
        }
    }
    // The session name is a fixed ten byte field, padded with spaces. Without the padding
    // the remaining bytes are rubbish and the receiver sees a different session in every
    // packet.
    o->session.resize(mold::kSessionLen, ' ');
    return true;
}

// Reads the port's own MAC, so the frames carry a real source address.
//
// Read as text out of sysfs rather than through ef_vi_get_mac as the trader does. Either
// works; this way nothing has to be opened first.
bool own_mac(const char* intf, std::uint8_t out[eth::kMacBytes]) {
    // The file holds one line like "aa:bb:cc:dd:ee:ff".
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/class/net/%s/address", intf);
    FILE* f = std::fopen(path, "r");
    // Usually a mistyped port name.
    if (f == nullptr) return false;
    // Read into unsigned first: %x wants an unsigned*, and giving it a uint8_t* would
    // write over the bytes next to it.
    unsigned v[eth::kMacBytes];
    const int n = std::fscanf(f, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3],
                              &v[4], &v[5]);
    std::fclose(f);
    // All six or nothing. A wrong source address still sends, it just dirties the address
    // table of the switch.
    if (n != static_cast<int>(eth::kMacBytes)) return false;
    // The values are two hex digits each, so narrowing is safe.
    for (std::size_t i = 0; i < eth::kMacBytes; ++i) {
        out[i] = static_cast<std::uint8_t>(v[i]);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: sender <itch-file> [--intf I] [--src-ip A.B.C.D] "
                     "[--dst-ip A.B.C.D] [--src-port N] [--dst-port N] "
                     "[--dst-port-b N] [--max-messages N] [--speed 10x] "
                     "[--stop-after-window] "
                     "[--frames-node N] "
                     "[--drift-out FILE]\n");
        return 2;
    }
    Options opt;
    if (!parse(argc, argv, &opt)) return 2;

    eth::Endpoint src{{}, opt.src_ip, opt.src_port};
    eth::Endpoint dst_a{{}, opt.dst_ip, opt.dst_port};
    eth::Endpoint dst_b{{}, opt.dst_ip, opt.dst_port_b};
    if (!own_mac(opt.intf, src.mac)) {
        std::fprintf(stderr, "cannot read the MAC of %s\n", opt.intf);
        return 1;
    }
    eth::multicast_mac(dst_a.ip, dst_a.mac);
    std::memcpy(dst_b.mac, dst_a.mac, eth::kMacBytes);

    io::SeqReader reader(opt.itch, kReadBuffer);
    if (!reader.ok()) {
        std::fprintf(stderr, "cannot open %s\n", opt.itch);
        return 1;
    }

    huge::choose(opt.frames_node >= 0 ? opt.frames_node
                                      : huge::node_of_cpu(sched_getcpu()));
    ef::Vi vi;
    // The opposite of the trader: no receive ring at all and a full transmit ring, since
    // this program only sends.
    //
    // Transmit timestamps are deliberately not asked for, and that was learned the hard
    // way. With them, what "the send completed" means changes: the card produces three
    // events per frame and ef_vi drops the first - which is the one saying the descriptor
    // can be reused. The descriptor is then only freed once the second half of the
    // timestamp arrives, and when the other port is busy those events lag, so the
    // transmit ring stays full waiting for a number this side has no use for. Both
    // latencies in the report come from the consumer's card; not one timestamp is needed
    // here.
    if (!vi.open(opt.intf, 0, kTxRing, EF_VI_FLAGS_DEFAULT)) return 1;
    const char* arena_mb = std::getenv("ITCH_ARENA_MB");
    const std::size_t arena_slots =
        (arena_mb != nullptr ? std::strtoull(arena_mb, nullptr, 10) : kDefaultArenaMb)
        << 20 >> 9;
    ef::Frames frames;
    if (!frames.alloc(vi, arena_slots, kSlotBytes)) return 1;

    const win::Params wp = win::params_from_env();
    win::Tracker tracker(wp);
    pace::Schedule sched(pace::gap_ns_from_env(), opt.speed);
    std::uint64_t paced_due = 0, paced_act = 0, prev_due = 0, prev_now = 0;
    bool have_prev = false;
    pkt::Packing packing;
    mold::Packer packer(opt.session.data(), 1, pkt::kMaxMessages, eth::kHeaderBytes);

    const double tps = tsc::ticks_per_ns();
    std::uint64_t packets = 0, messages = 0, retries = 0, sent_bytes = 0;
    // How far the replay drifts inside one window, which is what actually
    // changes the spacing the consumer sees.
    std::uint64_t paced_windows = 0, worst_spread = 0, spread_sum = 0;
    // One row per window: how far behind the replay was when the window
    // opened, and how far it ranged while the window was being recorded.
    // The aggregate cannot tell a window that opened late and caught up
    // from one that fell behind while it was open, and those two have
    // different causes.
    struct Window { std::int64_t at_open, lo, hi; };
    std::vector<Window> per_window;
    std::int64_t lo = 0, hi = 0;
    bool in_window = false;
    std::uint64_t in_flight = 0;
    std::uint64_t tx_events = 0;
    std::uint64_t packet_ts = 0;
    win::Phase packet_phase = win::Phase::kGap;
    const std::size_t per_packet = opt.dst_port_b != 0 ? 2 : 1;

    // Frees whatever the card has finished with. One completion can cover
    // several descriptors, so it has to be unbundled to know how many.
    const auto poll = [&] {
        ef_event evs[64];
        // This interface only sends, so everything that arrives is a send report.
        const int n = ef_eventq_poll(vi.get(), evs, 64);
        for (int i = 0; i < n; ++i) {
            // These frames have gone.
            if (EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_TX) {
                // A completion is not one frame. The card bundles the completions of
                // several frames into a single event, so it has to be unbundled to learn
                // how many it stands for - which is what ef_vi_transmit_unbundle returns,
                // filling in the request ids as it goes.
                //
                // Counting one event as one frame would make in_flight climb without
                // bound; once it reached the depth of the ring every packet would block,
                // while the wire was in fact idle.
                ef_request_id ids[EF_VI_TRANSMIT_BATCH];
                // The array is sized by EF_VI_TRANSMIT_BATCH, the most one event can
                // stand for. Only the count is used here: a frame is never touched again
                // after it is built, so there is nothing to be told about. (The trader
                // does need the ids, because it reads the timestamp out of the
                // completion.)
                in_flight -= static_cast<std::uint64_t>(
                    ef_vi_transmit_unbundle(vi.get(), &evs[i], ids));
                ++tx_events;
            // A frame that did not go out.
            } else if (EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_TX_ERROR) {
                // On a direct cable this should not happen, so it is printed and looked
                // into.
                std::fprintf(stderr, "transmit error\n");
                // It is no longer with the card either, so it comes off the count.
                --in_flight;
            }
        }
    };
    // Collects completions until fewer than limit frames are still with the card. Done
    // before sending the next packet so there is always room in the ring - nothing should
    // have to wait at the moment a packet is due.
    const auto drain_to = [&](std::uint64_t limit) {
        while (in_flight > limit) poll();
    };

    // Before the clock starts, or the whole file would be read while the replay
    // it is timed against is already running.
    reader.fill();
    std::printf("input          %s, %.1f GB in memory\n", opt.itch, reader.size() / 1e9);


    // A packet that is made and waiting: where its frames are, how long they
    // are, and the moment it is due. Nothing else is needed once the moment
    // arrives, which is the whole point.
    struct Ready {
        std::uint32_t slot;
        std::uint16_t len;
        win::Phase phase;
        std::uint64_t due;
    };
    // Half the arena, and the half matters. Frames are written at one point in
    // the ring and read by the card at another, and both ends of the ring are a
    // bad place to be: run the queue almost empty and the card is reading lines
    // the processor wrote microseconds ago, run it almost full and the
    // processor is writing lines the card read microseconds ago. Either way the
    // two are fighting over the same cache lines. Kept half full, each is about
    // a gigabyte of frames away from the other, which is a second of replay
    // even at the busiest, and neither ever sees the other's line.
    const std::size_t ready_cap = arena_slots / per_packet / 2;
    std::vector<Ready> ready(ready_cap);
    std::size_t build_slot = 0;
    std::uint64_t build_idx = 0, send_idx = 0;
    bool exhausted = false;
    bool saw_window = false;

    // The whole file is in memory, so walking it is an index rather than a read
    // and a record can never be cut in half by a buffer boundary.
    const std::uint8_t* cur = reader.data();
    const std::uint8_t* const walk_end = cur + reader.size();

    const auto emit = [&] {
        const auto bytes = packer.seal();
        const std::size_t len = bytes.size();
        const std::size_t after_eth = len - eth::kHeaderBytes;
        std::uint8_t* a = frames.at(build_slot);
        std::memcpy(a + eth::kHeaderBytes, bytes.data() + eth::kHeaderBytes, after_eth);
        eth::write(a, src, dst_a, after_eth);
        // The second path carries the same bytes to a different port, the way
        // an exchange sends A and B.
        if (per_packet == 2) {
            std::uint8_t* b = frames.at(build_slot + 1);
            std::memcpy(b + eth::kHeaderBytes, a + eth::kHeaderBytes, after_eth);
            eth::write(b, src, dst_b, after_eth);
        }
        ready[build_idx % ready_cap] = {static_cast<std::uint32_t>(build_slot),
                                        static_cast<std::uint16_t>(len), packet_phase,
                                        sched.next(packet_ts, packet_phase)};
        ++build_idx;
        build_slot += per_packet;
        if (build_slot == arena_slots) build_slot = 0;
        ++packets;
        packing.close();
        packer.next();
    };

    // Builds one packet - the other half of this program, the send loop being the first.
    //
    // It walks forward from where the file was left, putting messages into a packet until
    // the gathering rule says the packet is finished, then records the frame and the
    // moment it is due and returns true. A false means the file is finished.
    //
    // Building and sending are deliberately separate: this is called over and over while
    // there is time to spare, filling the two gigabyte ring with hundreds of thousands of
    // finished packets ahead of time. At the moment one is due, only its address is
    // handed over.
    const auto build_one = [&] {
        // The file is finished and no more packets can be made, which is how the send
        // loop knows the run is over.
        if (exhausted) return false;
        // Message by message, until what is left is too short even for a length prefix.
        while (cur + itch::kLenPrefix <= walk_end) {
            const std::size_t body = itch::read_be<std::uint16_t>(cur);
            // A zero length, or a message running past the end: the file is finished or
            // damaged.
            if (body == 0 || cur + itch::kLenPrefix + body > walk_end) break;
            // A view, a pointer and a length, with nothing copied.
            const itch::Message m{cur + itch::kLenPrefix, static_cast<std::uint16_t>(body)};
            // The market time of this message, which is what the whole replay is paced by.
            const std::uint64_t ts = m.timestamp();
            // If this is the opening or closing system event, note it. The trader calls
            // the same function: each end learns the session from the feed, so a half day
            // needs no configuration at all.
            win::note_session(m, &tracker);
            // Which stretch this message falls in: window, warm up, tail or full speed.
            const win::Phase p = tracker.advance(ts);
            // Having been in a window and then back at full speed means the last window
            // has closed. With ITCH_WINDOW_FROM and _TO given there is one window whose
            // boundaries never move, so this is exactly "everything worth measuring has
            // been replayed".
            if (p == win::Phase::kWindow) saw_window = true;
            if (opt.stop_after_window && saw_window && p == win::Phase::kGap) break;
            // The whole record including its length prefix, which is what the gathering
            // counts.
            const std::size_t rec = body + itch::kLenPrefix;
            // The message that closes a packet belongs to the next one, so it
            // stays where it is and this call ends here.
            if (packing.should_close(ts, rec, p)) {
                emit();
                // cur deliberately does not move: this message belongs to the next
                // packet and will still be here on the next call.
                return true;
            }
            // It fits, so the gathering rule is told first - it counts the bytes and the
            // messages.
            packing.add(ts, rec, p);
            // Which stretch the packet belongs to, decided by its first message and not
            // changed afterwards.
            packet_phase = packing.phase();
            // The packet's moment is that of its last message, since a packet cannot go
            // out before its own last message has happened.
            packet_ts = ts;
            // Copied in as it is. Nothing is parsed and nothing is rewritten: the bytes
            // replayed are the bytes the exchange sent that day.
            packer.add(cur, rec);
            cur += rec;
            // The quickest setting, which turns half an hour into two minutes forty five.
            // The count was chosen against the original file, so on the thinned one it
            // cuts in the wrong place.
            if (++messages == opt.max_messages) break;
        }
        exhausted = true;
        const bool last = !packer.empty();
        if (last) emit();
        return last;
    };

    // Fill the lead before the clock starts, so the first window comes out of already
    // built frames like every other window. Without it the first window would be built
    // and sent at the same time, and its spacing would not be the day's.
    while (build_idx - send_idx < ready_cap - 1 && build_one()) {
    }
    std::printf("built ahead    %" PRIu64 " packets, %zu MB of frames\n", build_idx,
                frames.bytes() >> 20);

    const std::uint64_t build_guard = static_cast<std::uint64_t>(kBuildGuardNs * tps);
    // Time zero for the whole replay; every packet's moment is measured from it.
    const std::uint64_t start = tsc::now();
    // The send loop. Each turn does four things: take the next built packet, write its
    // descriptor into the ring, spin until the moment it is due, and ring the doorbell.
    for (;;) {
        // Everything built has gone, so build another; failing that, the file is done.
        if (send_idx == build_idx && !build_one()) break;
        // The next entry: which slot the frame is in, how long it is, when it is due.
        const Ready e = ready[send_idx % ready_cap];
        // The moment, converted to counter ticks now, so the spin below is one comparison
        // with no arithmetic in it.
        const std::uint64_t due = static_cast<std::uint64_t>(e.due * tps);

        drain_to(kMaxInFlight - per_packet);
        // This is what lets the program keep time, and it is worth spelling out.
        //
        // Sending a frame is really two steps:
        //   ef_vi_transmit_init  writes the descriptor into the ring; the card has not
        //                        looked yet
        //   ef_vi_transmit_push  rings the doorbell, and only then does the card send
        // (The trader uses ef_vi_transmit, which is both at once.)
        //
        // Split apart, the descriptor can be written well in advance and the doorbell
        // held back until the moment itself. All the waiting then happens before the
        // doorbell rather than before the descriptor, and the only thing left to do at
        // the moment is press it.
        //
        // So -EAGAIN here is not an error. It says the ring is full; collecting
        // completions makes room. And since the moment has not arrived yet, waiting costs
        // nothing at all.
        for (std::size_t i = 0; i < per_packet; ++i) {
            int rc;
            // Retried until it succeeds. It cannot spin forever: the card keeps returning
            // descriptors of frames already sent, unless the link is really down.
            while ((rc = ef_vi_transmit_init(vi.get(), frames.dma(e.slot + i), e.len,
                                             static_cast<ef_request_id>(e.slot + i))) ==
                   -EAGAIN) {
                // Counted, because a large number means we are trying to send faster than
                // the wire - the back pressure doing its job.
                ++retries;
                poll();
            }
            if (rc < 0) {
                std::fprintf(stderr, "ef_vi_transmit_init: %d\n", rc);
                return 1;
            }
        }

        std::uint64_t now = tsc::now() - start;
        while (now < due) {
            // Completions are collected here rather than when a descriptor is next
            // needed. Left until then, the count of frames the card has not reported
            // would climb to the depth of the ring, and after that every packet would
            // block once - while the wire was nearly idle.
            if (in_flight != 0) poll();
            // Whatever is left of the wait goes into making the frames that
            // come later, but only while there is more of it left than a packet
            // takes to build.
            if (!exhausted && build_idx - send_idx < ready_cap - 1 &&
                (due - now > build_guard || build_idx - send_idx < ready_cap / 8)) {
                build_one();
            }
            now = tsc::now() - start;
        }
        // The doorbell: this line is the moment of sending. The wait is already over, so
        // there is no arithmetic here, only one write - and one doorbell releases both
        // feeds of this packet at once.
        ef_vi_transmit_push(vi.get());
        in_flight += per_packet;
        // What the frames really occupy on the wire. Twenty four bytes more each for the
        // preamble, the check sequence and the gap that has to follow a frame. Leaving
        // those out would make the computed line rate too high, so it would look as
        // though there were headroom that is not there.
        sent_bytes += (e.len + 24) * per_packet;  // preamble, check and the gap after

        if (win::Tracker::one_to_one(e.phase)) {
            if (have_prev) {
                paced_due += due - prev_due;
                paced_act += now - prev_now;
            }
            have_prev = true;
            prev_due = due;
            prev_now = now;
            const std::int64_t behind = static_cast<std::int64_t>(now - due);
            // What matters is not how far behind the replay is but how much that changes
            // inside one window. Being late overall only shifts the whole stream along,
            // and the spacing between two packets is still the exchange's; the spacing
            // only changes when the amount of lateness changes. A window is where the
            // samples come from, so the spread is measured there - the difference between
            // the least and the most behind, which is the most any spacing inside that
            // window can be out by.
            if (e.phase == win::Phase::kWindow) {
                // The window has just opened: record how far behind it started, and begin
                // the range from there.
                if (!in_window) {
                    in_window = true;
                    lo = hi = behind;
                    ++paced_windows;
                    // A row per window when the drift is being exported: how far behind at
                    // the open, and the least and the most behind while it was open. The
                    // summary cannot tell a window that opened late and caught up from one
                    // that fell further behind while open, and those have different causes.
                    if (opt.drift_out != nullptr) per_window.push_back({behind, behind, behind});
                } else {
                    // Inside a window, only the range moves.
                    if (behind < lo) lo = behind;
                    if (behind > hi) hi = behind;
                }
            // Just left a window, so its spread is settled.
            } else if (in_window) {
                in_window = false;
                const auto spread = static_cast<std::uint64_t>(hi - lo);
                spread_sum += spread;
                if (spread > worst_spread) worst_spread = spread;
                if (!per_window.empty()) {
                    per_window.back().lo = lo;
                    per_window.back().hi = hi;
                }
            }
        } else {
            have_prev = false;
        }
        ++send_idx;
    }
    // Wait for the last frames to reach the wire. Exiting with them still in the ring
    // would look to the consumer like the feed had stopped dead.
    drain_to(0);
    const double wall = (tsc::now() - start) / tps / 1e9;

    // The report. Read it in this order: first whether the replay was faithful, which is
    // the paced replay lines, and then whether the rate was right, which is the wall time
    // line.
    //
    // Frames on the wire. With two feeds every packet goes out twice.
    const std::uint64_t frames_sent = packets * (opt.dst_port_b != 0 ? 2 : 1);
    std::printf("interface      %s, src port %u, dst %u.%u.%u.%u:%u%s\n", opt.intf,
                opt.src_port, opt.dst_ip >> 24, (opt.dst_ip >> 16) & 0xff,
                (opt.dst_ip >> 8) & 0xff, opt.dst_ip & 0xff, opt.dst_port,
                opt.dst_port_b != 0 ? " and a second feed" : "");
    // How many ITCH messages went out. It should match the trader's count; a difference
    // means something was lost in between.
    std::printf("messages       %" PRIu64 "\n", messages);
    // And how they were gathered. The average says a lot about the shape of a run: one or
    // two messages a packet while a window is replayed at the real pace, a dozen or more
    // when a frame is filled at full speed, and an overall average in the teens because
    // 97.7% of the packets are in the fast stretch.
    std::printf("packets        %" PRIu64 " (%.2f messages each)\n", packets,
                packets ? static_cast<double>(messages) / packets : 0.0);
    std::printf("frames on wire %" PRIu64 ", %.1f MB\n", frames_sent, sent_bytes / 1e6);
    // How long it took, and the average rate. The Gb/s figure is how to tell whether the
    // wire is the limit: close to ten and it is full, and asking for more will not help.
    std::printf("wall time      %.3f s (%.2f M messages/s, %.2f Gb/s)\n", wall,
                messages / wall / 1e6, sent_bytes * 8.0 / wall / 1e9);
    // How often the ring was full and the handover had to be retried. A large number is
    // not a problem - it is the hardware's back pressure working, because we were trying
    // to send faster than the wire.
    std::printf("transmit ring  %" PRIu64 " retries on a full ring\n", retries);
    // The line that matters most in this report: was the replay faithful. It gives the
    // difference, within a window, between the least and the most behind.
    //
    // Why the spread rather than the lateness: being late overall only shifts the whole
    // stream, and the spacing between packets is still the exchange's. Spacing only
    // changes when the lateness changes. Tens of microseconds here means the consumer did
    // not see the day's rhythm, and the latency measured in that run does not stand for a
    // real market.
    std::printf("replay speed   %.3fx asked for, %.3fx actually delivered in the "
                "paced stretch\n",
                sched.speed() / double(pace::kUnitSpeed),
                paced_act != 0 ? sched.speed() / double(pace::kUnitSpeed) *
                                     static_cast<double>(paced_due) /
                                     static_cast<double>(paced_act)
                               : 0.0);
    std::printf("paced replay   %" PRIu64 " windows, spacing off by up to %.1f us in the worst "
                "and %.1f us on average\n",
                paced_windows, worst_spread / tps / 1e3,
                paced_windows ? spread_sum / static_cast<double>(paced_windows) / tps / 1e3 : 0.0);
    // Completions against frames. Dividing one by the other says how many frames the card
    // bundles into an event on average, which is exactly why the unbundling above is
    // needed.
    std::printf("completions    %" PRIu64 " events for %" PRIu64 " frames\n", tx_events,
                frames_sent);
    if (opt.drift_out != nullptr) {
        if (std::FILE* f = std::fopen(opt.drift_out, "w")) {
            std::fprintf(f, "window,at_open_us,lowest_us,highest_us\n");
            for (std::size_t i = 0; i < per_window.size(); ++i) {
                const auto& w = per_window[i];
                std::fprintf(f, "%zu,%.1f,%.1f,%.1f\n", i, w.at_open / tps / 1e3,
                             w.lo / tps / 1e3, w.hi / tps / 1e3);
            }
            std::fclose(f);
            std::printf("drift by window %s, %zu rows\n", opt.drift_out, per_window.size());
        } else {
            std::fprintf(stderr, "cannot write %s\n", opt.drift_out);
        }
    }
    // The window parameters this run used, printed as they were loaded. There is one
    // reason for this line: both ends have to use the same values and they work them out
    // separately, without talking. If one is set wrongly, comparing the two logs shows it
    // - rather than two different experiments running quietly side by side.
    std::printf("window         shift=%" PRIu64 " mask=%" PRIu64 " slot=%" PRIu64
                " settle=%" PRIu64 " ms tail=%" PRIu64 " ms, rate %" PRIu64
                " ns per packet\n",
                wp.shift, wp.mask, wp.slot, wp.settle_ns / 1000000,
                wp.tail_ns / 1000000, sched.gap_ns());
    return 0;
}
