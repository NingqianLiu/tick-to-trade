#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <vector>

#include "book/price_levels.hpp"

namespace {

using book::PriceLevels;

constexpr std::uint8_t kBuy = PriceLevels::kBuy;
constexpr std::uint8_t kSell = PriceLevels::kSell;

PriceLevels one(std::uint32_t reference = 1000000) {
    PriceLevels p(std::vector<std::uint32_t>{reference});
    EXPECT_TRUE(p.bind(7, reference));
    return p;
}

TEST(PriceLevels, shares_come_back_at_the_price_they_went_in) {
    PriceLevels p = one();
    p.add(7, kBuy, 999900, 300);
    p.add(7, kBuy, 999900, 200);
    p.add(7, kSell, 1000100, 50);

    EXPECT_EQ(p.at(7, kBuy, 999900), 500u);
    EXPECT_EQ(p.at(7, kSell, 1000100), 50u);
    EXPECT_EQ(p.at(7, kBuy, 1000100), 0u);

    p.remove(7, kBuy, 999900, 200);
    EXPECT_EQ(p.at(7, kBuy, 999900), 300u);
    p.remove(7, kBuy, 999900, 300);
    EXPECT_EQ(p.at(7, kBuy, 999900), 0u);
}

TEST(PriceLevels, the_best_price_is_the_highest_bid_and_the_lowest_offer) {
    PriceLevels p = one();
    p.add(7, kBuy, 999800, 10);
    p.add(7, kBuy, 999900, 20);
    p.add(7, kBuy, 999700, 30);
    p.add(7, kSell, 1000200, 40);
    p.add(7, kSell, 1000100, 50);

    std::uint32_t price = 0, shares = 0;
    ASSERT_TRUE(p.best(7, kBuy, &price, &shares));
    EXPECT_EQ(price, 999900u);
    EXPECT_EQ(shares, 20u);
    ASSERT_TRUE(p.best(7, kSell, &price, &shares));
    EXPECT_EQ(price, 1000100u);
    EXPECT_EQ(shares, 50u);

    p.remove(7, kBuy, 999900, 20);
    ASSERT_TRUE(p.best(7, kBuy, &price, &shares));
    EXPECT_EQ(price, 999800u);
    EXPECT_EQ(shares, 10u);
}

TEST(PriceLevels, three_deep_or_nothing) {
    PriceLevels p = one();
    EXPECT_EQ(p.top3(7, kBuy), 0u);
    p.add(7, kBuy, 999900, 1);
    EXPECT_EQ(p.top3(7, kBuy), 0u);
    p.add(7, kBuy, 999800, 2);
    EXPECT_EQ(p.top3(7, kBuy), 0u);
    p.add(7, kBuy, 999700, 4);
    EXPECT_EQ(p.top3(7, kBuy), 7u);

    p.add(7, kBuy, 999600, 8);
    EXPECT_EQ(p.top3(7, kBuy), 7u);
    p.add(7, kBuy, 1000000, 16);
    EXPECT_EQ(p.top3(7, kBuy), 16u + 1u + 2u);

    p.add(7, kSell, 1000100, 3);
    p.add(7, kSell, 1000200, 5);
    p.add(7, kSell, 1000300, 9);
    p.add(7, kSell, 1000400, 17);
    EXPECT_EQ(p.top3(7, kSell), 3u + 5u + 9u);
}

TEST(PriceLevels, far_apart_prices_still_come_out_in_order) {
    PriceLevels p = one();
    p.add(7, kBuy, 200000, 1);
    p.add(7, kBuy, 700000, 2);
    p.add(7, kBuy, 1500000, 4);
    EXPECT_EQ(p.top3(7, kBuy), 7u);

    std::uint32_t price = 0, shares = 0;
    ASSERT_TRUE(p.best(7, kBuy, &price, &shares));
    EXPECT_EQ(price, 1500000u);
    ASSERT_TRUE(p.best(7, kSell, &price, &shares) == false);
}

TEST(PriceLevels, a_cheap_security_gets_both_steps) {
    const std::uint32_t reference = 40000;
    PriceLevels p(std::vector<std::uint32_t>{reference});
    ASSERT_TRUE(p.bind(3, reference));

    p.add(3, kBuy, 9876, 100);
    p.add(3, kBuy, 20000, 200);
    EXPECT_EQ(p.at(3, kBuy, 9876), 100u);
    EXPECT_EQ(p.at(3, kBuy, 20000), 200u);

    std::uint32_t price = 0, shares = 0;
    ASSERT_TRUE(p.best(3, kBuy, &price, &shares));
    EXPECT_EQ(price, 20000u);

    p.add(3, kSell, 9877, 5);
    p.add(3, kSell, 30000, 6);
    ASSERT_TRUE(p.best(3, kSell, &price, &shares));
    EXPECT_EQ(price, 9877u);
    EXPECT_EQ(shares, 5u);

    p.add(3, kSell, 9900, 7);
    EXPECT_EQ(p.top3(3, kSell), 5u + 7u + 6u);
}

TEST(PriceLevels, a_price_outside_the_band_is_left_untracked) {
    PriceLevels p = one();
    p.add(7, kBuy, 1999999900, 1000);
    p.add(7, kBuy, 100, 1000);
    EXPECT_EQ(p.at(7, kBuy, 1999999900), 0u);
    EXPECT_EQ(p.top3(7, kBuy), 0u);

    p.add(7, kBuy, 999900, 5);
    p.remove(7, kBuy, 1999999900, 1000);
    EXPECT_EQ(p.at(7, kBuy, 999900), 5u);
}

TEST(PriceLevels, an_unbound_security_answers_nothing) {
    PriceLevels p = one();
    p.add(9, kBuy, 999900, 100);
    EXPECT_EQ(p.at(9, kBuy, 999900), 0u);
    EXPECT_EQ(p.top3(9, kBuy), 0u);
    EXPECT_FALSE(p.bound(9));
    EXPECT_TRUE(p.bound(7));
}

TEST(PriceLevels, it_matches_a_sorted_map_under_churn) {
    const std::uint32_t reference = 500000;
    PriceLevels p(std::vector<std::uint32_t>{reference});
    ASSERT_TRUE(p.bind(1, reference));
    std::map<std::uint32_t, std::uint64_t> ref[2];
    std::mt19937_64 rng(20260808);

    const auto want = [&](int s) {
        std::uint64_t sum = 0;
        if (ref[s].size() < 3) return std::uint64_t{0};
        if (s == kBuy) {
            auto it = ref[s].rbegin();
            for (int i = 0; i < 3; ++i, ++it) sum += it->second;
        } else {
            auto it = ref[s].begin();
            for (int i = 0; i < 3; ++i, ++it) sum += it->second;
        }
        return sum;
    };

    for (int step = 0; step < 100000; ++step) {
        const int s = static_cast<int>(rng() & 1);
        const std::uint32_t price =
            (400000 + static_cast<std::uint32_t>(rng() % 2000) * 100);
        const std::uint32_t shares = static_cast<std::uint32_t>(rng() % 1000 + 1);
        if ((rng() & 1) != 0) {
            p.add(1, static_cast<std::uint8_t>(s), price, shares);
            ref[s][price] += shares;
        } else {
            p.remove(1, static_cast<std::uint8_t>(s), price, shares);
            const auto it = ref[s].find(price);
            if (it != ref[s].end()) {
                if (it->second <= shares) ref[s].erase(it);
                else it->second -= shares;
            }
        }
        ASSERT_EQ(p.at(1, static_cast<std::uint8_t>(s), price),
                  ref[s].count(price) != 0 ? ref[s][price] : 0u)
            << "at step " << step;
        ASSERT_EQ(p.top3(1, kBuy), want(kBuy)) << "at step " << step;
        ASSERT_EQ(p.top3(1, kSell), want(kSell)) << "at step " << step;
    }
}

TEST(PriceLevels, a_security_under_a_dime_has_only_the_fine_step) {
    const std::uint32_t reference = 400;
    PriceLevels p(std::vector<std::uint32_t>{reference});
    ASSERT_TRUE(p.bind(2, reference));
    EXPECT_LT(p.bytes(), 1u << 20);

    p.add(2, kBuy, 350, 10);
    p.add(2, kBuy, 340, 20);
    p.add(2, kBuy, 330, 30);
    EXPECT_EQ(p.top3(2, kBuy), 60u);
    std::uint32_t price = 0, shares = 0;
    ASSERT_TRUE(p.best(2, kBuy, &price, &shares));
    EXPECT_EQ(price, 350u);
    p.add(2, kBuy, 10000, 99);
    EXPECT_EQ(p.at(2, kBuy, 10000), 0u);
}

}
