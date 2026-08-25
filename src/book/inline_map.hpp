// The third implementation of the order table: make every bucket large and hold the first order
// inside the bucket itself.
//
// "The third" means there are three interchangeable order tables in this repository, in three
// files side by side: pool_map.hpp (the one in use), order_map.hpp, and this inline_map.hpp.
// Which one is used is decided by the line `using OrderTable = ...` in order_book.hpp.
//
// The conclusion first, to save reading it for nothing: this version was turned down by an A/B on
// the real machine and is not used now.
// It stays as a record - the idea, the code, and why it lost are all in this file.
//
// It looks as though looking an order up were one memory access. It is in fact two, and the two
// cannot be done at once: read the bucket array first to get the index of the head of the list,
// then jump by that index into the node pool to read the order.
// Where the second one goes is only known once the first has come back - so they can only queue
// one after the other, and the two together are about two hundred nanoseconds. Every message
// makes that trip, eight hundred million times over a day.
//
// Set against the timing of segments: the order table lookup and the price level change together
// are 171 nanoseconds, more than half of what one message costs in the "build the book and decide
// the signal" part. So this hop really is worth thinking about.
//
// What this file does is make the bucket itself large so that the first order lives inside it.
// The bucket count is opened to twice the order count, so the vast majority of buckets hold one
// order - and reading the bucket brings the order back with it.
// Only where a second or later order joins does it go on into the node pool, and only those make
// the original two hops.
//
// The cost is memory: a bucket goes from four bytes an entry to twenty four. A micro benchmark
// measured it as over forty percent quicker with a small table, and only one to three percent
// once the table reaches several gigabytes - the extra memory brings more cache misses of its
// own, which eats the hop that was saved.
//
// The interface is identical to PoolMap's, so changing one alias in order_book.hpp switches
// between them.
//
// The result of the A/B on the real machine: the p99.9 is 38.8% worse than PoolMap's and the p99
// 26.9% worse.
// The reason is exactly that "the cost is memory" - a bucket going from 4 bytes to 24 makes the
// order table take 671 MB more, and those 671 MB are squeezed out of the price space's cache. The
// hop that was saved does not pay that back.
//
// It also went wrong once in another way: one morning the measurement was finished and "reverted"
// was written down, but the code was never really changed back, so every round of that whole day
// ran on this worse implementation and every absolute figure was out.
// So before merging, that using OrderTable line in order_book.hpp has to be checked.

// This header only has to be included once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Everything to do with the order book in this repository goes in the book namespace, so that
// ordinary names like Order and Slot do not collide with ones elsewhere.
namespace book {

// A table from an order id to an order's content.
// Given an order id it gives back how many shares that order has left, what price it rests at,
// and whether it is a buy or a sell.
class InlineMap {
// What follows is what the outside can use: one constructor, five actions (place one, look one
// up, take shares off, cancel one whole, and send the memory requests out in advance), and two
// for asking about the state (how many now, and how many at most).
public:
    // What one order looks like from outside.
    // The fields are identical to PoolMap's, so that the two classes can be swapped directly and
    // order_book.hpp above only changes one alias.
    struct Order {
        // How many shares of this order are left untraded.
        std::uint32_t shares;
        // What price it rests at.
        // There is no decimal point here: what arrives on the wire is an integer, with the last
        // four digits agreed to be decimals.
        // So 1234500 reads as 123.45 dollars. Only an integer can be compared directly, and it
        // has no floating point error.
        std::uint32_t price;
        // Buy or sell. 0 is a buy and 1 is a sell.
        std::uint8_t side;
    // The end of the structure. It is only a shell for carrying things about, and the table does
    // not hold them in this shape inside (it squeezes the side and the price into one thirty two
    // bit field, see pack below).
    };

