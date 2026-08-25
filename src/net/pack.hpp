#pragma once

#include <cstddef>
#include <cstdint>

#include "common/settings.hpp"
#include "common/window.hpp"

namespace pkt {

inline constexpr std::uint16_t kMaxMessages = 0xffff;

class Packing {
public:
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::uint16_t count() const noexcept { return count_; }
    [[nodiscard]] std::size_t payload() const noexcept { return payload_; }
    [[nodiscard]] win::Phase phase() const noexcept { return phase_; }
    [[nodiscard]] std::uint64_t open_ns() const noexcept { return open_ns_; }

    [[nodiscard]] bool should_close(std::uint64_t ts_ns, std::size_t record,
                                    win::Phase p) const noexcept {
        if (count_ == 0) return false;
        if (p != phase_ || count_ == kMaxMessages) return true;
        if (payload_ + record > cfg::kMaxPacketPayload) return true;
        return win::Tracker::one_to_one(phase_) &&
               ts_ns - open_ns_ >= cfg::kCoalesceNs;
    }

    void add(std::uint64_t ts_ns, std::size_t record, win::Phase p) noexcept {
        if (count_ == 0) {
            open_ns_ = ts_ns;
            phase_ = p;
        }
        payload_ += record;
        ++count_;
    }

    void close() noexcept {
        payload_ = 0;
        count_ = 0;
    }

private:
    std::uint64_t open_ns_ = 0;
    std::size_t payload_ = 0;
    std::uint16_t count_ = 0;
    win::Phase phase_ = win::Phase::kGap;
};

}
