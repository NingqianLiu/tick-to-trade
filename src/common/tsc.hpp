#pragma once

// The clock the send loop keeps time by.
//
// One thing to separate first: what is read here only decides when a packet is handed to
// the card. It never reports a latency. Both latency numbers this project prints come
// from hardware timestamps the card itself writes, and nothing in this file touches them.
// So a few nanoseconds of error here only move the moment a packet goes out; it cannot
// reach the measurement.
//
// The other usual suspicion is frequency scaling: if the core slows down, is the counter
// still right? On this chip, yes. It runs at a fixed rate whatever the core is doing.
// Older processors did not, and on those this counter cannot be used for pacing at all.

// clock_gettime and timespec, for the kernel clock the calibration below compares against.
#include <time.h>

// __rdtsc reads the counter; _mm_lfence is the barrier that stops it being moved.
#include <x86intrin.h>

// std::uint64_t: both the counter and the nanosecond values are 64 bit.
#include <cstdint>

namespace tsc {

// Reads the counter: ticks since the machine started. About twenty cycles, the cheapest
// way to ask the time on this machine.
[[nodiscard]] inline std::uint64_t now() noexcept { return __rdtsc(); }

// The same read with a barrier on each side, so the processor cannot move it.
//
// rdtsc keeps no order with anything around it. It can be run before the work in front of
// it has finished, or after the work behind it has started.
//
// Over a few microseconds that does not matter. Over a hundred or two hundred nanoseconds,
// twenty or thirty cycles of movement is more than ten percent, and in the worst case the
// answer comes out as almost no time at all.
//
// The barrier costs about twenty cycles, which is why the pacing loop does not use this
// one - a little error there is harmless - while the segment timing does.
[[nodiscard]] inline std::uint64_t fenced() noexcept {
    // Nothing after this may start until every read before it has finished, so the counter
    // read cannot be pulled in front of the work being measured.
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    // And nothing behind it can be pulled in front of the read.
    _mm_lfence();
    // Two of these subtracted, divided by the ticks per nanosecond below, is how long a
    // stretch took.
    return t;
}

// The kernel's monotonic clock in nanoseconds. Monotonic means it never jumps forward or
// backward because somebody changed the system time, so it is safe for measuring how long
// something took. It is far more expensive than rdtsc, so it is only used for calibration
// and at the end of a run, never on the hot path.
[[nodiscard]] inline std::uint64_t monotonic_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    // Widened before the multiplication, or the seconds would overflow.
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

// Measures how many counter ticks go by per nanosecond, by watching both clocks over a
// stretch and dividing. Twenty milliseconds by default, long enough that the cost of
// reading the clocks disappears into it.
[[nodiscard]] inline double ticks_per_ns(std::uint64_t settle_ns = 20000000) {
    // The expensive clock first and the cheap one second, so only one cheap call sits
    // between the two readings.
    const std::uint64_t t0 = monotonic_ns();
    const std::uint64_t c0 = now();
    // Spin rather than sleep. Sleeping would let the thread be scheduled away and the core
    // drop into a low power state, and the cost of waking up again would land inside the
    // measurement.
    while (monotonic_ns() - t0 < settle_ns) {
    }
    // The other way round at this end - counter first, kernel clock second - so that what
    // each pair of readings includes at one end it excludes at the other, and the two
    // errors largely cancel.
    const std::uint64_t c1 = now();
    const std::uint64_t t1 = monotonic_ns();
    // Ticks over nanoseconds. The caller uses it to turn a difference of two counter reads
    // into nanoseconds, which is what every printed percentile depends on.
    return static_cast<double>(c1 - c0) / static_cast<double>(t1 - t0);
}

}  // namespace tsc