    // Construction. orders is at most how many orders have to be held.
    //
    // This is the first main piece of the class, doing four things whose order cannot change:
    // work out the bucket count from the order count and open the bucket array, open the node
    // pool, work out how far the hash has to shift right, and string the node pool into a free
    // list.
    // None of the four changes once the run is going, so the hot path allocates no memory at all.
    explicit InlineMap(std::size_t orders) {
        // The bucket count starts at sixteen. It has to be a power of two, because bucket() below
        // places an order by taking the top bits, and taking the top bits only covers every
        // bucket exactly when the count is a power of two.
        std::size_t n = 16;
        // Doubled until the bucket count is at least twice the order count.
        // Twice is the point of this implementation: at a load of only a half, the vast majority
        // of buckets hold one order, so "that entry in the bucket" is enough and one memory
        // access finds it - which is exactly the hop it means to save.
        while (n < orders * 2) n <<= 1;
        // The bucket array is opened with every entry a default Slot - shares of zero, meaning
        // empty.
        // It looks as though resize would do the same here. It would not: resize only governs
        // what it adds and touches not one word of the entries already there. assign writes every
        // entry to the value given.
        // (In the constructor the two really do come out the same, since there was no entry at
        // all. assign is written so that this line reads as "empty all of it", without having to
        // think about what was there before.)
        buckets_.assign(n, Slot{});
        // The node pool only holds the second and later orders, and in theory could not need this
        // many.
        // It is still opened to the full order count: at worst every order collides into the same
        // few buckets, and then the overflow lists would be full of them. Finding it too small
        // halfway through a run cannot be repaired, so it is better to open too much.
        // The lower limit of sixteen is for tests that pass a very small orders, so that
        // nodes_.back() below does not point at nothing.
        nodes_.resize(orders < 16 ? 16 : orders);
        // The two lines below work out a shift for bucket() at the very bottom.
        // It seems as though which bucket something falls in would be a modulo. A modulo is a
        // division, twenty odd cycles; and since the bucket count is a power of two, "take the
        // top bits" is the same as a modulo and is one shift instruction.
        // How many bits to take, which is how far to shift right, is what these two lines work
        // out.
        // Starting at sixty four - shifting right by sixty four keeps not one bit.
        shift_ = 64;
        // Every doubling of the bucket count shifts one bit less, so that the bits kept come to
        // exactly the width of the bucket count.
        // For instance, with 256 buckets (eight bits) the loop runs eight times and shift_
        // becomes 56, so (hash >> 56) lands between 0 and 255, one value per bucket.
        for (std::size_t c = n; c > 1; c >>= 1) --shift_;
        // What follows strings the node pool into a free list: every entry's next points at the
        // next.
        // An insert takes an entry from the head, and a delete returns one to the head.
        // That way "find a free place" is reading one index, with nothing to scan and no malloc.
        for (std::size_t i = 0; i + 1 < nodes_.size(); ++i) {
            // The entry after entry i is entry i+1.
            // Narrowed to thirty two bits because what the list holds is an index and the index
            // field is thirty two bits wide - which also pins the order limit at four thousand
            // million and more, far more than enough.
            nodes_[i].next = static_cast<std::uint32_t>(i + 1);
        }
        // There is nothing after the last entry, so kEnd ends it.
        // The loop above is i + 1 < size() and deliberately left the last entry alone, for this
        // line.
        nodes_.back().next = kEnd;
        // The head of the free list points at entry 0, so the whole pool is available.
        free_ = 0;
    }

    // How many live orders the table holds now.
    // It only reads an internal count and walks nothing, so a caller may ask as often as it
    // likes.
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    // At most how many can be held. That is the node pool opened at construction.
    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.size(); }

