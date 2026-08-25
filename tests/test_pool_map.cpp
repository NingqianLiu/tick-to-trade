#include "book/pool_map.hpp"

#include "map_cases.hpp"

namespace {

using book::PoolMap;

TEST(PoolMap, a_stored_order_comes_back_whole) {
    cases::a_stored_order_comes_back_whole<PoolMap>();
}
TEST(PoolMap, the_fields_hold_their_extremes) {
    cases::the_fields_hold_their_extremes<PoolMap>();
}
TEST(PoolMap, a_partial_fill_leaves_the_rest_resting) {
    cases::a_partial_fill_leaves_the_rest_resting<PoolMap>();
}
TEST(PoolMap, a_full_fill_removes_the_order) {
    cases::a_full_fill_removes_the_order<PoolMap>();
}
TEST(PoolMap, erase_reports_what_it_removed) {
    cases::erase_reports_what_it_removed<PoolMap>();
}
TEST(PoolMap, it_refuses_to_grow) { cases::it_refuses_to_grow<PoolMap>(); }
TEST(PoolMap, it_matches_a_plain_map_under_churn) {
    cases::it_matches_a_plain_map_under_churn<PoolMap>();
}

// A freed node goes back on the front of the list, so the next order to arrive
// gets the node just given up rather than a cold one from further along. That
// is the whole reason for the pool, so pin it: fill the block, hand one back,
// and the next insert must land on the node that was released.
TEST(PoolMap, a_released_node_is_the_next_one_handed_out) {
    PoolMap m(16);
    const std::size_t cap = m.capacity();
    for (std::uint64_t k = 1; k <= cap; ++k) {
        ASSERT_TRUE(m.insert(k, cases::mk<PoolMap>(1, 10, 0)));
    }
    EXPECT_EQ(m.size(), cap);
    // Nothing left to hand out.
    EXPECT_FALSE(m.insert(cap + 1, cases::mk<PoolMap>(1, 10, 0)));

    PoolMap::Order gone{};
    ASSERT_TRUE(m.erase(7, &gone));
    EXPECT_TRUE(m.insert(cap + 1, cases::mk<PoolMap>(2, 20, 1)));
    EXPECT_EQ(m.size(), cap);

    PoolMap::Order o{};
    EXPECT_FALSE(m.find(7, &o));
    ASSERT_TRUE(m.find(cap + 1, &o));
    EXPECT_EQ(o.shares, 2u);
}

// The slot API exists so a caller can look an order up once and come back to it
// later, after other messages in the same batch have been applied. That only
// holds if a slot never moves, so pin it: take a slot, churn the table around
// it, and the slot must still describe the same order.
TEST(PoolMap, a_slot_keeps_pointing_at_its_order_through_churn) {
    PoolMap m(64);
    const std::uint32_t slot = m.insert_at(1000, PoolMap::Order{50, 777, 1, 9});
    ASSERT_NE(slot, PoolMap::kNoSlot);

    for (std::uint64_t k = 1; k <= 20; ++k) {
        ASSERT_NE(m.insert_at(k, PoolMap::Order{1, 10, 0, 0}), PoolMap::kNoSlot);
    }
    for (std::uint64_t k = 1; k <= 20; k += 2) {
        PoolMap::Order gone{};
        ASSERT_TRUE(m.erase(k, &gone));
    }

    const PoolMap::Order o = m.at(slot);
    EXPECT_EQ(o.shares, 50u);
    EXPECT_EQ(o.price, 777u);
    EXPECT_EQ(o.side, 1u);
    EXPECT_EQ(o.sym, 9u);
}

// insert_at and find_slot have to agree, otherwise the two halves of a replace
// would work on different rows.
TEST(PoolMap, find_slot_returns_what_insert_at_handed_out) {
    PoolMap m(64);
    const std::uint32_t slot = m.insert_at(42, PoolMap::Order{5, 100, 0, 3});
    ASSERT_NE(slot, PoolMap::kNoSlot);
    EXPECT_EQ(m.find_slot(42), slot);
    EXPECT_EQ(m.find_slot(43), PoolMap::kNoSlot);
}

// A replace arrives without a side, so the new order is created blank and the
// side and symbol are filled in later from the order it replaces. Filling them
// in must not disturb the shares and price that came off the wire.
TEST(PoolMap, set_side_sym_at_fills_the_blanks_and_leaves_the_rest) {
    PoolMap m(64);
    const std::uint32_t slot = m.insert_at(7, PoolMap::Order{200, 12345, 0, 0});
    ASSERT_NE(slot, PoolMap::kNoSlot);

    m.set_side_sym_at(slot, 1, 501);

    const PoolMap::Order o = m.at(slot);
    EXPECT_EQ(o.side, 1u);
    EXPECT_EQ(o.sym, 501u);
    EXPECT_EQ(o.shares, 200u);
    EXPECT_EQ(o.price, 12345u);
}

// A partial fill only moves the share count.
TEST(PoolMap, set_shares_at_leaves_the_rest_alone) {
    PoolMap m(64);
    const std::uint32_t slot = m.insert_at(7, PoolMap::Order{200, 12345, 1, 501});
    ASSERT_NE(slot, PoolMap::kNoSlot);

    m.set_shares_at(slot, 30);

    const PoolMap::Order o = m.at(slot);
    EXPECT_EQ(o.shares, 30u);
    EXPECT_EQ(o.price, 12345u);
    EXPECT_EQ(o.side, 1u);
    EXPECT_EQ(o.sym, 501u);
}

// erase_at has to unlink the node from its bucket and give it back to the free
// list, exactly like erase by id does. If it only did one of the two the table
// would either leak nodes or keep answering finds for a dead order.
TEST(PoolMap, erase_at_removes_the_order_and_returns_the_node) {
    PoolMap m(64);
    const std::size_t before = m.size();
    const std::uint32_t slot = m.insert_at(7, PoolMap::Order{200, 12345, 1, 501});
    ASSERT_NE(slot, PoolMap::kNoSlot);
    EXPECT_EQ(m.size(), before + 1);

    m.erase_at(slot);

    EXPECT_EQ(m.size(), before);
    EXPECT_EQ(m.find_slot(7), PoolMap::kNoSlot);
    PoolMap::Order o{};
    EXPECT_FALSE(m.find(7, &o));
    EXPECT_NE(m.insert_at(8, PoolMap::Order{1, 1, 0, 0}), PoolMap::kNoSlot);
}

// The same order id arriving twice overwrites in place rather than taking a
// second node, so the slot the caller already holds stays valid.
TEST(PoolMap, insert_at_overwrites_in_place_for_a_repeated_id) {
    PoolMap m(64);
    const std::uint32_t first = m.insert_at(7, PoolMap::Order{200, 12345, 1, 501});
    ASSERT_NE(first, PoolMap::kNoSlot);
    const std::size_t used = m.size();

    const std::uint32_t again = m.insert_at(7, PoolMap::Order{9, 42, 0, 3});

    EXPECT_EQ(again, first);
    EXPECT_EQ(m.size(), used);
    EXPECT_EQ(m.at(first).shares, 9u);
}

// Running out of nodes has to be reported, not papered over: the seven passes
// carry the returned slot around and would write through a bad one.
TEST(PoolMap, insert_at_says_no_slot_when_the_nodes_run_out) {
    PoolMap m(16);
    const std::size_t cap = m.capacity();
    for (std::uint64_t k = 1; k <= cap; ++k) {
        ASSERT_NE(m.insert_at(k, PoolMap::Order{1, 10, 0, 0}), PoolMap::kNoSlot);
    }
    EXPECT_EQ(m.insert_at(cap + 1, PoolMap::Order{1, 10, 0, 0}), PoolMap::kNoSlot);
}

}  // namespace
