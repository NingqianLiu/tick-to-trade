#include <cstdio>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "itch/types.hpp"

namespace {

void print_time(std::uint64_t ns) {
    const std::uint64_t s = ns / 1000000000ull;
    std::printf("%02llu:%02llu:%02llu.%09llu",
                static_cast<unsigned long long>(s / 3600),
                static_cast<unsigned long long>(s % 3600 / 60),
                static_cast<unsigned long long>(s % 60),
                static_cast<unsigned long long>(ns % 1000000000ull));
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "live_peak <file> [--symbols list.csv]\n");
        return 2;
    }
    const char* input = argv[1];
    const char* symbols = nullptr;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--symbols") == 0 && i + 1 < argc) symbols = argv[++i];
    }

    std::unordered_set<std::string> wanted;
    if (symbols != nullptr) {
        std::FILE* f = std::fopen(symbols, "r");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot open %s\n", symbols);
            return 1;
        }
        char line[256];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            char* p = line;
            while (*p != '\0' && *p != ',' && *p != '\n' && *p != '\r' && *p != ' ') ++p;
            *p = '\0';
            if (line[0] != '\0') wanted.insert(line);
        }
        std::fclose(f);
        std::printf("%zu names in the list\n", wanted.size());
    }

    std::vector<std::uint8_t> mine(65536, symbols == nullptr ? 1 : 0);
    std::unordered_map<std::uint64_t, std::uint16_t> owner;
    owner.reserve(20000000);

    std::vector<std::uint64_t> low6(64, 0);
    std::vector<std::uint64_t> last_oid(65536, 0);
    std::uint64_t gap_sum = 0, gap_n = 0, gap_max = 0;

    std::uint64_t live = 0, peak = 0, peak_at = 0, messages = 0, last_ts = 0;
    std::uint64_t live_all = 0, peak_all = 0, peak_all_at = 0;

    io::SeqReader reader(input, 1u << 22);
    if (!reader.ok()) {
        std::fprintf(stderr, "cannot open %s\n", input);
        return 1;
    }

    while (reader.fill()) {
        const auto r = itch::for_each_message(
            reader.data(), reader.size(), [&](const itch::Message& m) {
                ++messages;
                last_ts = m.timestamp();
                const char t = m.type();
                if (t == 'R') {
                    if (symbols == nullptr) return true;
                    const std::uint16_t sym = itch::read_be<std::uint16_t>(
                        m.body + itch::kLocateOff);
                    const char* s = reinterpret_cast<const char*>(
                        m.body + itch::kStockSymbolOff);
                    std::size_t n = itch::kStockSymbolLen;
                    while (n > 0 && s[n - 1] == ' ') --n;
                    mine[sym] = wanted.count(std::string(s, n)) != 0 ? 1 : 0;
                    return true;
                }
                if (t == 'A' || t == 'F') {
                    const std::uint16_t sym = itch::read_be<std::uint16_t>(
                        m.body + itch::kLocateOff);
                    const std::uint64_t oid = itch::read_be<std::uint64_t>(
                        m.body + itch::kAddRefOff);
                    owner[oid] = sym;
                    ++live_all;
                    if (live_all > peak_all) { peak_all = live_all; peak_all_at = last_ts; }
                    if (mine[sym] != 0) {
                        ++live;
                        if (live > peak) { peak = live; peak_at = last_ts; }
                        ++low6[oid & 63];
                        if (last_oid[sym] != 0 && oid > last_oid[sym]) {
                            const std::uint64_t g = oid - last_oid[sym];
                            gap_sum += g;
                            ++gap_n;
                            if (g > gap_max) gap_max = g;
                        }
                        last_oid[sym] = oid;
                    }
                    return true;
                }
                if (t == 'D' || t == 'U') {
                    const std::uint64_t oid = itch::read_be<std::uint64_t>(
                        m.body + itch::kDeleteRefOff);
                    const auto it = owner.find(oid);
                    if (it != owner.end()) {
                        if (live_all > 0) --live_all;
                        if (mine[it->second] != 0 && live > 0) --live;
                        if (t == 'U') {
                            const std::uint64_t nid = itch::read_be<std::uint64_t>(
                                m.body + itch::kReplaceNewRefOff);
                            const std::uint16_t sym = it->second;
                            owner.erase(it);
                            owner[nid] = sym;
                            ++live_all;
                            if (live_all > peak_all) { peak_all = live_all; peak_all_at = last_ts; }
                            if (mine[sym] != 0) {
                                ++live;
                                if (live > peak) { peak = live; peak_at = last_ts; }
                            }
                        } else {
                            owner.erase(it);
                        }
                    }
                    return true;
                }
                return true;
            });
        reader.consume(r.consumed);
        if (r.consumed == 0) break;
    }

    std::printf("%s\n", input);
    std::printf("  messages %" PRIu64 "\n", messages);
    if (symbols != nullptr) {
        std::printf("  peak live orders for the names in the list: %" PRIu64 ", at ", peak);
        print_time(peak_at);
        std::printf("\n");
    }
    std::printf("  peak live orders for the whole market: %" PRIu64 ", at ", peak_all);
    print_time(peak_all_at);
    std::printf("\n");
    if (symbols != nullptr && peak_all > 0) {
        std::printf("  the list is %.1f%% of the market\n", 100.0 * peak / peak_all);
    }
    if (gap_n > 0) {
        std::uint64_t lo = ~std::uint64_t{0}, hi = 0, tot = 0;
        for (std::uint64_t c : low6) { if (c < lo) lo = c; if (c > hi) hi = c; tot += c; }
        std::printf("  low six bits of the order id, over sixty four buckets: least %" PRIu64 " most %" PRIu64
                    ", average %" PRIu64 "\n", lo, hi, tot / 64);
        std::printf("  most / least = %.2f, close to 1.00 if the ids run in order\n",
                    lo > 0 ? static_cast<double>(hi) / static_cast<double>(lo) : 0.0);
        std::printf("  id gap between two adjacent adds on the same security: average %" PRIu64
                    ", largest %" PRIu64 "\n", gap_sum / gap_n, gap_max);
    }
    return 0;
}
