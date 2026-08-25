#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <cstdint>
#include <thread>
#include <vector>

#include "net/ring.hpp"

namespace {

using ring::Ring;
using ring::View;
using State = ring::Ring::State;

TEST(Ring, an_empty_ring_makes_a_reader_wait) {
    Ring r(8);
    View v;
    EXPECT_EQ(r.take(1, &v), State::kWaiting);
    EXPECT_EQ(r.published(), 0u);
}

TEST(Ring, what_goes_in_comes_out) {
    Ring r(8);
    const std::uint8_t payload[4] = {1, 2, 3, 4};
    r.publish(payload, 4, 123456789, 7);

    View v;
    ASSERT_EQ(r.take(1, &v), State::kReady);
    EXPECT_EQ(v.buf, payload);
    EXPECT_EQ(v.len, 4u);
    EXPECT_EQ(v.hw_ts, 123456789u);
    EXPECT_EQ(v.flags, 7u);
    EXPECT_EQ(r.published(), 1u);
    EXPECT_EQ(r.take(2, &v), State::kWaiting);
}

TEST(Ring, sequence_numbers_run_from_one) {
    Ring r(8);
    const std::uint8_t byte = 0;
    for (int i = 0; i < 5; ++i) r.publish(&byte, static_cast<std::uint32_t>(i), i);
    View v;
    for (std::uint64_t s = 1; s <= 5; ++s) {
        ASSERT_EQ(r.take(s, &v), State::kReady) << "at " << s;
        EXPECT_EQ(v.len, s - 1);
        EXPECT_EQ(v.hw_ts, s - 1);
    }
}

TEST(Ring, a_reader_left_behind_is_told_so) {
    Ring r(4);
    const std::uint8_t byte = 0;
    for (int i = 0; i < 6; ++i) r.publish(&byte, 1, 0);

    View v;
    EXPECT_EQ(r.take(1, &v), State::kLapped);
    EXPECT_EQ(r.take(2, &v), State::kLapped);
    EXPECT_EQ(r.take(3, &v), State::kReady);
    EXPECT_EQ(r.take(6, &v), State::kReady);
    EXPECT_EQ(r.take(7, &v), State::kWaiting);
}

TEST(Ring, slots_round_up_to_a_power_of_two) {
    EXPECT_EQ(Ring(5).slots(), 8u);
    EXPECT_EQ(Ring(64).slots(), 64u);
    EXPECT_EQ(Ring(65).slots(), 128u);
}

TEST(Ring, every_reader_sees_every_message_with_its_payload) {
    constexpr std::uint64_t kMessages = 200000;
    constexpr int kReaders = 3;
    Ring r(1u << 16);

    std::vector<std::uint64_t> payload(r.slots(), 0);
    std::atomic<bool> go{false};
    std::atomic<int> bad{0};
    std::vector<std::atomic<std::uint64_t>> at(kReaders);
    for (auto& a : at) a.store(0, std::memory_order_relaxed);

    std::vector<std::thread> readers;
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&, t] {
            while (!go.load(std::memory_order_acquire)) {}
            ring::Cursor cur;
            View v;
            while (cur.want <= kMessages) {
                const State s = r.take(cur.want, &v);
                if (s == State::kWaiting) continue;
                if (s == State::kLapped) { ++bad; return; }
                std::uint64_t saw = 0;
                std::memcpy(&saw, v.buf, sizeof(saw));
                if (saw != cur.want * 3 + 1 || v.hw_ts != cur.want * 7) ++bad;
                ++cur.want;
                at[t].store(cur.want, std::memory_order_release);
            }
        });
    }

    go.store(true, std::memory_order_release);
    const std::uint64_t room = r.slots() - 1024;
    for (std::uint64_t s = 1; s <= kMessages; ++s) {
        for (;;) {
            std::uint64_t slowest = s;
            for (auto& a : at) {
                const std::uint64_t v = a.load(std::memory_order_acquire);
                if (v < slowest) slowest = v;
            }
            if (s - slowest < room) break;
        }
        std::uint64_t& cell = payload[s & (r.slots() - 1)];
        cell = s * 3 + 1;
        r.publish(reinterpret_cast<const std::uint8_t*>(&cell), sizeof(cell), s * 7);
    }
    for (auto& t : readers) t.join();

    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(r.published(), kMessages);
}

TEST(Ring, each_reader_gets_a_line_of_its_own) {
    ring::Cursor c[2];
    EXPECT_EQ(sizeof(ring::Cursor), 64u);
    const auto a = reinterpret_cast<std::uintptr_t>(&c[0]);
    const auto b = reinterpret_cast<std::uintptr_t>(&c[1]);
    EXPECT_EQ(a % 64, 0u);
    EXPECT_EQ(b - a, 64u);
}

}
