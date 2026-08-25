#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "itch/reader.hpp"
#include "itch/types.hpp"

namespace {

constexpr std::size_t kTradePriceOff = 32;
constexpr std::size_t kCrossPriceOff = 27;
constexpr std::size_t kCrossTypeOff = 39;
constexpr char kOpeningCross = 'O';
constexpr std::uint32_t kDollar = 10000;
constexpr std::size_t kSecurities = 1u << 16;

struct Found {
    std::uint32_t cross = 0;
    std::uint32_t first = 0;
    std::uint32_t middle = 0;
};

std::uint32_t on_grid(std::uint32_t price) {
    return price >= kDollar ? price / 100 * 100 : price;
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: make_prev_close ITCH_FILE OUT_CSV\n");
        return 2;
    }
    io::SeqReader reader(argv[1], 32u << 20);
    if (!reader.ok()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    std::vector<std::string> name(kSecurities);
    std::vector<Found> found(kSecurities);
    std::unordered_map<std::uint64_t, std::uint32_t> resting;
    resting.reserve(1u << 22);
    std::vector<std::map<std::uint32_t, std::uint32_t>> side[2];
    side[0].resize(kSecurities);
    side[1].resize(kSecurities);

    bool opened = false;
    bool stopped = false;
    std::uint64_t crosses = 0;

    const auto trade = [&](std::uint16_t id, std::uint32_t price) {
        if (price != 0 && found[id].first == 0) found[id].first = price;
    };

    while (!stopped && reader.fill()) {
        const auto r = itch::for_each_message(
            reader.data(), reader.size(), [&](const itch::Message& m) {
                const std::uint16_t id = m.stock_locate();
                switch (m.type()) {
                    case 'S': {
                        if (m.body[itch::kHeaderLen] != itch::kEventStartOfMarketHours) break;
                        opened = true;
                        for (std::size_t s = 0; s < kSecurities; ++s) {
                            const auto& bids = side[0][s];
                            const auto& asks = side[1][s];
                            if (bids.empty() || asks.empty()) continue;
                            const std::uint64_t bid = bids.rbegin()->first;
                            const std::uint64_t ask = asks.begin()->first;
                            found[s].middle =
                                on_grid(static_cast<std::uint32_t>((bid + ask) / 2));
                        }
                        side[0].clear();
                        side[0].shrink_to_fit();
                        side[1].clear();
                        side[1].shrink_to_fit();
                        break;
                    }
                    case 'R': {
                        const char* s =
                            reinterpret_cast<const char*>(m.body + itch::kStockSymbolOff);
                        std::size_t n = itch::kStockSymbolLen;
                        while (n > 0 && s[n - 1] == ' ') --n;
                        name[id].assign(s, n);
                        break;
                    }
                    case 'A':
                    case 'F': {
                        const std::uint32_t price =
                            itch::read_be<std::uint32_t>(m.body + itch::kAddPriceOff);
                        resting[itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff)] = price;
                        if (!opened && price != 0) {
                            ++side[m.body[itch::kAddSideOff] == 'B' ? 0 : 1][id][price];
                        }
                        break;
                    }
                    case 'D':
                    case 'X':
                    case 'E':
                    case 'C': {
                        const std::size_t off = m.type() == 'D'   ? itch::kDeleteRefOff
                                                : m.type() == 'X' ? itch::kCancelRefOff
                                                                  : itch::kExecRefOff;
                        const auto it =
                            resting.find(itch::read_be<std::uint64_t>(m.body + off));
                        if (it == resting.end()) break;
                        if (m.type() == 'E') trade(id, it->second);
                        if (m.type() == 'C') {
                            trade(id, itch::read_be<std::uint32_t>(m.body + kTradePriceOff));
                        }
                        if (!opened && m.type() == 'D') {
                            for (int s = 0; s < 2; ++s) {
                                auto& lv = side[s][id];
                                const auto at = lv.find(it->second);
                                if (at == lv.end()) continue;
                                if (--at->second == 0) lv.erase(at);
                            }
                        }
                        if (m.type() == 'D') resting.erase(it);
                        break;
                    }
                    case 'U': {
                        const auto it = resting.find(
                            itch::read_be<std::uint64_t>(m.body + itch::kReplaceOldRefOff));
                        const std::uint32_t fresh =
                            itch::read_be<std::uint32_t>(m.body + itch::kReplacePriceOff);
                        if (it != resting.end()) resting.erase(it);
                        resting[itch::read_be<std::uint64_t>(
                            m.body + itch::kReplaceNewRefOff)] = fresh;
                        break;
                    }
                    case 'P':
                        trade(id, itch::read_be<std::uint32_t>(m.body + kTradePriceOff));
                        break;
                    case 'Q': {
                        const std::uint32_t price =
                            itch::read_be<std::uint32_t>(m.body + kCrossPriceOff);
                        trade(id, price);
                        if (m.body[kCrossTypeOff] == kOpeningCross && price != 0 &&
                            found[id].cross == 0) {
                            found[id].cross = price;
                            ++crosses;
                        }
                        break;
                    }
                    default:
                        break;
                }
                return true;
            });
        reader.consume(r.consumed);
        if (r.stop == itch::FrameStop::kZeroLength) stopped = true;
        else if (r.consumed == 0) break;
    }

    std::FILE* out = std::fopen(argv[2], "w");
    if (out == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    std::fprintf(out, "symbol,price,source\n");
    std::uint64_t from_cross = 0, from_trade = 0, from_middle = 0, without = 0;
    for (std::size_t s = 0; s < kSecurities; ++s) {
        if (name[s].empty()) continue;
        const Found& f = found[s];
        const char* how = nullptr;
        std::uint32_t price = 0;
        if (f.cross != 0) { price = f.cross; how = "cross"; ++from_cross; }
        else if (f.first != 0) { price = f.first; how = "trade"; ++from_trade; }
        else if (f.middle != 0) { price = f.middle; how = "middle"; ++from_middle; }
        else { ++without; continue; }
        std::fprintf(out, "%s,%u,%s\n", name[s].c_str(), price, how);
    }
    std::fclose(out);

    std::fprintf(stderr,
                 "wrote %s\n  from the opening cross %llu\n  from the first trade %llu\n"
                 "  from the bid and ask at the open %llu\n  left out, never priced %llu\n"
                 "  opening crosses seen %llu\n",
                 argv[2], (unsigned long long)from_cross, (unsigned long long)from_trade,
                 (unsigned long long)from_middle, (unsigned long long)without,
                 (unsigned long long)crosses);
    return 0;
}
