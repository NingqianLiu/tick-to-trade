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

// Inside a window the original spacing is the whole point, so the send times
// have to follow the timestamps and nothing else.
TEST(Pace, the_window_keeps_the_original_spacing) {
    pace::Schedule s(500);
    EXPECT_EQ(s.next(1000, kWin), 0);
    EXPECT_EQ(s.next(1300, kWin), 300);
    EXPECT_EQ(s.next(9300, kWin), 8300);
    // The settle period and the tail are replayed the same way.
    EXPECT_EQ(s.next(9400, win::Phase::kSettle), 8400);
    EXPECT_EQ(s.next(9500, win::Phase::kTail), 8500);
}

// Outside it the timestamps are ignored and every packet costs the same, no
// matter how many messages it carries.
TEST(Pace, the_fixed_rate_charges_per_packet) {
    pace::Schedule s(500);
    EXPECT_EQ(s.next(0, kGap), 0);
    EXPECT_EQ(s.next(1000000000, kGap), 500);
    EXPECT_EQ(s.next(2000000000, kGap), 1000);
}

// Crossing into a window uses the real gap to the last fixed-rate packet, so
// the window opens at its own spacing rather than a rate that is not its own.
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

// A speed of 10x divides the spacing the exchange had by ten and changes nothing else.
TEST(Pace, the_speed_divides_the_original_spacing) {
    pace::Schedule s(500, 10 * pace::kUnitSpeed);
    EXPECT_EQ(s.next(1000, kWin), 0);
    EXPECT_EQ(s.next(1300, kWin), 30);
    EXPECT_EQ(s.next(9300, kWin), 830);
}

// The fixed rate stretch has a spacing we chose rather than one the market had, so the
// speed multiplier has to leave it alone.
TEST(Pace, the_speed_leaves_the_fixed_rate_alone) {
    pace::Schedule s(500, 10 * pace::kUnitSpeed);
    EXPECT_EQ(s.next(0, kGap), 0);
    EXPECT_EQ(s.next(1000000000, kGap), 500);
}

// What the division leaves over has to be carried. At 3x a spacing of 100 ns works out
// at 33 and a third, and throwing the fraction away loses a nanosecond every three
// packets: tens of microseconds over a window, and drift inside a window is exactly what
// this program exists to measure.
TEST(Pace, the_leftover_of_the_division_is_carried) {
    pace::Schedule s(500, 3 * pace::kUnitSpeed);
    EXPECT_EQ(s.next(1000, kWin), 0);
    EXPECT_EQ(s.next(1100, kWin), 33);
    EXPECT_EQ(s.next(1200, kWin), 66);
    EXPECT_EQ(s.next(1300, kWin), 100);
}

// With no speed given the replay runs at the exchange's own pace, exactly as it did
// before the multiplier existed.
TEST(Pace, no_speed_means_the_exchange_speed) {
    pace::Schedule s(500);
    EXPECT_EQ(s.speed(), pace::kUnitSpeed);
    EXPECT_EQ(s.next(1000, kWin), 0);
    EXPECT_EQ(s.next(1300, kWin), 300);
}

// The forms accepted on the command line, and the 0 returned for anything else so the
// caller can exit instead of guessing.
TEST(Pace, the_speed_is_read_off_the_command_line) {
    EXPECT_EQ(pace::speed_from_text("1x"), pace::kUnitSpeed);
    EXPECT_EQ(pace::speed_from_text("10"), 10 * pace::kUnitSpeed);
    EXPECT_EQ(pace::speed_from_text("100x"), 100 * pace::kUnitSpeed);
    EXPECT_EQ(pace::speed_from_text("2.5x"), 2500u);
    // Slower than the market is allowed as well, to see whether the tail follows the load.
    EXPECT_EQ(pace::speed_from_text("0.5x"), 500u);
    EXPECT_EQ(pace::speed_from_text("0x"), 0u);
    EXPECT_EQ(pace::speed_from_text("-2x"), 0u);
    EXPECT_EQ(pace::speed_from_text("fast"), 0u);
    EXPECT_EQ(pace::speed_from_text("10y"), 0u);
    EXPECT_EQ(pace::speed_from_text("10xx"), 0u);
    EXPECT_EQ(pace::speed_from_text(""), 0u);
}

}  // namespace