    // Places an order. The same order id placed again is an overwrite.
    //
    // This is one of the main pieces, taking four cases in turn and leaving as soon as one fits:
    // the entry in the bucket is empty -> it moves straight in; the bucket holds this very order
    // -> overwrite; the overflow list holds it -> overwrite; none of them -> take an entry from
    // the free list and join it at the head.
    // A false comes back for two reasons only: zero shares, or the node pool being used up.
    bool insert(std::uint64_t oid, const Order& o) {
        // An order of zero shares means nothing and is turned away.
        // This line has a side effect as well: since a valid order certainly has non zero shares,
        // "shares equal zero" can double as the mark for "this entry is empty" and saves a flag
        // of its own.
        if (o.shares == 0) return false;
        // Work out which bucket it falls in and take a reference to that entry.
        // This line is the dearest in the whole class. The trader's default order count is
        // 12,582,912, which pushes the bucket count to 33,554,432, and at twenty four bytes an
        // entry the bucket array is 805 MB - the cache is tens of megabytes and order ids are
        // spread about, so nearly every one goes to memory, a hundred odd nanoseconds.
        // (PoolMap's entry is only four bytes, and the same bucket count there is 134 MB.
        //  The 671 MB of difference is why this version lost.)
        Slot& s = buckets_[bucket(oid)];
        // Case one: the entry in the bucket is empty and the first order moves straight in.
        // This is the most common path, at a load of only a half, and is the reason this
        // implementation exists.
        if (s.shares == 0) {
            // The order id is recorded. It has to be - more than one order id falls in the same
            // bucket, and a lookup uses it to confirm that what lives here really is the order
            // wanted.
            s.oid = oid;
            // The shares are recorded. A non zero value written makes this entry occupied.
            s.shares = o.shares;
            // The side and the price are squeezed into one thirty two bit field.
            s.side_price = pack(o);
            // One more live order in the table - this entry has just gone from empty to occupied.
            ++count_;
            // Placed. The caller sees a true, meaning this order can be found from now on.
            return true;
        }
        // Case two: the bucket already holds an order and it is this very order id, which is an
        // overwrite.
        // Clean market data should not place the same order id twice, and overwriting here keeps
        // the behaviour the same as PoolMap's, so that the two implementations do not work out
        // different books.
        if (s.oid == oid) {
            // The shares become the new ones.
            s.shares = o.shares;
            // The side and the price become the new ones.
            s.side_price = pack(o);
            // The overwrite is done and the count of live orders is unchanged - this order was
            // already in the table.
            return true;
        }
        // Case three: the bucket is taken by somebody else, so the overflow list is walked to see
        // whether this order id is already on it.
        // From the head, one entry at a time along next, until kEnd.
        for (std::uint32_t i = s.next; i != kEnd; i = nodes_[i].next) {
            // This entry on the list is the same order id, so it is an overwrite again.
            if (nodes_[i].oid == oid) {
                // The shares are overwritten as before.
                nodes_[i].shares = o.shares;
                // The side and the price are overwritten as before.
                nodes_[i].side_price = pack(o);
                // The overwrite is done and the count of live orders is unchanged.
                return true;
            }
        }
        // Case four: it is not on the list either, so this is an entirely new order and an entry
        // has to come from the free list.
        // Nothing to take means the node pool is used up. All that can be done here is return
        // false, which the layer above treats as a failed insert; a run that really reaches this
        // was given too small an orders at construction and the table has to be opened larger.
        if (free_ == kEnd) return false;
        // Take the head of the free list.
        const std::uint32_t i = free_;
        // The head of the free list moves along one.
        free_ = nodes_[i].next;
        // The three lines below write the order into the entry just taken, exactly as they would
        // into a bucket.
        // The order id first.
        nodes_[i].oid = oid;
        // The shares.
        nodes_[i].shares = o.shares;
        // The side and the price, squeezed into one thirty two bit field.
        nodes_[i].side_price = pack(o);
        // The two lines below put this entry at the front of the overflow list.
        // The new entry points at what was the head.
        nodes_[i].next = s.next;
        // And the bucket points at the new entry. At the head rather than the end, because there
        // is no walking to the end and it is one step.
        s.next = i;
        // One more live order in the table - this one rests on an overflow list.
        ++count_;
        // Inserted.
        return true;
    }

    // Looks an order up. Found, its content goes into out and a true comes back; not found gives
    // a false.
    //
    // This is the most important function of the whole class and its entire reason for being:
    // where the entry in the bucket is the order wanted, that one memory access brings back every
    // field, and there is no second hop.
    [[nodiscard]] bool find(std::uint64_t oid, Order* out) const {
        // This is that memory access. The point is that it brings back not only "where the head
        // of the list is" but the order id, the shares, the side and the price all together - a
        // cache line is sixty four bytes and an entry is twenty four, so one read has all of it
        // in hand.
        const Slot& s = buckets_[bucket(oid)];
        // Two things have to hold for a hit: somebody lives in this entry (the shares are not
        // zero), and who lives there is the one wanted.
        // It seems as though comparing the order id would be enough. It is not: an empty entry's
        // order id is zero, and should somebody really look up a zero it would hit an empty entry
        // and read an order that does not exist.
        if (s.shares != 0 && s.oid == oid) {
            // The two stored fields are restored into what the outside sees and written to the
            // caller.
            *out = unpack(s.shares, s.side_price);
            // Found in one hop.
            return true;
        }
        // Not in the bucket, so the overflow list has to be walked. This path is the original two
        // hops.
        for (std::uint32_t i = s.next; i != kEnd; i = nodes_[i].next) {
            // The order id matches.
            if (nodes_[i].oid == oid) {
                // Restored into what the outside sees as before.
                *out = unpack(nodes_[i].shares, nodes_[i].side_price);
                // Found, only one hop dearer.
                return true;
            }
        }
        // Neither in the bucket nor on the list.
        // That is not necessarily a fault: an order placed days ago and cancelled today has an
        // add message we never saw, so it was never in the table. On a false the layer above
        // skips this message.
        return false;
    }

