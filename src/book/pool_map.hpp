// This file implements PoolMap, which finds an order's remaining shares, price and side by its
// order id.
// It prepares the buckets and the nodes once at start up, and once market data arrives it only
// reuses that memory and never allocates on the hot path.
// order_book.hpp uses it for adds, executions, cancels, deletes and replaces; the tests use the
// same interface to compare it against OrderMap.

// This file is included indirectly by several .cpp files; expanding it once avoids defining the
// class and its inline methods more than once.
#pragma once

#include <cstddef>
#include <cstdint>

// common/huge.hpp provides huge::Buffer; both the node pool and the bucket array use it to ask
// for a continuous block of huge pages at start up.
#include "common/huge.hpp"

// This namespace puts the book's data structures in the book module, so that PoolMap does not
// collide with names in other modules.
namespace book {

// PoolMap holds the correspondence between an order id and an order's state.
// Order ids with the same hash land in the same bucket and are joined into a singly linked list
// by the next field in each node.
// A node that is deleted goes onto a separate free list, and the next insert takes it straight
// back.
// The capacity never grows by itself, so running out of nodes returns false plainly and the
// layer above marks that round as table full.
// The order book only uses insert, find, reduce and erase, and never sees a Node or a list
// index.
// That interface matches OrderMap's, so comparing the two ways of handling hash collisions later
// does not mean changing anything above.
class PoolMap {
// The public part is what the order book sees; how the nodes are joined stays in the private
// part.
public:
    // Order is one order's state as PoolMap presents it.
    // Inside, the side and the price are packed into one integer, while the outside still sees
    // three separate fields that are easy to use.
    // An add builds one, find fills one in, and reduce and erase use one to hand back the order
    // as it was before the change.
    // The security number is not kept here, because the common header of every ITCH message
    // carries it already.
    struct Order {
        // How many shares of this order are left; reduce lowers it, and reaching zero removes
        // the whole order.
        // A delete message carries no share count, so the order book has to get from here how
        // much to take off the price level.
        // uint32_t is as wide as the ITCH shares field and holds the largest single quantity in
        // the data.
        std::uint32_t shares;
        // This is the integer price from the ITCH message, which the order book uses to find the
        // price level to change.
        // It stays in the format of the wire, an integer with four decimals, and is never turned
        // into dollars or a floating point number on the hot path.
        // Prices in the data are below 2^31, so the top bit that is left can be given to the
        // side.
        std::uint32_t price;
        // The side tells buy from sell with 0 and 1; pack puts it in the top bit of side_price.
        // The order book works it out from ITCH's B or S, and a delete uses it again to choose
        // the buy or the sell array.
        // A uint8_t on the outside makes it plain that it is not part of the shares or the
        // price.
        std::uint8_t side;
        // Which security this order belongs to.
        // The interface that works by slot number below - insert_at, at, erase_at - has to have
        // it: by the time those run, all that is in hand is a slot number and not the message,
        // and taking this order off a price level means knowing which security it is.
        // The path that applies one message at a time still takes the security number from the
        // message's common header and does not read this field.
        std::uint16_t sym;
    };

