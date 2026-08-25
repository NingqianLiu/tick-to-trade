#pragma once

// Where a packet ends and the next begins - that rule lives here.
//
// Why it has a file of its own rather than sitting inside the sender: two places need it.
// The sender packs as it sends; and an offline checking tool applies the same rule again to the
// bytes that were sent, to verify them.
// The two have to agree exactly. Written twice they would drift apart sooner or later. So it can
// only live somewhere shared.
//
// A packet closes on whichever of these four happens first:
//   1 it has been open long enough, having reached the merging window
//   2 one more message would go past what a frame can hold
//   3 the count field is about to overflow
//   4 the stretch of the pacing has changed
// The fourth is the easiest to miss, and missing it puts the whole replay's timeline out: a
// packet carries one send time, so it can only belong to one stretch of the timeline; straddling
// two, which stretch that time belongs to cannot be said.

#include <cstddef>
#include <cstdint>

// cfg::kMaxPacketPayload is how many bytes of content a frame can hold.
// cfg::kCoalesceNs is how many nanoseconds the gathering window is.
// When the first and second above fire depends entirely on those two.
#include "common/settings.hpp"
// win::Phase says which stretch of the timeline this is.
// win::Tracker::one_to_one says whether that stretch is replayed 1:1 at the real time.
// The fourth rule and the window both need it.
#include "common/window.hpp"

namespace pkt {

// At most how many messages a packet holds. This limit fires practically never - a frame is a
// thousand odd bytes and fills at forty odd messages, nowhere near sixty five thousand. It is
// here so that the field really cannot overflow.
// The limit comes from the two byte field in the MoldUDP64 header.
// It is what the protocol says, not something to adjust.
inline constexpr std::uint16_t kMaxMessages = 0xffff;  // the MoldUDP64 count field

// This class holds no bytes at all. The bytes are in a buffer elsewhere.
// It only keeps four numbers about the packet being gathered:
// when it opened, how many bytes it holds, how many messages, and which stretch it belongs to.
// And it answers one question: is it time to close.
class Packing {
public:
    // Not one message yet. Callers use it to ask whether there is half a packet in hand.
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    // How many messages so far. Closing writes it into the MoldUDP64 header.
    [[nodiscard]] std::uint16_t count() const noexcept { return count_; }
    // How many bytes so far. It counts the messages themselves and no header.
    [[nodiscard]] std::size_t payload() const noexcept { return payload_; }
    // Which stretch of the timeline this packet belongs to. Settled by the first message and
    // never changed after.
    [[nodiscard]] win::Phase phase() const noexcept { return phase_; }
    // When this packet opened, being the time of its first message in nanoseconds. The merging
    // window is counted from it.
    [[nodiscard]] std::uint64_t open_ns() const noexcept { return open_ns_; }

    // This is the body of the class: before putting the next message in, ask whether the packet
    // should be closed first.
    // It takes the time of this message, how many bytes it is, and which stretch it belongs to.
    // A true means the caller should close and send what it holds and open a new packet for this
    // message.
    [[nodiscard]] bool should_close(std::uint64_t ts_ns, std::size_t record,
                                    win::Phase p) const noexcept {
        // An empty packet has nothing to close. With this line the tests below may safely assume
        // there is something in it.
        if (count_ == 0) return false;
        // The two below are "closing any later would be wrong", which is different from the two
        // after them, which are "any later and it will not fit":
        // one is that this message belongs to another stretch, and a packet can belong to only
        // one (the fourth rule at the top of this file);
        // the other is that the count has reached the limit of sixteen bits and one more would
        // overflow.
        if (p != phase_ || count_ == kMaxMessages) return true;
        // One more message would go past what a frame can hold, so what is there now is closed
        // first.
        if (payload_ + record > cfg::kMaxPacketPayload) return true;
        // What is left is "it has been open long enough", and that only counts in the stretches
        // replayed 1:1.
        //
        // Why the stretches differ: the merging window exists so that packets arrive in the
        // replay at the same rhythm they did on the day, and that only means something at the
        // original speed. The full speed stretch is already ninety times faster than the day, and
        // closing by time there gives packets of one message each - with both paths sending, that
        // is about 170 nanoseconds more on the wire per message, which is slower than the
        // receiving side ought to be. What that measures is the capacity of this link, not the
        // capacity of the receiver.
        // So the full speed stretch closes only when a frame is full, which the test above
        // catches first.
        return win::Tracker::one_to_one(phase_) &&
               ts_ns - open_ns_ >= cfg::kCoalesceNs;
    }

    // Record a message in this packet. It updates these four numbers only; the caller moves the
    // bytes itself.
    void add(std::uint64_t ts_ns, std::size_t record, win::Phase p) noexcept {
        // This is the first message of the packet, so it settles when the packet started and
        // which stretch it belongs to.
        if (count_ == 0) {
            // The time of the first message rather than the wall clock now - what is being
            // measured is how long a stretch of the day this packet covers, which has nothing to
            // do with how fast the replay runs.
            open_ns_ = ts_ns;
            // The packet belongs to this stretch from now on; a later message from another
            // stretch triggers the closing test above.
            phase_ = p;
        }
        // The byte count grows, which the next should_close needs to ask whether the next message
        // fits.
        payload_ += record;
        // This also closes the count_ == 0 branch above: from the second message on, the
        // packet's start and stretch are never rewritten.
        ++count_;
    }

    // The packet has been closed and sent, so the state goes back to an empty packet.
    void close() noexcept {
        // The byte count goes to zero. Missing this, the next packet would think it already
        // holds as much as the last one did, and one message would be enough to make it look
        // full.
        payload_ = 0;
        // The message count goes to zero.
        // open_ns_ and phase_ are deliberately left alone.
        // When the next message arrives, the count_ == 0 branch in add writes them again.
        count_ = 0;
    }

private:
    // The time of this packet's first message, in nanoseconds. The start of the merging window.
    std::uint64_t open_ns_ = 0;
    // How many bytes so far, without any header.
    // should_close compares it with what a frame can hold.
    // So that limit has to be on the same basis, without headers, and the two have to agree.
    std::size_t payload_ = 0;
    // How many messages so far. It doubles as the test for whether the packet is empty.
    std::uint16_t count_ = 0;
    // Which stretch of the timeline this packet belongs to. It starts as the gap between two
    // windows.
    win::Phase phase_ = win::Phase::kGap;
};

}  // namespace pkt
