// This header only has to be included once.
#pragma once

// The reference order book.
//
// This file is deliberately slow.
// One unordered_map indexed by order id, with no memory pool and no thought for the cache.
// It only runs offline, where "plainly right at a glance" matters far more than being quick.
//
// It looks like another implementation of the order book that might be swapped in later. It is
// not - it is the answer. The consumer being tested has a book of its own, both run the same
// market data, and each works out a hash at the end; comparing them says whether the quick
// implementation got anything wrong. It never reaches the hot path.
//
// It also answers two numbers that have to be known before a run:
// how many orders are resting across the whole market at once - which decides how large the order
// table is;
// and how those orders are spread across the securities - which decides how large one shard is.
// Both of those were estimated wrongly at first: the peak of resting orders is twenty two times
// what was assumed, and one security alone took 36.5% of the whole market's resting orders. Which
// is why this class exists.

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// common/sha256.hpp provides crypto::Sha256, a small implementation carried in this repository
// that depends on no external library such as OpenSSL. It is used here to squeeze the whole book
// into a string of hexadecimal, to compare directly against the string the side being tested
// works out.
#include "common/sha256.hpp"
// itch/reader.hpp provides itch::read_be, which takes an integer out of a message body most
// significant byte first.
// ITCH is in network byte order while x86 is least significant first, so every number taken has
// to go through it.
#include "itch/reader.hpp"
// itch/types.hpp provides itch::Message (a message's type and where its body is), and the offset
// constants of the fields within a body (kAddRefOff and the like).
// Every read_be below uses one of those offsets.
#include "itch/types.hpp"

// Everything to do with the order book goes in the book namespace, so that an ordinary name like
// Order does not collide with one elsewhere.
namespace book {

// Every resting order in the whole market, one entry each.
//
// It looks like another implementation of the order book that might be swapped in later. It is
// not - it is the correct answer and never reaches the hot path. The consumer being tested works
// out a hash at the end of a run, this side works out another, and comparing them says whether
// the quick implementation got anything wrong.
//
// The only method really called again and again from outside is apply: it takes a message and
// updates the book.
// The rest are for asking about the result once a run is finished.
class RefBook {
// What follows is for outside use. The only one called again and again is apply.
public:
    // What one order records.
    // Two things more than the side being tested: the security number, and the side kept as the
    // original character.
    // Saving memory is not the point here; keeping things as they came is the least likely to go
    // wrong.
    struct Order {
        // How many shares of this order are left. An execution and a partial cancel both lower
        // it.
        std::uint32_t shares;
        // What price it rests at.
        // There is no decimal point here: what arrives on the wire is an integer, with the last
        // four digits agreed to be decimals.
        // So 1234500 reads as 123.45 dollars. Only an integer can be compared directly, and it
        // has no floating point error.
        std::uint32_t price;
        // Which security. ITCH uses a two byte number in place of a ticker, fixed for the whole
        // day.
        std::uint16_t locate;
        // Buy or sell. The character from the wire is kept directly ('B' or 'S'), rather than
        // being turned into 0 and 1 as the side being tested does - one conversion fewer is one
        // place fewer for something to go wrong.
        char side;
    // The end of one order.
    };

    // How many messages of each kind there were over a whole round.
    // The first five are ordinary volume, to see the scale. The last three should never happen,
    // and a non zero means stopping to look.
    struct Counters {
        // How many new orders.
        std::uint64_t added = 0;
        // How many executions.
        std::uint64_t executed = 0;
        // How many partial cancels.
        std::uint64_t cancelled = 0;
        // How many whole cancels.
        std::uint64_t deleted = 0;
        // How many replaces.
        std::uint64_t replaced = 0;
        // A message mentioned an order id we do not hold.
        // A non zero must not be taken as a bug at once: an order placed days ago and cancelled
        // only today has an add message we never saw, so running a whole day's original file
        // leaves this number non zero naturally.
        // What is really worth watching is it being much larger than usual - that means messages
        // were lost or parsed wrongly.
        std::uint64_t orphan_ref = 0;
        // An execution for more shares than that order has left. Something else that should never
        // happen.
        std::uint64_t oversized_exec = 0;
        // The same order id was placed twice.
        std::uint64_t duplicate_ref = 0;
    // The end of the counters.
    };

