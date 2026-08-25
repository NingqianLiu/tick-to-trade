
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "book/imbalance.hpp"
#include "book/order_book.hpp"
#include "book/ref_book.hpp"
#include "common/window.hpp"
#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "itch/reader.hpp"
#include "itch/types.hpp"
#include "net/pack.hpp"

namespace {

constexpr std::size_t kOrderCapacity = 8u << 20;

bool read_reference(const char* path, std::unordered_map<std::string, std::uint32_t>* out) {
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    char line[128];
    if (std::fgets(line, sizeof(line), f) == nullptr) {
        std::fclose(f);
        return false;
    }
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char* comma = std::strchr(line, ',');
        if (comma == nullptr) continue;
        *comma = '\0';
        (*out)[line] = static_cast<std::uint32_t>(std::strtoul(comma + 1, nullptr, 10));
    }
    std::fclose(f);
    return true;
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: book_check ITCH_FILE REFERENCE_CSV [--threshold N] [--stop N]\n");
        return 2;
    }
    std::uint32_t threshold = 88;
    std::uint64_t stop_after = 0;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--stop") == 0 && i + 1 < argc) {
            stop_after = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }

    std::unordered_map<std::string, std::uint32_t> reference;
    if (!read_reference(argv[2], &reference)) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
    }
    std::vector<std::uint32_t> prices;
    prices.reserve(reference.size());
    for (const auto& [sym, price] : reference) prices.push_back(price);

    io::SeqReader reader(argv[1], 32u << 20);
    if (!reader.ok()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    book::OrderBook fast(kOrderCapacity, prices);
    book::RefBook slow;
    const book::Imbalance signal(threshold);
    const win::Params wp = win::params_from_env();
    win::Tracker tracker(wp);
    pkt::Packing packing;

    std::uint64_t messages = 0, bound = 0, unknown = 0;
    std::uint64_t window_messages = 0, window_packets = 0;
    std::uint64_t buys = 0, sells = 0, packets_with_a_signal = 0;
    std::uint64_t in_this_packet = 0;
    std::uint64_t mismatch = 0;
    win::Phase packet_phase = win::Phase::kGap;
    bool stopped = false;

    const auto close_packet = [&] {
        if (packet_phase == win::Phase::kWindow) {
            ++window_packets;
            if (in_this_packet != 0) ++packets_with_a_signal;
        }
        in_this_packet = 0;
        packing.close();
    };

    while (!stopped && reader.fill()) {
        const auto r = itch::for_each_message(
            reader.data(), reader.size(), [&](const itch::Message& m) {
                const std::uint64_t ts = m.timestamp();
                win::note_session(m, &tracker);
                const win::Phase p = tracker.advance(ts);
                const std::size_t rec = m.len + itch::kLenPrefix;
                if (packing.should_close(ts, rec, p)) close_packet();
                packing.add(ts, rec, p);
                packet_phase = packing.phase();
                ++messages;
                if (p == win::Phase::kWindow) ++window_messages;

                if (m.type() == 'R') {
                    const char* s =
                        reinterpret_cast<const char*>(m.body + itch::kStockSymbolOff);
                    std::size_t n = itch::kStockSymbolLen;
                    while (n > 0 && s[n - 1] == ' ') --n;
                    const auto it = reference.find(std::string(s, n));
                    if (it == reference.end()) ++unknown;
                    else if (fast.bind(m.stock_locate(), it->second)) ++bound;
                }

                std::uint16_t sym = 0;
                const bool moved = fast.apply(m, &sym);
                slow.apply(m);

                if (moved && p == win::Phase::kWindow) {
                    const auto what = signal.check(fast.top3(sym, book::PriceLevels::kBuy),
                                                   fast.top3(sym, book::PriceLevels::kSell));
                    if (what == book::Imbalance::Signal::kBuy) { ++buys; ++in_this_packet; }
                    if (what == book::Imbalance::Signal::kSell) { ++sells; ++in_this_packet; }
                }
                return stop_after == 0 || messages < stop_after;
            });
        reader.consume(r.consumed);
        if (r.stop == itch::FrameStop::kCallerStopped ||
            r.stop == itch::FrameStop::kZeroLength) stopped = true;
        else if (r.consumed == 0) break;
    }
    close_packet();

    const book::OrderBook::Counters& f = fast.counters();
    const book::RefBook::Counters& s = slow.counters();
    const auto same = [&](const char* what, std::uint64_t a, std::uint64_t b) {
        const bool ok = a == b;
        if (!ok) ++mismatch;
        std::printf("  %-22s %14llu %14llu  %s\n", what, (unsigned long long)a,
                    (unsigned long long)b, ok ? "same" : "DIFFERENT");
    };

    std::printf("messages %llu\n", (unsigned long long)messages);
    std::printf("securities given prices %llu, named but not in the reference file %llu\n",
                (unsigned long long)bound, (unsigned long long)unknown);
    std::printf("\n  %-22s %14s %14s\n", "", "this book", "reference");
    same("orders alive", fast.live(), slow.live());
    same("added", f.added, s.added);
    same("executed", f.executed, s.executed);
    same("cancelled", f.cancelled, s.cancelled);
    same("deleted", f.deleted, s.deleted);
    same("replaced", f.replaced, s.replaced);
    same("orphan", f.orphan, s.orphan_ref);
    same("oversized", f.oversized, s.oversized_exec);
    same("duplicate", f.duplicate, s.duplicate_ref);
    std::printf("  %-22s %14llu\n", "priced out of band",
                (unsigned long long)f.untracked);

    std::printf("\nimbalance at %u%% over the sampled windows\n", threshold);
    std::printf("  window messages   %llu\n", (unsigned long long)window_messages);
    std::printf("  window packets    %llu\n", (unsigned long long)window_packets);
    std::printf("  orders sent       %llu  (buy %llu, sell %llu) = %.4f%% of messages\n",
                (unsigned long long)(buys + sells), (unsigned long long)buys,
                (unsigned long long)sells,
                window_messages ? 100.0 * (buys + sells) / window_messages : 0.0);
    std::printf("  packets that sent %llu = %.4f%% of packets\n",
                (unsigned long long)packets_with_a_signal,
                window_packets ? 100.0 * packets_with_a_signal / window_packets : 0.0);

    std::printf("\n%s\n", mismatch == 0 ? "the two books agree"
                                        : "THE TWO BOOKS DISAGREE");
    return mismatch == 0 ? 0 : 1;
}
