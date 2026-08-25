#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <random>

#include "common/tsc.hpp"
#include "book/order_book.hpp"
#include "book/imbalance.hpp"
#include "itch/framing.hpp"
#include "itch/types.hpp"

namespace {

constexpr std::size_t kSlot = 2048;

std::size_t read_messages(const char* path, std::size_t want_bytes,
                          std::vector<std::uint8_t>* out) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return 0;
    out->resize(want_bytes);
    const std::size_t got = std::fread(out->data(), 1, want_bytes, f);
    std::fclose(f);
    out->resize(got);
    return got;
}

}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "data/S121225-v50.txt";
    const std::size_t pkts_per_batch =
        argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 6;
    const std::size_t msgs_per_batch =
        argc > 3 ? static_cast<std::size_t>(std::atoi(argv[3])) : 33;
    const std::size_t batches =
        argc > 4 ? static_cast<std::size_t>(std::atoll(argv[4])) : 200000;

    const std::size_t want = 12ull << 30;
    std::vector<std::uint8_t> raw;
    std::printf("  正在读 %s 的前 %.1f GB ...\n", path, want / 1073741824.0);
    if (read_messages(path, want, &raw) == 0) {
        std::fprintf(stderr, "打不开 %s\n", path);
        return 1;
    }
    std::printf("  读到 %.1f GB\n", raw.size() / 1073741824.0);

    constexpr std::uint64_t kOpenNs = 9ull * 3600 * 1000000000ull + 30ull * 60 * 1000000000ull;
    std::size_t open_at = 0;
    std::uint16_t max_sym = 0;
    {
        std::size_t off = 0;
        while (off + itch::kLenPrefix <= raw.size()) {
            const std::size_t len =
                (static_cast<std::size_t>(raw[off]) << 8) | raw[off + 1];
            if (len == 0 || off + itch::kLenPrefix + len > raw.size()) break;
            const std::uint8_t* body = raw.data() + off + itch::kLenPrefix;
            if (len >= 3) {
                const std::uint16_t s =
                    static_cast<std::uint16_t>((body[1] << 8) | body[2]);
                if (s > max_sym) max_sym = s;
            }
            if (len >= itch::kTimestampOff + 6) {
                std::uint64_t ts = 0;
                for (int i = 0; i < 6; ++i) {
                    ts = (ts << 8) | body[itch::kTimestampOff + i];
                }
                if (ts >= kOpenNs) { open_at = off; break; }
            }
            off += itch::kLenPrefix + len;
        }
    }
    if (open_at == 0) {
        std::fprintf(stderr, "在读进来的这一段里没走到开盘, 把 want 调大\n");
        return 1;
    }
    std::printf("  开盘落在第 %.2f GB 处, 一共 %u 只证券\n",
                open_at / 1073741824.0, max_sym + 1);

    book::OrderBook book(12582912, static_cast<std::size_t>(max_sym) + 1);
    book::Imbalance signal(88);

    {
        std::uint64_t n = 0;
        const auto r = itch::for_each_message(
            raw.data(), open_at, [&](const itch::Message& m) {
                std::uint16_t touched = 0;
                (void)book.apply(m, &touched);
                ++n;
                return true;
            });
        (void)r;
        std::printf("  预热完了: 应用了 %llu 条消息, 表里还有 %zu 条活单\n",
                    static_cast<unsigned long long>(n), book.live());
    }

    const std::size_t msgs_per_pkt =
        msgs_per_batch / pkts_per_batch > 0 ? msgs_per_batch / pkts_per_batch : 1;
    const std::size_t want_pkts = batches * pkts_per_batch;

    std::vector<std::uint8_t> flat;
    flat.reserve(want_pkts * 256);
    std::vector<std::size_t> pkt_off, pkt_len;
    {
        std::size_t off = open_at, in_pkt = 0, begin = 0;
        while (off + itch::kLenPrefix <= raw.size() && pkt_off.size() < want_pkts) {
            const std::size_t len =
                (static_cast<std::size_t>(raw[off]) << 8) | raw[off + 1];
            if (len == 0 || off + itch::kLenPrefix + len > raw.size()) break;
            if (in_pkt == 0) begin = flat.size();
            flat.insert(flat.end(), raw.begin() + off,
                        raw.begin() + off + itch::kLenPrefix + len);
            off += itch::kLenPrefix + len;
            if (++in_pkt >= msgs_per_pkt) {
                pkt_off.push_back(begin);
                pkt_len.push_back(flat.size() - begin);
                in_pkt = 0;
            }
        }
    }
    if (pkt_off.size() < pkts_per_batch) {
        std::fprintf(stderr, "开盘之后的消息不够装一批\n");
        return 1;
    }

    std::vector<std::uint8_t> ring(pkt_off.size() * kSlot + kSlot, 0);
    {
        std::vector<std::size_t> order(pkt_off.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::mt19937_64 shuf(4242);
        for (std::size_t i = order.size(); i > 1; --i) {
            std::swap(order[i - 1], order[shuf() % i]);
        }
        for (std::size_t i = 0; i < pkt_off.size(); ++i) {
            std::memcpy(ring.data() + order[i] * kSlot, flat.data() + pkt_off[i],
                        pkt_len[i]);
            pkt_off[i] = order[i] * kSlot;
        }
    }
    std::vector<std::uint8_t>().swap(flat);
    std::vector<std::uint8_t>().swap(raw);

    const std::size_t real_batches = pkt_off.size() / pkts_per_batch;
    std::printf("  装好 %zu 个包 (每包 %zu 条), 摊在 %.1f GB 里, 能跑 %zu 批\n",
                pkt_off.size(), msgs_per_pkt, ring.size() / 1073741824.0,
                real_batches);

    const double tpn = tsc::ticks_per_ns();
    std::vector<double> per_batch;
    per_batch.reserve(real_batches);
    std::uint64_t applied = 0, seen = 0, fired = 0;

    for (std::size_t b = 0; b < real_batches; ++b) {
        const std::uint64_t t0 = tsc::now();
        for (std::size_t p = b * pkts_per_batch; p < (b + 1) * pkts_per_batch; ++p) {
            const std::uint8_t* d = ring.data() + pkt_off[p];
            const auto r = itch::for_each_message(
                d, pkt_len[p], [&](const itch::Message& m) {
                    ++seen;
                    std::uint16_t touched = 0;
                    if (!book.apply(m, &touched)) return true;
                    ++applied;
                    const auto what =
                        signal.check(book.top3(touched, book::PriceLevels::kBuy),
                                     book.top3(touched, book::PriceLevels::kSell));
                    if (what != book::Imbalance::Signal::kNone) ++fired;
                    return true;
                });
            (void)r;
        }
        const std::uint64_t t1 = tsc::now();
        per_batch.push_back((t1 - t0) / tpn);
    }

    std::sort(per_batch.begin(), per_batch.end());
    const std::size_t n = per_batch.size();
    auto at = [&](double q) {
        return per_batch[std::min(n - 1, static_cast<std::size_t>(n * q))];
    };
    double total = 0;
    for (double x : per_batch) total += x;

    std::printf("\n  一批 = %zu 个包 / 约 %zu 条消息 (真机窗口内的 p99.9)\n",
                pkts_per_batch, pkts_per_batch * msgs_per_pkt);
    std::printf("  跑了 %zu 批, 走了 %llu 条消息, 其中 %llu 条建了簿, %llu 次出信号\n",
                n, static_cast<unsigned long long>(seen),
                static_cast<unsigned long long>(applied),
                static_cast<unsigned long long>(fired));
    std::printf("\n  处理一批要多久 (ns):\n");
    std::printf("    p50 %8.0f   p90 %8.0f   p99 %8.0f   p99.9 %8.0f   p99.99 %8.0f   最大 %8.0f\n",
                at(0.5), at(0.9), at(0.99), at(0.999), at(0.9999), per_batch.back());
    std::printf("\n  ⭐ 平均每条消息 %.0f ns; 极限吞吐 %.2f M 条/秒\n",
                total / static_cast<double>(seen),
                static_cast<double>(seen) / total * 1000.0);
    return 0;
}
