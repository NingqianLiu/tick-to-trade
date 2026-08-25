#pragma once

// How many shares rest at every price on both sides of every security, plus a set of bitmaps
// saying which prices hold anything.
//
// It seems as though this ought to be a hash table, prices against share counts. It is not.
//
// The strategy asks one question from beginning to end: how many shares rest on the best three
// prices altogether.
// Asked of a hash, that is three unrelated random memory accesses, hundreds of cycles each.
// And our whole wire to wire p50 is only three thousand nanoseconds, of which three random
// accesses eat a good part.
// Turned into "an array indexed by the price itself", the whole thing becomes essentially one
// read.
// Going through a day of ITCH shows that seven times in ten, the best three prices are within
// three minimum increments of one another. Which is three neighbouring entries of the array,
// twelve bytes altogether.
// One cache line holds that.
//
// Indexed by price, then: once a price has risen, does that stretch have to move with it?
// It does not. A security's price space is settled at start up and never moves after.
// Moving it would mean copying the content across.
// And the new stretch of prices moved into has no history to copy, while what was there is gone.
// The cost is paid in address space instead: every security has eight times its reference price
// left above and below.
// That covers every security of the day. The one that moved hardest that day moved four and a
// half times.
//
// Why does one security have two price spaces?
// A share below a dollar may be quoted to a hundredth of a cent. Above a dollar it may only be
// quoted to a whole cent.
// The two minimum increments differ by a hundred times.
// Forced into one stretch, everything above a dollar would waste ninety nine entries out of a
// hundred.
// So everything below a dollar gets a fine stretch of its own.
// About one security in five across the market is cheap enough to need it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// huge::Buffer is an array allocated in huge pages, every page of which was touched before the
// run.
// Both blocks of memory of this layer use it.
// The price space is several gigabytes. In ordinary pages the address translation cache could not
// hold it at all.
// Every access would then make one more trip through a lookup.
#include "common/huge.hpp"

namespace book {

// One shard's own table of price levels: two large blocks of memory and a row saying where each
// security's space is.
class PriceLevels {
public:
    // A price in ITCH is an integer in hundredths of a cent. So a dollar is written as 10000.
    // This number appears all over what follows. It is exactly the boundary between the two price
    // spaces.
    static constexpr std::uint32_t kDollar = 10000;  // in hundredths of a cent
    // A cent is 100 of the minimum units.
    // It is also the step of the coarse stretch.
    // Which is what the rule "above a dollar only whole cents may be quoted" looks like in code.
    static constexpr std::uint32_t kCent = 100;
    // The largest value a price may take.
    // The limit is 31 bits rather than 32.
    // Because the order table squeezes the price and the side into one 32 bit field.
    // The side takes the top bit.
    static constexpr std::uint32_t kTopPrice = 0x7fffffffu;  // what 31 bits hold
    // The numbers of the buy and the sell side. They are 0 and 1 so they can index an array
    // directly.
    static constexpr std::uint8_t kBuy = 0, kSell = 1;

    // Construction (the single thread path). It takes the reference prices of every security.
    // How much memory they need together is worked out from them and asked for at once.
    // Then every page is touched.
    // That way no write to memory once the run is going has to stop and ask the kernel for a
    // page.
    explicit PriceLevels(const std::vector<std::uint32_t>& references)
        : PriceLevels(budget_for(references)) {}

    // Construction (the sharded path). An allowance is given directly.
    //
    // A shard has to work this way.
    // It only learns which securities are its own once the day's market data names them.
    // And the memory has to exist before that.
    // So the space is asked for by share first, and bind follows once the names arrive.
    explicit PriceLevels(std::size_t words) {
        // The whole price space, cleared to zero. A zero means nothing rests at that price.
        block_.assign(words, 0);
        // A directory of one entry per security. It is indexed by the security number directly,
        // so this row is sixty five thousand entries even where only a hundred and one names are
        // subscribed to - it is small (a few dozen bytes an entry) and what it buys is a lookup
        // that costs nothing.
        book_.assign(kSecurities, Security{});
    }

