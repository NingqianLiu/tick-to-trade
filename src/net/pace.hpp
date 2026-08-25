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

class Schedule {
public:
    explicit Schedule(std::uint64_t gap_ns) noexcept : gap_ns_(gap_ns) {}

    [[nodiscard]] std::uint64_t next(std::uint64_t ts_ns, win::Phase p) noexcept {
        if (started_) {
            now_ += win::Tracker::one_to_one(p)
                        ? (ts_ns > prev_ts_ ? ts_ns - prev_ts_ : 0)
                        : gap_ns_;
        }
        started_ = true;
        prev_ts_ = ts_ns;
        return now_;
    }

    [[nodiscard]] std::uint64_t gap_ns() const noexcept { return gap_ns_; }

private:
    std::uint64_t gap_ns_;
    std::uint64_t now_ = 0;
    std::uint64_t prev_ts_ = 0;
    bool started_ = false;
};

}
