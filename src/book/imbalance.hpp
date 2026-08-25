
#pragma once

#include <cstdint>

namespace book {

class Imbalance {
public:
    enum class Signal : std::uint8_t { kNone, kBuy, kSell };

    explicit constexpr Imbalance(std::uint32_t percent = 88) noexcept : pct_(percent) {}

    [[nodiscard]] constexpr Signal check(std::uint64_t bid3,
                                         std::uint64_t ask3) const noexcept {
        if (bid3 == 0 || ask3 == 0) return Signal::kNone;
        const std::uint64_t need = (bid3 + ask3) * pct_;
        if (bid3 * 100 > need) return Signal::kBuy;
        if (ask3 * 100 > need) return Signal::kSell;
        return Signal::kNone;
    }

private:
    std::uint32_t pct_;
};

}