    // How many eight byte words both sides of one security need altogether.
    // It is public because a caller dividing the work by shard has to divide this space by the
    // same arithmetic.
    // Worked out differently on the two sides, some shard would find its space short halfway
    // through.
    static std::size_t budget_for(const std::vector<std::uint32_t>& references) {
        // This is the total to ask for at once and not an estimate of an upper bound.
        // Too little and binding fails halfway through a run.
        // Too much and several gigabytes of huge pages are held for nothing - and huge pages are
        // shared by the whole machine.
        std::size_t words = 0;
        // The buy side and the sell side each need a whole set (the share array and the three
        // bitmaps), so it is doubled.
        for (std::uint32_t r : references) words += 2 * words_for(r);
        // The unit is eight byte words rather than bytes - the constructor opens its array in
        // that unit.
        return words;
    }

    // How many bytes this price space is altogether. Printed at the start of a run, to confirm
    // how large this round opened it.
    [[nodiscard]] std::size_t bytes() const noexcept { return block_.size() * 8; }

    // Cuts out a price space of its own for one security.
    // A false means it was not cut - usually because this block was worked out from another set
    // of reference prices and is not enough for this one.
    bool bind(std::uint16_t sym, std::uint32_t reference) {
        // A reference price of zero gives no centre.
        // And one already bound must not be bound again.
        // Otherwise the space cut out the first time could never be found again and would simply
        // leak.
        if (reference == 0 || book_[sym].bound) return false;
        // Whether the space left is enough for both sides of this security. Not enough means not
        // binding at all - binding half of it is worse.
        if (next_ + 2 * words_for(reference) > block_.size()) return false;
        // One share is cut for the buy side and one for the sell, and the arithmetic of the range
        // is identical for both.
        for (int s = 0; s < 2; ++s) {
            // The two sides have exactly the same range, so one piece of code runs twice.
            Side& side = book_[sym].side[s];
            // An eighth of it is left below.
            // Why eight? The security that moved hardest that day moved four and a half times.
            // Eight leaves room over.
            // And once this range is settled it can never change.
            const std::uint32_t lo = reference / 8;
            // Eight times is left above as well.
            // Widened to 64 bits before the multiplication.
            // Multiplied in 32 bits, a dear security overflows.
            // The upper bound then works out smaller than the reference price, and not one of its
            // prices can be placed at all.
            // And nothing reports it.
            const std::uint64_t hi = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(reference) * 8, kTopPrice);
            // The lower bound is still below a dollar, so this security needs that fine stretch.
            if (lo < kDollar) {
                // The fine stretch starts at lo with one minimum unit an entry, and runs to
                // whichever comes first of "a dollar" and the upper bound - everything above a
                // dollar belongs to the coarse stretch below.
                carve(side.fine, lo, 1,
                      static_cast<std::uint32_t>(std::min<std::uint64_t>(kDollar, hi + 1)) - lo);
            }
            // A security so cheap that even eight times it is below a dollar needs no coarse
            // stretch at all.
            if (hi >= kDollar) {
                // Where the coarse stretch starts: at a dollar where the lower bound is below
                // one, and at the lower bound otherwise; and pressed down onto a whole cent,
                // because an entry of this stretch is one cent.
                const std::uint32_t base = (lo < kDollar ? kDollar : lo) / kCent * kCent;
                // How many whole cents there are between the start and the upper bound, plus one
                // to count both ends.
                carve(side.coarse, base, kCent,
                      static_cast<std::uint32_t>((hi - base) / kCent) + 1);
            }
        }
        // This security is marked as having space. Without this line every lookup below would
        // take it as not existing.
        book_[sym].bound = true;
        // Bound. The caller uses it to count how many securities can really produce signals.
        return true;
    }

    // Whether this security got a price space. One that did not can never produce a signal.
    [[nodiscard]] bool bound(std::uint16_t sym) const noexcept { return book_[sym].bound; }

