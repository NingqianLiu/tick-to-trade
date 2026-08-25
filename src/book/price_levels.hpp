#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "common/huge.hpp"

namespace book {

class PriceLevels {
public:
    static constexpr std::uint32_t kDollar = 10000;
    static constexpr std::uint32_t kCent = 100;
    static constexpr std::uint32_t kTopPrice = 0x7fffffffu;
    static constexpr std::uint8_t kBuy = 0, kSell = 1;

    explicit PriceLevels(const std::vector<std::uint32_t>& references)
        : PriceLevels(budget_for(references)) {}

    explicit PriceLevels(std::size_t words) {
        block_.assign(words, 0);
        book_.assign(kSecurities, Security{});
    }

    static std::size_t budget_for(const std::vector<std::uint32_t>& references) {
        std::size_t words = 0;
        for (std::uint32_t r : references) words += 2 * words_for(r);
        return words;
    }

    [[nodiscard]] std::size_t bytes() const noexcept { return block_.size() * 8; }

    bool bind(std::uint16_t sym, std::uint32_t reference) {
        if (reference == 0 || book_[sym].bound) return false;
        if (next_ + 2 * words_for(reference) > block_.size()) return false;
        for (int s = 0; s < 2; ++s) {
            Side& side = book_[sym].side[s];
            const std::uint32_t lo = reference / 8;
            const std::uint64_t hi = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(reference) * 8, kTopPrice);
            if (lo < kDollar) {
                carve(side.fine, lo, 1,
                      static_cast<std::uint32_t>(std::min<std::uint64_t>(kDollar, hi + 1)) - lo);
            }
            if (hi >= kDollar) {
                const std::uint32_t base = (lo < kDollar ? kDollar : lo) / kCent * kCent;
                carve(side.coarse, base, kCent,
                      static_cast<std::uint32_t>((hi - base) / kCent) + 1);
            }
        }
        book_[sym].bound = true;
        return true;
    }

    [[nodiscard]] bool bound(std::uint16_t sym) const noexcept { return book_[sym].bound; }

    void add(std::uint16_t sym, std::uint8_t s, std::uint32_t price, std::uint32_t shares) {
        Band* b = band(sym, s, price);
        if (b == nullptr) return;
        const std::uint32_t i = (price - b->base) / b->step;
        if (b->qty[i] == 0) set(*b, i);
        b->qty[i] += shares;
    }

    void remove(std::uint16_t sym, std::uint8_t s, std::uint32_t price, std::uint32_t shares) {
        Band* b = band(sym, s, price);
        if (b == nullptr) return;
        const std::uint32_t i = (price - b->base) / b->step;
        b->qty[i] = shares >= b->qty[i] ? 0 : b->qty[i] - shares;
        if (b->qty[i] == 0) clear(*b, i);
    }

    [[nodiscard]] std::uint32_t at(std::uint16_t sym, std::uint8_t s,
                                   std::uint32_t price) const {
        const Band* b = band(sym, s, price);
        if (b == nullptr) return 0;
        return b->qty[(price - b->base) / b->step];
    }

    [[nodiscard]] bool best(std::uint16_t sym, std::uint8_t s, std::uint32_t* price,
                            std::uint32_t* shares) const {
        return step_from(sym, s, kNone, price, shares);
    }

    [[nodiscard]] std::uint64_t top3(std::uint16_t sym, std::uint8_t s) const {
        std::uint64_t sum = 0;
        std::uint32_t price = kNone, shares = 0;
        for (int n = 0; n < 3; ++n) {
            if (!step_from(sym, s, price, &price, &shares)) return 0;
            sum += shares;
        }
        return sum;
    }

private:
    static constexpr std::size_t kSecurities = 1u << 16;
    static constexpr std::uint32_t kNone = 0xffffffffu;
    static constexpr std::uint64_t kPastStart = ~0ull;

    struct Band {
        std::uint32_t base = 0, step = 1, len = 0;
        std::uint32_t* qty = nullptr;
        std::uint64_t* w0 = nullptr;
        std::uint64_t* w1 = nullptr;
        std::uint64_t* w2 = nullptr;
        std::uint32_t n0 = 0, n1 = 0, n2 = 0;
    };
    struct Side {
        Band fine;
        Band coarse;
    };
    struct Security {
        Side side[2];
        bool bound = false;
    };

    static std::uint32_t up(std::uint32_t v, std::uint32_t d) { return (v + d - 1) / d; }

public:
    static std::size_t words_for(std::uint32_t reference) {
        const std::uint32_t lo = reference / 8;
        const std::uint64_t hi = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(reference) * 8, kTopPrice);
        std::size_t w = 0;
        if (lo < kDollar) {
            w += band_words(
                static_cast<std::uint32_t>(std::min<std::uint64_t>(kDollar, hi + 1)) - lo);
        }
        if (hi >= kDollar) {
            const std::uint32_t base = (lo < kDollar ? kDollar : lo) / kCent * kCent;
            w += band_words(static_cast<std::uint32_t>((hi - base) / kCent) + 1);
        }
        return w;
    }

