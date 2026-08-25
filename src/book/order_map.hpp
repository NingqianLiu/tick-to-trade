#pragma once

// Every resting order, indexed by the order id the exchange gave it.
//
// Read this part first, so the rest is not misleading: this is one of three order table
// implementations, and it is the one that lost. Measured on the real machine, its p99.9
// is 31.8% worse than the PoolMap this project uses. The verdict is the line in
// order_book.hpp that says using OrderTable = PoolMap. This file is kept as a record of
// what was tried, not as an option to switch back to.
//
// Why it looked like it would win. PoolMap is an array of buckets with a chain hanging
// off each one: read the bucket to get the head, then jump into the node pool. Two random
// accesses, and the address of the second is not known until the first comes back, so
// they cannot overlap. Open addressing computes a position and the data is there. One
// access.
//
// What that hop is worth: this chip has no path from the card into the last level cache,
// so both the packets and the table live in DRAM, where a random access costs about 200
// cycles - 77 ns at 2.6 GHz, so over 150 ns for two. Measured end to end, a lookup plus
// the price level update it feeds is 171 ns in total. The hop really is most of it, and
// removing it looks like a large win.
//
// In a microbenchmark it is a large win. But in a microbenchmark this table is the only
// thing running. On the real machine it is bigger, and what it pushes out of the cache -
// mostly the price space - costs more than the hop it saves.
//
// The lesson is worth more than the implementation: trying yet another hash table is not
// where the time is. Two shapes have now been measured, the difference is around ten
// percent, and it goes the wrong way. The room is in batching, in overlapping the dozens
// of lookups one poll has in hand at the same time.
//
// Why the table has to exist at all: of the five message types that change the book,
// three carry nothing but an order id. A delete is nineteen bytes - eleven of header and
// eight of id - and never mentions a share count. So the shares still resting, and the
// price they rest at, have to be remembered here; without them there is no way to know
// what to take off the price level when the order goes away. A replace is the same, and
// its new order inherits the side and the security from the one it replaces.

// std::size_t for slot counts, indexes and the mask. This table can hold tens of
// millions of slots, which an int would not.
#include <cstddef>
// An order id is eight bytes; shares and price are four each.
#include <cstdint>
// std::vector. Every slot is allocated once in the constructor and the table never grows
// afterwards - see below for why.
#include <vector>

namespace book {

// An order table with open addressing and linear probing. A slot is sixteen bytes, so
// four of them share a cache line and stepping a few slots along usually costs no extra
// memory access.
class OrderMap {
public:
    // The three things an order has to remember: exactly what a delete does not carry and
    // what taking it off a price level needs.
    struct Order {
        std::uint32_t shares;
        std::uint32_t price;
        std::uint8_t side;  // 0 buys, 1 sells
    };

    // Takes a rough number of orders and allocates more than that: it rounds up to a
    // power of two, and it sizes for a load of seventy percent, because linear probing
    // gets sharply worse past that point.
    //
    // The size is fixed here and never changes again. Growing would mean moving every
    // order in the table, and the moment it would want to grow is the moment the market
    // is busiest.
    explicit OrderMap(std::size_t orders) {
        std::size_t n = 16;
        // Double until seventy percent of n is enough. Written as a comparison of whole
        // numbers so there is no fraction to round.
        while (n * 7 < orders * 10) n <<= 1;
        slots_.assign(n, Slot{});
        // n is a power of two, so wrapping around is a single bitwise and.
        mask_ = n - 1;
        // How far right a hash has to be shifted to land inside n slots.
        shift_ = 64;
        for (std::size_t c = n; c > 1; c >>= 1) --shift_;
    }

    // How many orders are resting right now. Read together with capacity() it says
    // whether the table was sized well.
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    // How many slots were allocated - more than what was asked for, by the margin above.
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    // Records a new order.
    //
    // A false means the table is full, and that is not a condition to recover from: it
    // voids the run. An order that was never recorded turns every later message about it
    // into an unknown id. A repeated id overwrites, which is what the reference
    // implementation does too.
    bool insert(std::uint64_t oid, const Order& o) {
        // An order of zero shares is refused because zero is also how this table marks a
        // free slot, so storing one would make the slot look empty.
        if (o.shares == 0) return false;
        // Start where the hash points and walk forward.
        std::size_t i = start(oid);
        while (slots_[i].shares != 0) {
            // The same id on the way: update it in place and stop.
            if (slots_[i].oid == oid) {
                slots_[i].shares = o.shares;
                slots_[i].side_price = pack(o);
                return true;
            }
            // One slot on, wrapping around the end of the table.
            i = (i + 1) & mask_;
        }
        // A free slot, but only take it if the table stays under seventy percent. Past
        // that the probe lengths grow badly, so refusing is better than filling.
        if ((count_ + 1) * 10 > slots_.size() * 7) return false;
        slots_[i] = Slot{oid, o.shares, pack(o)};
        ++count_;
        return true;
    }

