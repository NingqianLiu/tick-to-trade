// The third order table: make each bucket big enough to hold its first order.
//
// Three interchangeable tables live side by side in this directory - pool_map.hpp, which
// is the one in use, order_map.hpp, and this. The line "using OrderTable = ..." in
// order_book.hpp picks one.
//
// The conclusion first, so nobody reads on expecting a recommendation: this version lost
// a real comparison on the machine and is not in use. It is kept as a record of the idea,
// the code, and why it lost.
//
// A lookup sounds like one memory access. It is two, and they cannot be overlapped: read
// the bucket array to get the index of the chain head, then follow that index into the
// node pool to reach the order. The address of the second is not known until the first
// comes back, so they queue up, and together they are around two hundred nanoseconds -
// on every message, eight hundred million times a day.
//
// In the segment timing, the table lookup plus the price level update comes to 171 ns,
// more than half of what a message spends building the book and checking the signal. So
// that hop is worth attacking.
//
// The idea here is to widen the bucket so the first order lives inside it. With twice as
// many buckets as orders, almost every bucket holds a single order, and the read of the
// bucket is the read of the order. Only a second order in the same bucket goes to the
// node pool and pays the old two hops.
//
// The price is memory: a bucket grows from four bytes to twenty four. In a microbenchmark
// it is over 40% faster while the table is small, and only 10 to 30% once the table is a
// few gigabytes - the extra memory brings its own cache misses, which eat the hop it
// saved.
//
// The interface matches PoolMap exactly, so switching is one line in order_book.hpp.
//
// The measured result: p99.9 38.8% worse than PoolMap and p99 26.9% worse. The cause is
// the line about memory above - 4 bytes to 24 per bucket is 671 MB more for the order
// table, and those 671 MB come out of the cache the price space was using. The hop it
// saves does not pay that back.
//
// One process note from the same episode: the morning it was measured, the result was
// written down as reverted, but the alias in order_book.hpp had not actually been changed
// back, so every round that day ran on the slower table and all of its absolute numbers
// were off. That line is worth checking before any merge.
#pragma once

// std::size_t for bucket counts, order counts and bucket indexes, which have to hold tens
// of millions.
#include <cstddef>
// Fixed width integers. The widths matter here: an order id is 64 bits, shares and price
// are 32 each, and the size of a bucket - twenty four bytes - follows from them.
#include <cstdint>
// std::vector for the bucket array and the node pool. Both are sized once in the
// constructor and never allocate again: a malloc on the hot path would be microseconds,
// and the tail would be nothing but that.
#include <vector>

namespace book {

// Order id to order: how many shares are left, at what price, on which side.
class InlineMap {
public:
    // What an order looks like from outside. The fields match PoolMap's exactly, which is
    // what makes the two interchangeable.
    struct Order {
        std::uint32_t shares;
        // No decimal point: the wire sends an integer with four implied decimals, so
        // 1234500 is $123.45. Integers compare exactly and carry no floating point error.
        std::uint32_t price;
        // 0 buys, 1 sells.
        std::uint8_t side;
    // Only a carrier. Inside, the side and the price share one 32 bit word - see pack().
    };

    // orders is the most that can rest at once.
    //
    // Four things in order: size and allocate the buckets, allocate the node pool, work
    // out the shift the hash needs, and thread the pool into a free list. None of them
    // changes afterwards, so the hot path never allocates.
    explicit InlineMap(std::size_t orders) {
        // A power of two, because bucket() lands an order by taking the top bits of a
        // hash, and that only covers every bucket when the count is a power of two.
        std::size_t n = 16;
        // Twice the orders. That is the whole idea: at half load almost every bucket holds
        // one order, so the bucket itself is enough and the lookup is one access.
        while (n < orders * 2) n <<= 1;
        // Every bucket empty - zero shares. assign rather than resize says "all of them",
        // without the reader having to think about what was there before.
        buckets_.assign(n, Slot{});
        // The pool only holds second and later orders in a bucket, so in principle it
        // needs fewer. It is sized for all of them anyway: in the worst case every order
        // collides, and running out halfway through a day cannot be recovered from. The
        // floor of sixteen is for small tests, so nodes_.back() below always exists.
        nodes_.resize(orders < 16 ? 16 : orders);
        // Which bucket an order lands in could be a modulo, but that is a division, twenty
        // odd cycles. With a power of two count, taking the top bits is the same answer in
        // one shift - and these two lines work out how far to shift.
        shift_ = 64;
        // One bit fewer for every doubling, so what is left is exactly as wide as the
        // bucket count. With 256 buckets, eight bits, the loop runs eight times and the
        // shift ends at 56, so hash >> 56 lands in 0 to 255.
        for (std::size_t c = n; c > 1; c >>= 1) --shift_;
        // Thread the pool into a free list, so finding a spare node is reading an index -
        // no scanning and no allocation.
        for (std::size_t i = 0; i + 1 < nodes_.size(); ++i) {
            // Indexes are 32 bits, which also caps the table at four billion orders.
            nodes_[i].next = static_cast<std::uint32_t>(i + 1);
        }
        // The loop deliberately stops one short so this line can end the list.
        nodes_.back().next = kEnd;
        free_ = 0;
    }