    // Takes some shares off an order, and writes what it looked like before into before.
    //
    // Why the old shape has to be handed out: the layer above uses it to lower the resting
    // quantity on a price level - how much, at which price, and on the buy or the sell side all
    // follow from what that order looked like.
    // Taken to zero, or asked for more than is left, the order goes entirely.
    bool reduce(std::uint64_t oid, std::uint32_t shares, Order* before) {
        // The same path as find: work out the bucket and read it once.
        Slot& s = buckets_[bucket(oid)];
        // The entry in the bucket is looked at first.
        if (s.shares != 0 && s.oid == oid) {
            // The old shape is handed out before anything is changed - the other way round would
            // hand out a value already changed.
            *before = unpack(s.shares, s.side_price);
            // The shares to take off are not below what is there, so this order goes.
            if (shares >= s.shares) {
                // Handed to lift: it brings the first entry of the overflow list up to take this
                // place.
                lift(s);
            // Fewer to take off than are left in the bucket takes this path.
            } else {
                // The shares merely go down and the order stays.
                s.shares -= shares;
            }
            // Gone or merely reduced, either way the caller sees "found and handled".
            return true;
        }
        // The bucket is not it, so the overflow list is walked.
        //
        // The link below is a pointer at "where the previous entry's next field is".
        // It seems as though an index would do. The trouble is that taking an entry off the list
        // changes the next of the entry before it, and this list runs one way - having reached
        // entry i, there is no going back to find what was before it.
        // So it points at "the address of the previous next" the whole way, and taking an entry
        // off is one write to it.
        // To begin with the previous entry is the bucket itself, so it points at the bucket's
        // next.
        std::uint32_t* link = &s.next;
        // From the head to the end. Every step moves link along to the current entry's next.
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            // Not this order id, so it carries on.
            if (nodes_[i].oid != oid) continue;
            // Found. The old shape is handed out first as before.
            *before = unpack(nodes_[i].shares, nodes_[i].side_price);
            // Not below what is there, so the whole order comes off.
            if (shares >= nodes_[i].shares) {
                // The previous entry goes past this one straight to the next.
                *link = nodes_[i].next;
                // The entry goes back to the free list: it points at what was the head first.
                nodes_[i].next = free_;
                // And the head of the free list points at it. The two lines together are the
                // return.
                free_ = i;
                // This order was taken to nothing, so the count of live orders goes down.
                --count_;
            // Fewer to take off than are left on the list takes this path.
            } else {
                // Only the shares change.
                nodes_[i].shares -= shares;
            }
            // Found and handled. Note the return is outside the if/else - taken to nothing or
            // only partly, either way the caller sees "I handled that order".
            return true;
        }
        // Neither the bucket nor the list holds this order id. The layer above skips this
        // message.
        return false;
    }

    // Cancels an order whole, and writes what it looked like before into gone.
    // The same walk as reduce, without the branch for "only take a little off" - a cancel is
    // always the whole order.
    bool erase(std::uint64_t oid, Order* gone) {
        // The same memory access.
        Slot& s = buckets_[bucket(oid)];
        // The entry in the bucket is it.
        if (s.shares != 0 && s.oid == oid) {
            // The old shape is handed out first, for the layer above to lower the price level
            // with.
            *gone = unpack(s.shares, s.side_price);
            // The first entry of the overflow list is brought up to take this place.
            lift(s);
            // Cancelled. The caller takes gone and lowers the resting quantity on the price
            // level.
            return true;
        }
        // The list is walked with a "pointer at the next field" as before, for the same reason as
        // in reduce.
        std::uint32_t* link = &s.next;
        // From the head all the way to the end.
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            // Not the one wanted, so it carries on.
            if (nodes_[i].oid != oid) continue;
            // Found, and the old shape is handed out first.
            *gone = unpack(nodes_[i].shares, nodes_[i].side_price);
            // The previous entry goes past this one.
            *link = nodes_[i].next;
            // The entry goes back to the free list.
            nodes_[i].next = free_;
            // The head of the free list points at it.
            free_ = i;
            // One fewer on the list, so the count of live orders goes down.
            --count_;
            // This order is cancelled cleanly, only one hop dearer than the path through the
            // bucket.
            return true;
        }
        // Not found. As with find, it is most likely an order placed days ago and not a fault.
        return false;
    }

    // Sends out the memory requests a batch of order ids will need, reading nothing and changing
    // nothing.
    //
    // Why it helps: a memory access is a hundred odd nanoseconds, but the processor can have
    // twenty odd of them in flight at once.
    // So rather than "send a request, wait, send the next" one order at a time, a whole batch of
    // requests goes out first and they overlap on the way, and by the time they are really needed
    // the data is in cache.
    // But it has a lower limit: with only one or two in hand at a time, a batch is 7-9% slower
    // than one at a time, and it saturates at about thirty (some +11%). Measured, the number of
    // messages a poll has in hand has a p50 of only 1 and reaches 33 at the p99.9 - so this kind
    // of batching can only treat the tail and never the median.
    // This implementation has one hop (the order is in the bucket), so one round of requests is
    // enough; PoolMap has two hops and needs two rounds, and the second has to wait for the first
    // to come back.
    void ask_for_all(const std::uint64_t* oids, std::size_t n) const {
        // One request per order id. This loop has no dependency in it, so all of them can be in
        // flight together.
        for (std::size_t j = 0; j < n; ++j) {
            // __builtin_prefetch is the compiler's built in prefetch, and its three arguments
            // are: the address; 0 for "only meaning to read" (1 is meaning to write, which asks
            // for exclusive ownership and is dearer); and 3 for "the highest locality", meaning
            // fetch it into the cache closest to the core (L1).
            // It returns nothing and cannot fail - even a wild address only sends one request for
            // nothing.
            __builtin_prefetch(&buckets_[bucket(oids[j])], 0, 3);
        }
    }

