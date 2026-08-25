// Reads the big endian integers of the ITCH wire format, and gives a view of the fields
// every message shares.
//
// framing.hpp checks that a body is completely inside the buffer and then builds a
// Message out of its address and length. The book, the window logic and the offline
// tools read fields through that Message, or through read_be directly; a message is
// never copied. Ordinary fields go through read_be<T>; the six byte timestamp, which has
// no integer type to match it, has read_be48 of its own.
//
// Nothing here decides whether a length is right for a type. That check belongs to
// whoever holds the Message.
#pragma once

// std::endian to check the host, std::byteswap to turn a big endian field around.
#include <bit>
// Fixed width integers: the width of every wire field is set by the protocol, so it
// cannot be left to whatever int happens to be.
#include <cstdint>
// memcpy, which is how a field that is not aligned to its own width is read safely.
#include <cstring>

// The offsets and the header length the four accessors below use.
#include "itch/types.hpp"

namespace itch {

// The readers copy first and swap afterwards, which is only correct on a little endian
// host. Failing at compile time is better than reading order ids, prices and timestamps
// wrongly at run time, and it costs the hot path nothing - the alternative would be a
// branch on every field.
static_assert(std::endian::native == std::endian::little,
              "the byte-swapping helpers assume a little-endian host");

// T is the width being read: uint16_t, uint32_t or uint64_t.
template <typename T>
// Reads a 2, 4 or 8 byte big endian field. p points at its first byte on the wire and
// need not be aligned for T, which is why this copies rather than casting the pointer:
// dereferencing an unaligned T* is undefined, while a fixed size memcpy is both legal
// and, at these sizes, compiled down to one ordinary load.
//
// The caller has to know that sizeof(T) bytes are there; this does not check the buffer
// again. It reads the wire and changes nothing.
[[nodiscard]] inline T read_be(const std::uint8_t* p) noexcept {
    // No initialiser is needed: the copy below writes every byte of it.
    T v;
    std::memcpy(&v, p, sizeof(T));
    // The bytes now sit exactly as they were on the wire, which a little endian machine
    // reads backwards; the swap turns them into the value the protocol meant.
    return std::byteswap(v);
}

// The six byte timestamp in the common header. C++ has no 48 bit integer, so six bytes
// are copied into a 64 bit one that starts out zero. After the swap those two zero bytes
// end up at the bottom, and the shift drops them: 12 34 56 78 9a bc comes out as
// 0x123456789abc.
//
// Copying six bytes rather than eight also matters at the end of a buffer, where reading
// two more would step into the next field or past the last message.
//
// The value is nanoseconds since midnight, taken from the exchange's own message. It is
// not the hardware timestamp the card writes when a packet arrives.
[[nodiscard]] inline std::uint64_t read_be48(const std::uint8_t* p) noexcept {
    // The two bytes the copy will not touch have to be zero before the shift can rely on
    // where they land.
    std::uint64_t v = 0;
    std::memcpy(&v, p, 6);
    return std::byteswap(v) >> 16;
}

// A view of one ITCH message whose boundaries framing.hpp has already checked. It owns
// nothing: once the packet or the read buffer behind it is gone or overwritten, the view
// is finished too, so it normally lives no longer than the callback it was handed to.
// Building one costs a pointer and a length, with no copy of the body.
struct Message {
    // The first byte of the body, which is the type. The two length bytes in front of it
    // are not included, which is what makes every offset in types.hpp count from here.
    // framing.hpp has checked that len bytes of body are present; it has not checked that
    // len is long enough for the type, which is left to the strict tools.
    const std::uint8_t* body;
    // The length of the body, without the prefix. A caller that wants to be sure the
    // stream is being read from the right place compares it with kBodyLen[type()].
    std::uint16_t len;

    // The one byte message type: A, F, E, C, X, D, U and the rest. One byte, so nothing
    // to swap.
    [[nodiscard]] char type() const noexcept {
        return static_cast<char>(body[kTypeOff]);
    }
    // The stock locate number: which security this message is about, as an integer that
    // indexes the per security arrays directly. It is never turned into a ticker on the
    // hot path.
    [[nodiscard]] std::uint16_t stock_locate() const noexcept {
        return read_be<std::uint16_t>(body + kLocateOff);
    }
    // The exchange timestamp, in nanoseconds since midnight. The replay uses it to
    // recover the spacing between messages and the window logic uses it to tell the full
    // speed stretch, the warm up, the window and the tail apart.
    [[nodiscard]] std::uint64_t timestamp() const noexcept {
        return read_be48(body + kTimestampOff);
    }

    // The event code of a System Event message, which sits right after the common header.
    // Only call it once type() has said S: on any other message this is simply whatever
    // that message keeps at offset 11.
    [[nodiscard]] char event_code() const noexcept {
        return static_cast<char>(body[kHeaderLen]);
    }
};

}
