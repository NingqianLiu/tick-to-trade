#include <gtest/gtest.h>

#include "book/imbalance.hpp"

namespace {

using book::Imbalance;
using Signal = book::Imbalance::Signal;

TEST(Imbalance, one_side_carrying_almost_everything_fires) {
    constexpr Imbalance im(88);
    // 90 against 10 is ninety percent of the two, past the threshold.
    EXPECT_EQ(im.check(90, 10), Signal::kBuy);
    EXPECT_EQ(im.check(10, 90), Signal::kSell);
    // 88 against 12 is exactly the threshold, which is not past it.
    EXPECT_EQ(im.check(88, 12), Signal::kNone);
    EXPECT_EQ(im.check(12, 88), Signal::kNone);
    // 881 against 119 just clears it.
    EXPECT_EQ(im.check(881, 119), Signal::kBuy);
}

TEST(Imbalance, an_even_book_does_nothing) {
    constexpr Imbalance im(88);
    EXPECT_EQ(im.check(50, 50), Signal::kNone);
    EXPECT_EQ(im.check(1000, 900), Signal::kNone);
}

// A side thinner than three prices reports zero, and that has to mean leave it
// alone rather than divide by nothing or read it as total imbalance.
TEST(Imbalance, a_side_without_three_prices_is_left_alone) {
    constexpr Imbalance im(88);
    EXPECT_EQ(im.check(0, 100), Signal::kNone);
    EXPECT_EQ(im.check(100, 0), Signal::kNone);
    EXPECT_EQ(im.check(0, 0), Signal::kNone);
}

TEST(Imbalance, the_threshold_moves_the_line) {
    EXPECT_EQ(Imbalance(85).check(86, 14), Signal::kBuy);
    EXPECT_EQ(Imbalance(90).check(86, 14), Signal::kNone);
    // Half means whichever side is merely larger.
    EXPECT_EQ(Imbalance(50).check(51, 49), Signal::kBuy);
    EXPECT_EQ(Imbalance(50).check(49, 51), Signal::kSell);
}

// The share counts are real share counts, so the arithmetic has to hold at the
// sizes a busy security reaches without wrapping.
TEST(Imbalance, it_holds_up_at_realistic_share_counts) {
    constexpr Imbalance im(88);
    const std::uint64_t big = 3'000'000'000ull;   // more than a signed int holds
    EXPECT_EQ(im.check(big, big / 100), Signal::kBuy);
    EXPECT_EQ(im.check(big, big), Signal::kNone);
}

}  // namespace
