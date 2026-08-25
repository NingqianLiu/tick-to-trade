
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/settings.hpp"
#include "common/window.hpp"
#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "net/pace.hpp"
#include "net/pack.hpp"
#include "net/packet_file.hpp"
#include "net/mold.hpp"

namespace {

constexpr std::size_t kReadBuffer = 32u << 20;

struct Options {
    std::uint64_t from_ns = 0;
    std::uint64_t to_ns = ~std::uint64_t{0};
    std::uint64_t max_messages = 0;
    std::string session = "ITCHBENCH0";
};

bool parse(int argc, char** argv, Options* o) {
    for (int i = 3; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--from-sec") == 0 && has_value) {
            o->from_ns = std::strtoull(argv[++i], nullptr, 10) * 1000000000ull;
        } else if (std::strcmp(argv[i], "--to-sec") == 0 && has_value) {
            o->to_ns = std::strtoull(argv[++i], nullptr, 10) * 1000000000ull;
        } else if (std::strcmp(argv[i], "--max-messages") == 0 && has_value) {
            o->max_messages = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--session") == 0 && has_value) {
            o->session = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return false;
        }
    }
    o->session.resize(mold::kSessionLen, ' ');
    return true;
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: make_packets <itch-file> <out-file> [--from-sec N] "
                     "[--to-sec N] [--max-messages N] [--session S]\n");
        return 2;
    }
    Options opt;
    if (!parse(argc, argv, &opt)) return 2;

    io::SeqReader reader(argv[1], kReadBuffer);
    if (!reader.ok()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    const std::string body_path = std::string(argv[2]) + ".tmp";
    FILE* body = std::fopen(body_path.c_str(), "wb");
    FILE* ent = std::tmpfile();
    if (body == nullptr || ent == nullptr) {
        std::fprintf(stderr, "cannot open scratch files\n");
        return 1;
    }

    const win::Params wp = win::params_from_env();
    win::Tracker tracker(wp);
    mold::Packer packer(opt.session.data(), 1, pkt::kMaxMessages, pkt::kHeadroom);
    std::uint64_t packets = 0, messages = 0, offset = 0;
    std::uint64_t in_window = 0, in_settle = 0, in_tail = 0;
    std::uint64_t first_ts = 0, last_ts = 0, packet_ts = 0;
    pkt::Packing packing;
    pace::Schedule sched(pace::gap_ns_from_env());
    std::uint64_t run_ns = 0;
    win::Phase packet_phase = win::Phase::kGap;
    bool stopped = false;

    const auto flush = [&] {
        const auto bytes = packer.seal();
        const pkt::Entry e{packet_ts, offset,
                           static_cast<std::uint32_t>(bytes.size()), packer.count()};
        std::fwrite(&e, sizeof(e), 1, ent);
        std::fwrite(bytes.data(), 1, bytes.size(), body);
        offset += bytes.size();
        run_ns = sched.next(packet_ts, packet_phase);
        last_ts = packet_ts;
        ++packets;
        packing.close();
        packer.next();
    };

    while (!stopped && reader.fill()) {
        const auto r = itch::for_each_message(
            reader.data(), reader.size(), [&](const itch::Message& m) {
                const std::uint64_t ts = m.timestamp();
                if (ts < opt.from_ns) return true;
                if (ts >= opt.to_ns) return false;
                win::note_session(m, &tracker);
                const win::Phase p = tracker.advance(ts);
                const std::size_t rec = m.len + itch::kLenPrefix;
                if (packing.should_close(ts, rec, p)) flush();
                packing.add(ts, rec, p);
                packet_phase = packing.phase();
                packet_ts = ts;
                if (p == win::Phase::kWindow) ++in_window;
                if (p == win::Phase::kSettle) ++in_settle;
                if (p == win::Phase::kTail) ++in_tail;
                packer.add(m.body - itch::kLenPrefix, rec);
                if (messages == 0) first_ts = ts;
                ++messages;
                return opt.max_messages == 0 || messages < opt.max_messages;
            });
        reader.consume(r.consumed);
        if (r.stop == itch::FrameStop::kCallerStopped ||
            r.stop == itch::FrameStop::kZeroLength) {
            stopped = true;
        } else if (r.consumed == 0) {
            break;
        }
    }
    if (!packer.empty()) flush();
    std::fclose(body);

    pkt::FileHeader h{};
    h.magic = pkt::kMagic;
    h.headroom = pkt::kHeadroom;
    h.packets = packets;
    h.messages = messages;
    h.first_seq = 1;
    h.body_bytes = offset;
    h.span_ns = last_ts - first_ts;
    std::memcpy(h.session, opt.session.data(), mold::kSessionLen);

    FILE* out = std::fopen(argv[2], "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::fwrite(&h, sizeof(h), 1, out);
    std::rewind(ent);
    std::vector<pkt::Entry> chunk(1 << 16);
    for (std::size_t n; (n = std::fread(chunk.data(), sizeof(pkt::Entry),
                                        chunk.size(), ent)) > 0;) {
        std::fwrite(chunk.data(), sizeof(pkt::Entry), n, out);
    }
    std::fclose(ent);
    body = std::fopen(body_path.c_str(), "rb");
    std::vector<std::uint8_t> buf(1 << 20);
    for (std::size_t n; (n = std::fread(buf.data(), 1, buf.size(), body)) > 0;) {
        std::fwrite(buf.data(), 1, n, out);
    }
    std::fclose(body);
    std::fclose(out);
    std::remove(body_path.c_str());

    std::printf("messages       %" PRIu64 "\n", messages);
    std::printf("packets        %" PRIu64 " (%.2f messages each, %" PRIu64
                " ns window, %zu B frame limit)\n",
                packets, packets ? static_cast<double>(messages) / packets : 0.0,
                cfg::kCoalesceNs, cfg::kMaxPacketPayload);
    std::printf("sequence       %" PRIu64 " .. %" PRIu64 "\n", h.first_seq,
                h.first_seq + messages - 1);
    std::printf("in window      %" PRIu64 " (%.2f%%)\n", in_window,
                messages ? 100.0 * in_window / messages : 0.0);
    std::printf("in settle      %" PRIu64 " (%.2f%%)\n", in_settle,
                messages ? 100.0 * in_settle / messages : 0.0);
    std::printf("in tail        %" PRIu64 " (%.2f%%)\n", in_tail,
                messages ? 100.0 * in_tail / messages : 0.0);
    const std::uint64_t at_rate = messages - in_window - in_settle - in_tail;
    std::printf("at fixed rate  %" PRIu64 " (%.2f%%)\n", at_rate,
                messages ? 100.0 * at_rate / messages : 0.0);
    std::printf("ITCH span      %" PRIu64 " .. %" PRIu64 " ns\n", first_ts, last_ts);
    std::printf("window         shift=%" PRIu64 " mask=%" PRIu64 " slot=%" PRIu64
                " settle=%" PRIu64 " ms tail=%" PRIu64
                " ms (unit %.6f s, period %.3f s)\n",
                wp.shift, wp.mask, wp.slot, wp.settle_ns / 1000000,
                wp.tail_ns / 1000000, wp.unit_ns() / 1e9, wp.period_ns() / 1e9);
    std::printf("run time       %.1f s (%.1f min) at %" PRIu64
                " ns per packet of fixed rate\n",
                run_ns / 1e9, run_ns / 6e10, sched.gap_ns());
    std::printf("body           %.1f MB\n", offset / 1048576.0);
    return 0;
}
