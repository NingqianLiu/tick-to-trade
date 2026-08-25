#pragma once

// How many shares rest on each price, for both sides of every security, plus a set of
// bitmaps saying which prices have anything on them.
//
// This sounds like it should be a hash table, price against shares. It is not.
//
// The strategy only ever asks one question: how many shares rest on the best three
// prices. Through a hash that is three unrelated random accesses, a few hundred cycles
// each, against a wire to wire p50 of three thousand nanoseconds. As an array indexed by
// the price itself it is close to a single read: walk a day of ITCH and seven times out
// of ten the best three prices are within three ticks of each other, which is three
// neighbouring slots, twelve bytes, one cache line.
//
// If it is indexed by price, does the space move when the price does? No. Each security's
// band is fixed when the run starts and never moves again. Moving it would mean copying,
// and the prices it moved to have no history to copy anyway - whatever was there is gone.
// Address space pays instead: eight times up and eight times down from a reference price.
// That covers every security in the day; the biggest mover only went 4.5 times.
//
// Why two bands per side? Below a dollar a stock may be quoted in hundredths of a cent;
// at a dollar and above only in whole cents. That is a hundred to one. Forcing both into
// one band would waste ninety nine slots out of every hundred above a dollar, so anything
// under a dollar gets a fine band of its own. About one security in five is cheap enough
// to need it.

// std::min, for holding the top of a band inside the legal range.
#include <algorithm>
// std::size_t for word counts, lengths and indexes.
#include <cstddef>
// Fixed width integers: prices, share counts and bitmap words all have a size.
#include <cstdint>
// Not called directly any more, but the line that reinterprets a piece of the 64 bit
// block as an array of 32 bit counts relies on the same memory model.
#include <cstring>
// std::vector, for the row of reference prices budget_for takes.
#include <vector>

// huge::Buffer: an array on huge pages, with every page touched before the run starts.
// Both blocks here use it. The price space is a few gigabytes, and on ordinary pages the
// translation cache could not hold it, so every access would pay an extra walk.
#include "common/huge.hpp"

namespace book {

// One shard's price levels: two large blocks, plus a row saying where each security's
// space is.
class PriceLevels {
public:
    // Prices on the wire are whole hundredths of a cent, so a dollar is 10,000. This is
    // also the line between the two bands.
    static constexpr std::uint32_t kDollar = 10000;  // in hundredths of a cent
    // A cent is a hundred of those units, and it is the step of the coarse band - the
    // rule that a dollar and up may only be quoted in whole cents, written as code.
    static constexpr std::uint32_t kCent = 100;
    // The highest price there can be. 31 bits rather than 32, because the order table
    // stores the price and the side in one 32 bit word and the side takes the top bit.
    static constexpr std::uint32_t kTopPrice = 0x7fffffffu;  // what 31 bits hold
    // 0 and 1 so they can index an array directly.
    static constexpr std::uint8_t kBuy = 0, kSell = 1;

    // Single threaded path: given every security's reference price, work out how much
    // memory they need altogether, take it in one piece, and touch every page. After that
    // no write during the run has to stop and ask the kernel for a page.
    explicit PriceLevels(const std::vector<std::uint32_t>& references)
        : PriceLevels(budget_for(references)) {}

    // Sharded path: just an allowance.
    //
    // A shard has no choice: it only learns which securities are its own when the day's
    // feed names them, and the memory has to exist before that. So it claims its share
    // first and binds securities as they arrive.
    explicit PriceLevels(std::size_t words) {
        // The whole price space, zeroed. Zero means nothing rests on that price.
        block_.assign(words, 0);
        // One entry per security, indexed by the security number directly. Even when only
        // a hundred names are traded this row has sixty five thousand entries: it is
        // small, and it makes the lookup free.
        book_.assign(kSecurities, Security{});
    }

    // How many eight byte words both sides of a set of securities need.
    //
    // Public because whoever splits the work across shards has to divide the space with
    // exactly this arithmetic. If the two disagreed, some shard would run out halfway
    // through the day.
    static std::size_t budget_for(const std::vector<std::uint32_t>& references) {
        // This is the amount needed, not an estimate of an upper bound. Too little and a
        // security fails to bind partway through; too much and gigabytes of huge pages,
        // which the whole machine shares, sit idle.
        std::size_t words = 0;
        // Each side needs a full set - the share counts and three bitmaps - hence twice.
        for (std::uint32_t r : references) words += 2 * words_for(r);
        return words;
    }