    // It rounds the number of buckets up to a power of two first, and then works out how far the
    // multiplying hash has to shift right.
    // Then it asks for a fixed number of nodes and strings all of them into a free list by
    // index.
    // Once construction is done, the ordinary market data path only changes fields in arrays and
    // never grows or moves a node.
    // explicit keeps a plain integer from being turned into a PoolMap by accident in a call.
    // This step may ask for huge pages and touch the whole block, so it belongs in the trader's
    // start up and nowhere else.
    // A failure at start up means the program does not start at all; it never enters a run with
    // a table that is too small.
    explicit PoolMap(std::size_t orders) {
        // At least 16 buckets are kept; a small test does not degenerate into one or two
        // buckets.
        // n exists only during construction, and its final value shows up in the size of
        // buckets_ and in shift_.
        std::size_t n = 16;
        // While n cannot hold orders, it doubles.
        // n starts as a power of two and only doubles, so it is still a power of two at the end.
        // The final n is the smallest usable value not below orders, which is what lets bucket()
        // simply take the top bits of the hash.
        // The body of the loop is only the shift; buckets_ is initialised once the condition is
        // false.
        // With orders at 16 or below the loop does not run at all and n stays 16.
        // Above that it doubles each time, so it never gives a count like 24 or 48 that could
        // not be taken as plain bits.
        // This only affects the cost of starting up; the number of buckets is not worked out
        // again on every lookup.
        while (n < orders) n <<= 1;
        // Create n bucket heads and fill them all with kEnd, meaning there is no list of orders
        // yet.
        // This assign both asks for the memory and initialises it, and the number of buckets
        // never changes after an insert.
        // Each entry holds one 32 bit node index rather than a pointer, so the format survives
        // the node pool's address changing.
        // When construction ends, find starting from any bucket sees kEnd at once and returns
        // not found.
        buckets_.assign(n, kEnd);
        // It starts at 64 and the loop below brings it down to 64-log2(n).
        // The field's default is 64 as well, but it is written again here so that construction
        // does not depend on the object's previous state.
        // What lookups really use is the value after the loop rather than this starting point.
        shift_ = 64;
        // c halves each time, so the loop runs exactly log2(n) times.
        // shift_ comes down with it, so shifting right by it in bucket() leaves log2(n) top
        // bits.
        // With n=16, for instance, it runs four times and ends with shift_=60, and the hash
        // lands between 0 and 15.
        // n is already a power of two, so a bit can be neither lost nor gained here.
        // c only counts how many times n can be shifted right and is not kept in the PoolMap.
        // Each turn uses the current c to decide, and then does c>>=1 and --shift_.
        // It ends when c becomes 1, because the log2 has all been counted by then.
        for (std::size_t c = n; c > 1; c >>= 1) --shift_;

        // The number of nodes is orders, but at least 16 are asked for, so that nodes_.back()
        // never faces an empty array.
        // resize only runs at start up, and after it every node's index stays where it is.
        // The true branch handles orders below 16 and the false branch asks for what the caller
        // gave as the limit.
        // Storing them continuously puts neighbouring nodes at nearby addresses, and lets a
        // 32 bit index join both kinds of list.
        // No extra nodes are asked for on account of the bucket count, so capacity() still means
        // how many orders can really be held.
        nodes_.resize(orders < 16 ? 16 : orders);
        // This is the body of building the free list.
        // i walks from 0 to the second to last entry, pointing each entry's next at the entry
        // physically after it.
        // next here does not yet mean a collision list, because every node is free at this
        // point.
        // The condition i+1<size also guarantees the next entry exists and the last one is never
        // run past.
        // After the loop the last entry is given kEnd separately, which completes the free list.
        // oid, shares and side_price have no meaning as an order yet, and this only has to
        // initialise next.
        // The first turn writes 0->1 and the last writes second to last -> last.
        // nodes_ has at least 16 entries, so the loop can always build the list safely.
        for (std::uint32_t i = 0; i + 1 < nodes_.size(); ++i) {
            // There are free nodes after this one, so the index of the next entry is recorded in
            // next.
            // When insert takes node i, it reads the new free_ from here first and then changes
            // next into a link in a bucket's list.
            // Both i and i+1 are 32 bit indices, with no pointer conversion and no allocation
            // per node.
            nodes_[i].next = i + 1;
        }
        // There is no node after the last entry, so kEnd says the free list ends here.
        // When the last entry is taken by an insert as well, free_ reads kEnd from this field.
        // That gives a plain place to tell "the capacity is exactly used up" from "running past
        // the end of the array".
        nodes_.back().next = kEnd;
        // Index 0 is the first node of the free list, and the first insert takes it.
        // count_ is still zero here, meaning every node is on the free list and none on a
        // bucket's list.
        // Every unlink afterwards puts the node it just freed at the front of this list.
        free_ = 0;
    }

    // Returns how many orders are live now; an insert of a new order raises it and an unlink on
    // a delete lowers it.
    // Neither find nor overwriting an existing order changes it, and the tests use it to check
    // that the list operations come in pairs.
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    // Returns how many nodes were prepared at start up; it does not change while the object
    // lives.
    // It is not how many are free now; what is left is capacity() less size().
    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.size(); }

