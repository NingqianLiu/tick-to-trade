
#include <sched.h>

#include <etherfabric/ef_vi.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/hist.hpp"
#include "common/huge.hpp"
#include "common/settings.hpp"
#include "common/tsc.hpp"
#include "common/window.hpp"
#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "net/ef.hpp"
#include "net/eth.hpp"
#include "net/mold.hpp"
#include "net/pace.hpp"
#include "net/pack.hpp"

namespace {

constexpr std::size_t kReadBuffer = 0;
constexpr std::size_t kSlotBytes = 512;
static_assert(cfg::kMaxFrameBytes <= kSlotBytes);
constexpr std::size_t kDefaultArenaMb = 2048;
constexpr int kTxRing = 2047;
constexpr std::uint64_t kMaxInFlight = kTxRing - 8;
constexpr std::uint64_t kBuildGuardNs = 3000;

struct Options {
    const char* itch = nullptr;
    const char* intf = "enp129s0f0";
    std::uint32_t src_ip = eth::ipv4(10, 9, 9, 1);
    std::uint32_t dst_ip = eth::ipv4(239, 9, 9, 1);
    std::uint16_t src_port = 40000;
    std::uint16_t dst_port = 26477;
    std::uint16_t dst_port_b = 0;
    std::uint64_t max_messages = 0;
    int frames_node = -1;
    bool profile = false;
    const char* drift_out = nullptr;
    std::string session = "ITCHBENCH0";
};

bool parse_ip(const char* s, std::uint32_t* out) {
    unsigned a, b, c, d;
    if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = eth::ipv4(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
                     static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(d));
    return true;
}

bool parse(int argc, char** argv, Options* o) {
    o->itch = argv[1];
    for (int i = 2; i < argc; ++i) {
        const bool has = i + 1 < argc;
        const char* a = argv[i];
        if (std::strcmp(a, "--intf") == 0 && has) {
            o->intf = argv[++i];
        } else if (std::strcmp(a, "--src-ip") == 0 && has) {
            if (!parse_ip(argv[++i], &o->src_ip)) return false;
        } else if (std::strcmp(a, "--dst-ip") == 0 && has) {
            if (!parse_ip(argv[++i], &o->dst_ip)) return false;
        } else if (std::strcmp(a, "--src-port") == 0 && has) {
            o->src_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--dst-port") == 0 && has) {
            o->dst_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--dst-port-b") == 0 && has) {
            o->dst_port_b = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--max-messages") == 0 && has) {
            o->max_messages = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(a, "--frames-node") == 0 && has) {
            o->frames_node = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--drift-out") == 0 && has) {
            o->drift_out = argv[++i];
        } else if (std::strcmp(a, "--profile") == 0) {
            o->profile = true;
        } else if (std::strcmp(a, "--session") == 0 && has) {
            o->session = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            return false;
        }
    }
    o->session.resize(mold::kSessionLen, ' ');
    return true;
}

bool own_mac(const char* intf, std::uint8_t out[eth::kMacBytes]) {
    char path[128];
    std::snprintf(path, sizeof(path), "/sys/class/net/%s/address", intf);
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    unsigned v[eth::kMacBytes];
    const int n = std::fscanf(f, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3],
                              &v[4], &v[5]);
    std::fclose(f);
    if (n != static_cast<int>(eth::kMacBytes)) return false;
    for (std::size_t i = 0; i < eth::kMacBytes; ++i) {
        out[i] = static_cast<std::uint8_t>(v[i]);
    }
    return true;
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: sender <itch-file> [--intf I] [--src-ip A.B.C.D] "
                     "[--dst-ip A.B.C.D] [--src-port N] [--dst-port N] "
                     "[--dst-port-b N] [--max-messages N] [--session S]\n");
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
    if (!vi.open(opt.intf, 0, kTxRing, EF_VI_FLAGS_DEFAULT)) return 1;
    const char* arena_mb = std::getenv("ITCH_ARENA_MB");
    const std::size_t arena_slots =
        (arena_mb != nullptr ? std::strtoull(arena_mb, nullptr, 10) : kDefaultArenaMb)
        << 20 >> 9;
    ef::Frames frames;
    if (!frames.alloc(vi, arena_slots, kSlotBytes)) return 1;

    const win::Params wp = win::params_from_env();
    win::Tracker tracker(wp);
    pace::Schedule sched(pace::gap_ns_from_env());
    pkt::Packing packing;
    mold::Packer packer(opt.session.data(), 1, pkt::kMaxMessages, eth::kHeaderBytes);

    const double tps = tsc::ticks_per_ns();
    std::uint64_t packets = 0, messages = 0, retries = 0, sent_bytes = 0;
    std::uint64_t late = 0, late_ticks = 0, worst_late = 0;
    std::uint64_t paced_windows = 0, worst_spread = 0, spread_sum = 0;
    struct Window { std::int64_t at_open, lo, hi; };
    std::vector<Window> per_window;
    std::int64_t lo = 0, hi = 0;
    bool in_window = false;
    std::uint64_t in_flight = 0;
    std::uint64_t tx_events = 0;
    std::uint64_t packet_ts = 0;
    win::Phase packet_phase = win::Phase::kGap;
    const std::size_t per_packet = opt.dst_port_b != 0 ? 2 : 1;

    const auto poll = [&] {
        ef_event evs[64];
        const int n = ef_eventq_poll(vi.get(), evs, 64);
        for (int i = 0; i < n; ++i) {
            if (EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_TX) {
                ef_request_id ids[EF_VI_TRANSMIT_BATCH];
                in_flight -= static_cast<std::uint64_t>(
                    ef_vi_transmit_unbundle(vi.get(), &evs[i], ids));
                ++tx_events;
            } else if (EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_TX_ERROR) {
                std::fprintf(stderr, "transmit error\n");
                --in_flight;
            }
        }
    };
    const auto drain_to = [&](std::uint64_t limit) {
        while (in_flight > limit) poll();
    };

    reader.fill();
    std::printf("input          %s, %.1f GB in memory\n", opt.itch, reader.size() / 1e9);

    std::uint64_t t_build = 0, t_queue = 0, t_wait = 0, t_push = 0;
    std::uint64_t t_reap = 0, t_init = 0, worst_reap = 0, worst_init = 0, worst_push = 0;
    hist::Hist h_reap, h_init, h_push, h_build;
    std::uint64_t worst_queue = 0, worst_build = 0, thin_queue = ~0ull;
    std::uint64_t worst_gap = 0, long_gaps = 0;
    const auto clock = [&] { return opt.profile ? tsc::fenced() : 0; };

    struct Ready {
        std::uint32_t slot;
        std::uint16_t len;
        win::Phase phase;
        std::uint64_t due;
    };
    const std::size_t ready_cap = arena_slots / per_packet / 2;
    std::vector<Ready> ready(ready_cap);
    std::size_t build_slot = 0;
    std::uint64_t build_idx = 0, send_idx = 0;
    bool exhausted = false;

    const std::uint8_t* cur = reader.data();
    const std::uint8_t* const walk_end = cur + reader.size();

    const auto emit = [&] {
        const auto bytes = packer.seal();
        const std::size_t len = bytes.size();
        const std::size_t after_eth = len - eth::kHeaderBytes;
        std::uint8_t* a = frames.at(build_slot);
        std::memcpy(a + eth::kHeaderBytes, bytes.data() + eth::kHeaderBytes, after_eth);
        eth::write(a, src, dst_a, after_eth);
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

    const auto build_one = [&] {
        if (exhausted) return false;
        const std::uint64_t t0 = clock();
        while (cur + itch::kLenPrefix <= walk_end) {
            const std::size_t body = itch::read_be<std::uint16_t>(cur);
            if (body == 0 || cur + itch::kLenPrefix + body > walk_end) break;
            const itch::Message m{cur + itch::kLenPrefix, static_cast<std::uint16_t>(body)};
            const std::uint64_t ts = m.timestamp();
            win::note_session(m, &tracker);
            const win::Phase p = tracker.advance(ts);
            const std::size_t rec = body + itch::kLenPrefix;
            if (packing.should_close(ts, rec, p)) {
                emit();
                if (opt.profile) t_build += tsc::now() - t0;
                return true;
            }
            packing.add(ts, rec, p);
            packet_phase = packing.phase();
            packet_ts = ts;
            packer.add(cur, rec);
            cur += rec;
            if (++messages == opt.max_messages) break;
        }
        exhausted = true;
        const bool last = !packer.empty();
        if (last) emit();
        if (opt.profile) t_build += tsc::now() - t0;
        return last;
    };

    while (build_idx - send_idx < ready_cap - 1 && build_one()) {
    }
    std::printf("built ahead    %" PRIu64 " packets, %zu MB of frames\n", build_idx,
                frames.bytes() >> 20);

    const std::uint64_t build_guard = static_cast<std::uint64_t>(kBuildGuardNs * tps);
    const std::uint64_t gap_floor = static_cast<std::uint64_t>(20000 * tps);
    const std::uint64_t start = tsc::now();
    for (;;) {
        if (send_idx == build_idx && !build_one()) break;
        const Ready e = ready[send_idx % ready_cap];
        const std::uint64_t due = static_cast<std::uint64_t>(e.due * tps);

        const std::uint64_t t0 = clock();
        drain_to(kMaxInFlight - per_packet);
        const std::uint64_t t0b = clock();
        for (std::size_t i = 0; i < per_packet; ++i) {
            int rc;
            while ((rc = ef_vi_transmit_init(vi.get(), frames.dma(e.slot + i), e.len,
                                             static_cast<ef_request_id>(e.slot + i))) ==
                   -EAGAIN) {
                ++retries;
                poll();
            }
            if (rc < 0) {
                std::fprintf(stderr, "ef_vi_transmit_init: %d\n", rc);
                return 1;
            }
        }

        const std::uint64_t t1 = clock();
        if (opt.profile) {
            t_reap += t0b - t0;
            t_init += t1 - t0b;
            if (t0b - t0 > worst_reap) worst_reap = t0b - t0;
            if (t1 - t0b > worst_init) worst_init = t1 - t0b;
            h_reap.add(static_cast<std::uint64_t>((t0b - t0) / tps));
            h_init.add(static_cast<std::uint64_t>((t1 - t0b) / tps));
        }
        const std::uint64_t was_building = t_build;
        std::uint64_t now = tsc::now() - start;
        while (now < due) {
            const std::uint64_t was = now;
            if (in_flight != 0) poll();
            if (!exhausted && build_idx - send_idx < ready_cap - 1 &&
                (due - now > build_guard || build_idx - send_idx < ready_cap / 8)) {
                const std::uint64_t b0 = tsc::now();
                build_one();
                const std::uint64_t took = tsc::now() - b0;
                if (took > worst_build) worst_build = took;
                if (opt.profile) h_build.add(static_cast<std::uint64_t>(took / tps));
            }
            now = tsc::now() - start;
            const std::uint64_t gap = now - was;
            if (gap > worst_gap) worst_gap = gap;
            if (gap > gap_floor) ++long_gaps;
        }
        const std::uint64_t t2 = clock();
        ef_vi_transmit_push(vi.get());
        in_flight += per_packet;
        sent_bytes += (e.len + 24) * per_packet;
        if (!exhausted && build_idx - send_idx < thin_queue) thin_queue = build_idx - send_idx;
        if (opt.profile) {
            const std::uint64_t pushed = tsc::fenced() - t2;
            if (pushed > worst_push) worst_push = pushed;
            h_push.add(static_cast<std::uint64_t>(pushed / tps));
            t_queue += t1 - t0;
            if (t1 - t0 > worst_queue) worst_queue = t1 - t0;
            t_wait += (t2 - t1) - (t_build - was_building);
            t_push += tsc::now() - t2;
        }

        if (win::Tracker::one_to_one(e.phase)) {
            const std::int64_t behind = static_cast<std::int64_t>(now - due);
            if (behind > 0) {
                ++late;
                late_ticks += static_cast<std::uint64_t>(behind);
                if (static_cast<std::uint64_t>(behind) > worst_late) {
                    worst_late = static_cast<std::uint64_t>(behind);
                }
            }
            if (e.phase == win::Phase::kWindow) {
                if (!in_window) {
                    in_window = true;
                    lo = hi = behind;
                    ++paced_windows;
                    if (opt.drift_out != nullptr) per_window.push_back({behind, behind, behind});
                } else {
                    if (behind < lo) lo = behind;
                    if (behind > hi) hi = behind;
                }
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
        }
        ++send_idx;
    }
    drain_to(0);
    const double wall = (tsc::now() - start) / tps / 1e9;

    const std::uint64_t frames_sent = packets * (opt.dst_port_b != 0 ? 2 : 1);
    std::printf("interface      %s, src port %u, dst %u.%u.%u.%u:%u%s\n", opt.intf,
                opt.src_port, opt.dst_ip >> 24, (opt.dst_ip >> 16) & 0xff,
                (opt.dst_ip >> 8) & 0xff, opt.dst_ip & 0xff, opt.dst_port,
                opt.dst_port_b != 0 ? " and a second feed" : "");
    std::printf("messages       %" PRIu64 "\n", messages);
    std::printf("packets        %" PRIu64 " (%.2f messages each)\n", packets,
                packets ? static_cast<double>(messages) / packets : 0.0);
    std::printf("frames on wire %" PRIu64 ", %.1f MB\n", frames_sent, sent_bytes / 1e6);
    std::printf("wall time      %.3f s (%.2f M messages/s, %.2f Gb/s)\n", wall,
                messages / wall / 1e6, sent_bytes * 8.0 / wall / 1e9);
    std::printf("transmit ring  %" PRIu64 " retries on a full ring\n", retries);
    std::printf("paced replay   %" PRIu64 " windows, spacing off by up to %.1f us in the worst "
                "and %.1f us on average\n",
                paced_windows, worst_spread / tps / 1e3,
                paced_windows ? spread_sum / static_cast<double>(paced_windows) / tps / 1e3 : 0.0);
    std::printf("               %" PRIu64 " packets behind their slot, %.3f s in total, "
                "worst %.1f us (a steady offset moves nothing)\n",
                late, late_ticks / tps / 1e9, worst_late / tps / 1e3);
    if (opt.profile) {
        const double f = frames_sent != 0 ? frames_sent : 1;
        std::printf("per frame      queue %.0f, wait %.0f, doorbell %.0f ns on the timed "
                    "path; building %.0f ns, off it\n",
                    t_queue / tps / f, t_wait / tps / f, t_push / tps / f, t_build / tps / f);
        std::printf("               of the queue: completions %.0f ns, descriptors %.0f ns; "
                    "worst one of each %.1f / %.1f / doorbell %.1f us\n",
                    t_reap / tps / f, t_init / tps / f, worst_reap / tps / 1e3,
                    worst_init / tps / 1e3, worst_push / tps / 1e3);
        std::printf("               worst queue %.1f us, worst build in the run %.1f us, "
                    "run-up never below %" PRIu64 " packets\n",
                    worst_queue / tps / 1e3, worst_build / tps / 1e3, thin_queue);
        const auto show = [&](const char* what, const hist::Hist& h) {
            std::printf("  %-12s p50 %6" PRIu64 "  p90 %7" PRIu64 "  p99 %8" PRIu64
                        "  p99.9 %9" PRIu64 "  p99.99 %10" PRIu64 "  max %10" PRIu64
                        " ns over %" PRIu64 "\n",
                        what, h.quantile(0.5), h.quantile(0.9), h.quantile(0.99),
                        h.quantile(0.999), h.quantile(0.9999), h.largest(), h.samples());
        };
        show("completions", h_reap);
        show("descriptors", h_init);
        show("doorbell", h_push);
        show("building", h_build);
        std::printf("               longest the thread went without running %.1f us, "
                    "%" PRIu64 " turns over 20 us\n", worst_gap / tps / 1e3, long_gaps);
    }
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
    std::printf("window         shift=%" PRIu64 " mask=%" PRIu64 " slot=%" PRIu64
                " settle=%" PRIu64 " ms tail=%" PRIu64 " ms, rate %" PRIu64
                " ns per packet\n",
                wp.shift, wp.mask, wp.slot, wp.settle_ns / 1000000,
                wp.tail_ns / 1000000, sched.gap_ns());
    return 0;
}
