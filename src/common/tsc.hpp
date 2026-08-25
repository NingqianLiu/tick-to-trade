#pragma once

#include <time.h>

#include <x86intrin.h>

#include <cstdint>

namespace tsc {

[[nodiscard]] inline std::uint64_t now() noexcept { return __rdtsc(); }

[[nodiscard]] inline std::uint64_t fenced() noexcept {
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

[[nodiscard]] inline std::uint64_t monotonic_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

[[nodiscard]] inline double ticks_per_ns(std::uint64_t settle_ns = 20000000) {
    const std::uint64_t t0 = monotonic_ns();
    const std::uint64_t c0 = now();
    while (monotonic_ns() - t0 < settle_ns) {
    }
    const std::uint64_t c1 = now();
    const std::uint64_t t1 = monotonic_ns();
    return static_cast<double>(c1 - c0) / static_cast<double>(t1 - t0);
}

}