    // This is the body of inserting or overwriting an order.
    // It turns away an order of zero shares first, and then looks only at the one bucket's list
    // that this order id maps to.
    // Finding the same order id overwrites the state there, adding no node and leaving count_
    // alone.
    // Not finding it takes a node from free_, fills its fields in, and joins it at the front of
    // the bucket's list.
    // A node pool that is used up returns false, and both the existing orders and the lists stay
    // as they were.
    // Both a successful overwrite and a successful addition return true, and the caller may go
    // on to update the price level.
    // o comes in by const reference and this method only copies three of its fields into a node.
    // A new node goes in at the head, so the collision list does not have to be walked again to
    // find its end.
    bool insert(std::uint64_t oid, const Order& o) {
        // With zero shares this order should not stay in the order book.
        // A true returns false at once, looking at no list and taking no node; a false carries
        // on.
        // This test also guarantees that a shares=0 in a node can never be taken as a live
        // order.
        // If an order id with zero shares was already there, the old order is not overwritten
        // with zero either; the caller gets a failure.
        // The return happens before the hash is worked out, so invalid input touches neither
        // buckets_ nor nodes_.
        // The caller must not read every false as "the table is full" and has to make sure it
        // never submits an add of zero shares.
        if (o.shares == 0) return false;
        // Hash the order id once and keep the one bucket index it can possibly be in.
        // Both the check for a duplicate and the insertion at the head below use the same b,
        // rather than doing the 64 bit multiplication twice.
        // const keeps the bucket from being changed by accident while the list is walked.
        // The range bucket() returns is guaranteed by n and shift_ from construction.
        const std::size_t b = bucket(oid);
        // This is the body of looking for a duplicate order id.
        // i starts at the bucket's head and moves along next, and kEnd means the end of the list
        // has been reached.
        // One bucket may hold several different order ids, so every node compares the whole
        // 64 bit order id.
        // Finding it overwrites and returns inside the loop; reaching the end goes on to the
        // path that adds a node.
        // On a live node, nodes_[i].next always means the next entry of the same bucket.
        // The loop keeps no previous node, because overwriting does not remove the current one.
        // An empty bucket makes i start as kEnd and the body does not run.
        // This search has to come before the test for a full table: even with no free nodes,
        // overwriting an existing order id should still succeed.
        for (std::uint32_t i = buckets_[b]; i != kEnd; i = nodes_[i].next) {
            // A true means this node is the same order; a false is only a hash collision and the
            // walk goes on to next.
            // The whole order id is compared rather than the bucket index, so colliding orders
            // cannot overwrite one another.
            // On a false nothing is written this turn, and the for's update reads next.
            if (nodes_[i].oid == oid) {
                // The new state of the same order id replaces the old remaining shares rather
                // than being added to it.
                // o.shares has passed the non zero test, so the node stays valid after the
                // overwrite.
                // Whether the price level has to change as well is for the layer above; PoolMap
                // only holds the order's state.
                nodes_[i].shares = o.shares;
                // The new side and price are overwritten together, so that nothing of the old
                // fields is left.
                // The result of pack covers exactly one 32 bit field and changes neither oid nor
                // next.
                // Even with a changed side or price, the node stays in the same order id's
                // bucket.
                nodes_[i].side_price = pack(o);
        // A free node still holds the security number of the previous order, so it is written
        // out in full here.
        nodes_[i].sym = o.sym;
                // The security number is overwritten too: this entry used to hold somebody
                // else's order.
                nodes_[i].sym = o.sym;
                // The order has been updated; no free node is taken and neither count_ nor the
                // position in the list changes.
                // What the caller sees after a true is the complete new state rather than "found
                // but not changed".
                // The return also stops a second node with the same order id - which cannot
                // exist - from being visited.
                return true;
            }
        }
        // Reaching here the order id does not exist and a new node is needed.
        // free_=kEnd means every node prepared is in use; a true returns false and only a false
        // can take a node safely.
        // Nothing in any bucket or in free_ has been changed before the failure, so no existing
        // data is damaged.
        // The layer above counts this false as table full rather than letting PoolMap grow by
        // itself on the hot path.
        // Not growing matters: growing means asking for memory and redistributing the existing
        // nodes, and the latency of that cannot be controlled.
        // When free_ is not kEnd it certainly came from construction or from an unlink, and is
        // a valid index into nodes_.
        // This comes after the search for a duplicate, so updating an existing order still works
        // with the capacity full.
        if (free_ == kEnd) return false;
        // Remember the head of the free list; the test above has already guaranteed it is a
        // valid node index.
        // The next line changes free_, so i is kept first and every field below is written into
        // that entry.
        // const only fixes the local index and does not stop nodes_[i] from being changed.
        const std::uint32_t i = free_;
        // A free node's next points at the next free entry, so moving free_ there takes node i
        // off the list.
        // If i is the last entry, this naturally turns free_ into kEnd.
        // After this line no other insert should treat i as free; the present implementation is
        // called from one thread and needs no atomic publication.
        // i's old next has been read, so it can now be turned into a link in a bucket's list.
        // Taking the node off first and filling the order in afterwards keeps one entry from
        // being on both the free list and a bucket's list at once.
        free_ = nodes_[i].next;
        // A free node may hold old data, so the new order id is written in full.
        // find, reduce and erase all rest on this whole value to decide whether it is really a
        // hit.
        nodes_[i].oid = oid;
        // The starting remaining shares, already confirmed non zero, are kept.
        // reduce changes it in place afterwards, while erase hands it to the caller through
        // unpack first.
        nodes_[i].shares = o.shares;
        // The side and the price are packed into one 32 bit field of the node.
        // That way a Node does not pay padding for a one byte side and the node pool stays
        // compact.
        nodes_[i].side_price = pack(o);
        // The new node points at what was the bucket's head; on an empty bucket what is written
        // here is kEnd.
        // On a non empty one the existing list is kept whole behind i and no node has to be
        // moved.
        // This line has not changed buckets_[b] yet, so the node's fields can be prepared in
        // full first.
        nodes_[i].next = buckets_[b];
        // The node's fields are complete by this step, so the bucket's head becomes i and a
        // lookup can find it.
        // This structure is used by one thread and this is not a publication point across
        // threads, so it needs no atomic.
        // After the head insertion, the next lookup of the same order id hits the new node
        // first.
        buckets_[b] = i;
        // Only the path that adds reaches here, so the number of live orders goes up by exactly
        // one.
        // It has to be after the overwrite path's return, or a repeated order id would make
        // size() too large.
        // One entry fewer on the free list and one more on a bucket's list agree with this
        // change of count.
        ++count_;
        // Both the node and the lists are complete, so the caller is told the insert succeeded.
        // After a true the layer above can safely add this order's shares to the price level.
        return true;
    }

