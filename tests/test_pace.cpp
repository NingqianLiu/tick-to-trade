#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include "net/pace.hpp"

namespace {

constexpr win::Phase kGap = win::Phase::kGap;
constexpr win::Phase kWin = win::Phase::kWindow;

TEST(Pace, the_first_packet_leaves_at_zero) {
    pace::Schedule s(500);
    EXPECT_EQ(s.next(1000000, kWin), 0);
}

TEST(Pace, the_window_keeps_the_original_spacing) {
    pace::Schedule s(500);
    EXPECT_EQ(s.next(1000, kWin), 0);
    EXPECT_EQ(s.next(1300, kWin), 300);
    EXPECT_EQ(s.next(9300, kWin), 8300);
    EXPECT_EQ(s.next(9400, win::Phase::kSettle), 8400);
    EXPECT_EQ(s.next(9500, win::Phase::kTail), 8500);
}

TEST(Pace, the_fixed_rate_charges_per_packet) {
    pace::Schedule s(500);
    EXPECT_EQ(s.next(0, kGap), 0);
    EXPECT_EQ(s.next(1000000000, kGap), 500);
    EXPECT_EQ(s.next(2000000000, kGap), 1000);
}

TEST(Pace, the_boundary_switches_to_the_original_spacing) {
    pace::Schedule s(500);
    EXPECT_EQ(s.next(1000, kGap), 0);
    EXPECT_EQ(s.next(1700, kWin), 700);
}

TEST(Pace, a_smaller_rate_shortens_the_run) {
    pace::Schedule fast(100), slow(1000);
    (void)fast.next(0, kGap);
    (void)slow.next(0, kGap);
    EXPECT_EQ(fast.next(0, kGap), 100);
    EXPECT_EQ(slow.next(0, kGap), 1000);
}

TEST(Pace, the_rate_comes_from_the_environment) {
    unsetenv("ITCH_GAP_NS");
    EXPECT_EQ(pace::gap_ns_from_env(), pace::kDefaultGapNs);
    setenv("ITCH_GAP_NS", "150", 1);
    EXPECT_EQ(pace::gap_ns_from_env(), 150);
    unsetenv("ITCH_GAP_NS");
}

}