    // Construction. It asks for 2^21 = 2,097,152 entries by default.
    //
    // That number is not "enough" but "fewer moves": the measured peak of resting orders across
    // the whole market is 6.68 million, more than three times it. A full map has to allocate
    // afresh and move every order across, and moving two million takes seconds.
    // Reserving only saves the first few of those moves. There is no hurry here, so there is no
    // need to open it to the peak.
    // Incidentally, that 6.68 million is itself a number this class measured - see peak_live()
    // below.
    explicit RefBook(std::size_t reserve = 1u << 21) { orders_.reserve(reserve); }

    // This is the body of the class: take a message and update this book.
    //
    // It takes one ITCH message already cut out and takes a path by its first character.
    // Seven kinds of message change the book (A F E C X D U) but there are only five pieces of
    // code - A and F share one, and E and C share another. Every other type, such as a system
    // event or the stock directory, is left alone.
    // It returns having handled it and outputs nothing - the result stays in the book, and the
    // methods below ask about it once a run is finished.
    void apply(const itch::Message& m) {
        // The message type is the first character of the body. The path is taken by it.
        switch (m.type()) {
            // A new order.
            case 'A':
            // 'F' is a new order as well, with only a broker number more than 'A', which makes no
            // difference to the book.
            // There is no break between the two cases, so 'F' falls through to the same code.
            case 'F':
                // Handed to add below.
                add(m);
                // The new order is handled, so out of the switch.
                break;
            // An execution.
            case 'E':
            // An execution at another price. Like 'E' it takes shares off that order, and its
            // fields are in the same places, so there is no break here either and it falls
            // through to the same piece.
            case 'C':
                // One execution recorded.
                ++c_.executed;
                // The order id and the shares executed are taken out of the message and handed to
                // reduce.
                // Both arguments have to go through read_be, because the wire is most significant
                // first.
                reduce(itch::read_be<std::uint64_t>(m.body + itch::kExecRefOff),
                       itch::read_be<std::uint32_t>(m.body + itch::kExecSharesOff));
                // The execution is handled.
                break;
            // Some shares cancelled.
            case 'X':
                // One partial cancel recorded.
                ++c_.cancelled;
                // It also takes shares off, but the field offsets differ from the two execution
                // kinds, so it cannot be joined with the piece above - a wrong offset would read
                // a stray number.
                reduce(itch::read_be<std::uint64_t>(m.body + itch::kCancelRefOff),
                       itch::read_be<std::uint32_t>(m.body + itch::kCancelSharesOff));
                // The partial cancel is handled.
                break;
            // The whole order cancelled.
            case 'D':
                // One whole cancel recorded.
                ++c_.deleted;
                // No function of its own is needed here: the map's erase takes a key and returns
                // how many were removed.
                // A zero means that order id is not held at all.
                if (orders_.erase(itch::read_be<std::uint64_t>(
                        m.body + itch::kDeleteRefOff)) == 0) {
                    // A cancel of an order id never seen, recorded.
                    // Running a whole day's original file this is not zero - an order placed days
                    // ago and cancelled today has an add message we never saw. What is worth
                    // watching is it growing suddenly.
                    ++c_.orphan_ref;
                }
                // The whole cancel is handled.
                break;
            // A replace: cancel the old one and place a new one, under a completely new order id.
            case 'U':
                // The case is involved enough to hand to replace below.
                replace(m);
                // The replace is handled.
                break;
            // No other type touches the book.
            default:
                // Nothing at all, straight out.
                break;
        }
    }

    // How many orders are resting across the whole market now, which is how many keys the map
    // has.
    [[nodiscard]] std::size_t live() const noexcept { return orders_.size(); }

