#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>

#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "itch/reader.hpp"
#include "itch/types.hpp"

namespace {

struct Order {
    std::uint16_t sym;
    std::uint8_t side;
    std::uint32_t price;
    std::uint32_t shares;
};

struct Sample {
    std::uint64_t ts;
    std::uint16_t sym;
    std::uint32_t bid;
    std::uint32_t ask;
};

void print_clock(std::uint64_t ns) {
    const std::uint64_t s = ns / 1000000000ull;
    std::printf("%02llu:%02llu:%02llu.%03llu",
                static_cast<unsigned long long>(s / 3600),
                static_cast<unsigned long long>((s / 60) % 60),
                static_cast<unsigned long long>(s % 60),
                static_cast<unsigned long long>((ns % 1000000000ull) / 1000000ull));
}

bool read_symbols(const char* path, std::unordered_map<std::string, bool>* out) {
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    char line[256];
    if (std::fgets(line, sizeof(line), f) == nullptr) { std::fclose(f); return false; }
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::size_t n = 0;
        while (line[n] != '\0' && line[n] != ',' && line[n] != '\n' && line[n] != '\r') ++n;
        if (n == 0) continue;
        (*out)[std::string(line, n)] = true;
    }
    std::fclose(f);
    return !out->empty();
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: cross_check ITCH_FILE SYMBOLS_CSV [--stop N]\n");
        return 2;
    }
    std::uint64_t stop_after = 0;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stop") == 0 && i + 1 < argc) {
            stop_after = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }

    std::unordered_map<std::string, bool> wanted;
    if (!read_symbols(argv[2], &wanted)) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
    }

    io::SeqReader reader(argv[1], 32u << 20);
    if (!reader.ok()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    std::vector<std::uint8_t> track(1u << 16, 0);
    std::vector<std::string> name(1u << 16);
    std::vector<std::map<std::uint32_t, std::uint64_t>> levels(1u << 17);
    std::unordered_map<std::uint64_t, Order> orders;
    orders.reserve(1u << 22);

    std::uint64_t messages = 0;
    std::uint64_t checks = 0;
    std::uint64_t locked = 0;
    std::uint64_t crossed = 0;
    std::uint64_t hit = 0, miss = 0, unknown = 0;
    std::uint64_t no_book = 0;
    std::vector<Sample> shown;

    const auto look = [&](std::uint16_t sym, std::uint64_t ts) {
        const auto& buy = levels[std::size_t(sym) * 2 + 0];
        const auto& sell = levels[std::size_t(sym) * 2 + 1];
        if (buy.empty() || sell.empty()) return;
        ++checks;
        const std::uint32_t bid = buy.rbegin()->first;
        const std::uint32_t ask = sell.begin()->first;
        if (bid == ask) { ++locked; return; }
        if (bid > ask) {
            ++crossed;
            if (shown.size() < 20) shown.push_back({ts, sym, bid, ask});
        }
    };

    const auto move = [&](std::uint16_t sym, std::uint8_t side, std::uint32_t price,
                          std::int64_t delta) {
        auto& tbl = levels[std::size_t(sym) * 2 + side];
        auto it = tbl.find(price);
        if (it == tbl.end()) {
            if (delta > 0) tbl.emplace(price, static_cast<std::uint64_t>(delta));
            return;
        }
        const std::int64_t left = static_cast<std::int64_t>(it->second) + delta;
        if (left <= 0) tbl.erase(it); else it->second = static_cast<std::uint64_t>(left);
    };

    bool stopped = false;
    while (!stopped && reader.fill()) {
        const auto r = itch::for_each_message(
            reader.data(), reader.size(), [&](const itch::Message& m) {
                if (stop_after != 0 && ++messages >= stop_after) { stopped = true; return false; }
                const std::uint64_t ts = m.timestamp();
                const std::uint16_t sym = m.stock_locate();
                const char t = m.type();
                if (t == 'R') {
                    const char* s = reinterpret_cast<const char*>(m.body + itch::kStockSymbolOff);
                    std::size_t n = itch::kStockSymbolLen;
                    while (n > 0 && s[n - 1] == ' ') --n;
                    const std::string code(s, n);
                    if (wanted.count(code) != 0) {
                        track[sym] = 1;
                        name[sym] = code;
                    }
                    return true;
                }
                if (track[sym] == 0) return true;
                if (t == 'A' || t == 'F') {
                    const std::uint64_t oid =
                        itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff);
                    const std::uint8_t side = m.body[itch::kAddSideOff] == 'B' ? 0 : 1;
                    const std::uint32_t sh =
                        itch::read_be<std::uint32_t>(m.body + itch::kAddSharesOff);
                    const std::uint32_t px =
                        itch::read_be<std::uint32_t>(m.body + itch::kAddPriceOff);
                    orders[oid] = Order{sym, side, px, sh};
                    move(sym, side, px, static_cast<std::int64_t>(sh));
                    look(sym, ts);
                    return true;
                }
                if (t == 'E' || t == 'C' || t == 'X') {
                    const std::uint64_t oid =
                        itch::read_be<std::uint64_t>(m.body + itch::kExecRefOff);
                    const std::uint32_t sh =
                        itch::read_be<std::uint32_t>(m.body + itch::kExecSharesOff);
                    auto it = orders.find(oid);
                    if (it == orders.end()) return true;
                    const std::uint32_t off = sh < it->second.shares ? sh : it->second.shares;
                    it->second.shares -= off;
                    move(it->second.sym, it->second.side, it->second.price,
                         -static_cast<std::int64_t>(off));
                    if (it->second.shares == 0) orders.erase(it);
                    look(sym, ts);
                    return true;
                }
                if (t == 'D') {
                    const std::uint64_t oid =
                        itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff);
                    auto it = orders.find(oid);
                    if (it == orders.end()) return true;
                    move(it->second.sym, it->second.side, it->second.price,
                         -static_cast<std::int64_t>(it->second.shares));
                    orders.erase(it);
                    look(sym, ts);
                    return true;
                }
                if (t == 'U') {
                    const std::uint64_t old =
                        itch::read_be<std::uint64_t>(m.body + itch::kReplaceOldRefOff);
                    auto it = orders.find(old);
                    if (it == orders.end()) return true;
                    const std::uint8_t side = it->second.side;
                    {
                        const std::uint32_t px =
                            itch::read_be<std::uint32_t>(m.body + itch::kReplacePriceOff);
                        auto& buy = levels[std::size_t(it->second.sym) * 2 + 0];
                        auto& sell = levels[std::size_t(it->second.sym) * 2 + 1];
                        move(it->second.sym, side, it->second.price,
                             -static_cast<std::int64_t>(it->second.shares));
                        if (buy.empty() || sell.empty()) {
                            ++no_book;
                        } else {
                            const std::uint32_t bid = buy.rbegin()->first;
                            const std::uint32_t ask = sell.begin()->first;
                            if (px <= bid) {
                                if (side == 0) ++hit; else { ++miss;
                                    if (shown.size() < 10) shown.push_back({ts, it->second.sym, bid, ask}); }
                            } else if (px >= ask) {
                                if (side == 1) ++hit; else { ++miss;
                                    if (shown.size() < 10) shown.push_back({ts, it->second.sym, bid, ask}); }
                            } else {
                                ++unknown;
                            }
                        }
                        move(it->second.sym, side, it->second.price,
                             static_cast<std::int64_t>(it->second.shares));
                    }
                    move(it->second.sym, side, it->second.price,
                         -static_cast<std::int64_t>(it->second.shares));
                    orders.erase(it);
                    const std::uint64_t fresh =
                        itch::read_be<std::uint64_t>(m.body + itch::kReplaceNewRefOff);
                    const std::uint32_t sh =
                        itch::read_be<std::uint32_t>(m.body + itch::kReplaceSharesOff);
                    const std::uint32_t px =
                        itch::read_be<std::uint32_t>(m.body + itch::kReplacePriceOff);
                    orders[fresh] = Order{sym, side, px, sh};
                    move(sym, side, px, static_cast<std::int64_t>(sh));
                    look(sym, ts);
                    return true;
                }
                return true;
            });
        reader.consume(r.consumed);
        if (r.stop == itch::FrameStop::kZeroLength) {
            std::fprintf(stderr, "zero length record\n");
            return 1;
        }
    }

    {
        const double tot = double(hit + miss + unknown);
        std::printf("replaces that could be decided  %.0f (another %llu had an empty side, nothing to compare)\n",
                    tot, (unsigned long long)no_book);
        std::printf("  right         %llu (%.2f%%)\n", (unsigned long long)hit,
                    tot ? 100.0 * double(hit) / tot : 0.0);
        std::printf("  wrong         %llu (%.2f%%)\n", (unsigned long long)miss,
                    tot ? 100.0 * double(miss) / tot : 0.0);
        std::printf("  undecided     %llu (%.2f%%)   the new price sits between the best bid and the best ask\n",
                    (unsigned long long)unknown, tot ? 100.0 * double(unknown) / tot : 0.0);
    }
    std::printf("messages       %llu\n", static_cast<unsigned long long>(messages));
    std::printf("checks         %llu   times both sides had size and could be compared\n",
                static_cast<unsigned long long>(checks));
    std::printf("locked         %llu (%.4f%%)   bid exactly equal to ask\n",
                static_cast<unsigned long long>(locked),
                checks ? 100.0 * double(locked) / double(checks) : 0.0);
    std::printf("crossed        %llu (%.4f%%)   bid above ask\n",
                static_cast<unsigned long long>(crossed),
                checks ? 100.0 * double(crossed) / double(checks) : 0.0);
    for (const Sample& s : shown) {
        std::printf("  ");
        print_clock(s.ts);
        std::printf("  %-6s bid %u ask %u   over by %u\n", name[s.sym].c_str(), s.bid, s.ask,
                    s.bid - s.ask);
    }
    return 0;
}
