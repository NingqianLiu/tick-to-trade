// The signal: compare the shares resting on the best three prices of each side of the
// book and decide whether that is lopsided enough to send an order.
//
// The trader updates the book from a message, hands the two totals to check(), and turns
// what comes back into the side of an OUCH order. This class keeps only the threshold.
// It has no position and no memory of the last signal, so the same two numbers always
// give the same answer.
#pragma once

// Fixed width integers: one byte for the answer, 32 bits for the threshold, 64 for the
// share totals.
#include <cstdint>

namespace book {

// Turns the two totals into buy, sell, or nothing. It uses integer multiplication rather
// than a division or a ratio in floating point, because this runs on every message that
// touches the book.
//
// It does not own the book: the caller adds up the two sides and passes two plain
// integers. There is no risk, position or exit logic here either - the answer is only
// about this one comparison.
class Imbalance {
public:
    // The three possible answers. An enum class rather than a bool or an int, so a
    // direction can never be confused with a count, and the trader has to write
    // Signal::kBuy or Signal::kSell where it means one.
    enum class Signal : std::uint8_t { kNone, kBuy, kSell };

    // The threshold, as a whole percent. constexpr so a fixed configuration is built at
    // compile time; explicit so an integer cannot quietly become an Imbalance. The class
    // does not police the range: passing something sensible between 0 and 100 is the
    // caller's job, and the tests', not this constructor's.
    explicit constexpr Imbalance(std::uint32_t percent = 75) noexcept : pct_(percent) {}

    // bid3 and ask3 are the shares resting on the best three prices of each side.
    //
    // A zero on either side means the book does not have three prices there yet. That is
    // missing data, not a side with no liquidity, so nothing is signalled - otherwise a
    // book with only one side would fire every single time.
    //
    // The comparison below is bid3 / (bid3 + ask3) > pct_ / 100 with both divisions
    // multiplied away, which keeps it exact and keeps a divide off the hot path. With 90
    // shares bid, 10 ask and a threshold of 75: need is 100 * 75 = 7,500 and the bid side
    // is 90 * 100 = 9,000, so it buys.
    //
    // Strictly greater, so a side sitting exactly at the threshold does not fire. Above
    // 50% both sides cannot pass at once. Nothing here is remembered for the next
    // message: check() only reads the threshold.
    [[nodiscard]] constexpr Signal check(std::uint64_t bid3,
                                         std::uint64_t ask3) const noexcept {
        if (bid3 == 0 || ask3 == 0) return Signal::kNone;
        // Worked out once and used for both directions, so the two sides are measured
        // against exactly the same number. Real totals are nowhere near the top of a
        // 64 bit integer, so there is room for the multiplications.
        const std::uint64_t need = (bid3 + ask3) * pct_;
        if (bid3 * 100 > need) return Signal::kBuy;
        if (ask3 * 100 > need) return Signal::kSell;
        // Both sides are there and neither is far enough ahead: no order this time.
        return Signal::kNone;
    }

private:
    // The percent a side has to beat. There is no setter, so it cannot change from one
    // message to the next inside a run.
    std::uint32_t pct_;
};

}