    // How many bytes the price space came to, printed at start up.
    [[nodiscard]] std::size_t bytes() const noexcept { return block_.size() * 8; }

    // Gives one security its own price space. A false usually means this block was sized
    // from a different set of reference prices and is too small for this one.
    bool bind(std::uint16_t sym, std::uint32_t reference) {
        // With no reference price there is no centre to build around. Binding twice is
        // refused as well: the space handed out the first time could never be found again.
        if (reference == 0 || book_[sym].bound) return false;
        // Enough left for both sides, or nothing at all - half a security is worse.
        if (next_ + 2 * words_for(reference) > block_.size()) return false;
        // Both sides get the same range, so the same code runs twice.
        for (int s = 0; s < 2; ++s) {
            Side& side = book_[sym].side[s];
            // Down to an eighth. Why eight: the biggest mover of the day only went 4.5
            // times, so eight leaves room - and once this is set it can never change.
            const std::uint32_t lo = reference / 8;
            // Eight times up, computed in 64 bits. In 32 bits an expensive security would
            // overflow, the top would come out below its own reference price, not a single
            // one of its prices would ever be tracked, and nothing would report an error.
            const std::uint64_t hi = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(reference) * 8, kTopPrice);
            // A bottom below a dollar means this security needs the fine band.
            if (lo < kDollar) {
                // From lo, one unit per slot, up to whichever comes first: a dollar, where
                // the coarse band takes over, or the top of the range.
                carve(side.fine, lo, 1,
                      static_cast<std::uint32_t>(std::min<std::uint64_t>(kDollar, hi + 1)) - lo);
            }
            // A security so cheap that even eight times up stays under a dollar needs no
            // coarse band at all.
            if (hi >= kDollar) {
                // It starts at a dollar if the bottom was below one, otherwise at the
                // bottom, rounded down to a whole cent because a slot here is a cent.
                const std::uint32_t base = (lo < kDollar ? kDollar : lo) / kCent * kCent;
                // How many whole cents from there to the top, plus one to count both ends.
                carve(side.coarse, base, kCent,
                      static_cast<std::uint32_t>((hi - base) / kCent) + 1);
            }
        }
        // Without this flag every lookup below would treat the security as unknown.
        book_[sym].bound = true;
        return true;
    }

    // Whether a security got any space. One that did not can never produce a signal.
    [[nodiscard]] bool bound(std::uint16_t sym) const noexcept { return book_[sym].bound; }

    // Adds shares to a price.
    //
    // A price outside this security's band does nothing at all, and reports nothing. That
    // is not lost data: a price eight times away from the reference can never reach the
    // best three, so the strategy would never look at it, and the order itself is still in
    // the order table, so a later cancel still knows what to take away.
    void add(std::uint16_t sym, std::uint8_t s, std::uint32_t price, std::uint32_t shares) {
        // Which band the price falls in, or nothing if it is outside the space.
        Band* b = band(sym, s, price);
        // The caller notices this case by the count not changing, and counts it.
        if (b == nullptr) return;
        const std::uint32_t i = (price - b->base) / b->step;
        // Light the bit only when a price goes from empty to occupied, which skips the
        // bitmap write on almost every add.
        if (b->qty[i] == 0) set(*b, i);
        b->qty[i] += shares;
    }

    // Takes shares off a price; the other half of add.
    void remove(std::uint16_t sym, std::uint8_t s, std::uint32_t price, std::uint32_t shares) {
        Band* b = band(sym, s, price);
        // Outside the space it was never added either, so the two agree.
        if (b == nullptr) return;
        const std::uint32_t i = (price - b->base) / b->step;
        // Clamped at zero. These are unsigned, so going below would wrap to an enormous
        // number, that price would sit in front of every other one for the rest of the day,
        // and every signal for the security would be wrong.
        b->qty[i] = shares >= b->qty[i] ? 0 : b->qty[i] - shares;
        // An empty price has to go dark, or the best three would count a level with
        // nothing on it.
        if (b->qty[i] == 0) clear(*b, i);
    }

