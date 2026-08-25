#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace cases {

template <class Map>
typename Map::Order mk(std::uint32_t shares, std::uint32_t price, std::uint8_t side) {
    return typename Map::Order{shares, price, side};
}

template <class Map>
void a_stored_order_comes_back_whole() {
    Map m(64);
    EXPECT_TRUE(m.insert(2, mk<Map>(100, 123450, 0)));
    EXPECT_TRUE(m.insert(3, mk<Map>(7, 1999999900, 1)));

    typename Map::Order o{};
    ASSERT_TRUE(m.find(2, &o));
    EXPECT_EQ(o.shares, 100u);
    EXPECT_EQ(o.price, 123450u);
    EXPECT_EQ(o.side, 0);

    ASSERT_TRUE(m.find(3, &o));
    EXPECT_EQ(o.shares, 7u);
    EXPECT_EQ(o.price, 1999999900u);
    EXPECT_EQ(o.side, 1);

    EXPECT_FALSE(m.find(4, &o));
    EXPECT_EQ(m.size(), 2u);
}

template <class Map>
void the_fields_hold_their_extremes() {
    Map m(16);
    EXPECT_TRUE(m.insert(1, mk<Map>(999999, 0x7fffffffu, 1)));
    typename Map::Order o{};
    ASSERT_TRUE(m.find(1, &o));
    EXPECT_EQ(o.shares, 999999u);
    EXPECT_EQ(o.price, 0x7fffffffu);
    EXPECT_EQ(o.side, 1);
}

template <class Map>
void a_partial_fill_leaves_the_rest_resting() {
    Map m(64);
    m.insert(9, mk<Map>(500, 100, 0));

    typename Map::Order before{};
    ASSERT_TRUE(m.reduce(9, 200, &before));
    EXPECT_EQ(before.shares, 500u);
    EXPECT_EQ(before.price, 100u);

    typename Map::Order o{};
    ASSERT_TRUE(m.find(9, &o));
    EXPECT_EQ(o.shares, 300u);
    EXPECT_EQ(m.size(), 1u);
}

template <class Map>
void a_full_fill_removes_the_order() {
    Map m(64);
    m.insert(9, mk<Map>(500, 100, 0));

    typename Map::Order before{};
    ASSERT_TRUE(m.reduce(9, 500, &before));
    EXPECT_EQ(before.shares, 500u);

    typename Map::Order o{};
    EXPECT_FALSE(m.find(9, &o));
    EXPECT_EQ(m.size(), 0u);

    m.insert(10, mk<Map>(50, 100, 0));
    ASSERT_TRUE(m.reduce(10, 999, &before));
    EXPECT_EQ(before.shares, 50u);
    EXPECT_EQ(m.size(), 0u);

    EXPECT_FALSE(m.reduce(11, 1, &before));
}

template <class Map>
void erase_reports_what_it_removed() {
    Map m(64);
    m.insert(42, mk<Map>(250, 987654, 1));

    typename Map::Order gone{};
    ASSERT_TRUE(m.erase(42, &gone));
    EXPECT_EQ(gone.shares, 250u);
    EXPECT_EQ(gone.price, 987654u);
    EXPECT_EQ(gone.side, 1);
    EXPECT_EQ(m.size(), 0u);
    EXPECT_FALSE(m.erase(42, &gone));
}

template <class Map>
void it_refuses_to_grow() {
    Map m(16);
    const std::size_t cap = m.capacity();
    std::size_t stored = 0;
    for (std::uint64_t k = 1; k < cap * 4; ++k) {
        if (!m.insert(k, mk<Map>(1, 10, 0))) break;
        ++stored;
    }
    EXPECT_EQ(m.capacity(), cap);
    EXPECT_EQ(m.size(), stored);
    EXPECT_LE(stored, cap);
    EXPECT_GT(stored, 0u);
}

template <class Map>
void it_matches_a_plain_map_under_churn() {
    Map m(1u << 14);
    std::unordered_map<std::uint64_t, typename Map::Order> ref;
    std::mt19937_64 rng(20260808);
    std::vector<std::uint64_t> live;

    for (int step = 0; step < 200000; ++step) {
        const int what = static_cast<int>(rng() % 100);
        if (what < 45 || live.empty()) {
            const std::uint64_t oid = rng() % 100000 + 1;
            if (ref.count(oid) != 0) continue;
            const typename Map::Order o =
                mk<Map>(static_cast<std::uint32_t>(rng() % 999999 + 1),
                        static_cast<std::uint32_t>(rng() % 2000000000),
                        static_cast<std::uint8_t>(rng() & 1));
            if (!m.insert(oid, o)) continue;
            ref[oid] = o;
            live.push_back(oid);
        } else {
            const std::size_t idx = rng() % live.size();
            const std::uint64_t oid = live[idx];
            const auto it = ref.find(oid);
            ASSERT_NE(it, ref.end());
            typename Map::Order got{};
            if (what < 75) {
                const std::uint32_t take =
                    static_cast<std::uint32_t>(rng() % (it->second.shares + 10) + 1);
                ASSERT_TRUE(m.reduce(oid, take, &got));
                EXPECT_EQ(got.shares, it->second.shares);
                EXPECT_EQ(got.price, it->second.price);
                EXPECT_EQ(got.side, it->second.side);
                if (take >= it->second.shares) {
                    ref.erase(it);
                    live[idx] = live.back();
                    live.pop_back();
                } else {
                    it->second.shares -= take;
                }
            } else {
                ASSERT_TRUE(m.erase(oid, &got));
                EXPECT_EQ(got.shares, it->second.shares);
                ref.erase(it);
                live[idx] = live.back();
                live.pop_back();
            }
        }
        ASSERT_EQ(m.size(), ref.size()) << "at step " << step;
    }

    for (const auto& [oid, want] : ref) {
        typename Map::Order got{};
        ASSERT_TRUE(m.find(oid, &got)) << "lost " << oid;
        EXPECT_EQ(got.shares, want.shares);
        EXPECT_EQ(got.price, want.price);
        EXPECT_EQ(got.side, want.side);
    }
}

}
