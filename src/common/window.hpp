#pragma once

// Which nanoseconds of the day count - the measurement window.
//
// How do the two ends agree on the same stretch? They do not talk. Both include this
// header and work it out themselves, and they cannot disagree, because the test is a pure
// function of the timestamp inside the message:
//
//   in_window(ts) == (((ts >> shift) & mask) == slot)
//
// No handshake, no control message, no state to keep in step.
//
// The numbers come from the environment, because changing one changes what the run is
// measuring: the slot picks which stretch of the day, the shift sets how long a window is.
// Both ends have to be given the same values, and each writes the values it used into its
// own results, so a mismatch shows up afterwards instead of quietly becoming two different
// experiments.

// Every time and length here is 64 bit nanoseconds.
#include <cstdint>
// strchr and strlen, for counting how many decimal places a millisecond was written with.
#include <cstring>
// sscanf for "10:58:30.100", and fprintf for the error when a parameter is impossible.
#include <cstdio>
// getenv and strtoull to read the environment, exit to refuse bad parameters.
#include <cstdlib>

// Message::timestamp() and its body, for recognising the session events below.
#include "itch/reader.hpp"
// The header length and the event codes those events carry.
#include "itch/types.hpp"

namespace win {

// How long a window is, as a shift: 2^30 nanoseconds is about 1.0737 seconds.
//
// A power of two rather than a round second, and that is the point: it does not divide
// evenly into decimal minutes, so the sampled windows land at every position inside a
// minute rather than always the same one. See the note on the slot below.
inline constexpr std::uint64_t kDefaultShift = 30;  // 2^30 ns = 1.073741824 s
// How many of those units make a period, so 31 means one window in every thirty two.
//
// Two settings are used in practice:
//   31   with the original file: 681 windows in a day, about 30 minutes a run, and what
//        every formal measurement uses
//   127  with the thinned file: 171 windows, about 9 minutes, for development, where only
//        the direction of a change is read and not the value
// The thinned file has its window parameters baked into it, so the number and the data
// file are a matched pair.
inline constexpr std::uint64_t kDefaultMask = 31;   // 32 units to a period
// Which unit of a period is measured.
//
// Any of the thirty two gives the same answer: the period and a decimal minute are very
// nearly coprime, so 681 windows spread evenly over the positions within a minute whichever
// one is chosen. Zero is used because it is the only choice nobody can suspect of having
// been picked because it looked good.
inline constexpr std::uint64_t kDefaultSlot = 0;
// How long before a window the replay runs at the day's real pace, in milliseconds.
//
// It is not optional. Outside a window packets go out flat out, and at the end of that the
// cache is packed with whatever just went through. Entering a window from there would
// measure too fast, because nothing would have to be fetched from memory.
inline constexpr std::uint64_t kDefaultSettleMs = 500;
// The same at the other end, for the reason under tail_ns below.
inline constexpr std::uint64_t kDefaultTailMs = 10;

// The window parameters of a run. Each end builds its own from the environment, and the
// two have to match.
struct Params {
    // How long one window is, as a power of two nanoseconds.
    std::uint64_t shift = kDefaultShift;
    // How many units to a period. It has to be all ones - 1, 3, 7, 15, 31 - which is
    // checked below.
    std::uint64_t mask = kDefaultMask;
    std::uint64_t slot = kDefaultSlot;
    // The warm up runs at the real pace like the window itself, and records nothing. Its
    // only job is to bring the receiver back from the state the full speed stretch left it
    // in, to the state a real market would leave it in - so it is as much a parameter both
    // ends must share as the three above.
    std::uint64_t settle_ns = kDefaultSettleMs * 1000000;
    // The same thing at the far end. The last messages of a window are still being worked
    // on when it closes, and switching straight back to full speed would queue that work
    // behind a pile of packets the window never contained, stretching the latency of the
    // last few samples by the fixture rather than by the code.
    std::uint64_t tail_ns = kDefaultTailMs * 1000000;

    // Nanoseconds in one unit.
    [[nodiscard]] std::uint64_t unit_ns() const noexcept {
        return std::uint64_t{1} << shift;
    }
    // Nanoseconds in a whole period, which is also how far apart two windows are - and the
    // warm up plus the tail have to fit in that gap, which is what the check below watches.
    [[nodiscard]] std::uint64_t period_ns() const noexcept {
        return unit_ns() * (mask + 1);
    }
    // A stretch given directly, in nanoseconds since midnight, used when from is below to.
    //
    // Why it exists: the periodic boundaries are powers of two - a unit is 1.1 minutes, 2.3
    // minutes, 4.6 - and no combination of them makes "10:00 to 10:03". When the question
    // is how the system behaves over one particular stretch of real trading, naming the two
    // times is far clearer than choosing units to approximate them.
    std::uint64_t from_ns = 0, to_ns = 0;

