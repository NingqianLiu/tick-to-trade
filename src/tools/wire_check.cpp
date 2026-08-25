#include <etherfabric/ef_vi.h>
#include <etherfabric/vi.h>
#include <netinet/in.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/settings.hpp"
#include "common/tsc.hpp"
#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "net/ef.hpp"
#include "net/eth.hpp"
#include "net/mold.hpp"
#include "net/nic_stats.hpp"

namespace {

constexpr std::size_t kReadBuffer = 32u << 20;
constexpr std::size_t kSlotBytes = cfg::kRxSlotBytes;
constexpr std::size_t kSlots = cfg::kRxDescriptors;

struct Options {
    const char* intf = "enp129s0f1";
    const char* itch = nullptr;
    std::uint32_t dst_ip = eth::ipv4(239, 9, 9, 1);
    std::uint16_t dst_port = 26477;
    std::uint64_t expect = 0;
    std::uint64_t idle_ms = 2000;
};

bool parse_ip(const char* s, std::uint32_t* out) {
    unsigned a, b, c, d;
    if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    *out = eth::ipv4(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
                     static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(d));
    return true;
}

std::uint64_t errors = 0;
void fail(const char* what, std::uint64_t where) {
    if (++errors <= 10) std::fprintf(stderr, "packet %" PRIu64 ": %s\n", where, what);
}

}

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const bool has = i + 1 < argc;
        const char* a = argv[i];
        if (std::strcmp(a, "--intf") == 0 && has) {
            opt.intf = argv[++i];
        } else if (std::strcmp(a, "--itch") == 0 && has) {
            opt.itch = argv[++i];
        } else if (std::strcmp(a, "--dst-ip") == 0 && has) {
            if (!parse_ip(argv[++i], &opt.dst_ip)) return 2;
        } else if (std::strcmp(a, "--dst-port") == 0 && has) {
            opt.dst_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--expect") == 0 && has) {
            opt.expect = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(a, "--idle-ms") == 0 && has) {
            opt.idle_ms = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr,
                         "usage: wire_check [--intf I] [--dst-ip A.B.C.D] "
                         "[--dst-port N] [--itch FILE] [--expect N] [--idle-ms N]\n");
            return 2;
        }
    }

    ef::Vi vi;
    if (!vi.open(opt.intf, static_cast<int>(kSlots), 0, EF_VI_RX_TIMESTAMPS)) return 1;
    ef_vi_receive_set_buffer_len(vi.get(), kSlotBytes);
    ef::Frames frames;
    if (!frames.alloc(vi, kSlots, kSlotBytes)) return 1;

    ef_filter_spec fs;
    ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
    int rc = ef_filter_spec_set_ip4_local(&fs, IPPROTO_UDP, htonl(opt.dst_ip),
                                          static_cast<int>(htons(opt.dst_port)));
    if (rc < 0) {
        std::fprintf(stderr, "ef_filter_spec_set_ip4_local: %d\n", rc);
        return 1;
    }
    ef_filter_cookie cookie;
    rc = ef_vi_filter_add(vi.get(), vi.dh(), &fs, &cookie);
    if (rc < 0) {
        std::fprintf(stderr, "ef_vi_filter_add: %d\n", rc);
        return 1;
    }

    const std::size_t prefix = static_cast<std::size_t>(ef_vi_receive_prefix_len(vi.get()));
    for (std::size_t i = 0; i < kSlots - 1; ++i) {
        rc = ef_vi_receive_init(vi.get(), frames.dma(i), i);
        if (rc < 0) {
            std::fprintf(stderr, "ef_vi_receive_init: %d\n", rc);
            return 1;
        }
    }
    ef_vi_receive_push(vi.get());

    io::SeqReader src(opt.itch != nullptr ? opt.itch : "", kReadBuffer);
    const bool compare = opt.itch != nullptr && src.ok();
    if (opt.itch != nullptr && !compare) {
        std::fprintf(stderr, "cannot open %s\n", opt.itch);
        return 1;
    }
    std::size_t src_off = 0;
    if (compare) src.fill();

    nic::Drops before;
    const bool have_counters = nic::read_drops(opt.intf, &before);

    std::vector<std::uint64_t> type_count(256, 0);
    std::uint64_t packets = 0, messages = 0, compared = 0, bytes = 0;
    std::uint64_t first_seq = 0, expect_seq = 0, holes = 0, hole_messages = 0;
    std::uint64_t largest = 0;
    const double tps = tsc::ticks_per_ns();
    const std::uint64_t idle_ticks = static_cast<std::uint64_t>(opt.idle_ms * 1e6 * tps);
    std::uint64_t last_seen = tsc::now(), start = 0;

    ef_event evs[64];
    for (;;) {
        const int n = ef_eventq_poll(vi.get(), evs, 64);
        if (n == 0) {
            if (packets != 0 && tsc::now() - last_seen > idle_ticks) break;
            if (packets == 0 && tsc::now() - last_seen > idle_ticks * 15) {
                std::fprintf(stderr, "nothing arrived\n");
                return 1;
            }
            continue;
        }
        last_seen = tsc::now();
        for (int i = 0; i < n; ++i) {
            if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) {
                if (EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_RX_NO_DESC_TRUNC ||
                    EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_RX_DISCARD) {
                    fail("the card had nowhere to put a packet", packets);
                }
                continue;
            }
            const std::size_t slot = EF_EVENT_RX_RQ_ID(evs[i]);
            const std::size_t len = EF_EVENT_RX_BYTES(evs[i]) - prefix;
            const std::uint8_t* frame = frames.at(slot) + prefix;
            if (packets == 0) start = tsc::now();
            ++packets;
            bytes += len + 24;
            largest = std::max<std::uint64_t>(largest, len);

            if (len < eth::kHeaderBytes + mold::kHeaderLen) {
                fail("frame is shorter than its own headers", packets);
            } else {
                const std::uint8_t* p = frame + eth::kHeaderBytes;
                const std::uint64_t seq = mold::sequence(p);
                const std::uint16_t count = mold::count(p);
                if (packets == 1) {
                    first_seq = seq;
                } else if (seq != expect_seq) {
                    ++holes;
                    if (seq > expect_seq) hole_messages += seq - expect_seq;
                    fail("sequence has a hole", packets);
                }
                expect_seq = seq + count;

                const std::size_t payload = len - eth::kHeaderBytes - mold::kHeaderLen;
                std::uint64_t seen = 0;
                const auto r = itch::for_each_message(
                    p + mold::kHeaderLen, payload, [&](const itch::Message& m) {
                        ++seen;
                        ++type_count[static_cast<unsigned char>(m.type())];
                        if (m.len != itch::kBodyLen[static_cast<unsigned char>(m.type())]) {
                            fail("message length does not match its type", packets);
                        }
                        if (compare) {
                            const std::size_t need = m.len + itch::kLenPrefix;
                            if (src_off + need > src.size()) {
                                src.consume(src_off);
                                src_off = 0;
                                src.fill();
                            }
                            if (src_off + need > src.size()) {
                                fail("source file ended early", packets);
                            } else if (std::memcmp(src.data() + src_off,
                                                   m.body - itch::kLenPrefix, need) != 0) {
                                fail("message body differs from the source file", packets);
                            } else {
                                ++compared;
                            }
                            src_off += need;
                        }
                        return true;
                    });
                if (r.consumed != payload) fail("payload has leftover bytes", packets);
                if (seen != count) fail("payload holds a different number of messages", packets);
                messages += seen;
            }
            rc = ef_vi_receive_init(vi.get(), frames.dma(slot), slot);
            if (rc < 0) fail("cannot repost the buffer", packets);
        }
        ef_vi_receive_push(vi.get());
    }
    const double wall = (last_seen - start) / tps / 1e9;

    nic::Drops after;
    const bool clean_counters =
        have_counters && nic::read_drops(opt.intf, &after) && before == after;

    std::printf("interface      %s, %u.%u.%u.%u:%u\n", opt.intf, opt.dst_ip >> 24,
                (opt.dst_ip >> 16) & 0xff, (opt.dst_ip >> 8) & 0xff,
                opt.dst_ip & 0xff, opt.dst_port);
    std::printf("packets        %" PRIu64 "\n", packets);
    std::printf("messages       %" PRIu64 " (%.2f per packet)\n", messages,
                packets ? static_cast<double>(messages) / packets : 0.0);
    std::printf("sequence       %" PRIu64 " .. %" PRIu64 " (%" PRIu64
                " holes, %" PRIu64 " messages missing)\n",
                first_seq, expect_seq - 1, holes, hole_messages);
    std::printf("largest frame  %" PRIu64 " B\n", largest);
    std::printf("rate           %.3f s, %.2f M messages/s, %.2f Gb/s\n", wall,
                wall > 0 ? messages / wall / 1e6 : 0.0,
                wall > 0 ? bytes * 8.0 / wall / 1e9 : 0.0);
    if (compare) {
        std::printf("bodies matched %" PRIu64 " / %" PRIu64 "\n", compared, messages);
    }
    if (opt.expect != 0 && messages != opt.expect) {
        std::fprintf(stderr, "expected %" PRIu64 " messages, saw %" PRIu64 "\n",
                     opt.expect, messages);
        ++errors;
    }
    std::printf("card counters  %s\n",
                !have_counters ? "could not be read"
                               : (clean_counters ? "unchanged" : "MOVED"));
    if (have_counters && !clean_counters) (void)nic::report_drops(before, after);
    std::printf("types          ");
    for (int t = 0; t < 256; ++t) {
        if (type_count[t] != 0) std::printf("%c=%" PRIu64 " ", t, type_count[t]);
    }
    std::printf("\nerrors         %" PRIu64 "\n", errors);
    return errors == 0 && (clean_counters || !have_counters) ? 0 : 1;
}