    // Counted per security, how many orders are resting for each.
    //
    // Why it is needed: live() counts the whole market, while one shard holds only its own
    // securities.
    // How large a shard's order table has to be is decided by the largest few numbers in this
    // row.
    //
    // It looks as though dividing the whole market's peak by the number of shards would do. It
    // does not - this row is exactly what measured it: a security called SPCX alone took 2.24
    // million orders, 36.5% of the whole market's peak of 6.68 million.
    // Dividing evenly is a fiction, and sizing has to look at the largest one.
    [[nodiscard]] std::vector<std::uint32_t> live_by_locate() const {
        // A security number is sixteen bits, so sixty five thousand entries are simply opened and
        // the number used directly as an index.
        // Sixty five thousand four byte integers are 256 KB, not worth a map to save.
        std::vector<std::uint32_t> v(1u << 16, 0);
        // Walk every resting order and record one in its own entry.
        // The ref here is not used; a structured binding simply has to name both.
        for (const auto& [ref, o] : orders_) ++v[o.locate];
        // The row goes to the caller, which picks out the largest few itself.
        return v;
    }

    // The largest number of resting orders seen in a whole day.
    // How large the order table is is decided by it - too small for the peak and the table fills
    // exactly when things are busiest.
    // The measured figure is 6.68 million for the whole market. The trader only subscribes to 101
    // names, where the measured figure is 1.525 million, so sizing the trader by the whole
    // market's number would make it eight times too large and push the cache out for nothing.
    [[nodiscard]] std::size_t peak_live() const noexcept { return peak_live_; }
    // The counters above, printed at the end to see whether anything that should never happen
    // did.
    [[nodiscard]] const Counters& counters() const noexcept { return c_; }

    // Squeezes the whole book into a string of hexadecimal.
    //
    // It looks as though walking the map from the start and feeding it all into the hash would
    // do. It would not: the order an unordered_map is walked in is arbitrary, and the same data
    // inserted in a different order walks in a different order, so the same book works out two
    // different hashes, let alone comparing against another implementation.
    //
    // So the way it is done is: sort every order by (security, side, price, order id) first, and
    // then feed them in that order.
    //
    // The records fed in carry "how many shares are left", and that matters.
    // Without it, an execution that was lost cannot be seen: that order is still there, its key
    // has not changed, and only the shares are wrong.
    // And catching exactly that kind of mistake is what this hash exists for.
    [[nodiscard]] std::string hash() const {
        // One row for sorting. The order of the first four fields is the priority of the sort,
        // and the comparison below is written to match.
        struct Row {
            // Which security. The first priority.
            std::uint16_t locate;
            // Buy or sell. The second.
            char side;
            // The price. The third.
            std::uint32_t price;
            // The order id. The fourth and last.
            std::uint64_t ref;
            // How many shares are left. It takes no part in the sort but is fed into the hash -
            // it is the field that matters, named above.
            std::uint32_t shares;
        // The end of one row.
        };
        // Every order spread into a row, ready to sort.
        std::vector<Row> rows;
        // Reserving here really is enough: the argument is how many orders there are now, not one
        // entry more or fewer.
        // Without it the vector would push and double as it went, moving several million rows a
        // dozen times over.
        rows.reserve(orders_.size());
        // Every order in the map copied into a row. ref is the key, the order id, and o the
        // value.
        for (const auto& [ref, o] : orders_) {
            // What is inside the braces is filled by position, so the order has to match Row's
            // declaration exactly.
            // Getting it wrong reports nothing and only works out a hash that does not agree,
            // which is the hardest kind of mistake to find.
            rows.push_back({o.locate, o.side, o.price, ref, o.shares});
        }
        // Compared by the four keys in turn.
        // The last is the order id, which is unique, so the order after the sort is entirely
        // settled - it cannot come out differently on two runs because std::sort is not stable.
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            // Different securities go by the smaller security number first.
            if (a.locate != b.locate) return a.locate < b.locate;
            // Within one security, the sides are kept apart ('B' has a lower character code than
            // 'S', so buys come first).
            if (a.side != b.side) return a.side < b.side;
            // Within one security and one side, by price from low to high.
            if (a.price != b.price) return a.price < b.price;
            // With the first three the same, the order id settles a unique order.
            return a.ref < b.ref;
        // This line closes two things at once: the brace closes the comparison and the
        // parenthesis closes this call to sort.
        // By here rows is in an entirely settled order, and only then may the hash be worked out
        // from it.
        });

