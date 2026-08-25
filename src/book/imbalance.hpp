// This file implements Imbalance: it compares the total shares on the best 3 price levels of the
// two sides of the order book and decides whether to produce a buy or a sell signal.
// Having applied one ITCH message, the trader hands bid3 and ask3 to check and turns what comes
// back into an OUCH side.
// This class holds only a percentage threshold and neither a position nor the previous signal;
// the same input always gives the same result.

// The class and all its constexpr methods are defined in this header; expanding it once avoids a
// duplicate definition.
#pragma once

#include <cstdint>

// Imbalance follows an update of the order book, so it goes in the book namespace.
namespace book {

// Imbalance squeezes the best three levels of the two sides into kNone, kBuy or kSell.
// It uses integer multiplication in place of a floating point ratio and a division, which suits
// a decision every message on the hot path passes through.
// The class does not own the order book; the caller works out the total of each side and passes
// in two plain integers.
// It does no risk, position or exit logic either, and what comes back only answers whether an
// imbalance of quantity appeared this time.
class Imbalance {
// The public part offers the result type, the constructor and the check method the trader needs.
public:
    // Signal limits the outcomes plainly to three.
    // kNone means send nothing, kBuy means the buy side's shares are past the threshold, and
    // kSell means the sell side's are.
    // enum class keeps a plain integer from being mistaken for a signal, and a uint8_t holds
    // these three values easily.
    // The trader has to write Signal::kBuy or Signal::kSell out, and nothing in the code guesses
    // a direction from a true or a false.
    // The three values carry only the outcome, and no share count, price or timestamp.
    enum class Signal : std::uint8_t { kNone, kBuy, kSell };

    // The constructor keeps the ratio that fires; with no argument it is 88%.
    // explicit keeps an integer from being turned into an Imbalance by accident, and constexpr
    // lets a fixed configuration be built at compile time.
    // The initialiser list writes percent straight into pct_, so the body needs no second
    // assignment.
    // noexcept says this step only keeps an integer and never leaves through an exception.
    // The class does not limit percent's range itself; the caller and the tests are responsible
    // for passing a sensible 0 to 100.
    explicit constexpr Imbalance(std::uint32_t percent = 75) noexcept : pct_(percent) {}

    // This is the body of producing a signal.
    // bid3 and ask3 are the total shares on the best 3 prices of the two sides, from the order
    // book's top3.
    // It deals first with either side having fewer than 3 levels, and then works out "the total
    // shares of both sides times the threshold".
    // Comparing one side's shares times 100 against that value is the same as working out a
    // ratio, but with no division and no floating point error.
    // The buy side strictly past the threshold gives kBuy, the sell side strictly past it gives
    // kSell, and exactly at the threshold fires neither.
    // On a kNone the trader keeps this update of the order book but does not enter the path that
    // sends an OUCH message.
    // check is const, because it only reads pct_; there is no hidden state between one decision
    // and the next.
    // constexpr lets a test's fixed input be worked out by the compiler, and noexcept keeps the
    // hot path free of exception handling.
    [[nodiscard]] constexpr Signal check(std::uint64_t bid3,
                                         // ask3 uses the same 64 bit range as bid3, so that
                                         // adding the best three cannot overflow 32 bits.
                                         // The arguments are on two lines only for readability;
                                         // together they are one snapshot of both sides.
                                         // After the brace the tests come in the order
                                         // incomplete, buy, sell, neither.
                                         std::uint64_t ask3) const noexcept {
        // top3 returns 0 for a side that has fewer than 3 prices.
        // A zero in either bid3 or ask3 means the input is incomplete, and the missing side must
        // not be taken as genuinely having no liquidity.
        // A true returns kNone at once; a false means both sides are complete and the ratios can
        // be compared.
        // It also keeps the tests below from making a book with data on one side alone fire a
        // signal every time.
        // The || short circuits from left to right, and a bid3 already zero settles it.
        // After the return neither need is worked out nor any choice of OUCH side entered.
        // This is not treating a genuine zero as balance; it says the best three are not
        // complete yet.
        if (bid3 == 0 || ask3 == 0) return Signal::kNone;
        // need is the total shares of both sides times pct_, shared by the two directions below.
        // With 90 against 10 and a threshold of 88, for instance, need is 8800 while the buy
        // side times 100 is 9000.
        // Everything stays in 64 bit integers and the hot path needs no division.
        // need is worked out once, and the buy and the sell comparison use exactly the same
        // denominator and threshold.
        // pct_ is an integer percentage, so the comparison after multiplying by 100 stays an
        // exact relation between integers.
        // A real total of the best three is far below a uint64_t's limit, leaving plenty of room
        // for these two multiplications.
        const std::uint64_t need = (bid3 + ask3) * pct_;
        // This comparison is the same as bid3/(bid3+ask3)>pct_/100.
        // A true gives kBuy; a false means the buy side is not strictly past the threshold and
        // the sell side is looked at next.
        // It uses > rather than >=, so exactly 88% against a threshold of 88 sends nothing.
        // After a kBuy the sell branch does not run, and the trader turns it into the buy
        // direction's OUCH fields.
        // This comparison only reads the input and need, and does not change the order book in
        // return.
        if (bid3 * 100 > need) return Signal::kBuy;
        // The sell side is checked on the same basis.
        // A true gives kSell; a false means neither side is strictly past the threshold and it
        // falls through to the kNone at the end.
        // At the usual thresholds above 50%, both sides cannot pass at once.
        // The sell side uses a strict greater than symmetric with the buy side, so the behaviour
        // at the boundary does not differ by direction.
        // After a kSell, check ends and the class remembers this signal for no later message.
        if (ask3 * 100 > need) return Signal::kSell;
        // Both sides have a complete best three but neither is past the threshold, so the trader
        // is told to send nothing this time.
        // That also covers 50/50 and a small difference that has not reached the threshold.
        // pct_, bid3 and ask3 were none of them changed, and the next message decides afresh on
        // its own.
        return Signal::kNone;
    }

// The private part holds only the fixed configuration the decision needs, which a caller cannot
// change without going through the constructor.
private:
    // pct_ is the percentage one side's shares have to be strictly past; check uses the same
    // value for both sides.
    // There is no setter after construction, so the threshold does not change between messages
    // within one round.
    std::uint32_t pct_;
// The end of Imbalance; it holds no dynamic resource and needs no clearing up when destroyed.
};

}