    // Adds some shares at a price.
    // Where the price falls outside this security's space, this does nothing at all and reports
    // nothing.
    // That is not losing data.
    // Such a price is eight times away from the best price and can never reach the best three,
    // and the strategy never looks at it.
    // And the order itself is still recorded in the order table. A later cancel still knows what
    // to cancel.
    void add(std::uint16_t sym, std::uint8_t s, std::uint32_t price, std::uint32_t shares) {
        // Which stretch this price falls in (fine or coarse) is found first; not found means it
        // is outside the space.
        Band* b = band(sym, s, price);
        // Not in the space, so it returns - the caller notices by the total not having changed
        // and counts it.
        if (b == nullptr) return;
        // The price is turned into which entry of that stretch it is.
        const std::uint32_t i = (price - b->base) / b->step;
        // It was empty and is about to hold something, so that entry is lit in the bitmaps.
        // Lit only on going from zero to non zero, which saves the vast majority of bitmap
        // writes.
        if (b->qty[i] == 0) set(*b, i);
        // The shares are added on.
        b->qty[i] += shares;
    }

    // Takes some shares off a price. It pairs with add.
    void remove(std::uint16_t sym, std::uint8_t s, std::uint32_t price, std::uint32_t shares) {
        // Which stretch this price belongs to is found first as before.
        Band* b = band(sym, s, price);
        // Outside the space means doing nothing - it was never added either, so the two agree.
        if (b == nullptr) return;
        // The price is turned into an entry number.
        const std::uint32_t i = (price - b->base) / b->step;
        // Asked for more than is there, it is clamped at zero.
        // These are unsigned, and taking off too much wraps round to an astronomical number.
        // That level would then rank first forever and the whole security's signals would be
        // worthless.
        b->qty[i] = shares >= b->qty[i] ? 0 : b->qty[i] - shares;
        // Down to zero, that entry is put out in the bitmaps, or the best three would count an
        // empty level.
        if (b->qty[i] == 0) clear(*b, i);
    }

    // How many shares rest at a price now.
    // Its main use is not asking a price but letting the caller compare "before and after an
    // add" to notice that a price fell outside the space - because an add outside is quietly
    // ignored.
    [[nodiscard]] std::uint32_t at(std::uint16_t sym, std::uint8_t s,
                                   std::uint32_t price) const {
        const Band* b = band(sym, s, price);
        if (b == nullptr) return 0;
        return b->qty[(price - b->base) / b->step];
    }

    // A side's best price and how many shares rest at it.
    // The buy side wants the highest price and the sell side the lowest - both being "the best
    // price of that side".
    [[nodiscard]] bool best(std::uint16_t sym, std::uint8_t s, std::uint32_t* price,
                            std::uint32_t* shares) const {
        // Passing kNone means "no starting limit, look from the best end".
        return step_from(sym, s, kNone, price, shares);
    }

    // How many shares rest on the best three prices altogether. A side with fewer than three
    // levels returns zero.
    // A zero says to the strategy "leave this one alone".
    //
    // This is one of the two functions the strategy calls on every decision, once per side.
    // Measured with --profile, one call is about 121 ns.
    // It was called once per message then, so it was thirty seven percent of the third to second
    // part.
    // From v8 it is called once per poll for each security touched.
    // And a deep poll is nearly always one security bursting (of the polls that took eight or
    // more events, 99.98% involved one security), so the deepest poll's 429 messages now call it
    // once.
    [[nodiscard]] std::uint64_t top3(std::uint16_t sym, std::uint8_t s) const {
        const Security& sec = book_[sym];
        if (!sec.bound) return 0;
        // The buy side wants the highest three prices and the sell side the lowest three. Both
        // count from their own best end, differing only in which way they walk and which stretch
        // of prices they look at first.
        const bool highest = s == kBuy;
        // The price space is in two stretches: one for below a dollar (an entry being a hundredth
        // of a cent) and one for a dollar and up (an entry being a cent). The buy side wants high
        // prices, so it looks at the coarse one first; the sell side wants low prices and looks at
        // the fine one first.
        const Band* first = highest ? &sec.side[s].coarse : &sec.side[s].fine;
        const Band* second = highest ? &sec.side[s].fine : &sec.side[s].coarse;
        std::uint64_t sum = 0;
        // How many levels are still needed. Reaching zero means it can return.
        int need = 3;
        // The first stretch is gathered from, and the second only if that was not enough.
        take(*first, highest, &need, &sum);
        if (need != 0) take(*second, highest, &need, &sum);
        // Fewer than three levels counts as no signal by the usual rule and returns zero.
        return need == 0 ? sum : 0;
    }

private:
    // A security number is 16 bits, so the directory is opened to all sixty five thousand and a
    // lookup is one index.
    static constexpr std::size_t kSecurities = 1u << 16;
    // The mark for "no starting limit". All ones, because it cannot be a valid entry number.
    static constexpr std::uint32_t kNone = 0xffffffffu;
    // The mark for "this price is below the start of this stretch".
    static constexpr std::uint64_t kPastStart = ~0ull;  // the price is below the stretch

