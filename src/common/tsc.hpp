#pragma once

// The clock the sending loop uses to keep to its schedule.
//
// The easiest thing to confuse is worth settling first: the clock read here is only used to
// decide when to hand a packet to the card, and never to report a latency. Both latency numbers a
// run produces come from the hardware timestamps the card stamps itself, and have nothing to do
// with this file. So being a few nanoseconds out here does not matter - what is out is the moment
// a packet was handed over, and it cannot reach the measurement.
//
// Another thing often suspected: the processor changes its clock speed, so is this counter still
// right?
// On this chip it is. It runs at a fixed frequency.
// However fast the core is running, and whether or not it has slowed down, this counter goes at
// the same speed.
// Older processors are not like that. On such a machine this counter cannot be used for pacing at
// all.

#include <time.h>

#include <x86intrin.h>

#include <cstdint>

namespace tsc {

// Reads the processor's counter once, returning how many ticks have passed since the machine
// started.
// The instruction is twenty odd cycles and is the cheapest way of taking a moment on this
// machine.
[[nodiscard]] inline std::uint64_t now() noexcept { return __rdtsc(); }

// The same counter, with a fence on each side so the processor may not move it about.
//
// Why this is ever needed: it seems as though reading a clock would honestly run where it is
// written.
// In fact rdtsc keeps no order with anything at all.
// The processor may bring it forward, running it while the work before it is not finished.
// It may equally hold it back, running it after the work after it has begun.
//
// Measuring a path of several microseconds, none of that matters.
// Measuring a path of one or two hundred nanoseconds, moving it twenty or thirty cycles is an
// error of over ten percent.
// At worst it can measure something as having taken almost no time at all.
//
// The cost is about twenty cycles a fence. So the pacing loop does not use it - being a little
// out there does not matter - while the timing of segments does, since being right is exactly
// what it wants.
[[nodiscard]] inline std::uint64_t fenced() noexcept {
    // What the first one means is: no instruction after it may begin until every read before it
    // has finished.
    // So there is no way for the rdtsc to be brought forward ahead of the work being measured.
    _mm_lfence();
    // The counter is really read. Caught between two fences, what it reads is this moment.
    const std::uint64_t t = __rdtsc();
    // The second one stops the work after it being brought forward ahead of this read.
    _mm_lfence();
    // The tick count goes to the caller. Subtracting two such reads and dividing by the ticks per
    // nanosecond below gives how many nanoseconds that stretch took.
    return t;
}

// Reads the kernel's monotonic clock, in nanoseconds.
// Monotonic means it cannot jump forward or backward because somebody changed the system's time,
// so measuring how long something took by it is safe.
// It is far dearer than rdtsc, so it is only used for calibration and at the end, and never once
// on the hot path.
[[nodiscard]] inline std::uint64_t monotonic_ns() noexcept {
    // The kernel fills the seconds and the nanoseconds into this structure, which is cleared
    // first.
    timespec ts{};
    // CLOCK_MONOTONIC is the clock above that does not jump.
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    // The seconds times a thousand million plus the nanoseconds make one count of nanoseconds.
    // Both are widened to sixty four bits before the multiplication, or the seconds times a
    // thousand million would overflow.
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

// This is the body of the file: measure how many ticks the processor's counter takes per
// nanosecond.
// The way it is done is watching two clocks at once - how many nanoseconds the kernel's clock
// went and how many ticks the processor's counter went - and dividing one by the other. It
// measures for twenty milliseconds by default, long enough for the cost of the two clock reads
// themselves to disappear.
[[nodiscard]] inline double ticks_per_ns(std::uint64_t settle_ns = 20000000) {
    // The dear one, the kernel's clock, is read first and the cheap one after. That way only one
    // cheap call sits between the two reads and the error is smallest.
    const std::uint64_t t0 = monotonic_ns();
    // The counter's starting point follows at once. The gap between the two reads counts as
    // error, so they are read together.
    const std::uint64_t c0 = now();
    // Spin until the kernel's clock has gone the length asked for.
    // It deliberately does nothing here, and deliberately does not sleep.
    // Sleeping would have the thread scheduled out and let the core go into a power saving state.
    // The cost of waking from it would all be counted into this measurement.
    while (monotonic_ns() - t0 < settle_ns) {
    }
    // The counter's end point is read first. The order is the opposite of the start - kernel then
    // counter there, counter then kernel here - so that the extra cost at each end cancels part
    // of itself out.
    const std::uint64_t c1 = now();
    // The dear one is read last. So the small amount of cost the two kernel reads enclose extra
    // roughly cancels the small amount the two counter reads enclose less.
    const std::uint64_t t1 = monotonic_ns();
    // The ticks the counter went divided by the nanoseconds the kernel's clock went. The caller
    // uses it to turn a difference of two rdtsc readings into nanoseconds, which is needed
    // everywhere when the percentiles are printed at the end.
    return static_cast<double>(c1 - c0) / static_cast<double>(t1 - t0);
}

}  // namespace tsc
