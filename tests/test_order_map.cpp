#include "book/order_map.hpp"

#include "map_cases.hpp"

namespace {

using book::OrderMap;

TEST(OrderMap, a_stored_order_comes_back_whole) {
    cases::a_stored_order_comes_back_whole<OrderMap>();
}
TEST(OrderMap, the_fields_hold_their_extremes) {
    cases::the_fields_hold_their_extremes<OrderMap>();
}
TEST(OrderMap, a_partial_fill_leaves_the_rest_resting) {
    cases::a_partial_fill_leaves_the_rest_resting<OrderMap>();
}
TEST(OrderMap, a_full_fill_removes_the_order) {
    cases::a_full_fill_removes_the_order<OrderMap>();
}
TEST(OrderMap, erase_reports_what_it_removed) {
    cases::erase_reports_what_it_removed<OrderMap>();
}
TEST(OrderMap, it_refuses_to_grow) { cases::it_refuses_to_grow<OrderMap>(); }
TEST(OrderMap, it_matches_a_plain_map_under_churn) {
    cases::it_matches_a_plain_map_under_churn<OrderMap>();
}

// Open addressing keeps colliding entries in the array itself, so a lookup
// walks forward from the slot the key hashed to until it matches or reaches a
// free slot. Removing an entry therefore cannot simply blank its slot: an entry
// that walked past it would become unreachable. Force that exact shape by
// filling one run of slots and taking the first of them away.
TEST(OrderMap, a_removal_does_not_strand_what_probed_past_it) {
    OrderMap m(1024);
    // Three references that genuinely land on the same slot, found by running
    // the map's own arithmetic rather than assumed, so the case cannot quietly
    // stop being a collision when the table size changes.
    int shift = 64;
    for (std::size_t c = m.capacity(); c > 1; c >>= 1) --shift;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> by_slot;
    std::uint64_t a = 0, b = 0, c = 0;
    for (std::uint64_t k = 1; k < 100000 && c == 0; ++k) {
        auto& same = by_slot[(k * 0x9E3779B97F4A7C15ull) >> shift];
        same.push_back(k);
        if (same.size() == 3) { a = same[0]; b = same[1]; c = same[2]; }
    }
    ASSERT_NE(c, 0u) << "no three references shared a slot";

    ASSERT_TRUE(m.insert(a, cases::mk<OrderMap>(10, 100, 0)));
    ASSERT_TRUE(m.insert(b, cases::mk<OrderMap>(20, 200, 0)));
    ASSERT_TRUE(m.insert(c, cases::mk<OrderMap>(30, 300, 0)));

    OrderMap::Order gone{};
    ASSERT_TRUE(m.erase(a, &gone));
    EXPECT_EQ(gone.shares, 10u);

    OrderMap::Order o{};
    ASSERT_TRUE(m.find(b, &o)) << "the entry behind the hole was lost";
    EXPECT_EQ(o.shares, 20u);
    ASSERT_TRUE(m.find(c, &o)) << "the entry two behind the hole was lost";
    EXPECT_EQ(o.shares, 30u);
    EXPECT_EQ(m.size(), 2u);
}

}  // namespace