    // One continuous stretch of prices.
    // The price entry i stands for is base plus i times step, with i below len.
    struct Band {
        // Which price this stretch starts at, how large an entry is, and how many entries there
        // are.
        std::uint32_t base = 0, step = 1, len = 0;
        // How many shares rest on each entry. They are 32 bits, so two entries take one eight
        // byte word.
        std::uint32_t* qty = nullptr;
        // The three layers below summarise which entries are not empty; they are not an index
        // tree.
        // Layer one: one bit per price.
        std::uint64_t* w0 = nullptr;  // a bit per price
        // Layer two: one bit per whole word of layer one, which is 64 prices.
        std::uint64_t* w1 = nullptr;  // a bit per w0 word
        // Layer three: one bit per whole word of layer two, which is 4096 prices.
        // With those two summaries, a great stretch of empty prices is skipped in two reads
        // rather than being scanned an entry at a time.
        std::uint64_t* w2 = nullptr;  // a bit per w1 word
        // How many words each of the three layers has.
        std::uint32_t n0 = 0, n1 = 0, n2 = 0;
    };
    // The two price stretches of one side.
    struct Side {
        Band fine;    // under a dollar, in hundredths of a cent
        Band coarse;  // a dollar and up, in cents
    };
    // One security: two sides and a flag for whether it got any space.
    struct Security {
        Side side[2];
        bool bound = false;
    };

    // Division rounding up. Used everywhere when working out how many words something takes.
    static std::uint32_t up(std::uint32_t v, std::uint32_t d) { return (v + d - 1) / d; }

public:
    // How many words one side of one security takes.
    // The arithmetic here has to match exactly what carve() spends - one working it out larger
    // and the other smaller either wastes memory or finds it short halfway through binding.
    static std::size_t words_for(std::uint32_t reference) {
        // The same bounds as in bind.
        const std::uint32_t lo = reference / 8;
        const std::uint64_t hi = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(reference) * 8, kTopPrice);
        std::size_t w = 0;
        // Needing a fine stretch adds that stretch's words.
        if (lo < kDollar) {
            w += band_words(
                static_cast<std::uint32_t>(std::min<std::uint64_t>(kDollar, hi + 1)) - lo);
        }
        // Needing a coarse stretch adds that stretch's words.
        if (hi >= kDollar) {
            const std::uint32_t base = (lo < kDollar ? kDollar : lo) / kCent * kCent;
            w += band_words(static_cast<std::uint32_t>((hi - base) / kCent) + 1);
        }
        // The two together are what this side costs.
        return w;
    }