private:
    static std::size_t band_words(std::uint32_t len) {
        const std::uint32_t n0 = up(len, 64), n1 = up(n0, 64), n2 = up(n1, 64);
        return up(len, 2) + n0 + n1 + n2;
    }

    void carve(Band& b, std::uint32_t base, std::uint32_t step, std::uint32_t len) {
        b.base = base;
        b.step = step;
        b.len = len;
        b.n0 = up(len, 64);
        b.n1 = up(b.n0, 64);
        b.n2 = up(b.n1, 64);
        b.qty = reinterpret_cast<std::uint32_t*>(&block_[next_]);
        next_ += up(len, 2);
        b.w0 = &block_[next_];
        next_ += b.n0;
        b.w1 = &block_[next_];
        next_ += b.n1;
        b.w2 = &block_[next_];
        next_ += b.n2;
    }

    [[nodiscard]] const Band* band(std::uint16_t sym, std::uint8_t s,
                                   std::uint32_t price) const {
        const Security& sec = book_[sym];
        if (!sec.bound) return nullptr;
        const Band& b = price < kDollar ? sec.side[s].fine : sec.side[s].coarse;
        if (b.len == 0 || price < b.base) return nullptr;
        if ((price - b.base) % b.step != 0) return nullptr;
        return (price - b.base) / b.step < b.len ? &b : nullptr;
    }
    [[nodiscard]] Band* band(std::uint16_t sym, std::uint8_t s, std::uint32_t price) {
        return const_cast<Band*>(
            static_cast<const PriceLevels*>(this)->band(sym, s, price));
    }

    static void set(Band& b, std::uint32_t i) {
        b.w0[i >> 6] |= 1ull << (i & 63);
        b.w1[i >> 12] |= 1ull << ((i >> 6) & 63);
        b.w2[i >> 18] |= 1ull << ((i >> 12) & 63);
    }
    static void clear(Band& b, std::uint32_t i) {
        b.w0[i >> 6] &= ~(1ull << (i & 63));
        if (b.w0[i >> 6] != 0) return;
        b.w1[i >> 12] &= ~(1ull << ((i >> 6) & 63));
        if (b.w1[i >> 12] != 0) return;
        b.w2[i >> 18] &= ~(1ull << ((i >> 12) & 63));
    }

    static bool pick(const Band& b, bool highest, std::uint32_t from, std::uint32_t* out) {
        if (b.len == 0) return false;
        for (std::uint32_t k2 = 0; k2 < b.n2; ++k2) {
            const std::uint32_t i2 = highest ? b.n2 - 1 - k2 : k2;
            std::uint64_t m2 = b.w2[i2];
            while (m2 != 0) {
                const std::uint32_t j1 = highest ? 63 - __builtin_clzll(m2)
                                                 : __builtin_ctzll(m2);
                m2 &= ~(1ull << j1);
                const std::uint32_t i1 = (i2 << 6) | j1;
                std::uint64_t m1 = b.w1[i1];
                while (m1 != 0) {
                    const std::uint32_t j0 = highest ? 63 - __builtin_clzll(m1)
                                                     : __builtin_ctzll(m1);
                    m1 &= ~(1ull << j0);
                    const std::uint32_t i0 = (i1 << 6) | j0;
                    std::uint64_t m0 = b.w0[i0];
                    while (m0 != 0) {
                        const std::uint32_t j = highest ? 63 - __builtin_clzll(m0)
                                                        : __builtin_ctzll(m0);
                        m0 &= ~(1ull << j);
                        const std::uint32_t i = (i0 << 6) | j;
                        if (from != kNone && (highest ? i >= from : i <= from)) continue;
                        *out = i;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool step_from(std::uint16_t sym, std::uint8_t s, std::uint32_t after,
                   std::uint32_t* price, std::uint32_t* shares) const {
        const Security& sec = book_[sym];
        if (!sec.bound) return false;
        const bool highest = s == kBuy;
        const Band* first = highest ? &sec.side[s].coarse : &sec.side[s].fine;
        const Band* second = highest ? &sec.side[s].fine : &sec.side[s].coarse;
        for (const Band* b : {first, second}) {
            if (b->len == 0) continue;
            std::uint32_t from = kNone;
            if (after != kNone) {
                const std::uint64_t at =
                    after >= b->base
                        ? (static_cast<std::uint64_t>(after) - b->base) / b->step
                        : kPastStart;
                if (at < b->len) {
                    from = static_cast<std::uint32_t>(at);
                } else if (highest ? after < b->base : at != kPastStart) {
                    continue;
                }
            }
            std::uint32_t i = 0;
            if (!pick(*b, highest, from, &i)) continue;
            *price = b->base + i * b->step;
            *shares = b->qty[i];
            return true;
        }
        return false;
    }

    huge::Buffer<std::uint64_t> block_;
    huge::Buffer<Security> book_;
    std::size_t next_ = 0;
};

}
