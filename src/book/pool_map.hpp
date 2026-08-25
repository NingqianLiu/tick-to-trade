// PoolMap: given an order id, what is left of that order - shares, price, side, and
// which security it belongs to.
//
// Everything it needs is allocated once at start up: an array of bucket heads and a pool
// of nodes. After that the feed only reuses that memory; nothing on the hot path calls
// malloc. order_book.hpp drives it with adds, executions, cancels, deletes and replaces,
// and the tests use the same interface to compare it against the other implementations.
#pragma once

// std::size_t for the order capacity, the bucket count and the current number of orders.
#include <cstddef>
// Fixed width integers, so an order id, a share count, a price and a node index all have
// the size the protocol and the pool need.
#include <cstdint>

// huge::Buffer, which is how both the node pool and the bucket array get contiguous
// memory on huge pages at start up.
#include "common/huge.hpp"

namespace book {

// Order ids that hash to the same bucket are chained together through a next index in
// each node. A node that is deleted goes onto a free list and the next insert takes it
// back.
//
// The pool never grows. When the nodes run out, insert returns false and the layer above
// counts a full table, which voids the run - growing would mean allocating and
// redistributing exactly when the market is busiest.
//
// The book only ever sees insert, find, reduce and erase; it never gets a node or an
// index. That is deliberately the same interface the other two implementations offer, so
// comparing them needs no change above.
class PoolMap {
public:
    // One order as the rest of the program sees it. Inside, the side and the price share
    // a word; here they are three plain fields.
    struct Order {
        // Shares still resting. A delete message carries no share count, so this is where
        // the book learns how much to take off the price level.
        std::uint32_t shares;
        // The price as an integer in the units of the wire, never converted to dollars or
        // to floating point on the hot path. Every price in the data is below 2^31, which
        // leaves the top bit free for the side.
        std::uint32_t price;
        // 0 buys, 1 sells.
        std::uint8_t side;
        // Which security the order belongs to.
        //
        // The slot based calls below need it: by the time they run they hold a slot
        // number and no message, and taking the order off a price level means knowing
        // which security it was. The message at a time path still reads the security out
        // of the message header and ignores this field.
        std::uint16_t sym;
    };

    // Sizes everything from the number of orders that may rest at once: bucket heads
    // rounded up to a power of two, the shift the hash needs, a fixed pool of nodes, and
    // every node threaded onto the free list.
    //
    // It can allocate huge pages and touch all of that memory, so it belongs to start up.
    // If the allocation fails the program does not start, rather than trading with a
    // table that is too small.
    explicit PoolMap(std::size_t orders) {
        // At least sixteen buckets, so a small test does not end up with one or two.
        std::size_t n = 16;
        // Double until it holds them all. Starting from a power of two and only doubling
        // keeps it a power of two, which is what lets bucket() simply take the top bits.
        while (n < orders) n <<= 1;
        // Every bucket starts empty, so a lookup before the first insert stops at once.
        // A head is a 32 bit node index rather than a pointer, which keeps it small.
        buckets_.assign(n, kEnd);
        // How far right to shift a 64 bit hash to leave log2(n) bits. With n = 16 the
        // loop runs four times and the shift ends at 60, so a hash lands in 0 to 15.
        shift_ = 64;
        for (std::size_t c = n; c > 1; c >>= 1) --shift_;

        // As many nodes as orders, with a floor of sixteen so nodes_.back() below always
        // exists. Node indices stay valid for the life of the object.
        nodes_.resize(orders < 16 ? 16 : orders);
        // Thread the free list: every node points at the one after it. These are not
        // bucket chains yet - at this point every node is free.
        for (std::uint32_t i = 0; i + 1 < nodes_.size(); ++i) {
            nodes_[i].next = i + 1;
        }
        // The last node ends the list, which is also how insert learns the pool is empty.
        nodes_.back().next = kEnd;
        // The first insert takes node zero.
        free_ = 0;
    }

    // Orders resting right now. Overwriting an existing id does not change it.
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    // Nodes allocated at start up; it never changes. Free capacity is this minus size().
    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.size(); }