    // Orders resting right now - a counter, not a walk.
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    // The size of the node pool.
    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.size(); }

    // Puts an order in; the same id again overwrites.
    //
    // Four cases in order, each one returning as soon as it applies: the bucket is empty,
    // so the order moves in; the bucket already holds this id, so it is overwritten; the
    // id is on the overflow chain, so that is overwritten; none of the above, so a node
    // comes off the free list onto the head of the chain.
    //
    // A false means either zero shares or an exhausted pool.
    bool insert(std::uint64_t oid, const Order& o) {
        // An order of zero shares is meaningless, and refusing it lets a zero share count
        // double as the mark for an empty bucket, saving a flag.
        if (o.shares == 0) return false;
        // This line is the most expensive one in the class. The trader asks for 12,582,912
        // orders, which makes 33,554,432 buckets, and at twenty four bytes each the bucket
        // array is 805 MB. The cache holds tens of megabytes and order ids are spread out,
        // so nearly every one of these goes to memory, a hundred odd nanoseconds. The same
        // number of PoolMap buckets, at four bytes each, is 134 MB - and the 671 MB
        // between them is why this version lost.
        Slot& s = buckets_[bucket(oid)];
        // The bucket is empty, so the first order moves straight in. At half load this is
        // the common path, and it is the reason the class exists.
        if (s.shares == 0) {
            // The id has to be stored: several ids land in the same bucket, and a lookup
            // needs it to know whether the order living here is the one being asked for.
            s.oid = oid;
            // A non zero share count is what marks the bucket as occupied.
            s.shares = o.shares;
            s.side_price = pack(o);
            ++count_;
            return true;
        }
        // The bucket holds this same id, so this is an overwrite. It should not happen on
        // a clean feed; overwriting matches what PoolMap does, so the two implementations
        // cannot disagree about a corner case.
        if (s.oid == oid) {
            s.shares = o.shares;
            s.side_price = pack(o);
            // The count does not move: the order was already there.
            return true;
        }
        // Someone else has the bucket, so look along the overflow chain for this id.
        for (std::uint32_t i = s.next; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid == oid) {
                nodes_[i].shares = o.shares;
                nodes_[i].side_price = pack(o);
                return true;
            }
        }
        // Genuinely new, so it needs a node. Nothing left means the pool is exhausted; the
        // layer above treats it as a failed insert, and it really means the table was
        // built too small.
        if (free_ == kEnd) return false;
        const std::uint32_t i = free_;
        free_ = nodes_[i].next;
        // The same three fields as a bucket would hold.
        nodes_[i].oid = oid;
        nodes_[i].shares = o.shares;
        nodes_[i].side_price = pack(o);
        // On the head of the chain, which takes one step rather than walking to the end.
        nodes_[i].next = s.next;
        s.next = i;
        ++count_;
        return true;
    }

    // Looks an order up.
    //
    // This is the point of the whole class: when the bucket holds the order being asked
    // for, the read of the bucket returns every field and there is no second hop.
    [[nodiscard]] bool find(std::uint64_t oid, Order* out) const {
        // That single access. It brings back not just where the chain starts but the id,
        // the shares, the side and the price: a cache line is sixty four bytes and a
        // bucket is twenty four, so one read has all of it.
        const Slot& s = buckets_[bucket(oid)];
        // Both conditions matter. Checking the id alone would not do: an empty bucket has
        // an id of zero, so asking for order zero would match an empty one and read back
        // an order that does not exist.
        if (s.shares != 0 && s.oid == oid) {
            *out = unpack(s.shares, s.side_price);
            return true;
        }
        // Not in the bucket, so along the chain - and this is the old two hop path.
        for (std::uint32_t i = s.next; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid == oid) {
                *out = unpack(nodes_[i].shares, nodes_[i].side_price);
                return true;
            }
        }
        // Not an error in itself: an order placed on an earlier day has no add message in
        // this file, so it was never in the table. The layer above skips the message.
        return false;
    }

    // Takes shares off an order and hands back what it looked like first.
    //
    // The caller needs the old state to update the price level: how much to take off,
    // from which price, on which side, all come from the order as it was. Reaching zero,
    // or being asked for more than is left, removes the order.
    bool reduce(std::uint64_t oid, std::uint32_t shares, Order* before) {
        Slot& s = buckets_[bucket(oid)];
        if (s.shares != 0 && s.oid == oid) {
            // Handed over before anything changes, or it would be the new state.
            *before = unpack(s.shares, s.side_price);
            if (shares >= s.shares) {
                // lift() promotes the first node of the chain into this bucket.
                lift(s);
            } else {
                s.shares -= shares;
            }
            return true;
        }
        // Not in the bucket, so along the chain.
        //
        // link points at the next field of whatever came before, rather than at a node.
        // Removing a node means changing the next of the one in front of it, and this
        // chain only runs forwards - once at node i there is no way back to its
        // predecessor. Holding the address of the last next solves that with one write.
        // It starts at the bucket's own next.
        std::uint32_t* link = &s.next;
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            if (nodes_[i].oid != oid) continue;
            *before = unpack(nodes_[i].shares, nodes_[i].side_price);
            if (shares >= nodes_[i].shares) {
                // The one in front steps over this node.
                *link = nodes_[i].next;
                // And the node goes back on the free list.
                nodes_[i].next = free_;
                free_ = i;
                --count_;
            } else {
                nodes_[i].shares -= shares;
            }
            // Outside the if: whether it was emptied or only reduced, the caller's message
            // was handled.
            return true;
        }
        return false;
    }

    // Removes an order and hands back what it was. The same walk as reduce, without the
    // partial case - a cancel always takes the whole order.
    bool erase(std::uint64_t oid, Order* gone) {
        Slot& s = buckets_[bucket(oid)];
        if (s.shares != 0 && s.oid == oid) {
            *gone = unpack(s.shares, s.side_price);
            lift(s);
            return true;
        }
        // The same pointer to the link, for the same reason as in reduce.
        std::uint32_t* link = &s.next;
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            if (nodes_[i].oid != oid) continue;
            *gone = unpack(nodes_[i].shares, nodes_[i].side_price);
            *link = nodes_[i].next;
            nodes_[i].next = free_;
            free_ = i;
            --count_;
            return true;
        }
        return false;
    }

    // Issues the memory requests a batch of ids will need, reading nothing and changing
    // nothing.
    //
    // Why it helps: one access is a hundred odd nanoseconds, but a core can have around
    // twenty in flight at once. Sending a batch of requests and letting them overlap beats
    // asking, waiting, and asking again, because by the time each one is really needed the
    // data has arrived.
    //
    // It has a floor, though. With one or two lookups in hand, batching is 7 to 9% slower
    // than doing them one at a time, and it only saturates around thirty (about +11%).
    // Measured, a poll has a median of one message in hand and 33 at p99.9 - so this kind
    // of batching can only help the tail, never the median.
    //
    // This implementation has one hop, so one round of requests is enough. PoolMap has two
    // and needs a second round, which has to wait for the first.
    void ask_for_all(const std::uint64_t* oids, std::size_t n) const {
        // Nothing in this loop depends on anything else in it, so all of it can be in
        // flight together.
        for (std::size_t j = 0; j < n; ++j) {
            // The arguments are the address, 0 for "only going to read it" (1 asks for
            // exclusive ownership and costs more), and 3 for the highest locality, meaning
            // bring it all the way into the cache nearest the core. It returns nothing and
            // cannot fail: a wild address only wastes a request.
            __builtin_prefetch(&buckets_[bucket(oids[j])], 0, 3);
        }
    }