        // The hasher. Freshly built it is in its starting state, and rows are fed in one at a
        // time below; it gathers sixty four bytes and works out a block itself (see
        // common/sha256.hpp).
        crypto::Sha256 sha;
        // One row spread into nineteen bytes: 2 + 1 + 4 + 8 + 4.
        // Why it is spread by hand rather than feeding a whole Row in: a structure holds padding
        // bytes the compiler inserted, whose content is not settled, and that would make the same
        // data work out different hashes on different compilers and different machines.
        std::uint8_t rec[19];
        // Every row of the sorted list, flattened and fed in turn.
        for (const Row& r : rows) {
            // The security number, the first two bytes.
            put_be(rec + 0, r.locate, 2);
            // The side, one byte, the character from the wire itself.
            rec[2] = static_cast<std::uint8_t>(r.side);
            // The price, four bytes, from byte 3.
            put_be(rec + 3, r.price, 4);
            // The order id, eight bytes, from byte 7.
            put_be(rec + 7, r.ref, 8);
            // How many shares are left, four bytes, from byte 15. The field that matters, named
            // above.
            put_be(rec + 15, r.shares, 4);
            // The nineteen bytes are fed to the hasher. One row at a time.
            sha.update(rec, sizeof(rec));
        }
        // Finish and give sixty four hexadecimal characters.
        // Only where the side being tested works out the same string is its book right.
        return sha.hex();
    }

