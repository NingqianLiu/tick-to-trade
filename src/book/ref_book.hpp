#pragma once

// The reference order book.
//
// This file is deliberately slow: an unordered_map keyed by order id, no pool, no thought
// given to the cache. It only ever runs offline, where being obviously correct matters
// far more than being quick.
//
// It is not another implementation that might be swapped in one day - it is the answer.
// The measured consumer keeps its own book, both are fed the same day, and at the end
// each produces a hash. Comparing the two says whether the fast one got it right. It
// never touches the hot path.
//
// It also answers two numbers that have to be known before a run: how many orders rest
// across the market at once, which is what the order table is sized from, and how they
// are spread across securities, which is what a shard is sized from. Both had been
// guessed wrong: the peak turned out to be twenty two times the original assumption, and
// one security alone held 36.5% of everything resting. That is why this class exists.

// std::sort, used before hashing - the orders have to be put in a fixed order first, for
// the reason given in hash() below - and std::max, for tracking the peak.
#include <algorithm>
// Fixed width integers. The widths are not arbitrary: hashing lays these fields out as
// bytes, so changing a width would change the hash and the two books would stop agreeing.
#include <cstdint>
// std::string, for the sixty four hex characters hash() returns.
#include <string>
// std::unordered_map - the deliberately slow part mentioned above. Every insert may go
// and ask for memory, which would be intolerable on the hot path and costs nothing here.
#include <unordered_map>
// std::vector, for flattening the orders before sorting, and for the row of counts per
// security.
#include <vector>

// crypto::Sha256, a small implementation kept in this repository rather than a dependency
// on OpenSSL, used to squeeze a whole book into one string that can be compared directly
// with the one the measured consumer produces.
#include "common/sha256.hpp"
// itch::read_be, since ITCH is big endian on the wire and x86 is not.
#include "itch/reader.hpp"
// The Message view and the field offsets, one of which every read_be below needs.
#include "itch/types.hpp"

namespace book {

// Every resting order in the market, one entry each.
//
// The only call made repeatedly is apply(); the rest are for asking questions once the
// run is over.
class RefBook {
public:
    // What an order needs to remember. Two things more than the measured version keeps -
    // the security, and the side as the character that came off the wire. Nothing here is
    // packed to save memory; storing things as they arrived leaves less to get wrong.
    struct Order {
        // Shares left. An execution or a partial cancel makes it smaller.
        std::uint32_t shares;
        // The price. There is no decimal point on the wire: it is an integer with four
        // implied decimals, so 1234500 is $123.45. Integers compare exactly and carry no
        // floating point error.
        std::uint32_t price;
        // Which security. ITCH uses a two byte number in place of the ticker, fixed for
        // the day.
        std::uint16_t locate;
        // Buy or sell, kept as the character from the wire rather than converted to 0 and
        // 1 as the measured version does - one conversion fewer to get wrong.
        char side;
    };

    // How many of each message went by. The first five are ordinary volume; the last three
    // should not happen and are worth stopping for.
    struct Counters {
        std::uint64_t added = 0;
        std::uint64_t executed = 0;
        std::uint64_t cancelled = 0;
        std::uint64_t deleted = 0;
        std::uint64_t replaced = 0;
        // A message about an order id we do not hold.
        //
        // A non zero here is not automatically a bug: an order placed on an earlier day
        // and cancelled today has no add message in this file, so replaying a whole day
        // always produces some. What matters is it being much larger than usual, which
        // does mean lost messages or a parsing error.
        std::uint64_t orphan_ref = 0;
        // An execution for more shares than the order had left.
        std::uint64_t oversized_exec = 0;
        // The same order id added twice.
        std::uint64_t duplicate_ref = 0;
    };

    // The default of 2^21 = 2,097,152 is not "enough", it just saves the first few
    // rehashes: the measured peak across the market is 6.68 million, more than three times
    // this. A map that fills up reallocates and moves every order, which takes seconds at
    // these sizes. Nothing here is in a hurry, so there is no need to reserve the peak.
    //
    // That 6.68 million figure came from this class - see peak_live() below.
    explicit RefBook(std::size_t reserve = 1u << 21) { orders_.reserve(reserve); }