    // Whether a moment is inside the window. This one test is the whole file, and it being
    // a pure function is exactly why the two ends need no handshake.
    [[nodiscard]] bool in_window(std::uint64_t ts_ns) const noexcept {
        // Given a stretch, two comparisons.
        if (from_ns < to_ns) return ts_ns >= from_ns && ts_ns < to_ns;
        // Otherwise: shift to find which unit this moment is in, and see where that unit
        // sits in the period. Two bit operations and a comparison, no division.
        return ((ts_ns >> shift) & mask) == slot;
    }
};

// Builds the parameters from the environment, leaving anything unset at its default. The
// names are ITCH_WINDOW_SHIFT / _MASK / _SLOT / _SETTLE_MS / _TAIL_MS.
[[nodiscard]] inline Params params_from_env() {
    // Reads one integer, used five times below.
    const auto get = [](const char* name, std::uint64_t fallback) {
        const char* v = std::getenv(name);
        return v == nullptr ? fallback : std::strtoull(v, nullptr, 10);
    };
    // ITCH_WINDOW_FROM and _TO are written as clock times - "9:30" or "10:00:30" - which is
    // easier to get right than nanoseconds since midnight. Both have to be given, with from
    // below to, for them to take effect.
    const auto clock = [](const char* name) -> std::uint64_t {
        const char* v = std::getenv(name);
        // Not set, so zero, and from < to fails outside: the periodic path is used.
        if (v == nullptr) return 0;
        // Hours and minutes, optionally seconds, optionally milliseconds after a dot.
        // Missing seconds or milliseconds are zero.
        unsigned h = 0, m = 0, sec = 0, ms = 0;
        // How many fields were filled says how far the caller wrote.
        const int got = std::sscanf(v, "%u:%u:%u.%u", &h, &m, &sec, &ms);
        // Not even hours and minutes, so treat it as unset. Deliberately not an error:
        // leaving these two unset is the normal case.
        if (got < 2) return 0;
        // One or two digits after the dot have to be scaled: ".1" is a hundred
        // milliseconds and ".05" is fifty, while sscanf reads them as 1 and 5.
        if (got >= 4) {
            const char* dot = std::strchr(v, '.');
            const std::size_t digits = dot != nullptr ? std::strlen(dot + 1) : 0;
            if (digits == 1) ms *= 100;
            // Three digits are already milliseconds and need nothing.
            else if (digits == 2) ms *= 10;
        }
        // Nanoseconds since midnight, the same reference the ITCH timestamps use.
        return ((static_cast<std::uint64_t>(h) * 3600 + m * 60 + sec) * 1000ull + ms) *
               1000000ull;
    };
    // In declaration order. The warm up and the tail are milliseconds in the environment
    // and nanoseconds in the struct.
    Params p{get("ITCH_WINDOW_SHIFT", kDefaultShift),
             get("ITCH_WINDOW_MASK", kDefaultMask),
             get("ITCH_WINDOW_SLOT", kDefaultSlot),
             get("ITCH_WINDOW_SETTLE_MS", kDefaultSettleMs) * 1000000,
             get("ITCH_WINDOW_TAIL_MS", kDefaultTailMs) * 1000000,
             clock("ITCH_WINDOW_FROM"), clock("ITCH_WINDOW_TO")};
    // With a stretch given, the periodic parameters are not used at all, and there is only
    // one window, so nothing has to fit between two of them.
    if (p.from_ns < p.to_ns) return p;
    // A mask of zero means one unit to a period and that unit is measured - the whole
    // session replayed at its own pace, with no sampling. There is no gap between windows
    // then, so the warm up and the tail have nowhere to go and must both be zero.
    const bool spacing_ok =
        p.mask == 0 ? p.settle_ns == 0 && p.tail_ns == 0
                    : p.settle_ns + p.tail_ns < p.period_ns() - p.unit_ns();
    // Five checks at once. A window cannot be zero long or long enough to shift a 64 bit
    // value away. The mask plus one has to be a power of two, which is to say the mask is
    // all ones: otherwise the period would not be a power of two, the sampled points would
    // line up with minute boundaries, every window would fall at the same position inside a
    // minute, and it would no longer be sampling.
    if (p.shift == 0 || p.shift > 62 || ((p.mask + 1) & p.mask) != 0 ||
        p.slot > p.mask || !spacing_ok) {
        // All five values are printed. "Bad parameters" would not help: this is usually one
        // mistyped environment variable, and finding it means seeing what was actually
        // loaded.
        std::fprintf(stderr,
                     "bad window parameters: shift=%llu mask=%llu slot=%llu "
                     "settle_ns=%llu tail_ns=%llu\n",
                     static_cast<unsigned long long>(p.shift),
                     static_cast<unsigned long long>(p.mask),
                     static_cast<unsigned long long>(p.slot),
                     static_cast<unsigned long long>(p.settle_ns),
                     static_cast<unsigned long long>(p.tail_ns));
        // Exit rather than fall back to the defaults. Carrying on with the wrong parameters
        // would quietly produce a set of numbers that mean nothing, which is far harder to
        // find than a program that stops.
        std::exit(2);
    }
    return p;
}

// The warm up and the tail both run at the real pace and neither records a sample, so why
// are they separate states? Because one of the outputs is a curve of latency against how
// long before the window a message arrived, and that can only use messages from before it.
// Mixing in the ones after would spoil the curve.
//
// The four:
//   kGap     outside a window, sent flat out, nothing recorded
//   kSettle  the warm up before a window, at the real pace, nothing recorded
//   kWindow  the one stretch that is recorded
//   kTail    after the window, at the real pace, nothing recorded
enum class Phase { kGap, kSettle, kWindow, kTail };

// The same question as in_window answers, without a single division: it keeps the two
// boundaries of the current window and slides them forward as time passes. The price is
// that the moments have to arrive in order.
class Tracker {
public:
    // Given a stretch, the day has one window whose boundaries are those two times. The
    // period is set to something unreachable, so the loop below that slides the window
    // never fires - a window that does not move is what "just this stretch" means.
    explicit Tracker(const Params& p) noexcept
        : settle_(p.settle_ns),
          tail_(p.tail_ns),
          period_(p.from_ns < p.to_ns ? ~std::uint64_t{0} / 4 : p.period_ns()),
          open_(p.from_ns < p.to_ns ? p.from_ns : p.slot * p.unit_ns()),
          close_(p.from_ns < p.to_ns ? p.to_ns : open_ + p.unit_ns()) {}