    // Inserts an order, or overwrites one that is already there.
    //
    // It walks only the chain the id hashes to. An id already on that chain is
    // overwritten in place, taking no new node and leaving the count alone; otherwise a
    // node comes off the free list and goes on the head of the chain. A false means the
    // pool is empty, and nothing has been changed.
    bool insert(std::uint64_t oid, const Order& o) {
        // An order of zero shares does not belong in the book, and refusing it here also
        // keeps a zero share count from ever looking like a live order.
        if (o.shares == 0) return false;
        // Hashed once and used both for the duplicate check and for the insert below.
        const std::size_t b = bucket(oid);
        // Look for the id on this chain. Several different ids can share a bucket, so the
        // full 64 bits are compared, not the bucket number.
        //
        // This has to come before the "pool is empty" check further down: overwriting an
        // order that is already there must still work when there are no free nodes left.
        for (std::uint32_t i = buckets_[b]; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid == oid) {
                // The new state replaces the old one; this is not an addition to it.
                nodes_[i].shares = o.shares;
                nodes_[i].side_price = pack(o);
        // A recycled node still holds the previous order's security, so it is written in
        // full rather than left alone.
        nodes_[i].sym = o.sym;
                nodes_[i].sym = o.sym;
                return true;
            }
        }
        // The id is new, so it needs a node. An empty free list is a full table: the
        // caller counts it and the run is void. Nothing has been touched yet, so the
        // existing orders and chains are unharmed.
        if (free_ == kEnd) return false;
        // Take the head of the free list, then move the list on. Detaching before filling
        // keeps a node from ever being on both lists at once.
        const std::uint32_t i = free_;
        free_ = nodes_[i].next;
        // A recycled node holds someone else's values, so every field is written.
        nodes_[i].oid = oid;
        nodes_[i].shares = o.shares;
        nodes_[i].side_price = pack(o);
        // The new node points at the old head, so nothing else has to move; then the head
        // becomes this node, which is the point at which a lookup can find it.
        nodes_[i].next = buckets_[b];
        buckets_[b] = i;
        ++count_;
        return true;
    }

    // The five calls below work by slot number, for building the book in passes.
    //
    // A pass finds a slot early and comes back to read or write it several passes later.
    // Going through the id every time would mean hashing again and walking the chain
    // again, while a slot number never changes once it is known: the pool is fixed at
    // start up, and deleting an order only returns its node to the free list.
    //
    // That holds as long as nothing is inserted in between, or a returned node could be
    // taken by someone else. This is why the passes put every add ahead of every delete.
    //
    // They are additions: the id based calls above are untouched and still used.

    // What find_slot returns when there is nothing to find.
    //
    // It is the same value as the internal end of chain marker, which is declared private
    // and so cannot be named here. The node count is far below it, so it can never be a
    // real slot.
    static constexpr std::uint32_t kNoSlot = 0xffffffffu;

    // Inserts and says which slot it landed in. An id already there is overwritten in its
    // existing slot; an empty pool gives kNoSlot. The only difference from insert() above
    // is what comes back: success there, a position here.
    std::uint32_t insert_at(std::uint64_t oid, const Order& o) {
        if (o.shares == 0) return kNoSlot;
        const std::size_t b = bucket(oid);
        // Already on this chain: overwrite in place, count unchanged.
        for (std::uint32_t i = buckets_[b]; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid != oid) continue;
            nodes_[i].shares = o.shares;
            nodes_[i].side_price = pack(o);
            nodes_[i].sym = o.sym;
            return i;
        }
        if (free_ == kEnd) return kNoSlot;
        const std::uint32_t i = free_;
        // Off the free list first, then filled, so it is never on two lists at once.
        free_ = nodes_[i].next;
        nodes_[i].oid = oid;
        nodes_[i].shares = o.shares;
        nodes_[i].side_price = pack(o);
        nodes_[i].sym = o.sym;
        // On the head of the chain, so nothing has to be walked to find the end.
        nodes_[i].next = buckets_[b];
        buckets_[b] = i;
        ++count_;
        return i;
    }

    // Which slot an order is in, or kNoSlot. It deliberately does not copy the contents
    // out: copying would give up the whole point of remembering the slot.
    [[nodiscard]] std::uint32_t find_slot(std::uint64_t oid) const {
        for (std::uint32_t i = buckets_[bucket(oid)]; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid == oid) return i;
        }
        return kNoSlot;
    }

    // Reads a slot. The caller has to know the slot was found in this batch and nothing
    // has deleted it since.
    [[nodiscard]] Order at(std::uint32_t slot) const { return unpack(nodes_[slot]); }

    // Changes the shares in a slot, for the pass that takes shares off.
    void set_shares_at(std::uint32_t slot, std::uint32_t shares) { nodes_[slot].shares = shares; }

    // Changes the side and the security of a slot. The replace pass builds the new order
    // with no side and writes the side back here once the old order gives it up.
    void set_side_sym_at(std::uint32_t slot, std::uint8_t side, std::uint16_t sym) {
        // The side is the top bit; the price in the low 31 is left as it is.
        nodes_[slot].side_price =
            (static_cast<std::uint32_t>(side) << 31) | (nodes_[slot].side_price & 0x7fffffffu);
        nodes_[slot].sym = sym;
    }

    // Removes the order in a slot.
    //
    // The chain still has to be walked to find what points at this node - that is what a
    // singly linked list costs. What is saved is the hashing and the 64 bit comparisons:
    // this compares slot numbers, which is one instruction.
    void erase_at(std::uint32_t slot) {
        std::uint32_t* link = &buckets_[bucket(nodes_[slot].oid)];
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            if (i != slot) continue;
            unlink(link, i);
            return;
        }
    }

    // Issues the memory requests a whole batch of ids will need, reading nothing and
    // changing nothing.
    //
    // Why it exists: a lookup is two hops - read the bucket for the head, then follow it
    // into the node pool - and the address of the second is not known until the first
    // comes back. One message chained like that is around two hundred nanoseconds, and
    // forty two of them one after another is eight or nine microseconds, which is most of
    // the tail. But those forty two do not depend on each other, and the memory
    // controller can have a couple of dozen requests outstanding at once. So the requests
    // all go out first, overlap while they are in flight, and the messages are applied
    // afterwards, by which time the data has arrived.
    //
    // Why not the usual shape, looking everything up and keeping the answers: that
    // reorders the work, and inside one batch an order can be deleted and then referred
    // to again. This only asks; the caller keeps the order of application exactly as it
    // was.
    void ask_for_all(const std::uint64_t* oids, std::size_t n) const {
        // First pass: addresses only, no reads at all, so the n requests go out together
        // rather than one waiting for the next.
        for (std::size_t j = 0; j < n; ++j) {
            __builtin_prefetch(&buckets_[bucket(oids[j])], 0, 3);
        }
        // Second pass: read the bucket head and immediately ask for the node it points
        // at. By now most of the first pass has arrived, so there is little waiting here.
        for (std::size_t j = 0; j < n; ++j) {
            const std::uint32_t i = buckets_[bucket(oids[j])];
            if (i != kEnd) __builtin_prefetch(&nodes_[i], 0, 3);
        }
    }

    // Looks an order up. A true means out was filled in; a false leaves it untouched, so
    // the caller has to check the return before reading it.
    [[nodiscard]] bool find(std::uint64_t oid, Order* out) const {
        // Only this one chain is walked, so the cost depends on its length rather than on
        // the size of the pool. The hash narrows the search; the full id decides it.
        for (std::uint32_t i = buckets_[bucket(oid)]; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid == oid) {
                // A copy, so the caller keeps a valid Order even after the node is reused.
                *out = unpack(nodes_[i]);
                return true;
            }
        }
        // Not found. out is deliberately left alone rather than zeroed, which would be a
        // pointless write. On a clean feed this means a message about an unknown order,
        // and the book counts it as an orphan.
        return false;
    }

    // Takes shares off an order, for an execution or a partial cancel.
    //
    // The state before the change goes into before, which is what the book needs to take
    // the same shares off the right price level. Taking everything that is left removes
    // the node; anything less just changes the count in place.
    bool reduce(std::uint64_t oid, std::uint32_t shares, Order* before) {
        const std::size_t b = bucket(oid);
        // link always points at the place the current node index is stored: the bucket
        // head for the first node, the previous node's next for the rest. That way a hit
        // can unlink by writing through link, with no separate case for the head of the
        // chain and no need to keep a previous index.
        std::uint32_t* link = &buckets_[b];
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            // A different order that happens to share the bucket.
            if (nodes_[i].oid != oid) continue;
            // Copied out before anything changes, and before any unlink: a node that is
            // freed can be handed to the very next insert.
            *before = unpack(nodes_[i]);
            // Taking at least everything left ends the order. Using >= also tolerates a
            // caller asking for more than there is, and keeps the unsigned subtraction
            // below from wrapping.
            if (shares >= nodes_[i].shares) {
                unlink(link, i);
            } else {
                nodes_[i].shares -= shares;
            }
            return true;
        }
        // Nothing on the chain: the layer above counts an orphan, and before was not
        // written, so it must not be used.
        return false;
    }

    // Removes an order by id and hands back what it was. This is what a delete message
    // uses, and the old half of a replace.
    //
    // A delete carries neither shares nor price, so the caller has to take both out of
    // gone to update the price level. A replace erases the old id here and then inserts
    // the new one.
    bool erase(std::uint64_t oid, Order* gone) {
        // Same pointer to the link as in reduce, so removing the head of a chain and
        // removing a node in the middle are one piece of code.
        std::uint32_t* link = &buckets_[bucket(oid)];
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            if (nodes_[i].oid != oid) continue;
            // Copied out before the node is recycled, since a freed node can be
            // overwritten almost immediately.
            *gone = unpack(nodes_[i]);
            unlink(link, i);
            return true;
        }
        // Not found: the caller records an unknown order rather than using gone, which
        // was left as it was.
        return false;
    }