    // Takes one message and updates the book.
    //
    // Seven message types change a book (A F E C X D U) but there are only five pieces of
    // code: A and F share one, E and C share another. Everything else - system events, the
    // stock directory and so on - is ignored. Nothing is returned; the result stays in the
    // book and is asked for afterwards.
    void apply(const itch::Message& m) {
        switch (m.type()) {
            case 'A':
            // F is an add as well, with a market participant id the book does not care
            // about, so it falls through to the same code.
            case 'F':
                add(m);
                break;
            case 'E':
            // Traded at a different price. It also takes shares off the order, and the
            // fields are in the same places, so it falls through too.
            case 'C':
                ++c_.executed;
                reduce(itch::read_be<std::uint64_t>(m.body + itch::kExecRefOff),
                       itch::read_be<std::uint32_t>(m.body + itch::kExecSharesOff));
                break;
            // Some shares withdrawn. The same subtraction, but the fields sit at different
            // offsets, which is why it cannot share the code above - the wrong offset
            // would read a meaningless number.
            case 'X':
                ++c_.cancelled;
                reduce(itch::read_be<std::uint64_t>(m.body + itch::kCancelRefOff),
                       itch::read_be<std::uint32_t>(m.body + itch::kCancelSharesOff));
                break;
            // The whole order withdrawn. No helper is needed: erase by key says how many
            // it removed, and zero means the id was not there.
            case 'D':
                ++c_.deleted;
                if (orders_.erase(itch::read_be<std::uint64_t>(
                        m.body + itch::kDeleteRefOff)) == 0) {
                    ++c_.orphan_ref;
                }
                break;
            // A replace is involved enough to have its own function.
            case 'U':
                replace(m);
                break;
            default:
                break;
        }
    }

    // How many orders rest across the market right now.
    [[nodiscard]] std::size_t live() const noexcept { return orders_.size(); }

    // The same count, per security.
    //
    // live() covers the whole market while a shard only holds its own securities, so a
    // shard's order table is sized from the largest entries in this row.
    //
    // Dividing the market peak by the number of shards does not work, and this row is what
    // showed it: one security, SPCX, held 2.24 million orders by itself, 36.5% of the 6.68
    // million peak. Sizing has to follow the largest one, not the average.
    [[nodiscard]] std::vector<std::uint32_t> live_by_locate() const {
        // A security number is sixteen bits, so the row simply has an entry for each. Sixty
        // five thousand four byte counts is 256 KB, not worth a map to avoid.
        std::vector<std::uint32_t> v(1u << 16, 0);
        for (const auto& [ref, o] : orders_) ++v[o.locate];
        return v;
    }

    // The most orders resting at any point in the day, which is what the order table is
    // sized from: too small and it fills up exactly when the market is busiest. Measured
    // at 6.68 million across the market - while the trader, which follows 101 names, peaks
    // at 1.525 million, so sizing it from the market figure would make its table eight
    // times too large and push everything else out of the cache.
    [[nodiscard]] std::size_t peak_live() const noexcept { return peak_live_; }
    [[nodiscard]] const Counters& counters() const noexcept { return c_; }

    // Squeezes the whole book into one hex string.
    //
    // Walking the map and feeding it straight into the hash does not work: the iteration
    // order of an unordered_map is arbitrary and changes with the insertion order, so the
    // same book would hash differently twice, let alone against another implementation.
    //
    // So every order is sorted by security, side, price and id first, and fed in that
    // order.
    //
    // The record includes the shares still resting, and that part matters: without it a
    // dropped execution would be invisible, since the order is still there and its key has
    // not changed - only the count is wrong. Catching exactly that is why this hash exists.
    [[nodiscard]] std::string hash() const {
        // One flattened order. The order of the first four fields is the sort priority,
        // and the comparison below follows it.
        struct Row {
            std::uint16_t locate;
            char side;
            std::uint32_t price;
            std::uint64_t ref;
            // Not sorted on, but hashed - the field described above.
            std::uint32_t shares;
        };
        std::vector<Row> rows;
        // Exactly as many as there are orders. Without this the vector would double its
        // way there, moving millions of rows a dozen times.
        rows.reserve(orders_.size());
        for (const auto& [ref, o] : orders_) {
            // Filled by position, so the order has to match the declaration exactly. Get it
            // wrong and nothing complains; the hash simply fails to match, which is the
            // hardest kind of mistake to find.
            rows.push_back({o.locate, o.side, o.price, ref, o.shares});
        }
        // Four keys in turn. The last one is the order id, which is unique, so the result
        // is fully determined and does not depend on std::sort being stable.
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.locate != b.locate) return a.locate < b.locate;
            // 'B' sorts before 'S', so buys come first.
            if (a.side != b.side) return a.side < b.side;
            if (a.price != b.price) return a.price < b.price;
            return a.ref < b.ref;
        });

        crypto::Sha256 sha;
        // Nineteen bytes per row: 2 + 1 + 4 + 8 + 4.
        //
        // Laid out by hand rather than hashing the struct itself, because a struct carries
        // padding the compiler chooses, and padding holds whatever it holds - which would
        // make the same data hash differently on another compiler or another machine.
        std::uint8_t rec[19];
        for (const Row& r : rows) {
            put_be(rec + 0, r.locate, 2);
            rec[2] = static_cast<std::uint8_t>(r.side);
            put_be(rec + 3, r.price, 4);
            put_be(rec + 7, r.ref, 8);
            put_be(rec + 15, r.shares, 4);
            sha.update(rec, sizeof(rec));
        }
        // Sixty four hex characters. The measured consumer has to produce the same string
        // for its book to be considered correct.
        return sha.hex();
    }

