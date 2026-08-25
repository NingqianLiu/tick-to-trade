#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include "common/window.hpp"

namespace {

const win::Params kDefault{};
constexpr std::uint64_t kUnit = std::uint64_t{1} << win::kDefaultShift;
constexpr std::uint64_t kPeriod = kUnit * (win::kDefaultMask + 1);
constexpr std::uint64_t kSlot = win::kDefaultSlot;

void agrees_over_a_day(const win::Params& p) {
    win::Tracker t(p);
    t.open_session(0);
    for (std::uint64_t ts = 0; ts < 23400ull * 1000000000ull; ts += 1000000) {
        EXPECT_EQ((t.advance(ts) == win::Phase::kWindow), p.in_window(ts));
    }
    win::Tracker fine(p);
    fine.open_session(0);
    for (std::uint64_t k = 0; k < 3; ++k) {
        for (std::uint64_t u = 0; u <= p.mask; ++u) {
            const std::uint64_t edge = k * p.period_ns() + u * p.unit_ns();
            for (std::uint64_t d = 0; d < 2; ++d) {
                EXPECT_EQ((fine.advance(edge + d) == win::Phase::kWindow), p.in_window(edge + d));
            }
        }
    }
}

TEST(Window, tracker_agrees_with_the_predicate) {
    agrees_over_a_day(kDefault);
    agrees_over_a_day(win::Params{27, 15, 0, 0, 0});
    agrees_over_a_day(win::Params{31, 63, 63, 0, 0});
}

TEST(Window, window_geometry) {
    EXPECT_EQ(kDefault.unit_ns(), 1073741824ull);
    EXPECT_EQ(kDefault.period_ns(), 34359738368ull);
    EXPECT_TRUE(kDefault.in_window(kSlot * kUnit));
    EXPECT_TRUE(kDefault.in_window((kSlot + 1) * kUnit - 1));
    EXPECT_FALSE(kDefault.in_window((kSlot + 1) * kUnit));
    EXPECT_FALSE(kDefault.in_window(kPeriod + kSlot * kUnit - 1));
    EXPECT_TRUE(kDefault.in_window(kPeriod + kSlot * kUnit));

    constexpr std::uint64_t kOpen = 34200ull * 1000000000ull;
    constexpr std::uint64_t kShut = 57600ull * 1000000000ull;
    std::uint64_t windows = 0;
    for (std::uint64_t u = 0; u * kUnit < kShut + kPeriod; ++u) {
        const std::uint64_t open = u * kUnit;
        if (kDefault.in_window(open) && open < kShut && open + kUnit > kOpen) ++windows;
    }
    EXPECT_EQ(windows, 681);
}

TEST(Window, settle_and_tail_bracket_the_window) {
    const std::uint64_t settle = 100ull * 1000000;
    const std::uint64_t tail = 2ull * 1000000;
    win::Params p = kDefault;
    p.settle_ns = settle;
    p.tail_ns = tail;
    win::Tracker t(p);
    t.open_session(0);
    const std::uint64_t open = kPeriod + kSlot * kUnit;
    EXPECT_EQ(t.advance(open - settle - 1), win::Phase::kGap);
    EXPECT_EQ(t.advance(open - settle), win::Phase::kSettle);
    EXPECT_EQ(t.advance(open - 1), win::Phase::kSettle);
    EXPECT_EQ(t.advance(open), win::Phase::kWindow);
    EXPECT_EQ(t.advance(open + kUnit - 1), win::Phase::kWindow);
    EXPECT_EQ(t.advance(open + kUnit), win::Phase::kTail);
    EXPECT_EQ(t.advance(open + kUnit + tail - 1), win::Phase::kTail);
    EXPECT_EQ(t.advance(open + kUnit + tail), win::Phase::kGap);
    EXPECT_FALSE(win::Tracker::one_to_one(win::Phase::kGap));
    EXPECT_TRUE(win::Tracker::one_to_one(win::Phase::kSettle));
    EXPECT_TRUE(win::Tracker::one_to_one(win::Phase::kWindow));
    EXPECT_TRUE(win::Tracker::one_to_one(win::Phase::kTail));
}

TEST(Window, tail_does_not_move_the_boundary_early) {
    win::Params p = kDefault;
    p.settle_ns = 0;
    p.tail_ns = 5ull * 1000000;
    win::Tracker t(p);
    t.open_session(0);
    const std::uint64_t open = kSlot * kUnit;
    EXPECT_EQ(t.advance(open), win::Phase::kWindow);
    EXPECT_EQ(t.index(), 0);
    EXPECT_EQ(t.advance(open + kUnit + p.tail_ns - 1), win::Phase::kTail);
    EXPECT_EQ(t.index(), 0);
    EXPECT_EQ(t.advance(kPeriod + open), win::Phase::kWindow);
    EXPECT_EQ(t.index(), 1);
}

TEST(Window, only_the_regular_session_is_replayed_one_to_one) {
    win::Tracker shut(kDefault);
    EXPECT_EQ(shut.advance(kSlot * kUnit), win::Phase::kGap);
    const std::uint64_t open = kSlot * kUnit;

    win::Tracker gated(kDefault);
    gated.open_session(kPeriod);
    gated.close_session(3 * kPeriod);
    EXPECT_EQ(gated.advance(open), win::Phase::kGap);

    EXPECT_EQ(gated.advance(kPeriod + open), win::Phase::kWindow);
    EXPECT_EQ(gated.advance(2 * kPeriod + open), win::Phase::kWindow);
    EXPECT_EQ(gated.advance(3 * kPeriod + open), win::Phase::kGap);
    EXPECT_EQ(gated.index(), 3);
}

TEST(Window, the_gate_follows_the_window_not_the_message) {
    win::Params p = kDefault;
    p.settle_ns = 500ull * 1000000;
    p.tail_ns = 10ull * 1000000;
    win::Tracker t(p);
    const std::uint64_t open = kPeriod + kSlot * kUnit;
    t.open_session(open - 1);
    t.close_session(open + kUnit + 1);
    EXPECT_EQ(t.advance(open - p.settle_ns), win::Phase::kSettle);
    EXPECT_EQ(t.advance(open), win::Phase::kWindow);
    EXPECT_EQ(t.advance(open + kUnit + p.tail_ns - 1), win::Phase::kTail);
}

TEST(Window, the_session_boundaries_come_from_the_system_events) {
    win::Tracker t(kDefault);
    std::uint8_t body[12] = {};
    body[itch::kTypeOff] = 'S';
    const auto stamp = [&](std::uint64_t ns, char code) {
        for (int i = 0; i < 6; ++i) {
            body[itch::kTimestampOff + i] = static_cast<std::uint8_t>(ns >> (8 * (5 - i)));
        }
        body[itch::kHeaderLen] = static_cast<std::uint8_t>(code);
        win::note_session(itch::Message{body, sizeof(body)}, &t);
    };
    const std::uint64_t open = kSlot * kUnit;
    stamp(kPeriod, itch::kEventStartOfMarketHours);
    stamp(3 * kPeriod, itch::kEventEndOfMarketHours);
    EXPECT_EQ(t.advance(open), win::Phase::kGap);
    EXPECT_EQ(t.advance(kPeriod + open), win::Phase::kWindow);
    EXPECT_EQ(t.advance(3 * kPeriod + open), win::Phase::kGap);
    stamp(4 * kPeriod, itch::kEventStartOfSystemHours);
    EXPECT_EQ(t.advance(4 * kPeriod + open), win::Phase::kGap);
}

TEST(Window, index_counts_windows) {
    win::Tracker t(kDefault);
    t.open_session(0);
    EXPECT_EQ(t.index(), 0);
    (void)t.advance(kSlot * kUnit);
    EXPECT_EQ(t.index(), 0);
    (void)t.advance(kPeriod + kSlot * kUnit);
    EXPECT_EQ(t.index(), 1);
    (void)t.advance(680 * kPeriod + kSlot * kUnit);
    EXPECT_EQ(t.index(), 680);
}

TEST(Window, env_overrides) {
    EXPECT_EQ(win::params_from_env().slot, win::kDefaultSlot);
    EXPECT_EQ(win::params_from_env().settle_ns, win::kDefaultSettleMs * 1000000);
    EXPECT_EQ(win::params_from_env().tail_ns, win::kDefaultTailMs * 1000000);
    setenv("ITCH_WINDOW_SHIFT", "28", 1);
    setenv("ITCH_WINDOW_MASK", "63", 1);
    setenv("ITCH_WINDOW_SLOT", "7", 1);
    setenv("ITCH_WINDOW_SETTLE_MS", "250", 1);
    setenv("ITCH_WINDOW_TAIL_MS", "3", 1);
    const win::Params p = win::params_from_env();
    EXPECT_TRUE(p.shift == 28 && p.mask == 63 && p.slot == 7);
    EXPECT_EQ(p.settle_ns, 250000000ull);
    EXPECT_EQ(p.tail_ns, 3000000ull);
    EXPECT_EQ(p.unit_ns(), (std::uint64_t{1} << 28));
    EXPECT_EQ(p.period_ns(), (std::uint64_t{1} << 28) * 64);
    EXPECT_TRUE(p.in_window(7 * p.unit_ns()));
    EXPECT_FALSE(p.in_window(8 * p.unit_ns()));
    unsetenv("ITCH_WINDOW_SHIFT");
    unsetenv("ITCH_WINDOW_MASK");
    unsetenv("ITCH_WINDOW_SLOT");
    unsetenv("ITCH_WINDOW_SETTLE_MS");
    unsetenv("ITCH_WINDOW_TAIL_MS");
}

}