// Everything below is the internals. The outside does not need to know what the book is held in.
private:
    // Writes an integer into a number of bytes, most significant first.
    // Why not simply copy the memory: x86 is least significant first, and a plain copy would work
    // out a different hash on a machine of the opposite byte order.
    // Laying it out by hand, most significant first, makes the hash independent of the machine.
    static void put_be(std::uint8_t* p, std::uint64_t v, int bytes) {
        // Starting from the most significant byte and working along.
        for (int i = 0; i < bytes; ++i) {
            // How far to shift: byte 0 shifts most and the last byte shifts by 0.
            // Narrowing to eight bits keeps only the low eight by itself, so no and with 0xff is
            // needed.
            p[i] = static_cast<std::uint8_t>(v >> (8 * (bytes - 1 - i)));
        }
    }

    // Places a new order. Both 'A' and 'F' in apply come here.
    void add(const itch::Message& m) {
        // The order id. Every later message about this order - an execution, a cancel, a replace
        // - uses it.
        const std::uint64_t ref =
            itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff);
        // The order's four things, filled in the order Order declares them: shares, price, which
        // security, buy or sell.
        // The first two are multi byte integers and have to go through read_be for the byte
        // order;
        // the security number is not in the body but in the header, where a method asks for it;
        // the side is one byte with no byte order to worry about and is taken directly.
        const Order o{itch::read_be<std::uint32_t>(m.body + itch::kAddSharesOff),
                      itch::read_be<std::uint32_t>(m.body + itch::kAddPriceOff),
                      m.stock_locate(),
                      static_cast<char>(m.body[itch::kAddSideOff])};
        // emplace does not overwrite a key that is already there and only says it did not insert.
        // So the second return value has to be caught to judge a collision.
        const auto [it, inserted] = orders_.emplace(ref, o);
        // Not inserted means this order id is in the table already.
        if (!inserted) {
            // A duplicate order id recorded.
            ++c_.duplicate_ref;
            // And then overwritten by hand. Not because overwriting is more sensible, but because
            // the side being tested does exactly that - the two have to behave alike, or the
            // hashes would fail to agree over a corner case like this.
            it->second = o;
        }
        // One new order recorded. It is outside the if - a collision is still an add message that
        // arrived.
        ++c_.added;
        // The peak is updated on the way. Doing it here and in replace is enough, because only an
        // insert into the table can push the number of resting orders up; taking shares off and
        // cancelling only make it smaller.
        peak_live_ = std::max(peak_live_, orders_.size());
    }

    // Takes some shares off an order. Executions ('E'/'C') and partial cancels ('X') share this
    // piece.
    void reduce(std::uint64_t ref, std::uint32_t shares) {
        // Found by order id first.
        const auto it = orders_.find(ref);
        // No such order id.
        if (it == orders_.end()) {
            // A message mentioning an order id we do not know, recorded.
            // It is not a bug: an order placed days ago and executed or cancelled today has an
            // add message we never saw, so it is naturally not in the table. What is worth
            // watching is this number growing suddenly.
            ++c_.orphan_ref;
            // An early end. There is nothing to do for this message and the book stays as it was.
            return;
        }
        // The shares to take off are not below what is there, so the whole order goes.
        if (shares >= it->second.shares) {
            // Strictly more is recorded separately, to tell it from taking off exactly what is
            // left - the latter is normal while taking off too much means messages were missed
            // earlier.
            if (shares > it->second.shares) ++c_.oversized_exec;
            // Removed from the table. What is passed is the iterator rather than the key - it has
            // been found once already and passing the key would make the map hash it again.
            orders_.erase(it);
        // Fewer to take off than are left takes this path.
        } else {
            // The order stays and only its shares go down.
            it->second.shares -= shares;
        }
    }

    // A replace: cancel the old order and place a new one.
    //
    // It looks as though a replace changes a price and a share count in place. It does not - the
    // old order id is void from then on, the exchange gives a completely new order id in the same
    // message, and every later message about that order uses the new one. So this is one removal
    // and one addition.
    void replace(const itch::Message& m) {
        // One replace recorded.
        ++c_.replaced;
        // The message carries two order ids, and this is the old one.
        const std::uint64_t old_ref =
            itch::read_be<std::uint64_t>(m.body + itch::kReplaceOldRefOff);
        // That order found by the old order id.
        const auto it = orders_.find(old_ref);
        // The old order is not held.
        if (it == orders_.end()) {
            // Then the new order has to be given up as well rather than forced in: a replace
            // message carries neither a side nor a security number, and both are inherited from
            // the old order.
            // Without the old order there is no telling which side of which security the new one
            // belongs to.
            ++c_.orphan_ref;
            // An early end, with the book left as it was.
            return;
        }
        // The old order is copied whole first, which inherits the side and the security number.
        Order o = it->second;
        // Removed after copying. The order matters - removed first, it would be invalid and what
        // was copied would be dead memory.
        orders_.erase(it);
        // The shares become the new value from the message.
        o.shares = itch::read_be<std::uint32_t>(m.body + itch::kReplaceSharesOff);
        // The price becomes the new value from the message as well.
        o.price = itch::read_be<std::uint32_t>(m.body + itch::kReplacePriceOff);
        // The new order id. Every later message about this order uses it.
        const std::uint64_t new_ref =
            itch::read_be<std::uint64_t>(m.body + itch::kReplaceNewRefOff);
        // Placed. The second value emplace returns is whether it was inserted, and since a new
        // order id ought to be entirely new, a failure means a collision and is recorded.
        // Note that this differs from add above: add overwrites by hand on a collision and this
        // does not.
        // And the side being tested uses an insert that overwrites on a collision - which is to
        // say that one real collision would leave the two books different and the hashes
        // disagreeing. What this rests on is duplicate_ref staying at zero throughout.
        if (!orders_.emplace(new_ref, o).second) ++c_.duplicate_ref;
        // The peak is updated here as well - an insert happened here too.
        peak_live_ = std::max(peak_live_, orders_.size());
    }

    // Every resting order, keyed by order id. The "deliberately slow" part named at the top of
    // this file.
    std::unordered_map<std::uint64_t, Order> orders_;
    // The counts of each kind of message. counters() reads this.
    Counters c_;
    // The largest number of resting orders seen in a whole day, which peak_live() reads.
    std::size_t peak_live_ = 0;
// The end of the class.
};

}  // namespace book