    // When the session opens is learned from the feed rather than written down, and it
    // starts out as never. So forgetting to feed the system events in means a run records
    // nothing at all, which is obvious. The other way round - defaulting to always open -
    // would quietly mix the fifteen hours outside market hours into the distribution.
    void open_session(std::uint64_t ts_ns) noexcept { session_open_ = ts_ns; }
    void close_session(std::uint64_t ts_ns) noexcept { session_close_ = ts_ns; }

    // Given a moment, which of the four stretches it belongs to.
    [[nodiscard]] Phase advance(std::uint64_t ts_ns) noexcept {
        // Slide the boundaries forward until the moment is inside this window and its tail.
        // A while and not an if: one jump in the full speed stretch can cross several
        // periods at once.
        while (ts_ns >= close_ + tail_) {
            open_ += period_;
            close_ += period_;
            // Which window of the day this is; the results are grouped by it.
            ++index_;
        }
        // The window is compared against the session, not this message, so a warm up that
        // straddles the opening bell still runs at the real pace for all of itself. Testing
        // the message instead would cut such a warm up in half and waste it.
        if (open_ >= session_close_ || close_ <= session_open_) return Phase::kGap;
        // Past the right edge but still inside the tail.
        if (ts_ns >= close_) return Phase::kTail;
        // Inside the window - the only stretch whose samples are kept.
        if (ts_ns >= open_) return Phase::kWindow;
        // Before the window but close enough to be the warm up.
        if (ts_ns + settle_ >= open_) return Phase::kSettle;
        // Far from any window, so it goes out at the fixed fast pace.
        return Phase::kGap;
    }

    // Only kGap runs at the fixed pace; the other three follow the day's real spacing.
    [[nodiscard]] static bool one_to_one(Phase p) noexcept {
        return p != Phase::kGap;
    }

    // Which window of the day. The results are grouped by it, and it is the N in the
    // "windows N" line a run prints.
    [[nodiscard]] std::uint64_t index() const noexcept { return index_; }

    // When the current window opens. Subtracted from the timestamp of a warm up message it
    // gives how long before the recording that message arrived, which is the horizontal
    // axis of the warm up curve.
    [[nodiscard]] std::uint64_t open() const noexcept { return open_; }

private:
    std::uint64_t settle_;
    std::uint64_t tail_;
    // How far apart two window starts are. With a stretch given it is set unreachably
    // large, so the window never slides.
    std::uint64_t period_;
    std::uint64_t open_;
    std::uint64_t close_;
    std::uint64_t index_ = 0;
    // All ones, which is to say the session opens infinitely far away: until a system event
    // arrives everything is kGap and nothing is recorded. That is deliberate, as explained
    // at open_session above.
    std::uint64_t session_open_ = ~std::uint64_t{0};
    std::uint64_t session_close_ = ~std::uint64_t{0};
};

// The opening and closing times are recognised from the feed rather than looked up in a
// calendar, so a half day or a holiday needs no change at all. Both ends have to recognise
// them the same way, which is why this is written once and included by both.
inline void note_session(const itch::Message& m, Tracker* t) {
    // Only a system event carries them, and it has to have a body: with a header and
    // nothing else, the read below would reach into whatever follows.
    if (m.type() != 'S' || m.len <= itch::kHeaderLen) return;
    // The event code is the first byte after the common header.
    const char code = static_cast<char>(m.body[itch::kHeaderLen]);
    // The message's own timestamp, not the moment it arrived: what matters is the time of
    // day in the market, not how far along the replay is.
    if (code == itch::kEventStartOfMarketHours) {
        t->open_session(m.timestamp());
    } else if (code == itch::kEventEndOfMarketHours) {
        t->close_session(m.timestamp());
    }
}

}  // namespace win