    // What rests on one price.
    //
    // Its main use is not asking a price: it is comparing before and after an add, which
    // is how the caller finds out the price was outside the space, since add is silent
    // about it.
    [[nodiscard]] std::uint32_t at(std::uint16_t sym, std::uint8_t s,
                                   std::uint32_t price) const {
        const Band* b = band(sym, s, price);
        if (b == nullptr) return 0;
        return b->qty[(price - b->base) / b->step];
    }

    // The best price of one side and what rests on it: the highest bid, the lowest offer.
    [[nodiscard]] bool best(std::uint16_t sym, std::uint8_t s, std::uint32_t* price,
                            std::uint32_t* shares) const {
        // kNone means "no starting point, begin at the best end".
        return step_from(sym, s, kNone, price, shares);
    }

    // Shares on the best three prices, or zero if that side does not have three, which the
    // strategy reads as "leave this one alone".
    //
    // This is one of the two calls the signal makes, once per side. Measured with
    // --profile it is about 121 ns. It used to run on every message, which made it 37% of
    // the processing segment; it now runs once per poll for each security the batch
    // touched. Since a deep poll is almost always one security in a burst - of the polls
    // that took eight or more events, 99.98% involved a single name - the deepest poll of
    // the day, 429 messages, now costs one call.
    [[nodiscard]] std::uint64_t top3(std::uint16_t sym, std::uint8_t s) const {
        const Security& sec = book_[sym];
        if (!sec.bound) return 0;
        // A bid wants the three highest prices, an offer the three lowest. Both count from
        // their own best end; only the direction and which band comes first differ.
        const bool highest = s == kBuy;
        // A bid wants high prices, so it starts in the coarse band; an offer starts fine.
        const Band* first = highest ? &sec.side[s].coarse : &sec.side[s].fine;
        const Band* second = highest ? &sec.side[s].fine : &sec.side[s].coarse;
        std::uint64_t sum = 0;
        // How many are still wanted; zero ends it.
        int need = 3;
        take(*first, highest, &need, &sum);
        if (need != 0) take(*second, highest, &need, &sum);
        // Fewer than three levels counts as no signal.
        return need == 0 ? sum : 0;
    }

private:
    // A security number is 16 bits, so the directory has an entry for every one of them
    // and a lookup is a single index.
    static constexpr std::size_t kSecurities = 1u << 16;
    // "No starting point". All ones, because it cannot be a real slot.
    static constexpr std::uint32_t kNone = 0xffffffffu;
    // "The price is below where this band starts".
    static constexpr std::uint64_t kPastStart = ~0ull;  // the price is below the stretch

    // A stretch of consecutive prices: slot i stands for base + i * step, for i below len.
    struct Band {
        std::uint32_t base = 0, step = 1, len = 0;
        // Shares per slot. 32 bits, so two slots share an eight byte word.
        std::uint32_t* qty = nullptr;
        // Three levels of summary of which slots are occupied - summaries, not an index
        // tree. One bit per price.
        std::uint64_t* w0 = nullptr;  // a bit per price
        // One bit per word of the level below, so one bit per 64 prices.
        std::uint64_t* w1 = nullptr;  // a bit per w0 word
        // One bit per word of that, so one bit per 4,096 prices. With these two summaries
        // a wide empty stretch is skipped in two reads instead of slot by slot.
        std::uint64_t* w2 = nullptr;  // a bit per w1 word
        std::uint32_t n0 = 0, n1 = 0, n2 = 0;
    };
    // The two bands of one side.
    struct Side {
        Band fine;    // under a dollar, in hundredths of a cent
        Band coarse;  // a dollar and up, in cents
    };
    // One security: both sides, and whether it was given any space.
    struct Security {
        Side side[2];
        bool bound = false;
    };

    // Division rounding up, used wherever a count of words is worked out.
    static std::uint32_t up(std::uint32_t v, std::uint32_t d) { return (v + d - 1) / d; }

public:
    // How many words one side of one security needs.
    //
    // This has to agree exactly with what carve() spends. If one counted differently from
    // the other, the block would either waste memory or run out partway through binding.
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
    // A band of len slots, with its three bitmaps, in eight byte words.
    static std::size_t band_words(std::uint32_t len) {
        // Each level of summary covers 64 units of the one below it.
        const std::uint32_t n0 = up(len, 64), n1 = up(n0, 64), n2 = up(n1, 64);
        // Share counts are 32 bits, so two slots to a word - hence up(len, 2), not len.
        return up(len, 2) + n0 + n1 + n2;  // quantities are half a word each
    }