private:
    // Writes an integer as bytes, most significant first.
    //
    // Not a memory copy: x86 is little endian, so copying would make the hash depend on
    // the machine it ran on. Laying the bytes out by hand keeps it independent of that.
    static void put_be(std::uint8_t* p, std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            // The narrowing to eight bits keeps the low byte, so no mask is needed.
            p[i] = static_cast<std::uint8_t>(v >> (8 * (bytes - 1 - i)));
        }
    }

    // A new order; both A and F arrive here.
    void add(const itch::Message& m) {
        const std::uint64_t ref =
            itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff);
        // Filled in declaration order: shares, price, security, side. The first two are
        // multi byte integers and go through read_be; the security number lives in the
        // header rather than the body and has its own accessor; the side is a single byte,
        // so byte order does not apply.
        const Order o{itch::read_be<std::uint32_t>(m.body + itch::kAddSharesOff),
                      itch::read_be<std::uint32_t>(m.body + itch::kAddPriceOff),
                      m.stock_locate(),
                      static_cast<char>(m.body[itch::kAddSideOff])};
        // emplace does not overwrite an existing key, it only reports that it did not
        // insert, so the second value has to be caught to notice a repeated id.
        const auto [it, inserted] = orders_.emplace(ref, o);
        if (!inserted) {
            ++c_.duplicate_ref;
            // Overwritten by hand - not because overwriting is more sensible, but because
            // it is what the measured implementation does, and the two have to behave
            // identically or the hashes would differ over a corner case.
            it->second = o;
        }
        // Outside the if: a repeated id was still an add message.
        ++c_.added;
        // Only an insert can push the count up, so the peak is tracked here and in
        // replace; taking shares off or deleting can only make it smaller.
        peak_live_ = std::max(peak_live_, orders_.size());
    }

    // Takes shares off an order. Executions and partial cancels share this.
    void reduce(std::uint64_t ref, std::uint32_t shares) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) {
            // Not a bug in itself: an order placed on an earlier day has no add message
            // here. A sudden rise in this count is the thing to watch.
            ++c_.orphan_ref;
            return;
        }
        // Taking at least everything left ends the order.
        if (shares >= it->second.shares) {
            // Strictly more is counted separately, to keep "exactly used up", which is
            // normal, apart from "more than there was", which means a missed message.
            if (shares > it->second.shares) ++c_.oversized_exec;
            // Erased by iterator rather than by key, since the lookup has already happened
            // and passing the key would hash it a second time.
            orders_.erase(it);
        } else {
            it->second.shares -= shares;
        }
    }

    // Replace: the old order goes and a new one takes its place.
    //
    // It is not an edit of the old order. The old id is finished, the same message carries
    // a brand new one, and every later message about this order uses the new id. So this
    // is a removal followed by an insertion.
    void replace(const itch::Message& m) {
        ++c_.replaced;
        // The message carries two ids; this is the old one.
        const std::uint64_t old_ref =
            itch::read_be<std::uint64_t>(m.body + itch::kReplaceOldRefOff);
        const auto it = orders_.find(old_ref);
        if (it == orders_.end()) {
            // The new order has to be given up with it. A replace carries neither side nor
            // security - both are inherited - so without the old order there is no way to
            // know where the new one belongs.
            ++c_.orphan_ref;
            return;
        }
        // Copied whole first, which is how the side and the security are inherited.
        Order o = it->second;
        // Copy before erase, not after: erasing first would invalidate the iterator and
        // the copy would come from freed memory.
        orders_.erase(it);
        o.shares = itch::read_be<std::uint32_t>(m.body + itch::kReplaceSharesOff);
        o.price = itch::read_be<std::uint32_t>(m.body + itch::kReplacePriceOff);
        const std::uint64_t new_ref =
            itch::read_be<std::uint64_t>(m.body + itch::kReplaceNewRefOff);
        // A new id should be new, so failing to insert means a collision and is counted.
        //
        // Note this differs from add above, which overwrites on a collision, while the
        // measured implementation overwrites in both places. If a collision ever really
        // happened here the two books would diverge and the hashes would not match; what
        // makes that safe is duplicate_ref staying at zero.
        if (!orders_.emplace(new_ref, o).second) ++c_.duplicate_ref;
        peak_live_ = std::max(peak_live_, orders_.size());
    }

    // Every resting order, keyed by id: the deliberately slow part.
    std::unordered_map<std::uint64_t, Order> orders_;
    Counters c_;
    std::size_t peak_live_ = 0;
};

}  // namespace book