    // Looks an order up. A false should not happen on a clean feed, so callers count it
    // as something to raise rather than something to handle.
    [[nodiscard]] bool find(std::uint64_t oid, Order* out) const {
        const std::size_t i = probe(oid);
        if (i == kMissing) return false;
        *out = unpack(slots_[i]);
        return true;
    }

    // Takes shares off an order and reports what it looked like beforehand, which is what
    // the caller needs to update the price level.
    //
    // Reaching zero deletes the order here rather than waiting for a delete message,
    // because four times out of five an execution consumes the whole order and the
    // exchange then sends nothing further about it. Leaving those behind would fill the
    // table over a day.
    bool reduce(std::uint64_t oid, std::uint32_t shares, Order* before) {
        const std::size_t i = probe(oid);
        if (i == kMissing) return false;
        *before = unpack(slots_[i]);
        // Taking at least everything that is left is the whole order going away.
        if (shares >= slots_[i].shares) {
            remove(i);
        } else {
            slots_[i].shares -= shares;
        }
        return true;
    }

    // Removes an order and reports what was removed. This is the path a delete takes, and
    // the old half of a replace.
    bool erase(std::uint64_t oid, Order* gone) {
        const std::size_t i = probe(oid);
        if (i == kMissing) return false;
        *gone = unpack(slots_[i]);
        remove(i);
        return true;
    }

private:
    // One slot, sixteen bytes, four to a cache line.
    struct Slot {
        // The id, needed while probing to tell this order from another that hashed here.
        std::uint64_t oid = 0;
        // Shares left. Zero doubles as "this slot is free", which is why insert refuses
        // an order of zero shares.
        std::uint32_t shares = 0;   // zero means the slot is free
        // Side and price in one word: the top bit is the side, the other 31 the price.
        std::uint32_t side_price = 0;  // top bit the side, the rest the price
    };
    // Sixteen bytes is not an accident. Add a field and a slot becomes twenty four, only
    // two fit in a cache line, and stepping along during a probe starts costing an extra
    // memory access - which is the entire advantage this table was meant to have.
    static_assert(sizeof(Slot) == 16);

    // "Not found". All ones, because it cannot be a real slot number.
    static constexpr std::size_t kMissing = static_cast<std::size_t>(-1);

    // A price fits in 31 bits: the highest price the exchange sent that day was
    // 1,999,999,900, which is $199,999.99 - prices on the wire are whole hundredths of a
    // cent. That leaves the top bit free for the side, and one word holds both.
    static std::uint32_t pack(const Order& o) {
        return (static_cast<std::uint32_t>(o.side) << 31) | o.price;
    }
    static Order unpack(const Slot& s) {
        return Order{s.shares, s.side_price & 0x7fffffffu,
                     static_cast<std::uint8_t>(s.side_price >> 31)};
    }

    // Which slot an id hashes to.
    //
    // The multiplication matters: exchange order ids are handed out in sequence, so using
    // the low bits directly would pack neighbouring orders into neighbouring slots.
    // Multiplying by the golden ratio constant and taking the high bits spreads them over
    // the whole table.
    [[nodiscard]] std::size_t start(std::uint64_t oid) const noexcept {
        return (oid * 0x9E3779B97F4A7C15ull) >> shift_;
    }

    // Walks forward from the hash looking for an id.
    //
    // An empty slot ends the search, because nothing could have been placed past it. That
    // is exactly the property remove() below has to preserve.
    [[nodiscard]] std::size_t probe(std::uint64_t oid) const noexcept {
        std::size_t i = start(oid);
        while (slots_[i].shares != 0) {
            if (slots_[i].oid == oid) return i;
            i = (i + 1) & mask_;
        }
        return kMissing;
    }

    // Empties a slot.
    //
    // The usual answer is a tombstone: mark the slot dead so a probe knows to keep going.
    // This shifts the following entries back instead, because tombstones pile up and
    // every later probe has to walk over them - and this table deletes all day long, four
    // executions in five taking an order with them.
    void remove(std::size_t hole) {
        std::size_t j = hole;
        for (;;) {
            j = (j + 1) & mask_;
            // An empty slot ends it: nothing beyond can belong to this hole.
            if (slots_[j].shares == 0) break;
            // Where this entry would have liked to sit.
            const std::size_t want = start(slots_[j].oid);
            // If that spot lies between the hole and here, the entry must stay: moving it
            // would put it in front of its own hash position, where a probe would stop
            // before reaching it. The two cases are for a range that wraps past the end
            // of the table.
            const bool between = hole <= j ? (hole < want && want <= j)
                                           : (hole < want || want <= j);
            if (between) continue;
            // Safe to move: it fills the hole, and the hole becomes where it used to be.
            slots_[hole] = slots_[j];
            hole = j;
        }
        slots_[hole] = Slot{};
        --count_;
    }

    // Allocated once in the constructor; the size never changes.
    std::vector<Slot> slots_;
    std::size_t mask_ = 0, count_ = 0;
    // How far right to shift a hash, worked out from the slot count.
    int shift_ = 64;
};

}  // namespace book