    // Cuts one band out of the block.
    //
    // Every band of every security is cut from the same block, one after another, with
    // next_ moving forward. The whole price space is therefore one contiguous piece of
    // memory, with no separate allocation anywhere.
    void carve(Band& b, std::uint32_t base, std::uint32_t step, std::uint32_t len) {
        b.base = base;
        b.step = step;
        b.len = len;
        b.n0 = up(len, 64);
        b.n1 = up(b.n0, 64);
        b.n2 = up(b.n1, 64);
        // The share counts first. The block is managed in 64 bit words while a count is
        // 32 bits, which is what this reinterpretation is for.
        b.qty = reinterpret_cast<std::uint32_t*>(&block_[next_]);
        // Two slots to a word.
        next_ += up(len, 2);
        b.w0 = &block_[next_];
        next_ += b.n0;
        b.w1 = &block_[next_];
        next_ += b.n1;
        // After the third level this side is complete and next_ stands at the start of the
        // next band.
        b.w2 = &block_[next_];
        next_ += b.n2;
    }

    // Which band a price belongs to, or nothing if it is outside this security's space.
    [[nodiscard]] const Band* band(std::uint16_t sym, std::uint8_t s,
                                   std::uint32_t price) const {
        const Security& sec = book_[sym];
        if (!sec.bound) return nullptr;
        const Band& b = price < kDollar ? sec.side[s].fine : sec.side[s].coarse;
        // The band does not exist, or the price is below where it starts.
        if (b.len == 0 || price < b.base) return nullptr;
        // A price between two whole cents has no slot in the coarse band. The exchange
        // does not allow one above a dollar either, so it is left untracked.
        if ((price - b.base) % b.step != 0) return nullptr;
        // And it has to be inside the length.
        return (price - b.base) / b.step < b.len ? &b : nullptr;
    }
    // The writable version. Same logic, so it calls the const one and casts the const
    // away, rather than keeping two copies that could drift apart.
    [[nodiscard]] Band* band(std::uint16_t sym, std::uint8_t s, std::uint32_t price) {
        return const_cast<Band*>(
            static_cast<const PriceLevels*>(this)->band(sym, s, price));
    }

    // Lights slot i in all three levels. All three matter: the upper two are summaries,
    // and leaving one dark makes a search skip the whole stretch.
    static void set(Band& b, std::uint32_t i) {
        // i >> 6 is which word, i & 63 is which bit in it.
        b.w0[i >> 6] |= 1ull << (i & 63);
        // A bit of the second level stands for a word of the first.
        b.w1[i >> 12] |= 1ull << ((i >> 6) & 63);
        // And a bit of the third for a word of the second.
        b.w2[i >> 18] |= 1ull << ((i >> 12) & 63);
    }
    // Puts slot i out.
    //
    // Not the mirror image of set: an upper level may only go dark once the whole word
    // below it is empty, or it would hide the other slots still lit in that word.
    static void clear(Band& b, std::uint32_t i) {
        b.w0[i >> 6] &= ~(1ull << (i & 63));
        if (b.w0[i >> 6] != 0) return;
        b.w1[i >> 12] &= ~(1ull << ((i >> 6) & 63));
        if (b.w1[i >> 12] != 0) return;
        b.w2[i >> 18] &= ~(1ull << ((i >> 12) & 63));
    }

    // Counts down from the best end of a band, adding to sum until need reaches zero.
    //
    // This shape is the point. The obvious version calls step_from three times, and each
    // call starts again at the outermost bitmap, so the second walk repeats the first and
    // the third repeats both - each level of bitmap is a random access, so two extra
    // walks are four accesses paid for nothing.
    //
    // Here, once a bitmap word has been reached, all three levels are finished inside it,
    // with m2, m1 and m0 staying in registers. The second and third levels almost never
    // touch memory again. On a price space of a couple of gigabytes a microbenchmark put
    // this 74% ahead.
    static void take(const Band& b, bool highest, int* need, std::uint64_t* sum) {
        // The band does not exist - for instance a security too expensive to have a fine
        // one.
        if (b.len == 0) return;
        for (std::uint32_t k2 = 0; k2 < b.n2; ++k2) {
            // A bid wants the highest price, so it starts at the last word and walks back;
            // an offer starts at the first.
            const std::uint32_t i2 = highest ? b.n2 - 1 - k2 : k2;
            // A true means three levels were found and the whole thing can stop.
            if (walk1(b, highest, b.w2[i2], i2, need, sum)) return;
        }
    }