// Everything below is for internal use. The outside should not touch it and does not need to know
// what a bucket looks like.
private:
    // The mark for the end of a list, which also means the free list is empty.
    // Why not 0 as the mark: 0 is a valid index, the first entry of the node pool.
    // Thirty two bits all set is four thousand two hundred million and more, and the node pool
    // cannot be opened that large, so it is the safest mark.
    static constexpr std::uint32_t kEnd = 0xffffffffu;

    // One entry of the bucket array. It holds the first order and an index into the overflow
    // list.
    //
    // An entry is twenty four bytes, and the smallest piece a processor fetches from memory - one
    // cache line - is sixty four. So reading the bucket brings the whole order back and there is
    // no second hop - which is what the whole implementation rests on.
    //
    // But it is not that clean: twenty four does not divide sixty four, so the entries lie
    // misaligned in memory. Working it out, two of every eight entries - a quarter - straddle two
    // cache lines and need two lines read. That is one of the reasons this version lost in the
    // end.
    struct Slot {
        // The order id of the order in this entry.
        std::uint64_t oid = 0;
        // How many shares are left. A zero also means this entry is empty, see the note in
        // insert.
        std::uint32_t shares = 0;
        // The side and the price squeezed together: the top bit is the side and the other thirty
        // one are the price.
        std::uint32_t side_price = 0;
        // The index of the head of the overflow list. kEnd means this bucket has no second order.
        std::uint32_t next = kEnd;
        // Padding to twenty four bytes. The compiler would add it by itself without this line
        // (there is an eight byte field, so the structure's size is aligned to eight); writing it
        // out only makes the twenty four bytes plain at a glance.
        std::uint32_t pad = 0;
    // The end of a bucket entry.
    };

    // One entry of the overflow list.
    // It seems odd that the fields are identical to Slot's above and the type is not shared.
    // They are apart because their roles differ (one is the bucket itself and the other an entry
    // on a list); that they look alike is deliberate - the code of the two paths can be copied
    // across, and changing one place is less likely to miss the other.
    struct Node {
        // The order id.
        std::uint64_t oid = 0;
        // How many shares are left.
        std::uint32_t shares = 0;
        // The side and the price, squeezed into one thirty two bit field.
        std::uint32_t side_price = 0;
        // The next entry on the list. While this entry is on the free list, this field strings
        // the free list as well.
        std::uint32_t next = kEnd;
        // Padding to line up with Slot.
        std::uint32_t pad = 0;
    // The end of one entry on the list.
    };

    // Squeezes the side and the price into one thirty two bit field.
    // The side goes in the top bit and the price takes the other thirty one.
    // Are thirty one bits enough: the measured largest price of a whole day is 1,999,999,900 (a
    // hundred and ninety nine thousand nine hundred and ninety nine dollars and ninety nine
    // cents, in hundredths of a cent), and thirty one bits hold two thousand one hundred million
    // and more, so yes.
    static std::uint32_t pack(const Order& o) {
        // The side is lifted to bit thirty one and the price ored in.
        // It is widened to thirty two bits before the shift - shifting an eight bit value that
        // far is undefined behaviour.
        return (static_cast<std::uint32_t>(o.side) << 31) | o.price;
    }
    // The reverse of pack: restore the two stored fields into the Order the outside sees.
    static Order unpack(std::uint32_t shares, std::uint32_t sp) {
        // The shares are copied; the price is the low thirty one bits, taken with a mask; the
        // side is the top bit, shifted right by thirty one.
        return Order{shares, sp & 0x7fffffffu, static_cast<std::uint8_t>(sp >> 31)};
    }

    // Once the entry in a bucket is cancelled, the first entry of the overflow list is brought up
    // to take its place.
    //
    // Why it has to be brought up rather than marking the bucket empty and leaving the list
    // hanging: a lookup decides "this bucket holds no order at all" by the entry in the bucket
    // being empty.
    // An empty bucket with orders still on the list would mean those orders could never be found
    // again.
    void lift(Slot& s) {
        // One live order fewer.
        --count_;
        // See whether the list has a next one.
        const std::uint32_t i = s.next;
        // The list is empty too, so this bucket really holds nothing.
        if (i == kEnd) {
            // The order id is cleared.
            s.oid = 0;
            // The shares are cleared - this line is the mark for "this entry is empty", and
            // without it the next lookup would hit it wrongly.
            s.shares = 0;
            // The side and the price are cleared as well, so that an old value does not confuse
            // somebody reading the code.
            s.side_price = 0;
            // An early end. This entry is a clean empty one and the moving below is not needed.
            return;
        }
        // The four lines below move the whole content of the list's first entry into the bucket.
        // The order id.
        s.oid = nodes_[i].oid;
        // The shares.
        s.shares = nodes_[i].shares;
        // The side and the price.
        s.side_price = nodes_[i].side_price;
        // The bucket's head moves along one - the entry that was emptied comes off the list.
        s.next = nodes_[i].next;
        // The entry taken off goes back to the free list: it points at what was the head first.
        nodes_[i].next = free_;
        // And the head of the free list points at it.
        free_ = i;
    }

    // Which bucket an order id falls in.
    //
    // It seems as though order ids would be spread about already and could be used directly. They
    // are not - the exchange gives them out in sequence (1, 2, 3 ...). Used directly, the orders
    // alive at any one moment would all crowd into a few neighbouring buckets: some buckets with
    // a long chain and great stretches always empty.
    // Multiplying by a large odd number to mix the bits first, and then taking the top bits, is
    // what spreads them.
    [[nodiscard]] std::size_t bucket(std::uint64_t oid) const noexcept {
        // That constant is the golden ratio taken to sixty four bits, a multiplier commonly used
        // in hashes.
        // The multiplication overflows, and it is meant to - the top bits carry everything the
        // low bits held, so shifting right for the top bits spreads better than taking the low
        // ones. How far to shift is settled by the shift_ worked out at construction.
        return (oid * 0x9E3779B97F4A7C15ull) >> shift_;
    }

    // The bucket array. Opened to twice the order count, so the vast majority of buckets hold one
    // order.
    // This row is the reason it was turned down: at twenty four bytes an entry it is 805 MB with
    // the trader's configuration, 671 MB more than PoolMap's 134 MB. What that extra took pushed
    // the price space out of the cache.
    std::vector<Slot> buckets_;
    // The node pool of the overflow lists. Only a second or later order colliding into one bucket
    // ever uses it.
    std::vector<Node> nodes_;
    // The head of the free node list. An insert takes an entry from here and a delete returns
    // one.
    // kEnd means the pool is used up, and then insert can only fail.
    std::uint32_t free_ = kEnd;
    // How many live orders are held now. size() reads this.
    std::size_t count_ = 0;
    // How far a hash has to shift right. Worked out from the bucket count at construction and
    // never changed after.
    int shift_ = 64;
// The end of the class.
};

}  // namespace book
