#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "common/window.hpp"

namespace pace {

inline constexpr std::uint64_t kDefaultGapNs = 10000;

[[nodiscard]] inline std::uint64_t gap_ns_from_env() {
    const char* v = std::getenv("ITCH_GAP_NS");
    const std::uint64_t ns =
        v == nullptr ? kDefaultGapNs : std::strtoull(v, nullptr, 10);
    if (ns == 0) {
        std::fprintf(stderr, "ITCH_GAP_NS must be positive\n");
        std::exit(2);
    }
    return ns;
}

inline constexpr std::uint32_t kUnitSpeed = 1000;

[[nodiscard]] inline std::uint32_t speed_from_text(const char* s) noexcept {
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s) return 0;
    if (*end == 'x' || *end == 'X') ++end;
    if (*end != '\0') return 0;
    if (!(v > 0.0) || v > 1e6) return 0;
    return static_cast<std::uint32_t>(v * kUnitSpeed + 0.5);
}

class Schedule {
public:
    explicit Schedule(std::uint64_t gap_ns, std::uint32_t speed = kUnitSpeed) noexcept
        : gap_ns_(gap_ns), speed_(speed) {}

    [[nodiscard]] std::uint64_t next(std::uint64_t ts_ns, win::Phase p) noexcept {
        if (started_) {
            if (win::Tracker::one_to_one(p)) {
                const std::uint64_t d =
                    (ts_ns > prev_ts_ ? ts_ns - prev_ts_ : 0) * kUnitSpeed + rem_;
                now_ += d / speed_;
                rem_ = d % speed_;
            } else {
                now_ += gap_ns_;
            }
        }
        started_ = true;
        prev_ts_ = ts_ns;
        return now_;
    }

    [[nodiscard]] std::uint64_t gap_ns() const noexcept { return gap_ns_; }

    [[nodiscard]] std::uint32_t speed() const noexcept { return speed_; }

private:
    std::uint64_t gap_ns_;
    std::uint64_t now_ = 0;
    std::uint64_t prev_ts_ = 0;
    bool started_ = false;
    std::uint32_t speed_ = kUnitSpeed;
    std::uint64_t rem_ = 0;
};

}
