#pragma once

// The order book the measured consumer keeps.
//
// "Order book" sounds like it should be one table: a price against the shares resting on
// it. There are two here:
//   one holds every resting order, indexed by the order id the exchange gave it
//   one holds how many shares rest on each price
//
// The second is what the strategy reads. So what is the first for? When the exchange
// cancels an order it sends the order id and nothing else - not how big the order was,
// not what price it rested at. Those have to have been remembered, or there is no way to
// know what to take off which price when the cancel arrives.
//
// Once it is built, nothing here allocates and nothing throws.
//
// A message about an order id we do not hold is counted and dropped. On a feed with no
// gaps that cannot happen, so the counter is not there to tolerate the case - it is an
// alarm. Anything other than zero means a lost packet or a bug, and it is one of the
// three numbers that decide whether a run can be used at all.

// Fixed width integers: security numbers, shares, prices and order ids all have a size
// the protocol fixes.
#include <cstdint>
// std::vector, for the table of reference prices the constructor takes.
#include <vector>

// Three order table implementations with the same interface. The using line below picks
// one. PoolMap is the one in use; the other two are kept as a record of what measured
// worse.
#include "book/inline_map.hpp"
#include "book/order_map.hpp"
#include "book/pool_map.hpp"
// PriceLevels answers how many shares rest on a given price of a given side of a given
// security, and what the best price and the best three are. This is the layer the
// strategy actually reads; those two questions are now asked once per poll rather than
// once per message.
#include "book/price_levels.hpp"
// read_be pulls a big endian integer out of a message body - every field below goes
// through it.
#include "itch/reader.hpp"
// The Message view and the offset of each field inside a body, such as kAddRefOff. All
// the parsing this file does is reading fields at those offsets.
#include "itch/types.hpp"

namespace book {

// Which order table to use. The three have the same interface, so switching is this one
// line.
//   PoolMap   an array of buckets with chains: read the bucket for the head, then follow
//             a pointer into the node pool. Two accesses, and the second address is not
//             known until the first returns, so they cannot overlap.
//   OrderMap  open addressing: compute a position and the data is there. One access.
//   InlineMap the first order of a bucket stored in the bucket itself, so one access
//             covers almost every lookup.
//
// Both alternatives were measured on the real machine and both were worse, so they are
// history rather than options:
//   OrderMap   p99.9 worse by 31.8%
//   InlineMap  p99.9 worse by 38.8% - a bucket grows from 4 bytes to 24, the table takes
//              671 MB more, and what that pushes out of the cache costs more than the
//              hop it saves
// In a microbenchmark both are faster, because there the table is the only thing running
// and nothing else is competing for the cache.
using OrderTable = PoolMap;

// One shard's own book: the two tables above and a set of counters.
class OrderBook {
public:
    // Not logging. These are the conditions a run has to meet, and a run that fails them
    // is thrown away. So what matters about each field below is what a non zero means.
    struct Counters {
        // The first five are ordinary volume; any value is fine. They confirm the whole
        // day really was replayed - rerun the same day and they should come out almost
        // identical, and a large difference means the wrong data file.
        std::uint64_t added = 0;
        // Both E and C are counted here. To the book they do the same thing, taking
        // shares off an order. They differ only in the price they traded at, and the book
        // holds the price the order was posted at, so that difference does not reach it.
        std::uint64_t executed = 0;
        // Partial cancels. Same code path as an execution, different field offsets.
        std::uint64_t cancelled = 0;
        // Full deletes: the only message that always removes an order from the table.
        std::uint64_t deleted = 0;
        // Replaces. One message doing two things, a delete and an insert, so it puts the
        // most pressure on the table.
        std::uint64_t replaced = 0;
        // A message about an order id we do not hold. Zero on a clean feed; anything else
        // means a lost packet or a bug.
        std::uint64_t orphan = 0;
        // A cancel or an execution for more shares than the order has left. Also in the
        // "should not happen" family.
        std::uint64_t oversized = 0;
        // The order table is full and a new order could not be recorded. This one must
        // never be allowed to drift above zero: once an order is missing, every later
        // message about it is answered wrongly, and the error keeps spreading, so the
        // book is broken from that point on.
        std::uint64_t full = 0;
        // The same order id added twice.
        std::uint64_t duplicate = 0;
        // The order's price fell outside the range this security was given. Its shares
        // are deliberately left off every price level: each security only gets a narrow
        // band of space around its reference price, and a price far outside it can never
        // reach the best three, so tracking it would waste memory.
        std::uint64_t untracked = 0;
    };

    // Single threaded path: how many orders the table should hold, and a reference price
    // per security, which the price layer uses to decide how wide a band to give each of
    // them.
    OrderBook(std::size_t orders, const std::vector<std::uint32_t>& references)
        : orders_(orders), levels_(references) {}

