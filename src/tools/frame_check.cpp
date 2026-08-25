#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "common/settings.hpp"
#include "common/window.hpp"
#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "net/packet_file.hpp"
#include "net/mold.hpp"

namespace {

constexpr std::size_t kReadBuffer = 32u << 20;
std::uint64_t errors = 0;

void fail(const char* what, std::uint64_t where) {
    if (++errors <= 10) {
        std::fprintf(stderr, "packet %" PRIu64 ": %s\n", where, what);
    }
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: frame_check <packet-file> [itch-file]\n");
        return 2;
    }
    const int fd = ::open(argv[1], O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    const auto* base = static_cast<const std::uint8_t*>(
        ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0));
    if (base == MAP_FAILED) {
        std::fprintf(stderr, "cannot map %s\n", argv[1]);
        return 1;
    }
    ::madvise(const_cast<std::uint8_t*>(base), static_cast<std::size_t>(st.st_size),
              MADV_SEQUENTIAL);
    pkt::FileHeader h{};
    if (static_cast<std::size_t>(st.st_size) < sizeof(h)) {
        std::fprintf(stderr, "not a make_packets file\n");
        return 1;
    }
    std::memcpy(&h, base, sizeof(h));
    const auto* entries = reinterpret_cast<const pkt::Entry*>(base + sizeof(h));
    const std::uint8_t* body =
        base + sizeof(h) + h.packets * sizeof(pkt::Entry);
    if (h.magic != pkt::kMagic ||
        static_cast<std::uint64_t>(st.st_size) !=
            sizeof(h) + h.packets * sizeof(pkt::Entry) + h.body_bytes) {
        std::fprintf(stderr, "not a make_packets file, or truncated\n");
        return 1;
    }

    io::SeqReader src(argc > 2 ? argv[2] : "", kReadBuffer);
    const bool compare = argc > 2 && src.ok();
    if (argc > 2 && !compare) {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
    }
    std::size_t src_off = 0;
    if (compare) src.fill();

    const win::Params wp = win::params_from_env();
    win::Tracker tracker(wp);
    std::uint64_t phase_count[4] = {0, 0, 0, 0};

    std::uint64_t expect_seq = h.first_seq;
    std::uint64_t expect_off = 0;
    std::uint64_t messages = 0, compared = 0, last_ts = 0, largest = 0;
    for (std::uint64_t i = 0; i < h.packets; ++i) {
        const pkt::Entry& e = entries[i];
        if (e.offset != expect_off) fail("offset is not contiguous", i);
        if (e.offset + e.len > h.body_bytes) {
            fail("packet runs past the body", i);
            break;
        }
        if (e.len < h.headroom + mold::kHeaderLen) {
            fail("packet is shorter than its own header", i);
            break;
        }
        if (e.ts_ns < last_ts) fail("ITCH timestamp goes backwards", i);
        last_ts = e.ts_ns;
        expect_off += e.len;
        largest = std::max<std::uint64_t>(largest, e.len);

        const std::uint8_t* p = body + e.offset + h.headroom;
        if (std::memcmp(p, h.session, mold::kSessionLen) != 0) fail("wrong session", i);
        if (mold::sequence(p) != expect_seq) fail("sequence has a hole", i);
        if (mold::count(p) != e.messages) fail("header count and entry disagree", i);

        const std::size_t payload = e.len - h.headroom - mold::kHeaderLen;
        if (payload > cfg::kMaxPacketPayload) fail("packet does not fit the frame", i);
        std::uint64_t seen = 0;
        std::uint64_t open_ts = 0;
        win::Phase phase = win::Phase::kGap;
        const auto r = itch::for_each_message(
            p + mold::kHeaderLen, payload, [&](const itch::Message& m) {
                ++seen;
                if (m.len != itch::kBodyLen[static_cast<unsigned char>(m.type())]) {
                    fail("message length does not match its type", i);
                }
                win::note_session(m, &tracker);
                const win::Phase ph = tracker.advance(m.timestamp());
                if (seen == 1) {
                    phase = ph;
                    open_ts = m.timestamp();
                } else if (ph != phase) {
                    fail("packet spans two pacing settings", i);
                }
                if (win::Tracker::one_to_one(phase) &&
                    m.timestamp() - open_ts >= cfg::kCoalesceNs) {
                    fail("packet was held past the coalescing window", i);
                }
                if (compare) {
                    const std::size_t need = m.len + itch::kLenPrefix;
                    if (src_off + need > src.size()) {
                        src.consume(src_off);
                        src_off = 0;
                        src.fill();
                    }
                    if (src_off + need > src.size()) {
                        fail("source file ended early", i);
                    } else if (std::memcmp(src.data() + src_off,
                                           m.body - itch::kLenPrefix, need) != 0) {
                        fail("message body differs from the source file", i);
                    } else {
                        ++compared;
                    }
                    src_off += need;
                }
                return true;
            });
        if (r.consumed != payload) fail("payload has leftover bytes", i);
        if (seen != e.messages) fail("payload holds a different number of messages", i);
        if (seen != 0) phase_count[static_cast<int>(phase)] += seen;
        messages += seen;
        expect_seq += e.messages;
    }

    if (expect_off != h.body_bytes) fail("body has trailing bytes", h.packets);
    if (messages != h.messages) fail("message total disagrees with the header", h.packets);

    std::printf("packets        %" PRIu64 "\n", h.packets);
    std::printf("messages       %" PRIu64 "\n", messages);
    std::printf("sequence       %" PRIu64 " .. %" PRIu64 " (no holes: %s)\n",
                h.first_seq, expect_seq - 1,
                expect_seq == h.first_seq + h.messages ? "yes" : "NO");
    std::printf("largest packet %" PRIu64 " B payload + %u B headroom (limit %zu B)\n",
                largest - h.headroom - mold::kHeaderLen, h.headroom,
                cfg::kMaxPacketPayload);
    std::printf("packing        %.2f messages each, %" PRIu64 " ns window\n",
                h.packets ? static_cast<double>(messages) / h.packets : 0.0,
                cfg::kCoalesceNs);
    std::printf("ITCH span      %.3f s\n", last_ts / 1e9);
    std::printf("pacing         window %" PRIu64 "  settle %" PRIu64
                "  tail %" PRIu64 "  fixed rate %" PRIu64 "\n",
                phase_count[static_cast<int>(win::Phase::kWindow)],
                phase_count[static_cast<int>(win::Phase::kSettle)],
                phase_count[static_cast<int>(win::Phase::kTail)],
                phase_count[static_cast<int>(win::Phase::kGap)]);
    if (compare) {
        std::printf("bodies matched %" PRIu64 " / %" PRIu64 "\n", compared, messages);
    } else {
        std::printf("bodies matched  (source file not given)\n");
    }
    std::printf("errors         %" PRIu64 "\n", errors);
    return errors == 0 ? 0 : 1;
}
