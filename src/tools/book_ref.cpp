#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "book/ref_book.hpp"
#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "itch/reader.hpp"

namespace {

constexpr std::size_t kReadBuffer = 32u << 20;
constexpr std::uint64_t kCheckpointNs = 1800ull * 1000000000ull;
constexpr int kCheckpoints = 13;

struct Checkpoint {
    std::uint64_t ts;
    std::size_t live;
    std::string hash;
    std::uint32_t busiest_live;
    std::uint16_t busiest_locate;
};

bool touches(const itch::Message& m, std::uint64_t ref) {
    switch (m.type()) {
        case 'A':
        case 'F':
            return itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff) == ref;
        case 'E':
        case 'C':
        case 'X':
        case 'D':
            return itch::read_be<std::uint64_t>(m.body + itch::kExecRefOff) == ref;
        case 'U':
            return itch::read_be<std::uint64_t>(m.body + itch::kReplaceOldRefOff) == ref ||
                   itch::read_be<std::uint64_t>(m.body + itch::kReplaceNewRefOff) == ref;
        default:
            return false;
    }
}

void print_trace(const itch::Message& m) {
    const std::uint64_t ts = m.timestamp();
    const std::uint64_t sec = ts / 1000000000ull;
    const std::uint64_t frac = ts - sec * 1000000000ull;
    std::printf("  %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%09" PRIu64 "  %c  locate=%u",
                sec / 3600, (sec / 60) % 60, sec % 60, frac, m.type(),
                m.stock_locate());
    switch (m.type()) {
        case 'A':
        case 'F':
            std::printf("  ref=%" PRIu64 " side=%c shares=%u price=%u",
                        itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff),
                        static_cast<char>(m.body[itch::kAddSideOff]),
                        itch::read_be<std::uint32_t>(m.body + itch::kAddSharesOff),
                        itch::read_be<std::uint32_t>(m.body + itch::kAddPriceOff));
            break;
        case 'E':
        case 'C':
        case 'X':
            std::printf("  ref=%" PRIu64 " shares=%u",
                        itch::read_be<std::uint64_t>(m.body + itch::kExecRefOff),
                        itch::read_be<std::uint32_t>(m.body + itch::kExecSharesOff));
            break;
        case 'D':
            std::printf("  ref=%" PRIu64,
                        itch::read_be<std::uint64_t>(m.body + itch::kDeleteRefOff));
            break;
        case 'U':
            std::printf("  old=%" PRIu64 " new=%" PRIu64 " shares=%u price=%u",
                        itch::read_be<std::uint64_t>(m.body + itch::kReplaceOldRefOff),
                        itch::read_be<std::uint64_t>(m.body + itch::kReplaceNewRefOff),
                        itch::read_be<std::uint32_t>(m.body + itch::kReplaceSharesOff),
                        itch::read_be<std::uint32_t>(m.body + itch::kReplacePriceOff));
            break;
        default:
            break;
    }
    std::printf("\n");
}