    // This is the body of a read only lookup.
    // It walks only the bucket's list that this order id maps to, and unpacks the internal Node
    // into out on a hit.
    // A true means out has been overwritten in full; a false means there is no such order and
    // out keeps whatever it held before the call.
    // The method is const and changes neither the links between nodes, the free list, nor the
    // number of orders.
    // out is prepared by the caller and no null check is made; every call site in this project
    // passes a valid Order address.
    // A lookup uses the same bucket() as an insert, and the two have to keep the same hash.
    // Sends out the memory requests a whole batch of order ids will need, reading nothing and
    // changing nothing.
    //
    // Why it exists: looking one order up takes two hops, reading the bucket for the head of the
    // list and then jumping from there to the node, and the address of the second hop is only
    // known once the first has come back. One message strung together that way is about two
    // hundred nanoseconds, and forty two messages one after another is eight or nine
    // microseconds - which is the bulk of our tail latency.
    // But those forty two do not depend on one another, and the memory controller can handle
    // twenty odd requests at once anyway.
    // So all forty two requests go out first and overlap while they are in flight, and then they
    // are applied one at a time, by which point the data is in cache.
    //
    // Why not "look them all up and store the results": that would disturb the order they are
    // applied in, and one batch may cancel an order and then refer to it. This only sends
    // requests and returns no results, so the order stays entirely with the caller and it is
    // safe.
    // The five below are the interface that works by slot number, for building the book in
    // passes.
    //
    // Why it exists: building in passes means finding a slot number, going through several
    //    passes, and coming back to read and write that entry.
    //    The interface by order id above hashes afresh and walks the bucket's list again every
    //    time; a slot number, once found, never changes (the node pool is prepared at start up,
    //    and deleting an order only returns that entry to the free list), so writing it down and
    //    using it directly means no lookup at all.
    // The condition is that no new order is inserted within that batch - otherwise an entry that
    //    was returned could be taken by somebody else.
    //    Building in passes puts every creation before every deletion for exactly that reason.
    // These five are an addition rather than a change: the interface by order id above stays as
    //    it was, and the path that applies one message at a time still uses it.

    // What find_slot returns when it finds nothing.
    // It is the same number as the internal end of list marker kEnd, but kEnd is declared in the
    // private part and cannot be reached here, so it is written out again. The number of nodes
    // is far below it and it cannot collide with a real slot number.
    static constexpr std::uint32_t kNoSlot = 0xffffffffu;