private:
    // How many eight byte words a stretch of len entries takes, with its three bitmaps.
    static std::size_t band_words(std::uint32_t len) {
        // How many words each of the three bitmaps takes: one word of a layer covers 64 units of
        // the layer below.
        const std::uint32_t n0 = up(len, 64), n1 = up(n0, 64), n2 = up(n1, 64);
        // A share count is 32 bits, so two entries take one eight byte word.
        // Which is why this is up(len, 2) rather than len.
        return up(len, 2) + n0 + n1 + n2;  // quantities are half a word each
    }

    // Cuts a stretch of prices out of that large block for one side.
    // Every Band's memory is cut from the same block one after another and next_ moves along - so
    // the whole price space is one continuous piece in memory with not one separate allocation.
    void carve(Band& b, std::uint32_t base, std::uint32_t step, std::uint32_t len) {
        // Which price this stretch starts at.
        b.base = base;
        // How large a price move an entry stands for.
        b.step = step;
        // How many entries there are.
        b.len = len;
        // How many words each of the three bitmaps takes.
        b.n0 = up(len, 64);
        b.n1 = up(b.n0, 64);
        b.n2 = up(b.n1, 64);
        // The share array is cut first. This block is managed in 64 bit words while a share count
        // is 32 bits, so the address is reinterpreted as a 32 bit pointer.
        b.qty = reinterpret_cast<std::uint32_t*>(&block_[next_]);
        // Moved along by the words len entries take - two entries to a word.
        next_ += up(len, 2);
        // The first bitmap layer follows.
        b.w0 = &block_[next_];
        next_ += b.n0;
        // The second.
        b.w1 = &block_[next_];
        next_ += b.n1;
        // The third. With it this side's stretch is complete and next_ stops at the start of the
        // next one.
        b.w2 = &block_[next_];
        next_ += b.n2;
    }

    // Which stretch a price belongs to. A null pointer means it is not in this security's space.
    [[nodiscard]] const Band* band(std::uint16_t sym, std::uint8_t s,
                                   std::uint32_t price) const {
        const Security& sec = book_[sym];
        // This security got no space at all.
        if (!sec.bound) return nullptr;
        // Below a dollar goes to the fine stretch and a dollar and up to the coarse.
        const Band& b = price < kDollar ? sec.side[s].fine : sec.side[s].coarse;
        // This stretch does not exist at all (its length is zero), or the price is below where it
        // starts.
        if (b.len == 0 || price < b.base) return nullptr;
        // A price in the coarse stretch between two whole cents has no entry of its own.
        // The exchange does not allow such a price above a dollar anyway.
        // So one appearing is something that should not be, and it is simply not tracked.
        if ((price - b.base) % b.step != 0) return nullptr;
        // Finally whether the entry number is past this stretch's length. Only within it is it
        // really inside.
        return (price - b.base) / b.step < b.len ? &b : nullptr;
    }
    // The writable version of the one above. The content is identical.
    // So it calls the const one and takes the const off.
    // Rather than writing the same logic twice and later changing only one of them.
    [[nodiscard]] Band* band(std::uint16_t sym, std::uint8_t s, std::uint32_t price) {
        return const_cast<Band*>(
            static_cast<const PriceLevels*>(this)->band(sym, s, price));
    }

    // Lights entry i in the three bitmap layers.
    // All three have to be lit: the upper two are summaries, and one layer unlit would make a
    // lookup skip that whole stretch.
    static void set(Band& b, std::uint32_t i) {
        // Layer one: bit i. i >> 6 is which word and i & 63 is which bit within it.
        b.w0[i >> 6] |= 1ull << (i & 63);
        // Layer two: a bit stands for a word of layer one, so the bit number is i >> 6.
        b.w1[i >> 12] |= 1ull << ((i >> 6) & 63);
        // Layer three: a bit stands for a word of layer two, and the bit number is i >> 12.
        b.w2[i >> 18] |= 1ull << ((i >> 12) & 63);
    }
    // Puts entry i out.
    // It is not symmetric with set: the upper two layers may only be put out once the whole word
    // of the layer below is empty, or other entries still lit in the same word would be hidden
    // along with it.
    static void clear(Band& b, std::uint32_t i) {
        // Layer one's bit is put out first.
        b.w0[i >> 6] &= ~(1ull << (i & 63));
        // Something else in that word is still lit, so the upper two must not be touched and it
        // ends here.
        if (b.w0[i >> 6] != 0) return;
        // Only with the whole word empty is the matching bit of layer two put out.
        b.w1[i >> 12] &= ~(1ull << ((i >> 6) & 63));
        // Something else in that word of layer two is still lit, so it ends here as well.
        if (b.w1[i >> 12] != 0) return;
        // The word of layer two is empty too, so layer three's bit is put out last.
        b.w2[i >> 18] &= ~(1ull << ((i >> 12) & 63));
    }

    // Counts down from the best end within one stretch of prices.
    // It stops once need levels are gathered, adding the quantities into sum.
    //
    // This is the heart of that change.
    // It used to call step_from three times.
    // Every call scanned the outermost of the three bitmap layers from the start.
    // The second walked the path the first had walked, and the third walked it again.
    // Each layer of the three is one random memory access, and walking it twice more is four paid
    // for nothing.
    // Now, having walked into one bitmap word, all three levels are gathered there and then.
    // The m2, m1 and m0 in those three whiles stay in registers throughout.
    // The second and the third level essentially never touch memory again.
    // A micro benchmark measured it as seventy four percent quicker over a price space of two odd
    // gigabytes.
    static void take(const Band& b, bool highest, int* need, std::uint64_t* sum) {
        // This stretch does not exist (for instance a security dear enough to have no fine
        // stretch), so it returns.
        if (b.len == 0) return;
        // The outermost layer: a bit stands for a word of the layer below. The buy side walks
        // from the top bit down and the sell side the other way.
        //
        for (std::uint32_t k2 = 0; k2 < b.n2; ++k2) {
            // The buy side wants the highest prices, so it walks from the last word back; the
            // sell side walks from the first word on.
            const std::uint32_t i2 = highest ? b.n2 - 1 - k2 : k2;
            // Down into it to count. A true means all three levels are gathered and the whole
            // function can end.
            if (walk1(b, highest, b.w2[i2], i2, need, sum)) return;
        }
    }

    // Walks down from one word of the outermost layer: every lit bit in it goes down to a word of
    // w1, every lit bit in that goes down to a word of w0, and finally the prices are counted a
    // bit at a time in w0.
    // Gathering need levels returns true, which stops the layer above at once.
    static bool walk1(const Band& b, bool highest, std::uint64_t m2,
                      std::uint32_t i2, int* need, std::uint64_t* sum) {
        // While this word of the outermost layer still has a lit bit.
        while (m2 != 0) {
            // Take the bit nearest the "good price" end of this word and clear it, so that the
            // next turn takes the one further along.
            // The two built in functions count "how many zeros in front" and "how many zeros
            // behind" and are one instruction each rather than a loop.
            const std::uint32_t j1 = highest ? 63 - __builtin_clzll(m2)
                                             : __builtin_ctzll(m2);
            // What is cleared is the local copy m2 and not the bitmap itself - the bitmap must
            // not be touched.
            m2 &= ~(1ull << j1);
            // "Which word" and "which bit within it" are joined into layer two's word number.
            const std::uint32_t i1 = (i2 << 6) | j1;
            // Read that word of layer two. It is one of the few real memory accesses in this
            // stretch.
            std::uint64_t m1 = b.w1[i1];
            // The same walk again, one layer down.
            while (m1 != 0) {
                const std::uint32_t j0 = highest ? 63 - __builtin_clzll(m1)
                                                 : __builtin_ctzll(m1);
                m1 &= ~(1ull << j0);
                // Layer one's word number.
                const std::uint32_t i0 = (i1 << 6) | j0;
                // Read that word of layer one, where one bit is one price.
                std::uint64_t m0 = b.w0[i0];
                while (m0 != 0) {
                    const std::uint32_t j = highest ? 63 - __builtin_clzll(m0)
                                                    : __builtin_ctzll(m0);
                    m0 &= ~(1ull << j);
                    // How many shares rest at the price that bit stands for, added on.
                    *sum += b.qty[(i0 << 6) | j];
                    // Enough gathered returns all the way, and neither loop above has to walk
                    // further.
                    if (--*need == 0) return true;
                }
            }
        }
        // Every lit bit of this outermost word has been walked without gathering enough, so it
        // goes back for the next word.
        return false;
    }

    // Finds the price nearest the "good" end of this stretch that is not empty, skipping from and
    // everything better than it.
    // It walks the same way take does but returns after one entry - best() uses it.
    static bool pick(const Band& b, bool highest, std::uint32_t from, std::uint32_t* out) {
        // This stretch does not exist.
        if (b.len == 0) return false;
        // The outermost layer, the buy side from high to low and the sell side from low to high.
        for (std::uint32_t k2 = 0; k2 < b.n2; ++k2) {
            const std::uint32_t i2 = highest ? b.n2 - 1 - k2 : k2;
            // Read that word of layer three.
            std::uint64_t m2 = b.w2[i2];
            while (m2 != 0) {
                // Take the bit nearest the good end and clear the local copy.
                const std::uint32_t j1 = highest ? 63 - __builtin_clzll(m2)
                                                 : __builtin_ctzll(m2);
                m2 &= ~(1ull << j1);
                // Down into layer two.
                const std::uint32_t i1 = (i2 << 6) | j1;
                std::uint64_t m1 = b.w1[i1];
                while (m1 != 0) {
                    const std::uint32_t j0 = highest ? 63 - __builtin_clzll(m1)
                                                     : __builtin_ctzll(m1);
                    m1 &= ~(1ull << j0);
                    // Down into layer one.
                    const std::uint32_t i0 = (i1 << 6) | j0;
                    std::uint64_t m0 = b.w0[i0];
                    while (m0 != 0) {
                        const std::uint32_t j = highest ? 63 - __builtin_clzll(m0)
                                                        : __builtin_ctzll(m0);
                        m0 &= ~(1ull << j);
                        // The entry number that bit stands for.
                        const std::uint32_t i = (i0 << 6) | j;
                        // What the caller wants is "the next level worse than from", so from
                        // itself and everything better than it are skipped.
                        if (from != kNone && (highest ? i >= from : i <= from)) continue;
                        // Found, and the entry number is written back.
                        *out = i;
                        return true;
                    }
                }
            }
        }
        // This stretch holds no entry that fits.
        return false;
    }

    // Passing kNone for after asks for the best level.
    // Passing an entry number asks for the level one worse than it.
    bool step_from(std::uint16_t sym, std::uint8_t s, std::uint32_t after,
                   std::uint32_t* price, std::uint32_t* shares) const {
        const Security& sec = book_[sym];
        // A security with no space has no price to ask about.
        if (!sec.bound) return false;
        // The buy side wants a high price and the sell side a low one.
        const bool highest = s == kBuy;
        // The buy side looks at the coarse stretch first (a dollar and up is certainly better
        // than below a dollar), and the sell side the other way, at the fine one first.
        const Band* first = highest ? &sec.side[s].coarse : &sec.side[s].fine;
        const Band* second = highest ? &sec.side[s].fine : &sec.side[s].coarse;
        // The two stretches are looked at in that order.
        for (const Band* b : {first, second}) {
            // A stretch that does not exist goes on to the next.
            if (b->len == 0) continue;
            // No starting limit by default.
            std::uint32_t from = kNone;
            // The caller asked for "one worse than this level", so it has to be turned into this
            // stretch's coordinates first.
            if (after != kNone) {
                // Where the previous level sits relative to this stretch, in three cases.
                // Inside this stretch, the scan starts there.
                // This whole stretch has already been walked, so it is skipped.
                // This whole stretch has not been reached yet, so all of it can be used.
                const std::uint64_t at =
                    after >= b->base
                        ? (static_cast<std::uint64_t>(after) - b->base) / b->step
                        : kPastStart;
                // Inside this stretch, so the search goes down from there.
                if (at < b->len) {
                    from = static_cast<std::uint32_t>(at);
                // Otherwise "already walked this stretch" has to be told from "not reached it
                // yet".
                // The buy side walks towards low prices, and a previous level below this
                // stretch's start means this stretch is finished;
                // the sell side walks towards high prices, and a previous level not below this
                // stretch means it is finished.
                } else if (highest ? after < b->base : at != kPastStart) {
                    continue;
                }
            }
            // To catch the entry number found.
            std::uint32_t i = 0;
            // No suitable entry in this stretch goes on to the next.
            if (!pick(*b, highest, from, &i)) continue;
            // The entry number is turned back into a price.
            *price = b->base + i * b->step;
            // How many shares rest at that level.
            *shares = b->qty[i];
            // Found.
            return true;
        }
        // Neither stretch had one - this side is empty now, and the caller sends nothing.
        return false;
    }

    // The whole price space. Every Band of every security is cut out of it one after another.
    huge::Buffer<std::uint64_t> block_;
    // The directory of every security, indexed by the security number directly.
    huge::Buffer<Security> book_;
    // How far this block has been cut. Every carve moves it along.
    std::size_t next_ = 0;
};

}  // namespace book