    // Sharded path: how many orders, and how much price space this shard gets.
    //
    // No reference prices here, because a shard only learns which securities are its own
    // once the day's feed names them. It claims its share of the space at start up and
    // binds the securities as they arrive.
    OrderBook(std::size_t orders, std::size_t level_words)
        : orders_(orders), levels_(level_words) {}

    // Ties a security to its reference price and cuts it a band of price space. A false
    // means it did not get one, usually because the space ran out, and that security will
    // never produce a signal.
    bool bind(std::uint16_t sym, std::uint32_t reference) {
        return levels_.bind(sym, reference);
    }

    // The main path of this class and the most expensive stretch on the hot path: take a
    // message and update the two tables.
    //
    // Taken apart per message with --profile, it used to be:
    //   the table lookup plus the price level update   171 ns
    //   asking for the best three                      121 ns
    //   deciding the signal                             31 ns
    // The last two moved out: they now happen once per poll, per security touched. What
    // is left on the per message path is essentially the 171 ns here.
    //
    // The return is not success or failure. It says whether the book changed. Trade
    // reports, imbalance notices and trading status messages do not touch it at all: they
    // return false and nothing is wrong.
    //
    // The second argument passes out which security the message was about, so the caller
    // can keep its list of what this batch touched.
    bool apply(const itch::Message& m, std::uint16_t* sym) {
        // Passed out before the switch, so the caller gets the right number even for a
        // message that does not touch the book.
        *sym = m.stock_locate();
        // These seven types are every message that can change the book; nothing else is
        // looked at.
        switch (m.type()) {
            // A plain add and an add carrying a market participant id. The book treats
            // them identically.
            case 'A':
            case 'F':
                return add(m);
            // Part of this order traded: take shares off it by id.
            case 'E':
                return take(m, itch::kExecRefOff, itch::kExecSharesOff, &c_.executed);
            // Traded at a different price, which changes nothing for the book, because
            // the book holds the price the order was posted at.
            case 'C':
                return take(m, itch::kExecRefOff, itch::kExecSharesOff, &c_.executed);
            // Some shares withdrawn. Same work, different field offsets, so they are
            // passed in.
            case 'X':
                return take(m, itch::kCancelRefOff, itch::kCancelSharesOff, &c_.cancelled);
            // The whole order withdrawn.
            case 'D':
                return remove(m);
            // Replace: the old order goes, a new one takes its place under a new id.
            case 'U':
                return replace(m, sym);
            // Everything else leaves the book alone. Returning false keeps this security
            // off the touched list, so no signal is worked out for it.
            default:
                return false;
        }
    }

    // Shares resting on the best three prices of one side. This is the number the signal
    // reads.
    [[nodiscard]] std::uint64_t top3(std::uint16_t sym, std::uint8_t side) const {
        return levels_.top3(sym, side);
    }
    // The best price of one side and the shares on it. Asked when an order is about to go
    // out: to buy, ask the sell side, because a taking order trades at the other side's
    // price. A false means that side is empty, and then no order can be sent.
    [[nodiscard]] bool best(std::uint16_t sym, std::uint8_t side, std::uint32_t* price,
                            std::uint32_t* shares) const {
        return levels_.best(sym, side, price, shares);
    }
    // How many orders are resting in the table right now.
    [[nodiscard]] std::size_t live() const noexcept { return orders_.size(); }
    // Which side the last apply changed. Only meaningful when that apply returned true.
    [[nodiscard]] std::uint8_t last_side() const noexcept { return last_side_; }
    // How many orders the table can hold. Read with live() it says whether it was sized
    // well.
    [[nodiscard]] std::size_t capacity() const noexcept { return orders_.capacity(); }
    // The counters above, printed at the end of a run and used to judge it.
    [[nodiscard]] const Counters& counters() const noexcept { return c_; }
    // The price layer itself, so a checking tool can walk it level by level.
    [[nodiscard]] const PriceLevels& levels() const noexcept { return levels_; }

    // The six calls below expose the table and the price layer by slot number, for
    // building the book in passes.
    //
    // They exist rather than being wrapped in more logic here because building in passes
    // separates looking an order up from changing a price level, with several passes in
    // between, while apply, add, take and remove each look up and change in one go.
    // Forcing the passes through those would glue back together exactly what they split
    // apart. They are additions: the message at a time path above is untouched.

    // What find_slot returns when there is nothing to find.
    static constexpr std::uint32_t kNoSlot = OrderTable::kNoSlot;

