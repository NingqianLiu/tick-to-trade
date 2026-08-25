#include <gtest/gtest.h>

#include <cstdint>

#include "net/pack.hpp"

namespace {

constexpr std::size_t kRec = 36;
constexpr win::Phase kGap = win::Phase::kGap;
constexpr win::Phase kWin = win::Phase::kWindow;

TEST(Pack, an_empty_packet_never_closes) {
    const pkt::Packing p;
    EXPECT_TRUE(p.empty());
    EXPECT_FALSE(p.should_close(0, kRec, kGap));
    EXPECT_FALSE(p.should_close(1000000, cfg::kMaxPacketPayload, win::Phase::kWindow));
}

TEST(Pack, the_window_is_measured_from_the_first_message) {
    pkt::Packing p;
    p.add(1000, kRec, kWin);
    p.add(1000 + cfg::kCoalesceNs - 1, kRec, kWin);
    EXPECT_FALSE(p.should_close(1000 + cfg::kCoalesceNs - 1, kRec, kWin));
    EXPECT_TRUE(p.should_close(1000 + cfg::kCoalesceNs, kRec, kWin));
    EXPECT_EQ(p.open_ns(), 1000);
    EXPECT_EQ(p.count(), 2);
}

TEST(Pack, the_fixed_rate_stretch_ignores_the_window) {
    pkt::Packing p;
    p.add(1000, kRec, kGap);
    EXPECT_FALSE(p.should_close(1000 + cfg::kCoalesceNs, kRec, kGap));
    EXPECT_FALSE(p.should_close(1000 + 1000000, kRec, kGap));
    while (!p.should_close(9999999, kRec, kGap)) p.add(9999999, kRec, kGap);
    EXPECT_GT(p.payload() + kRec, cfg::kMaxPacketPayload);
    EXPECT_TRUE(pkt::Packing{}.empty());
}

TEST(Pack, the_frame_limit_closes_it_early) {
    pkt::Packing p;
    std::uint16_t n = 0;
    while (!p.should_close(0, kRec, kWin)) {
        p.add(0, kRec, kWin);
        ++n;
    }
    EXPECT_EQ(p.payload(), n * kRec);
    EXPECT_LE(p.payload(), cfg::kMaxPacketPayload);
    EXPECT_GT(p.payload() + kRec, cfg::kMaxPacketPayload);
    EXPECT_EQ(n, cfg::kMaxPacketPayload / kRec);
}

TEST(Pack, a_change_of_pacing_closes_it) {
    pkt::Packing p;
    p.add(0, kRec, kGap);
    EXPECT_FALSE(p.should_close(1, kRec, kGap));
    EXPECT_TRUE(p.should_close(1, kRec, win::Phase::kSettle));
    EXPECT_TRUE(p.should_close(1, kRec, kWin));
    EXPECT_TRUE(p.should_close(1, kRec, win::Phase::kTail));
}

TEST(Pack, closing_starts_a_fresh_packet) {
    pkt::Packing p;
    p.add(500, kRec, kWin);
    p.close();
    EXPECT_TRUE(p.empty());
    EXPECT_EQ(p.payload(), 0);
    p.add(9000, kRec, kGap);
    EXPECT_EQ(p.open_ns(), 9000);
    EXPECT_EQ(p.phase(), kGap);
}

}