private:
    // One node of the pool. While it holds an order it is on a bucket chain; while it is
    // free it is on the free list, and next serves both. Nodes are joined by index rather
    // than by pointer, and the buffer never moves after it is built.
    //
    // A freed node keeps its old values: nothing reads them, and the next insert writes
    // every field before the node becomes reachable again.
    struct Node {
        // The full 64 bits, which is what tells two orders in the same bucket apart. A
        // node is valid because of the list it is on, not because this is non zero.
        std::uint64_t oid = 0;
        // Shares left; above zero for as long as the node holds an order.
        std::uint32_t shares = 0;
        // The side in the top bit and the price in the low 31, so the side costs no
        // padding of its own and both come back in one read.
        std::uint32_t side_price = 0;
        // The next node of whichever list this one is on. kEnd ends both of them, so no
        // extra flag is needed to say which.
        std::uint32_t next = 0;
        // Which security this order belongs to.
        //
        // It is free: the four fields above come to twenty bytes, which eight byte
        // alignment rounds up to twenty four anyway, and these two bytes sit in that
        // padding. Adding it left sizeof(Node) exactly where it was.
        std::uint16_t sym = 0;
    };

    // End of a chain. 0xffffffff cannot be a real node index, and the pool is far smaller
    // than that, so one value serves both lists.
    static constexpr std::uint32_t kEnd = 0xffffffffu;

    // Side into the top bit, price into the low 31. Every price in the data is below
    // 2^31, so the two cannot run into each other.
    static std::uint32_t pack(const Order& o) {
        return (static_cast<std::uint32_t>(o.side) << 31) | o.price;
    }
    // The other direction, used by find, reduce and erase, so all three read a node the
    // same way. The result is an independent Order rather than a view of the node.
    static Order unpack(const Node& n) {
        return Order{n.shares, n.side_price & 0x7fffffffu,
                     static_cast<std::uint8_t>(n.side_price >> 31),
                     n.sym};
    }

    // Fibonacci hashing: multiply, then keep the top bits.
    //
    // The multiplication mixes the bits of ids that are close together, which matters
    // because the exchange hands them out in sequence, and the shift keeps as many top
    // bits as there are buckets. No division, so a lookup costs one multiply and one
    // shift. The same hash is used by the other implementation, so a comparison between
    // them is about how collisions are handled and nothing else.
    [[nodiscard]] std::size_t bucket(std::uint64_t oid) const noexcept {
        return (oid * 0x9E3779B97F4A7C15ull) >> shift_;
    }

    // Takes node i off its bucket chain and puts it on the front of the free list.
    //
    // Writing through link steps over i whether it was the head of the chain or in the
    // middle. Putting it on the front means the next insert takes the node that was just
    // touched, which is the one most likely to still be in cache.
    void unlink(std::uint32_t* link, std::uint32_t i) {
        *link = nodes_[i].next;
        // Its old order fields can stay: the free list does not read them.
        nodes_[i].next = free_;
        free_ = i;
        --count_;
    }

    // Every node. Contiguous memory on huge pages rather than a malloc per order, and the
    // size never changes after the constructor.
    huge::Buffer<Node> nodes_;
    // One chain head per bucket, kEnd when empty. Kept apart from the nodes so that
    // hashing is followed by reading only a small 32 bit head.
    huge::Buffer<std::uint32_t> buckets_;
    // Orders resting, not counting nodes on the free list.
    std::size_t count_ = 0;
    // The next node available, kEnd when the pool is exhausted.
    std::uint32_t free_ = kEnd;
    // How far right a hash is shifted, worked out in the constructor from the bucket
    // count.
    int shift_ = 64;
};

}