    // Inserts, and tells the caller which slot it landed in.
    // The same order id already there overwrites that entry and the slot number does not change;
    // a node pool that is used up returns kNoSlot.
    // The only difference from the insert above is the return value: that one returns whether it
    // succeeded, this one returns where it landed.
    std::uint32_t insert_at(std::uint64_t oid, const Order& o) {
        // An order of zero shares should not stay in the book, and is turned away first as in
        // insert.
        if (o.shares == 0) return kNoSlot;
        // Which bucket it should land in. The same hash as insert and find.
        const std::size_t b = bucket(oid);
        // Look first for the same order id on this list. Finding it overwrites in place and the
        // number of live orders does not change.
        for (std::uint32_t i = buckets_[b]; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid != oid) continue;
            nodes_[i].shares = o.shares;
            nodes_[i].side_price = pack(o);
            nodes_[i].sym = o.sym;
            return i;
        }
        // No such order id, so an entry is taken from the free list. None left returns kNoSlot
        // and the caller treats it as a full table.
        if (free_ == kEnd) return kNoSlot;
        const std::uint32_t i = free_;
        // The entry comes off the free list first and is filled in afterwards - one entry is
        // never on two lists at once.
        free_ = nodes_[i].next;
        nodes_[i].oid = oid;
        nodes_[i].shares = o.shares;
        nodes_[i].side_price = pack(o);
        nodes_[i].sym = o.sym;
        // Joined at the head of the bucket's list. A head insertion does not walk the list again
        // to find its end.
        nodes_[i].next = buckets_[b];
        buckets_[b] = i;
        ++count_;
        return i;
    }

    // Finds which slot this order is in. Not found returns kNoSlot.
    // It does not copy the content out - copying it would give up the very benefit of writing
    // the slot number down and using it directly.
    [[nodiscard]] std::uint32_t find_slot(std::uint64_t oid) const {
        for (std::uint32_t i = buckets_[bucket(oid)]; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid == oid) return i;
        }
        return kNoSlot;
    }

    // Reads an order by slot number. The caller has to make sure the slot number was just found
    // and nothing deleted it in between.
    [[nodiscard]] Order at(std::uint32_t slot) const { return unpack(nodes_[slot]); }

    // Changes an order's shares by slot number. The pass that takes shares off uses it.
    void set_shares_at(std::uint32_t slot, std::uint32_t shares) { nodes_[slot].shares = shares; }

    // Changes an order's side and security number by slot number.
    // The replace pass uses it: the new order is created first with the side empty, and once the
    // old order's side is in hand it is written back by slot number.
    void set_side_sym_at(std::uint32_t slot, std::uint8_t side, std::uint16_t sym) {
        // The side is in the top bit and the price in the low 31. Only the top bit is exchanged
        // and the price stays as it was.
        nodes_[slot].side_price =
            (static_cast<std::uint32_t>(side) << 31) | (nodes_[slot].side_price & 0x7fffffffu);
        nodes_[slot].sym = sym;
    }

    // Deletes an order by slot number.
    // The bucket's list still has to be walked to find the place that points at it - a singly
    // linked list is like that and there is no avoiding it.
    // What is saved is hashing afresh and comparing order ids: what is compared here is a slot
    // number, one instruction.
    void erase_at(std::uint32_t slot) {
        std::uint32_t* link = &buckets_[bucket(nodes_[slot].oid)];
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            if (i != slot) continue;
            unlink(link, i);
            return;
        }
    }

    void ask_for_all(const std::uint64_t* oids, std::size_t n) const {
        // The first loop only works out addresses and sends requests. Not one byte has been read
        // in it, so the n requests fly out together rather than one waiting for another.
        for (std::size_t j = 0; j < n; ++j) {
            __builtin_prefetch(&buckets_[bucket(oids[j])], 0, 3);
        }
        // The second loop reads the bucket heads and, having the head of a list, sends the
        // request for the second hop at once.
        // By this loop most of what the first sent has come back, so there is essentially no
        // waiting here.
        // Reading a bucket head changes nothing; it takes out a number that has already been
        // fetched and looks at it.
        for (std::size_t j = 0; j < n; ++j) {
            const std::uint32_t i = buckets_[bucket(oids[j])];
            if (i != kEnd) __builtin_prefetch(&nodes_[i], 0, 3);
        }
    }

    [[nodiscard]] bool find(std::uint64_t oid, Order* out) const {
        // Starting at the head of the target bucket's list, comparing whole order ids one at a
        // time until a hit or kEnd.
        // An empty bucket starts as kEnd and the loop does not run once.
        // What a lookup costs depends only on the length of this one collision list and never
        // scans the whole node pool.
        // The method is const, so what nodes_ gives here is only for reading.
        // Every miss moves along the current node's next until the end of the list.
        // The hash only narrows the search; correctness still rests on comparing whole order
        // ids.
        for (std::uint32_t i = buckets_[bucket(oid)]; i != kEnd; i = nodes_[i].next) {
            // A false means only another order in the same bucket and the loop goes on; only a
            // true may write out.
            // On a live node the order id was written in full by insert and does not rest on
            // what a free node left behind.
            // A hit does not move the node to the head of the list, so find itself does not
            // change the order of later accesses.
            if (nodes_[i].oid == oid) {
                // The shares, price and side are restored from the node's internal format into
                // the caller's Order.
                // out has to point at valid space; on a successful lookup all three fields are
                // overwritten.
                // unpack returns an Order of its own and never exposes the address of an
                // internal Node to the layer above.
                // Even if PoolMap changes that node afterwards, the copy the caller holds does
                // not follow.
                *out = unpack(nodes_[i]);
                // out can be used now, so a true is returned and this list is not walked any
                // further.
                // The true is what lets the caller read out; it does not mean the order's state
                // was changed.
                return true;
            }
        }
        // The list has been walked with no hit; a false is returned and out is deliberately not
        // written, saving a pointless clearing.
        // The caller has to look at the return value first and must not go on using what a
        // previous lookup left in out.
        // This failure usually means a message arrived referring to an unknown order id, and the
        // layer above raises its orphan count.
        return false;
    }

    // This is the body of handling the quantity of an execution or a cancel.
    // Having found the order id, it hands out the complete order as it was through before.
    // With shares below the remaining quantity it only subtracts, and the node stays in the same
    // bucket.
    // With shares reaching or exceeding it, unlink is called, the node is removed and returned
    // to the free list.
    // The old price and side in before are what let the order book take the right amount off the
    // right price level.
    // Not finding the order id returns false, and before is not written by this method.
    // shares may equal, be below, or exceed the current remaining quantity; reaching it is
    // treated as the whole order ending.
    // This method takes no new price or side, because an execution and a cancel only change the
    // quantity of the original order.
    bool reduce(std::uint64_t oid, std::uint32_t shares, Order* before) {
        // The bucket index is kept, because the address of the bucket head itself is needed
        // below.
        // The hash is done once and the walk stays on this one collision list throughout.
        // b is a size_t and can index buckets_ directly.
        const std::size_t b = bucket(oid);
        // link always points at the place where the current node's index is stored.
        // The first entry is pointed at by the bucket head, and later nodes by the previous
        // node's next.
        // That way a hit can change *link directly to take the current node off the list,
        // without having to find its predecessor again.
        // If the target is the first entry, link points at buckets_[b], and deleting moves the
        // bucket head along naturally.
        // If the target is in the middle, link points at the previous node's next, and the same
        // unlink still applies.
        // This "pointer to the link" avoids keeping a predecessor index separately and handling
        // two branches for a deletion.
        std::uint32_t* link = &buckets_[b];
        // This is the body of walking the list while keeping the link to the predecessor.
        // i takes what *link points at; on a miss link moves to the current node's next and i
        // takes the new *link.
        // kEnd means the list has been walked, and after the loop it returns false.
        // A continue does not skip the moving along, because the for's third part still runs.
        // The order of the update is link first pointing at the current next, and then the new
        // *link giving the following node.
        // Since a hit returns or unlinks at once, the loop's update only runs when the current
        // node is kept.
        // An empty bucket makes i start as kEnd and before is not written.
        // Every turn compares the whole order id, so a hash collision cannot take shares off
        // somebody else.
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            // Not the target order id keeps this node and leaves the for's update to move on.
            // A continue skips only the rest of this turn and does not jump back before the
            // condition and forget to move link.
            // The false path writes no before and changes neither shares nor count_.
            if (nodes_[i].oid != oid) continue;
            // The old state is kept before the node is changed; the order book needs it to take
            // the right amount off the price level.
            // Unpacking has to come first: a reduction of the whole order is followed at once by
            // an unlink, and that entry may be reused by the next insert immediately.
            // before gets a copy of its own and stays safe to use after the node is recycled.
            *before = unpack(nodes_[i]);
            // A true means this reduction takes every remaining share, and the node cannot go on
            // existing as a live order.
            // A false means shares are left and only this quantity comes off the node in place.
            // Using >= tolerates the layer above passing more than what is left, and keeps the
            // unsigned subtraction from going below zero.
            // The true path lowers size() by one and the false path does not change the number
            // of orders.
            // Both paths keep the shares before the change in before, and the caller updates the
            // price level by that real old value.
            // The test changes neither the order id, the price, nor the side.
            if (shares >= nodes_[i].shares) {
                // link points at the way into the current node, and unlink takes over its next
                // and returns the node to free_.
                // unlink also lowers count_, so the number of orders is not maintained again
                // here.
                unlink(link, i);
            // The true path above has removed the node completely; this else only handles shares
            // still being left.
            // A false condition already proves shares is below what is left, so the subtraction
            // below cannot wrap round into a very large unsigned number.
            } else {
                // Only this order's remaining shares come down; the order id, price, side and
                // position in the list all stay.
                // The condition has already guaranteed shares is below the old value, so the
                // result is still above zero and the node stays live.
                // The matching reduction of the price level is done by the order book after this
                // method returns.
                nodes_[i].shares -= shares;
            }
            // The target has been found and handled; before is valid, a true is returned, and no
            // later node of the same bucket is looked at.
            // The caller uses the true to tell "it has been reduced" from "an unknown order id".
            // Whether the node still exists follows from before.shares against this shares, and
            // no extra status is returned.
            return true;
        }
        // The whole list holds no such order id; a false is returned and the layer above treats
        // it as an orphan.
        // before keeps what it held before the call, and the caller must not read it on the
        // false path to update a price level.
        // No node has had anything taken off it, and free_ and count_ are entirely unchanged.
        return false;
    }

    // This is the body of deleting an order in full by order id, used by an ITCH delete and by
    // the old order id of a replace.
    // Like reduce it keeps the address of "what points at the current node", but makes no
    // comparison of quantities.
    // On a hit the order being deleted is written into gone first, and then the node is unlinked
    // and true returned.
    // On a miss it returns false and gone stays as it was.
    // An ITCH delete carries neither the remaining shares nor the price, so the layer above has
    // to take those old fields from gone.
    // A replace deletes the old order id with it first, and only then inserts afresh under the
    // new order id with the new state.
    bool erase(std::uint64_t oid, Order* gone) {
        // Starting at the head of the target bucket; link follows the walk to the previous
        // node's next afterwards.
        // Keeping the address of the link directly lets deleting the head and deleting a middle
        // node use one piece of code.
        // bucket() uses the same hash as insert, so the target cannot be in another bucket.
        std::uint32_t* link = &buckets_[bucket(oid)];
        // i takes *link every turn, so on a hit link can go round the current node directly.
        // A mismatch does a continue, but the for's update still moves link and i.
        // kEnd is the end of the list, and walking it out changes no node.
        // i is always the node *link points at, so on a hit link goes round it exactly.
        // An empty list ends the loop at once; a non empty one compares whole order ids entry by
        // entry along next.
        // This walk never scans another part of the node pool for the sake of a deletion.
        // The method returns straight after a hit, so the next unlink changed is never read by
        // the loop's update.
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            // The current node not being the target carries on; the same hash must not delete
            // somebody else's order.
            // The false path writes no gone and changes no links.
            if (nodes_[i].oid != oid) continue;
            // The complete old state goes to the order book before the deletion, so it can take
            // the remaining shares off the right price level.
            // This step has to come before the unlink, because a recycled node can be
            // overwritten by a new order very soon after.
            // gone is a copy the caller owns and does not depend on whether the node is still in
            // use afterwards.
            *gone = unpack(nodes_[i]);
            // i comes off the bucket's list and that entry joins the front of the free list.
            // unlink also lowers count_, so size() shows the deletion at once.
            // The deleted node's old order id and other fields need no clearing; the next insert
            // overwrites all of them.
            unlink(link, i);
            // The deletion is done, gone is valid, and no later node has to be looked at.
            // The true lets the caller go on to take gone.shares off the matching price level.
            return true;
        }
        // No such order id; a false is returned so the caller records an unknown order rather
        // than using the old content of gone.
        // A failure does not clear gone, because that would be one more write and the return
        // value already says plainly that the content is invalid.
        // The bucket's list, the free list and count_ all stay as they were before the call.
        return false;
    }