    // Walks down from one word of the outermost bitmap: every lit bit leads to a word of
    // the middle level, every lit bit there to a word of the bottom one, and there the
    // prices are counted one bit at a time.
    static bool walk1(const Band& b, bool highest, std::uint64_t m2,
                      std::uint32_t i2, int* need, std::uint64_t* sum) {
        while (m2 != 0) {
            // The lit bit nearest the good end, then cleared so the next turn takes the
            // one after it. Counting leading or trailing zeros is one instruction each,
            // not a loop.
            const std::uint32_t j1 = highest ? 63 - __builtin_clzll(m2)
                                             : __builtin_ctzll(m2);
            // What is cleared is this local copy, never the bitmap itself.
            m2 &= ~(1ull << j1);
            // Which word, and which bit in it, make the index of the level below.
            const std::uint32_t i1 = (i2 << 6) | j1;
            // One of the few real memory accesses in this walk.
            std::uint64_t m1 = b.w1[i1];
            while (m1 != 0) {
                const std::uint32_t j0 = highest ? 63 - __builtin_clzll(m1)
                                                 : __builtin_ctzll(m1);
                m1 &= ~(1ull << j0);
                const std::uint32_t i0 = (i1 << 6) | j0;
                // In this word a bit is a price.
                std::uint64_t m0 = b.w0[i0];
                while (m0 != 0) {
                    const std::uint32_t j = highest ? 63 - __builtin_clzll(m0)
                                                    : __builtin_ctzll(m0);
                    m0 &= ~(1ull << j);
                    *sum += b.qty[(i0 << 6) | j];
                    // Enough: return all the way out, leaving both outer loops.
                    if (--*need == 0) return true;
                }
            }
        }
        // This outer word is used up without reaching three.
        return false;
    }

    // Finds the one occupied price nearest the good end, skipping from and everything
    // better than it. Same walk as take, but it stops at the first slot; best() uses it.
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
                        // The caller wants the next level after from, so from itself and
                        // anything better than it is skipped.
                        if (from != kNone && (highest ? i >= from : i <= from)) continue;
                        *out = i;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // With kNone, the best level of a side. With a slot number, the next one after it.
    bool step_from(std::uint16_t sym, std::uint8_t s, std::uint32_t after,
                   std::uint32_t* price, std::uint32_t* shares) const {
        const Security& sec = book_[sym];
        if (!sec.bound) return false;
        const bool highest = s == kBuy;
        // A bid looks in the coarse band first, since a dollar and up always beats below
        // a dollar; an offer the other way round.
        const Band* first = highest ? &sec.side[s].coarse : &sec.side[s].fine;
        const Band* second = highest ? &sec.side[s].fine : &sec.side[s].coarse;
        for (const Band* b : {first, second}) {
            if (b->len == 0) continue;
            std::uint32_t from = kNone;
            // A caller asking for the next level after one has to have that level
            // translated into this band's coordinates first.
            if (after != kNone) {
                // Three cases: inside this band, past it already, or not reached yet.
                const std::uint64_t at =
                    after >= b->base
                        ? (static_cast<std::uint64_t>(after) - b->base) / b->step
                        : kPastStart;
                if (at < b->len) {
                    from = static_cast<std::uint32_t>(at);
                // Telling "already walked past this band" from "not there yet": a bid moves
                // towards lower prices, so a level below this band's start means the band
                // is done; an offer moves the other way.
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
        // Neither band has anything: this side is empty and no order goes out.
        return false;
    }

    // The whole price space. Every band of every security is cut out of this.
    huge::Buffer<std::uint64_t> block_;
    // One entry per security, indexed by the security number.
    huge::Buffer<Security> book_;
    // How far into the block the carving has got.
    std::size_t next_ = 0;
};

}  // namespace book
