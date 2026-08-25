#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "itch/reader.hpp"
#include "itch/types.hpp"

namespace win {

inline constexpr std::uint64_t kDefaultShift = 30;
inline constexpr std::uint64_t kDefaultMask = 31;
inline constexpr std::uint64_t kDefaultSlot = 0;
inline constexpr std::uint64_t kDefaultSettleMs = 500;
inline constexpr std::uint64_t kDefaultTailMs = 10;

struct Params {
    std::uint64_t shift = kDefaultShift;
    std::uint64_t mask = kDefaultMask;
    std::uint64_t slot = kDefaultSlot;
    std::uint64_t settle_ns = kDefaultSettleMs * 1000000;
    std::uint64_t tail_ns = kDefaultTailMs * 1000000;

    [[nodiscard]] std::uint64_t unit_ns() const noexcept {
        return std::uint64_t{1} << shift;
    }
    [[nodiscard]] std::uint64_t period_ns() const noexcept {
        return unit_ns() * (mask + 1);
    }
    [[nodiscard]] bool in_window(std::uint64_t ts_ns) const noexcept {
        return ((ts_ns >> shift) & mask) == slot;
    }
};

[[nodiscard]] inline Params params_from_env() {
    const auto get = [](const char* name, std::uint64_t fallback) {
        const char* v = std::getenv(name);
        return v == nullptr ? fallback : std::strtoull(v, nullptr, 10);
    };
    Params p{get("ITCH_WINDOW_SHIFT", kDefaultShift),
             get("ITCH_WINDOW_MASK", kDefaultMask),
             get("ITCH_WINDOW_SLOT", kDefaultSlot),
             get("ITCH_WINDOW_SETTLE_MS", kDefaultSettleMs) * 1000000,
             get("ITCH_WINDOW_TAIL_MS", kDefaultTailMs) * 1000000};
    const bool spacing_ok =
        p.mask == 0 ? p.settle_ns == 0 && p.tail_ns == 0
                    : p.settle_ns + p.tail_ns < p.period_ns() - p.unit_ns();
    if (p.shift == 0 || p.shift > 62 || ((p.mask + 1) & p.mask) != 0 ||
        p.slot > p.mask || !spacing_ok) {
        std::fprintf(stderr,
                     "bad window parameters: shift=%llu mask=%llu slot=%llu "
                     "settle_ns=%llu tail_ns=%llu\n",
                     static_cast<unsigned long long>(p.shift),
                     static_cast<unsigned long long>(p.mask),
                     static_cast<unsigned long long>(p.slot),
                     static_cast<unsigned long long>(p.settle_ns),
                     static_cast<unsigned long long>(p.tail_ns));
        std::exit(2);
    }
    return p;
}

enum class Phase { kGap, kSettle, kWindow, kTail };

class Tracker {
public:
    explicit Tracker(const Params& p) noexcept
        : settle_(p.settle_ns),
          tail_(p.tail_ns),
          period_(p.period_ns()),
          open_(p.slot * p.unit_ns()),
          close_(open_ + p.unit_ns()) {}

    void open_session(std::uint64_t ts_ns) noexcept { session_open_ = ts_ns; }
    void close_session(std::uint64_t ts_ns) noexcept { session_close_ = ts_ns; }

    [[nodiscard]] Phase advance(std::uint64_t ts_ns) noexcept {
        while (ts_ns >= close_ + tail_) {
            open_ += period_;
            close_ += period_;
            ++index_;
        }
        if (open_ >= session_close_ || close_ <= session_open_) return Phase::kGap;
        if (ts_ns >= close_) return Phase::kTail;
        if (ts_ns >= open_) return Phase::kWindow;
        if (ts_ns + settle_ >= open_) return Phase::kSettle;
        return Phase::kGap;
    }

    [[nodiscard]] static bool one_to_one(Phase p) noexcept {
        return p != Phase::kGap;
    }

    [[nodiscard]] std::uint64_t index() const noexcept { return index_; }

    [[nodiscard]] std::uint64_t open() const noexcept { return open_; }

private:
    std::uint64_t settle_;
    std::uint64_t tail_;
    std::uint64_t period_;
    std::uint64_t open_;
    std::uint64_t close_;
    std::uint64_t index_ = 0;
    std::uint64_t session_open_ = ~std::uint64_t{0};
    std::uint64_t session_close_ = ~std::uint64_t{0};
};

inline void note_session(const itch::Message& m, Tracker* t) {
    if (m.type() != 'S' || m.len <= itch::kHeaderLen) return;
    const char code = static_cast<char>(m.body[itch::kHeaderLen]);
    if (code == itch::kEventStartOfMarketHours) {
        t->open_session(m.timestamp());
    } else if (code == itch::kEventEndOfMarketHours) {
        t->close_session(m.timestamp());
    }
}

}
