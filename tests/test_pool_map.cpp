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

TEST(PoolMap, a_released_node_is_the_next_one_handed_out) {
    PoolMap m(16);
    const std::size_t cap = m.capacity();
    for (std::uint64_t k = 1; k <= cap; ++k) {
        ASSERT_TRUE(m.insert(k, cases::mk<PoolMap>(1, 10, 0)));
    }
    EXPECT_EQ(m.size(), cap);
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

}
