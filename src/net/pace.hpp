#pragma once

// When each packet goes on the wire.
//
// A day of market data is replayed at two rhythms: inside a measurement window, in the warm up
// before it and in the tail after it, the real spacing of the day is kept; everywhere else the
// packets are pushed out at a fixed speed.
// Why not the real speed throughout?
// The other 95% of a day's messages at the original speed would make a round six and a half
// hours.
// Nothing could be iterated on at that rate.
// And that 95% cannot simply be left out, or the book would be incomplete.
//
// The fixed rhythm is one packet every ITCH_GAP_NS nanoseconds.
// Note that it counts packets and not messages: outside a window a packet is only sent when a
// frame is full, forty odd messages at a time, so the two ways of counting differ by forty
// times.
// ITCH_GAP_NS is an environment variable, so changing it needs no rebuild.
// That is what the staircase test - finding how much the receiving side can take - depends on,
// since it changes speed every thirty seconds.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// win::Phase says which stretch of the timeline this is.
// win::Tracker::one_to_one says whether that stretch keeps the real spacing.
// The two rhythms are switched by it.
#include "common/window.hpp"

namespace pace {

// The default of the fixed pace: every ten thousand nanoseconds, which is a packet every ten
// microseconds.
// At that speed the full speed stretch is eighty or ninety times the real rhythm of the day.
// 97.7% of a day's packets are in that stretch, and they produce not one sample.
inline constexpr std::uint64_t kDefaultGapNs = 10000;

// Reads the fixed pace's interval from the environment, using the default when it is not set.
[[nodiscard]] inline std::uint64_t gap_ns_from_env() {
    // Read the variable. Unset, it returns a null pointer.
    const char* v = std::getenv("ITCH_GAP_NS");
    // Unset means the default of ten microseconds; set, it is parsed as decimal.
    const std::uint64_t ns =
        v == nullptr ? kDefaultGapNs : std::strtoull(v, nullptr, 10);
    // Zero is not valid. It means no waiting at all, which would push the whole file out in one
    // breath and the receiving side would certainly lose packets - and that mistake only shows
    // once the round has finished, wasting the whole of it. So it is stopped here.
    if (ns == 0) {
        // Which variable is wrong is said, so an exit code is not something to guess at.
        std::fprintf(stderr, "ITCH_GAP_NS must be positive\n");
        // The process ends here. This runs before the round starts and there is nothing to
        // clean up.
        std::exit(2);
    }
    // The interval goes to the caller, which uses it to build the Schedule below.
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

// Turns each packet into which nanosecond after the start it should leave at.
// One packet at a time, and they have to come in order.
// It moves forward by comparing against the previous packet. Fed out of order, the times it
// works out are wrong.
class Schedule {
public:
    // Construction only records the fixed pace's interval; the rest of the state is settled by
    // the first packet.
    explicit Schedule(std::uint64_t gap_ns, std::uint32_t speed = kUnitSpeed) noexcept
        : gap_ns_(gap_ns), speed_(speed) {}

    // This is the body of the class.
    // It takes the time of this packet's last message and the stretch it belongs to.
    // It returns which nanosecond after the start it should go out at. The first packet is
    // always 0.
    //
    // Why the time of the last message?
    // Because a packet cannot go out earlier than its own last message.
    [[nodiscard]] std::uint64_t next(std::uint64_t ts_ns, win::Phase p) noexcept {
        // The first packet does not move forward and goes out at time zero; only from the second
        // is there a wait to work out.
        if (started_) {
            // In the 1:1 stretches the wait is the real interval between these two packets on
            // the day.
            // Elsewhere it is the fixed interval. Note that what is added is gap_ns_ rather than
            // the real interval, so when a packet in the full speed stretch goes out has nothing
            // to do with the day.
            if (win::Tracker::one_to_one(p)) {
                // Times should only ever move forward, but the file really does contain equal
                // and even backward ones. Those are treated as zero - this is unsigned
                // subtraction, and one step backward would work out an astronomical wait and the
                // whole replay would hang there.
                const std::uint64_t d =
                    (ts_ns > prev_ts_ ? ts_ns - prev_ts_ : 0) * kUnitSpeed + rem_;
                now_ += d / speed_;
                rem_ = d % speed_;
            } else {
                now_ += gap_ns_;
            }
        }
        // Once the first packet is through, this flag never goes back.
        // The only reason it exists is that the first packet has no previous packet to compare
        // against.
        // Could prev_ts_ == 0 serve instead? No, a time really can be zero.
        started_ = true;
        // This packet's time is recorded, for the next packet to work out the real interval
        // from.
        prev_ts_ = ts_ns;
        // The moment this packet should go out, in nanoseconds from the start.
        // The sender spins until the processor's counter reaches it and then hands the packet to
        // the card.
        return now_;
    }

    // What the fixed pace's interval is. Printed at the start of a run, to confirm which setting
    // this round uses.
    [[nodiscard]] std::uint64_t gap_ns() const noexcept { return gap_ns_; }

    [[nodiscard]] std::uint32_t speed() const noexcept { return speed_; }

private:
    // The fixed pace's interval, settled at construction and never changed.
    std::uint64_t gap_ns_;
    // Which nanosecond has been reached. Every packet moves it forward once.
    std::uint64_t now_ = 0;
    // The ITCH time of the previous packet's last message, used to work out the real interval.
    std::uint64_t prev_ts_ = 0;
    // Whether any packet has been fed. The first one adds no wait at all.
    bool started_ = false;
    std::uint32_t speed_ = kUnitSpeed;
    std::uint64_t rem_ = 0;
};

}  // namespace pace
