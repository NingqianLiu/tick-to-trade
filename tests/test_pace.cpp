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

TEST(Pace, the_speed_divides_the_original_spacing) {
    pace::Schedule s(500, 10 * pace::kUnitSpeed);
    EXPECT_EQ(s.next(1000, kWin), 0);
    EXPECT_EQ(s.next(1300, kWin), 30);
    EXPECT_EQ(s.next(9300, kWin), 830);
}

TEST(Pace, the_speed_leaves_the_fixed_rate_alone) {
    pace::Schedule s(500, 10 * pace::kUnitSpeed);
    EXPECT_EQ(s.next(0, kGap), 0);
    EXPECT_EQ(s.next(1000000000, kGap), 500);
}

TEST(Pace, the_leftover_of_the_division_is_carried) {
    pace::Schedule s(500, 3 * pace::kUnitSpeed);
    EXPECT_EQ(s.next(1000, kWin), 0);
    EXPECT_EQ(s.next(1100, kWin), 33);
    EXPECT_EQ(s.next(1200, kWin), 66);
    EXPECT_EQ(s.next(1300, kWin), 100);
}

TEST(Pace, no_speed_means_the_exchange_speed) {
    pace::Schedule s(500);
    EXPECT_EQ(s.speed(), pace::kUnitSpeed);
    EXPECT_EQ(s.next(1000, kWin), 0);
    EXPECT_EQ(s.next(1300, kWin), 300);
}

TEST(Pace, the_speed_is_read_off_the_command_line) {
    EXPECT_EQ(pace::speed_from_text("1x"), pace::kUnitSpeed);
    EXPECT_EQ(pace::speed_from_text("10"), 10 * pace::kUnitSpeed);
    EXPECT_EQ(pace::speed_from_text("100x"), 100 * pace::kUnitSpeed);
    EXPECT_EQ(pace::speed_from_text("2.5x"), 2500u);
    EXPECT_EQ(pace::speed_from_text("0.5x"), 500u);
    EXPECT_EQ(pace::speed_from_text("0x"), 0u);
    EXPECT_EQ(pace::speed_from_text("-2x"), 0u);
    EXPECT_EQ(pace::speed_from_text("fast"), 0u);
    EXPECT_EQ(pace::speed_from_text("10y"), 0u);
    EXPECT_EQ(pace::speed_from_text("10xx"), 0u);
    EXPECT_EQ(pace::speed_from_text(""), 0u);
}

}