private:
    // End of a chain, and also "the free list is empty". Zero could not serve: it is a
    // legal index, the first node of the pool. All ones is over four billion, far more
    // than the pool can ever hold.
    static constexpr std::uint32_t kEnd = 0xffffffffu;

    // One bucket: the first order, plus the index of the overflow chain.
    //
    // Twenty four bytes, against a cache line of sixty four, which is what makes the read
    // of a bucket the read of an order and removes the second hop.
    //
    // It is not that clean, though: twenty four does not divide sixty four, so buckets sit
    // across line boundaries - two in every eight straddle two lines and need both read.
    // That is one of the reasons this version lost.
    struct Slot {
        std::uint64_t oid = 0;
        // Zero also means the bucket is empty, as insert explains.
        std::uint32_t shares = 0;
        // Side in the top bit, price in the other 31.
        std::uint32_t side_price = 0;
        // Head of the overflow chain; kEnd when there is no second order.
        std::uint32_t next = kEnd;
        // Padding to twenty four. The compiler would add it anyway, since an eight byte
        // field makes the struct align to eight; writing it down makes the size visible.
        std::uint32_t pad = 0;
    };

    // A node of an overflow chain.
    //
    // The fields are the same as a bucket's on purpose: the two are different roles - one
    // is the bucket itself, the other a link in a chain - but keeping them identical means
    // the code for the two paths reads the same and a change to one is hard to forget in
    // the other.
    struct Node {
        std::uint64_t oid = 0;
        std::uint32_t shares = 0;
        std::uint32_t side_price = 0;
        // The next link, and while this node is free, the next free node.
        std::uint32_t next = kEnd;
        std::uint32_t pad = 0;
    };

    // Side into the top bit, price into the other 31.
    //
    // 31 bits is enough: the highest price in the day is 1,999,999,900, which is
    // $199,999.99 in hundredths of a cent, against a limit over two billion.
    static std::uint32_t pack(const Order& o) {
        // Widened before the shift; shifting an eight bit value by 31 would be undefined.
        return (static_cast<std::uint32_t>(o.side) << 31) | o.price;
    }
    static Order unpack(std::uint32_t shares, std::uint32_t sp) {
        return Order{shares, sp & 0x7fffffffu, static_cast<std::uint8_t>(sp >> 31)};
    }

    // After the order in a bucket goes, the first node of the chain is promoted into it.
    //
    // Marking the bucket empty and leaving the chain hanging would not do: a lookup uses
    // an empty bucket to mean the bucket holds nothing at all, so those orders would
    // become unreachable.
    void lift(Slot& s) {
        --count_;
        const std::uint32_t i = s.next;
        // Nothing on the chain either, so the bucket really is empty now.
        if (i == kEnd) {
            s.oid = 0;
            // This is the line that marks it empty; without it a later lookup would match.
            s.shares = 0;
            s.side_price = 0;
            return;
        }
        // Move the first node's contents into the bucket.
        s.oid = nodes_[i].oid;
        s.shares = nodes_[i].shares;
        s.side_price = nodes_[i].side_price;
        // The chain moves on, and the emptied node goes back on the free list.
        s.next = nodes_[i].next;
        nodes_[i].next = free_;
        free_ = i;
    }

    // Which bucket an order id lands in.
    //
    // The ids are not already spread out: the exchange hands them out in sequence. Used
    // directly, the orders alive at any moment would crowd into neighbouring buckets -
    // long chains in a few, wide stretches empty. Multiplying first mixes the bits, and
    // then the top ones spread properly.
    [[nodiscard]] std::size_t bucket(std::uint64_t oid) const noexcept {
        // The constant is the golden ratio scaled to 64 bits, a common multiplier for
        // this. The multiplication overflows on purpose: that is what folds the low bits
        // into the high ones, which is why the top bits spread better than the bottom.
        return (oid * 0x9E3779B97F4A7C15ull) >> shift_;
    }

    // Twice as many buckets as orders, so almost every one holds a single order.
    //
    // This array is why the version lost: twenty four bytes each is 805 MB in the trader's
    // configuration, 671 MB more than PoolMap's 134 MB, and that difference pushed the
    // price space out of the cache.
    std::vector<Slot> buckets_;
    // The overflow pool, used only from the second order in a bucket onwards.
    std::vector<Node> nodes_;
    // Head of the free list; kEnd means the pool is exhausted and insert can only fail.
    std::uint32_t free_ = kEnd;
    std::size_t count_ = 0;
    // How far right to shift a hash, worked out in the constructor.
    int shift_ = 64;
};

}  // namespace book