// The private part holds the node's internal format, the hash, and the state of the two lists.
private:
    // Node is one entry of the node pool.
    // The same entry belongs to a bucket's list while it is live and to the free_ list while it
    // is free; next is reused in both states.
    // The order id and the three order fields stay in the node, which is what lets an order be
    // restored on a deletion when ITCH gives only the order id.
    // Nodes are joined by index and keep no raw pointer, and the whole Buffer never moves after
    // construction.
    // The old order fields of a free node mean nothing, so recycling only has to change next
    // rather than clear the whole entry.
    // The next insert overwrites every live field before the node is published to a bucket's
    // list again.
    struct Node {
        // Holds the whole 64 bit order id, which a lookup uses to tell apart different orders
        // that landed in the same bucket.
        // The default zero is only for initialising; whether a node is live follows from which
        // list it is on rather than from the order id being zero.
        std::uint64_t oid = 0;
        // Holds the remaining shares; on a live node it is always above zero.
        // Some paths of reduce change it in place, and the whole order ending recycles the node
        // to free_.
        std::uint32_t shares = 0;
        // The top bit holds the side and the low 31 the price, with pack and unpack doing the
        // conversion.
        // Putting them together keeps the side from taking padding of its own and gives both
        // values in one read.
        std::uint32_t side_price = 0;
        // On a live node it points at the next entry of the same bucket, and on a free one at
        // the next reusable entry.
        // kEnd means the end in both lists, so the one field needs no extra flag.
        std::uint32_t next = 0;
        // Which security this order belongs to.
        // It is free: the four fields above come to 20 bytes and alignment to 8 bytes would pad
        // to 24 anyway, and these two bytes sit in exactly that padding - sizeof is still 24
        // with it and not one entry got larger.
        std::uint16_t sym = 0;
    // The end of Node's internal fields.
    };

    // 0xffffffff cannot be a valid index into the node array, and it means the end of a list
    // throughout.
    // The node capacity is far below it, so it cannot be confused with a real index.
    // A plain 32 bit constant also matches the types of next, free_ and a bucket head.
    static constexpr std::uint32_t kEnd = 0xffffffffu;

    // pack turns the outside's Order into the compact format a node uses.
    // The side is widened to 32 bits and shifted to the top bit, while the price stays in the
    // low 31.
    // The largest price in this project's data is below 2^31, so the two parts cannot cover one
    // another.
    // The bitwise or gives a result that can be written into side_price in one go.
    // o is only read, and the return value refers to nothing of the caller's Order.
    // The side is expected to be only 0 or 1; the layer above works it out from ITCH's B or S
    // character.
    // With a side of 1 the shift gives 0x80000000, and with 0 the top bit stays zero.
    static std::uint32_t pack(const Order& o) {
        // With side=0 the top bit is zero and with side=1 it is one; every other bit comes from
        // the price.
        // The static_cast widens the one byte side to 32 bits first, so the shift does not
        // happen in a narrower type.
        // The bitwise or does not change the low 31 bits of the price.
        return (static_cast<std::uint32_t>(o.side) << 31) | o.price;
    }
    // unpack restores a Node into an Order the caller can use directly.
    // The shares come back as they are, a mask takes the side bit off to give the price, and a
    // shift right by 31 gives the side.
    // The new Order refers to nothing of the Node, and the caller changing it afterwards does
    // not change the node pool.
    // This helper is used by find, reduce and erase alike, so all three paths read the fields
    // the same way.
    // n is only read, and a node's packed content does not change on being unpacked.
    // Returning an aggregate avoids building an empty Order and assigning field by field.
    static Order unpack(const Node& n) {
        // Aggregate initialisation fills shares, price and side in the order of Order's fields.
        // 0x7fffffff clears the top side bit and keeps the other 31 bits of the price.
        return Order{n.shares, n.side_price & 0x7fffffffu,
                     // The shift leaves only the top bit, narrowed to a uint8_t as the side.
                     // The static_cast makes the narrowing plain rather than resting on the
                     // rules of an implicit conversion.
                     static_cast<std::uint8_t>(n.side_price >> 31),
                     // The security number is carried out as it is.
                     n.sym};
    }

    // bucket uses Fibonacci hashing to spread a 64 bit order id across buckets_.
    // The multiplication mixes the bits of nearby order ids apart, and shifting right by shift_
    // keeps only the top bits the bucket count needs.
    // The constructor has already guaranteed the bucket count is a power of two and worked out
    // shift_ for it.
    // The method only reads configuration, so it can be called on find's const path.
    // The same is used for OrderMap, so that a comparison later differs only in how collisions
    // are handled.
    // The multiplying constant comes from 64 bit Fibonacci hashing, and its top bits spread more
    // evenly than the low bits of an order id.
    // A size_t is returned so buckets_ can be indexed directly, though the real range needs only
    // log2(n) bits.
    [[nodiscard]] std::size_t bucket(std::uint64_t oid) const noexcept {
        // The multiplication wraps round naturally as a uint64_t, and after the shift the value
        // certainly falls within the buckets there are.
        // shift_ is 60 with n=16, and every doubling of n shifts one bit less and keeps one more
        // top bit.
        // There is no division here, and the hot path of a lookup pays one multiplication and
        // one shift.
        return (oid * 0x9E3779B97F4A7C15ull) >> shift_;
    }

    // unlink takes node i off a bucket's list and puts it at the front of the free list.
    // link points either at a bucket head or at the previous node's next, so changing *link goes
    // round i.
    // A node just deleted is the first to be reused, and its data is more likely to be in cache
    // still.
    // This method is only called once i has been confirmed valid, so it does not look the order
    // id up again.
    // The link the caller passes may belong to buckets_ or to another Node, and it is written
    // the same way for both.
    // The order of operations is the bucket's list first, then joining the free list, and the
    // count last.
    // This class is used by one order book thread, so these ordinary writes need no atomic
    // operations.
    void unlink(std::uint32_t* link, std::uint32_t i) {
        // The place pointing at i now points at what follows i, and the bucket's list no longer
        // holds i.
        // If i was the end of the list, what follows is kEnd; if it was the head, the bucket
        // head becomes the second entry directly.
        *link = nodes_[i].next;
        // i now points at what was the head of the free list and begins serving as a free node.
        // Its old order id, shares and side_price may stay, because the free list reads none of
        // those fields.
        nodes_[i].next = free_;
        // free_ is published as i, and the next insert takes this entry first.
        // A head insertion reuses the node touched most recently, which is more likely to hit
        // cache than taking a cold one from the end of the list.
        free_ = i;
        // One live order fewer, keeping size() in step with the number of nodes on the buckets'
        // lists.
        // The call sites only unlink a live node once, so this cannot go below zero as an
        // unsigned number.
        --count_;
    }

    // nodes_ holds every node; its size never changes after construction, and both the buckets
    // and free_ hold only indices into it.
    // Continuous huge pages also avoid a separate malloc per add leaving nodes scattered about.
    huge::Buffer<Node> nodes_;
    // Each entry of buckets_ is the index of the head node of one collision list, and kEnd means
    // that bucket is empty.
    // Keeping it apart from nodes_ means that, once the hash is worked out, only a very small
    // 32 bit head is read first.
    huge::Buffer<std::uint32_t> buckets_;
    // count_ is how many orders are live now, not counting the nodes on the free list.
    // It starts at zero and changes in only two places, an addition and an unlink.
    std::size_t count_ = 0;
    // free_ points at the next usable node, and kEnd means the fixed node pool is used up.
    // The constructor changes it to 0, and the field's starting value mainly keeps the state
    // plain while the object is being initialised.
    std::uint32_t free_ = kEnd;
    // shift_ decides how many top bits of the hash are kept, and the constructor works it out
    // afresh from the real bucket count.
    // The starting 64 is written again before the constructor's loop, and it does not change
    // once construction is done.
    int shift_ = 64;
// The end of PoolMap; the Buffers release the two blocks asked for at construction when the
// object is destroyed.
};

}