void write_outputs(const std::filesystem::path& dir, const std::string& input,
                   const std::vector<Checkpoint>& cps, const book::RefBook& bk,
                   std::uint64_t applied, double seconds,
                   const std::vector<std::uint32_t>& dist,
                   const std::vector<std::string>& symbols) {
    std::filesystem::create_directories(dir);

    if (FILE* f = std::fopen((dir / "live_per_symbol.csv").c_str(), "w")) {
        std::vector<std::uint16_t> order;
        for (std::size_t i = 0; i < dist.size(); ++i) {
            if (dist[i] != 0) order.push_back(static_cast<std::uint16_t>(i));
        }
        std::sort(order.begin(), order.end(),
                  [&](std::uint16_t a, std::uint16_t b) { return dist[a] > dist[b]; });
        std::fprintf(f, "rank,stock_locate,symbol,live_orders\n");
        for (std::size_t i = 0; i < order.size(); ++i) {
            std::fprintf(f, "%zu,%u,%s,%u\n", i, order[i],
                         symbols[order[i]].c_str(), dist[order[i]]);
        }
        std::fclose(f);
    }

    if (FILE* f = std::fopen((dir / "golden_hashes.csv").c_str(), "w")) {
        std::fprintf(f, "checkpoint,timestamp_ns,time_et,live_orders,sha256\n");
        for (std::size_t i = 0; i < cps.size(); ++i) {
            const std::uint64_t sec = cps[i].ts / 1000000000ull;
            std::fprintf(f,
                         "%zu,%" PRIu64 ",%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64
                         ",%zu,%s\n",
                         i, cps[i].ts, sec / 3600, (sec / 60) % 60, sec % 60,
                         cps[i].live, cps[i].hash.c_str());
        }
        std::fclose(f);
    }

    FILE* f = std::fopen((dir / "summary.json").c_str(), "w");
    if (f == nullptr) return;
    const auto& c = bk.counters();
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"input\": \"%s\",\n", input.c_str());
    std::fprintf(f, "  \"messages_applied\": %" PRIu64 ",\n", applied);
    std::fprintf(f, "  \"checkpoints\": %zu,\n", cps.size());
    std::fprintf(f, "  \"live_orders_at_end\": %zu,\n", bk.live());
    std::fprintf(f, "  \"peak_live_orders\": %zu,\n", bk.peak_live());
    std::fprintf(f, "  \"securities_with_live_orders\": %zu,\n",
                 static_cast<std::size_t>(std::count_if(
                     dist.begin(), dist.end(), [](std::uint32_t n) { return n != 0; })));
    std::fprintf(f, "  \"added\": %" PRIu64 ",\n", c.added);
    std::fprintf(f, "  \"executed\": %" PRIu64 ",\n", c.executed);
    std::fprintf(f, "  \"cancelled\": %" PRIu64 ",\n", c.cancelled);
    std::fprintf(f, "  \"deleted\": %" PRIu64 ",\n", c.deleted);
    std::fprintf(f, "  \"replaced\": %" PRIu64 ",\n", c.replaced);
    std::fprintf(f, "  \"orphan_ref\": %" PRIu64 ",\n", c.orphan_ref);
    std::fprintf(f, "  \"oversized_exec\": %" PRIu64 ",\n", c.oversized_exec);
    std::fprintf(f, "  \"duplicate_ref\": %" PRIu64 ",\n", c.duplicate_ref);
    std::fprintf(f, "  \"build_seconds\": %.2f\n", seconds);
    std::fprintf(f, "}\n");
    std::fclose(f);
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: book_ref <file> [--max-messages N] [--out DIR] "
                     "[--trace-order REF]\n");
        return 2;
    }
    const std::string input = argv[1];
    std::uint64_t max_messages = 0;
    std::uint64_t trace_ref = 0;
    bool tracing = false;
    std::filesystem::path out;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--max-messages") == 0 && i + 1 < argc) {
            max_messages = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (std::strcmp(argv[i], "--trace-order") == 0 && i + 1 < argc) {
            trace_ref = std::strtoull(argv[++i], nullptr, 10);
            tracing = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (out.empty()) {
        out = std::filesystem::path("results") /
              ("book_" + std::filesystem::path(input).stem().string());
    }

    io::SeqReader reader(input.c_str(), kReadBuffer);
    if (!reader.ok()) {
        std::fprintf(stderr, "cannot open %s\n", input.c_str());
        return 1;
    }

    book::RefBook bk;
    std::vector<Checkpoint> cps;
    std::vector<std::string> symbols(1u << 16);
    std::vector<std::uint32_t> fullest_dist(1u << 16, 0);
    std::size_t fullest_live = 0;
    std::uint64_t applied = 0;
    std::uint64_t next_cp = 0;
    bool armed = false;
    bool stopped = false;

    if (tracing) std::printf("order %" PRIu64 " lifecycle:\n", trace_ref);
    const auto started = std::chrono::steady_clock::now();
    while (!stopped && reader.fill()) {
        const auto r = itch::for_each_message(
            reader.data(), reader.size(), [&](const itch::Message& m) {
                const std::uint64_t ts = m.timestamp();
                while (armed && cps.size() < kCheckpoints && ts >= next_cp) {
                    const auto dist = bk.live_by_locate();
                    std::uint16_t top = 0;
                    for (std::size_t i = 0; i < dist.size(); ++i) {
                        if (dist[i] > dist[top]) top = static_cast<std::uint16_t>(i);
                    }
                    cps.push_back({next_cp, bk.live(), bk.hash(), dist[top], top});
                    if (bk.live() >= fullest_live) {
                        fullest_live = bk.live();
                        fullest_dist = dist;
                    }
                    next_cp += kCheckpointNs;
                }
                if (m.type() == 'R') {
                    std::string& s = symbols[m.stock_locate()];
                    s.assign(reinterpret_cast<const char*>(m.body + itch::kStockSymbolOff),
                             itch::kStockSymbolLen);
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                }
                if (tracing && touches(m, trace_ref)) print_trace(m);
                bk.apply(m);
                if (m.type() == 'S' &&
                    m.event_code() == itch::kEventStartOfMarketHours) {
                    armed = true;
                    next_cp = ts;
                }
                ++applied;
                if ((applied & 0x3ffffff) == 0) {
                    std::fprintf(stderr, "\r%" PRIu64 "M messages, %zu live",
                                 applied / 1000000, bk.live());
                }
                return max_messages == 0 || applied < max_messages;
            });
        reader.consume(r.consumed);
        if (r.stop == itch::FrameStop::kCallerStopped ||
            r.stop == itch::FrameStop::kZeroLength) {
            stopped = true;
        } else if (r.consumed == 0) {
            break;
        }
    }
    std::fprintf(stderr, "\r                                        \r");

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
            .count();
    const auto& c = bk.counters();
    std::printf("messages applied  %" PRIu64 "\n", applied);
    std::printf("checkpoints       %zu / %d\n", cps.size(), kCheckpoints);
    std::printf("peak live orders  %zu\n", bk.peak_live());
    std::printf("live at end       %zu\n", bk.live());
    std::printf("add/exec/cxl/del/rpl  %" PRIu64 " / %" PRIu64 " / %" PRIu64
                " / %" PRIu64 " / %" PRIu64 "\n",
                c.added, c.executed, c.cancelled, c.deleted, c.replaced);
    std::printf("orphan ref        %" PRIu64 "\n", c.orphan_ref);
    std::printf("oversized exec    %" PRIu64 "\n", c.oversized_exec);
    std::printf("duplicate ref     %" PRIu64 "\n", c.duplicate_ref);
    std::printf("build             %.2f s\n", seconds);
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const std::uint64_t sec = cps[i].ts / 1000000000ull;
        std::printf("  cp%-2zu %02" PRIu64 ":%02" PRIu64
                    "  live=%-8zu busiest=%-7u (%s)  %s\n",
                    i, sec / 3600, (sec / 60) % 60, cps[i].live, cps[i].busiest_live,
                    symbols[cps[i].busiest_locate].c_str(), cps[i].hash.c_str());
    }

    write_outputs(out, input, cps, bk, applied, seconds, fullest_dist, symbols);
    std::printf("wrote             %s\n", out.c_str());

    return c.orphan_ref == 0 ? 0 : 1;
}