    // Puts an order in the table and says which slot it landed in, or kNoSlot if the
    // table is full.
    std::uint32_t insert_at(std::uint64_t oid, const OrderTable::Order& o) {
        return orders_.insert_at(oid, o);
    }
    // Which slot an order is in, or kNoSlot.
    [[nodiscard]] std::uint32_t find_slot(std::uint64_t oid) const {
        return orders_.find_slot(oid);
    }
    // Reads the order in a slot.
    [[nodiscard]] OrderTable::Order at(std::uint32_t slot) const { return orders_.at(slot); }
    // Changes the shares in a slot, for the pass that takes shares off.
    void set_shares_at(std::uint32_t slot, std::uint32_t shares) {
        orders_.set_shares_at(slot, shares);
    }
    // Changes the side and security of a slot. The replace pass needs it: the new order
    // is built with no side, and the side is written back once the old order gives it up.
    void set_side_sym_at(std::uint32_t slot, std::uint8_t side, std::uint16_t sym) {
        orders_.set_side_sym_at(slot, side, sym);
    }
    // Removes the order in a slot. It does not touch the price levels - the caller works
    // those out itself.
    void erase_at(std::uint32_t slot) { orders_.erase_at(slot); }

    // Adds to or takes from one price of one side: positive adds, negative takes away.
    // A price outside the security's band does nothing, exactly as add and remove behave.
    void level_move(std::uint16_t sym, std::uint8_t side, std::uint32_t price,
                    std::int64_t delta) {
        // Two branches because the price layer has separate calls, and the one that takes
        // away wants an unsigned number.
        if (delta > 0) {
            levels_.add(sym, side, price, static_cast<std::uint32_t>(delta));
        } else {
            levels_.remove(sym, side, price, static_cast<std::uint32_t>(-delta));
        }
    }

    // Issues the memory requests a batch of order ids will need, reading nothing and
    // changing nothing. The caller does this before applying them one at a time, so the
    // requests are in flight together.
    void ask_for(const std::uint64_t* oids, std::size_t n) const {
        orders_.ask_for_all(oids, n);
    }

private:
    // The side an add message is on, in the form the price layer wants.
    static std::uint8_t side_of(const itch::Message& m) {
        return m.body[itch::kAddSideOff] == 'B' ? PriceLevels::kBuy : PriceLevels::kSell;
    }

    // A new order: record it, and add its shares to the price it rests at.
    bool add(const itch::Message& m) {
        // These eight bytes are the point made at the top of the file: when this order is
        // cancelled, this id is all the exchange will send. So it is the index.
        const std::uint64_t oid = itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff);
        // How many shares to take off, from which price, on which side - none of the
        // three comes again on the wire, so all three are stored now.
        const OrderTable::Order o{
            itch::read_be<std::uint32_t>(m.body + itch::kAddSharesOff),
            itch::read_be<std::uint32_t>(m.body + itch::kAddPriceOff), side_of(m)};
        // Counted before the steps that can fail, so this is messages received rather
        // than orders recorded. The difference is exactly full plus duplicate below.
        ++c_.added;
        // The lookup writes its answer here. It has to be initialised: on a miss the
        // variable is left alone, and whatever the last call put there would remain.
        OrderTable::Order old{};
        // Why an add looks the id up at all: the same id twice cannot happen on a clean
        // feed, but ignoring it if it did would leave the older order's shares on the
        // price level forever, and that security's book would read high from then on.
        if (orders_.find(oid, &old)) {
            ++c_.duplicate;
            // Tell the caller which side moved, so it does not recompute both.
            last_side_ = old.side;
            // The repair that matters: take the old order off its level first. Without
            // it the insert below overwrites the old record, and those shares can never
            // be removed by anything that follows.
            levels_.remove(m.stock_locate(), old.side, old.price, old.shares);
        }
        // Table first, price level second, and not the other way round: if the table is
        // full, the levels have not been touched and the book is still consistent - short
        // one order rather than holding shares that can never be taken off.
        if (!orders_.insert(oid, o)) {
            ++c_.full;
            return false;
        }
        // Only now the price level, where put() also notices a price outside the band.
        put(m.stock_locate(), o);
        return true;
    }

    // Takes shares off an order that is already there. An execution and a partial cancel
    // share this code; they differ only in where the id and the shares sit in the body,
    // and in which counter to raise.
    bool take(const itch::Message& m, std::size_t ref_off, std::size_t shares_off,
              std::uint64_t* counter) {
        ++*counter;
        const std::uint64_t oid = itch::read_be<std::uint64_t>(m.body + ref_off);
        // This is how many to take away, not how many are left. Reading it the other way
        // would grow the book instead of shrinking it.
        const std::uint32_t want = itch::read_be<std::uint32_t>(m.body + shares_off);
        // Which price and which side to take them off is known only to the order as it
        // was before, since the message carries neither.
        OrderTable::Order before{};
        // Taking the last share also removes the order from the table, so there is no
        // separate delete to do here.
        if (!orders_.reduce(oid, want, &before)) {
            // A message about an id we do not hold. Impossible on a clean feed, so a non
            // zero here means a lost packet or a bug, and either way the run stops being
            // trustworthy. The trade itself did happen; we simply lost track of it.
            ++c_.orphan;
            return false;
        }
        if (want > before.shares) ++c_.oversized;
        // Take the smaller of the two. Subtracting want directly would take a price level
        // below zero, and it is unsigned, so it would wrap to about four billion. That
        // level would then sit in front of every other one for the rest of the day, every
        // signal for that security would be wrong, and nothing would report an error.
        const std::uint32_t took = want < before.shares ? want : before.shares;
        // The side from before the change, because the order may no longer exist.
        last_side_ = before.side;
        // The price from before as well: the order's price never changes, but its record
        // may already be gone.
        levels_.remove(m.stock_locate(), before.side, before.price, took);
        return true;
    }

    // A full cancel: remove the order and take all of its shares off the price level.
    bool remove(const itch::Message& m) {
        ++c_.deleted;
        // The message carries an order id and nothing else. How many shares, at what
        // price, on which side all have to come back out of the table - which is the
        // entire reason the table exists.
        OrderTable::Order gone{};
        // erase both removes it and hands back what it was, so there is no need to look
        // it up first and remove it afterwards.
        if (!orders_.erase(itch::read_be<std::uint64_t>(m.body + itch::kDeleteRefOff),
                           &gone)) {
            ++c_.orphan;
            return false;
        }
        // Only this side needs its best three worked out again.
        last_side_ = gone.side;
        // The whole order goes, so all of its shares come off, unlike an execution.
        levels_.remove(m.stock_locate(), gone.side, gone.price, gone.shares);
        return true;
    }

    // Replace: the old order goes and a new one takes its place under a new id.
    //
    // One thing is not on the wire and can only be inherited: the side. The message
    // carries new shares and a new price but no side, so it has to be copied from the
    // order being replaced, as does the security.
    bool replace(const itch::Message& m, std::uint16_t* sym) {
        // One message, counted once, even though it becomes a delete and an insert.
        ++c_.replaced;
        OrderTable::Order gone{};
        // A replace carries two order ids. This is the old one; using the wrong one would
        // delete an order nobody replaced.
        if (!orders_.erase(itch::read_be<std::uint64_t>(m.body + itch::kReplaceOldRefOff),
                           &gone)) {
            // Without the old order the new one has to be given up as well, because the
            // message has no side and there is no other way to learn it.
            ++c_.orphan;
            return false;
        }
        // Both halves are on the same side, so this is recorded once.
        last_side_ = gone.side;
        // The old shares come off first. Past this line the book is in a state where the
        // old order is gone and the new one is not there yet, which is what makes the
        // failing insert below serious.
        levels_.remove(*sym, gone.side, gone.price, gone.shares);
        // Shares and price come from the message; the side is inherited, and so is the
        // security, which is the one passed in.
        const OrderTable::Order fresh{
            itch::read_be<std::uint32_t>(m.body + itch::kReplaceSharesOff),
            itch::read_be<std::uint32_t>(m.body + itch::kReplacePriceOff), gone.side};
        // The new id: every later message about this order will use it.
        if (!orders_.insert(itch::read_be<std::uint64_t>(m.body + itch::kReplaceNewRefOff),
                            fresh)) {
            // The table is full and the new order was not recorded, while the remove above
            // has already happened. Returning false only says "no signal for this one";
            // the book itself is now short. Another reason a full table voids a run.
            return false;
        }
        // The new order goes on, and the half finished state ends here.
        put(*sym, fresh);
        return true;
    }

    // Adds an order's shares to its price level, and notices a price outside the band.
    void put(std::uint16_t sym, const OrderTable::Order& o) {
        // Read once so it can be compared afterwards. For speed the price layer ignores a
        // price outside the band silently, without returning anything, so "added but
        // unchanged" is the only way to see it.
        const std::uint32_t was = levels_.at(sym, o.side, o.price);
        // Both an add and a replace come through here, so neither has to record the side
        // itself.
        last_side_ = o.side;
        levels_.add(sym, o.side, o.price, o.shares);
        if (levels_.at(sym, o.side, o.price) == was) ++c_.untracked;
    }

    // Which side the last apply changed: 0 buys, 1 sells.
    //
    // One message can only change one side. A cancel, an execution and a reduction all
    // act on the side their own order rests on, and a replace inherits the side of the
    // order it replaces, so both halves are on the same one. The caller uses this to skip
    // recomputing the other side, whose last answer is still correct.
    std::uint8_t last_side_ = 0;

    // The first table: every resting order, by exchange order id.
    OrderTable orders_;
    // The second table: shares per price. This is the one the strategy reads.
    PriceLevels levels_;
    // The counters for the whole run.
    Counters c_;
};

}  // namespace book
