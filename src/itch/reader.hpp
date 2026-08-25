// This file reads the big-endian integers on the ITCH wire and offers a view of a message's
// common fields.
// framing.hpp confirms first that the length does not run past the buffer, and then builds a
// Message from the body's address and its length.
// The order book, the window logic and the various tools then read fields through Message or
// read_be, without copying a whole message.
// Ordinary fields use read_be<T>, and the six byte timestamp peculiar to this protocol is
// handled separately by read_be48.
// These methods do not judge what length a message type ought to have; a strict length check is
// done by whoever received the Message.

// The templates and the small methods are all defined in this header; expanding it once avoids a
// duplicate definition inside one translation unit.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

// itch/types.hpp provides the offsets of the common header's type, security number and
// timestamp, and the header's length.
// All four reading methods of Message place their fields by these constants, rather than
// repeating numbers all over the place.
#include "itch/types.hpp"

// These helpers only interpret ITCH wire data, so they go in the itch namespace.
// Writing itch::read_be and itch::Message at a call site shows that the pointer at hand points
// at protocol data.
namespace itch {

// The reading methods below work by "copy on a little-endian host first, then byteswap".
// On a machine that is not little-endian, that order gives the wrong result.
// The static_assert turns such a target down at compile time rather than letting the program run
// and quietly read order ids, prices or timestamps wrongly.
// The string on the second line appears in the compiler's error and says plainly which
// assumption failed.
// When the condition is true it produces no instructions at all; when it is false the whole
// build fails.
// That is simpler than testing the byte order on every read, and adds no branch to the hot path.
static_assert(std::endian::native == std::endian::little,
              // This text is only for diagnosis when the build fails and never reaches the
              // running code.
              // It points at the helpers' assumption about the host rather than at a damaged
              // input file.
              "the byte-swapping helpers assume a little-endian host");

// T is the width of integer the caller wants to read, such as uint16_t, uint32_t or uint64_t.
// A template lets one piece of unaligned reading cover every ordinary ITCH integer field.
// This project only calls it with unsigned integers of fixed width, so sizeof(T) matches the
// protocol field's width exactly.
template <typename T>
// This is the body of reading a big-endian field of 2, 4 or 8 bytes.
// p points at the field's first byte on the wire, which does not necessarily meet T's alignment.
// The function copies exactly sizeof(T) bytes, reverses the byte order, and returns an integer
// the host can work with directly.
// It only reads the buffer it was given, keeps no copy of p, and changes no byte on the wire.
// inline lets this very short hot path method be expanded at the call site, and nodiscard keeps
// the result from being dropped by accident.
// noexcept says that whatever the field's value, it never leaves through an exception.
// The caller has to have guaranteed that at least sizeof(T) valid bytes follow p; this method
// does not check the buffer's bounds again.
[[nodiscard]] inline T read_be(const std::uint8_t* p) noexcept {
    // v is the temporary integer of this read.
    // p is deliberately not cast to a T*, because dereferencing something unaligned breaks the
    // rules of C++.
    // The next line covers all sizeof(T) bytes, so v does not have to be cleared when it is
    // declared.
    // v exists only for this call, and what the caller receives on return is a copy of the
    // value.
    // Leaving it uninitialised is safe here, because memcpy writes every one of its bytes.
    // Should somebody pass a type that is not a plain integer later, that assumption has to be
    // looked at again.
    T v;
    // Copy exactly one T's worth of bytes from p.
    // memcpy accepts an unaligned address, and a fixed small size is usually turned by the
    // compiler into one ordinary load.
    // After the copy the bytes in memory are still those of the wire, but a little-endian
    // processor still reads their value the wrong way round.
    // This line only does the safe loading, and the next puts it into the host's byte order.
    // The source p is not changed and the only destination is v on the stack.
    // For a fixed length of 2, 4 or 8 bytes, the optimised code does not really call the general
    // memcpy function.
    // Using memcpy still makes the unaligned access and the aliasing rules plainly legal rather
    // than resting on a compiler's extension.
    std::memcpy(&v, p, sizeof(T));
    // Reverse all the bytes of v, turning ITCH's big-endian representation into the integer this
    // little-endian host uses.
    // What is returned is a value of its own; the original buffer is unchanged and other modules
    // can still read the same message.
    // uint16_t, uint32_t and uint64_t each use the byte swap of their own width.
    // Order ids, share counts, prices and MoldUDP64 sequence numbers all become host integers
    // through this return.
    return std::byteswap(v);
}

// read_be48 reads the 6 byte timestamp of the ITCH common header and nothing else.
// C++ has no uint48_t, so six bytes are caught in a uint64_t that was cleared first.
// After the byteswap, the two zeros that were added land in the low 16 bits; shifting right by
// 16 gives the right 48 bit value.
// Copying only six bytes also keeps a timestamp at the end of a buffer from being read two bytes
// too far.
// What comes back is nanoseconds from midnight of that day, used by the sender's pacing and by
// window::Tracker.
// This timestamp comes from the exchange's message and is not the hardware timestamp the card
// wrote when the packet arrived.
// Six readable bytes really have to follow p; this function only converts a field and does not
// check the range for the caller.
[[nodiscard]] inline std::uint64_t read_be48(const std::uint8_t* p) noexcept {
    // The 64 bit container is cleared first, so that the two bytes not copied from the wire are
    // certainly zero.
    // Unlike read_be<T>, only six bytes are covered here, so the other two have to be
    // initialised first.
    // Where those zeros sit moves after the byteswap, and the next line's shift depends on that
    // starting value being right.
    std::uint64_t v = 0;
    // An ITCH timestamp is always six bytes, so this says 6 and must not use sizeof(v)'s eight.
    // p may be unaligned and memcpy is still legal; this line only reads the buffer and changes
    // no message.
    // The six wire bytes land in the low 48 bits of v in little-endian memory, and the other two
    // stay zero.
    // Reading 6 rather than 8 avoids reaching into the next field or the next message when a
    // timestamp sits exactly at the end of what may be read.
    // After the copy v is still not the final value; both a byte swap and a shift into place are
    // still to come.
    std::memcpy(&v, p, 6);
    // The byteswap moves the two added zeros to the lowest 16 bits, and the shift right throws
    // them away.
    // For instance 12 34 56 78 9a bc on the wire comes out as 0x123456789abc.
    // A uint64_t leaves plenty of room for adding and subtracting nanoseconds and comparing
    // window boundaries afterwards.
    // The shift is a logical one, because v is unsigned and no sign bit is filled in at the top.
    // On return this temporary v ends its life; the function caches no timestamp.
    return std::byteswap(v) >> 16;
}

// Message is a view of one ITCH message that has already passed framing's boundary check.
// It does not own the memory body points at; once a packet or a file's read buffer goes away, a
// Message cannot be used any longer.
// The common fields are read by the four methods below, while the fields peculiar to an order
// are read by the order book at the offsets in types.hpp.
// This struct is one pointer and one length, and building it neither allocates memory nor copies
// the body.
// Callers usually only use it during for_each_message's callback and should not keep it for
// long.
struct Message {
    // body points at the first byte of the message's body, which is where the type is, and not
    // at the two byte length in front of it.
    // It usually points straight into a MoldUDP64 payload or into SeqReader's buffer, so no
    // whole message is copied.
    // const guarantees that these reading helpers do not change the market data received; the
    // caller has to guarantee that at least len bytes are still valid.
    // framing.hpp confirms before building a Message that len bytes of body do not run past the
    // current buffer.
    // It does not prove len is enough for the common header; a strict tool also compares len
    // against kBodyLen[type].
    // body points at the body rather than at the length prefix, so every offset from types.hpp
    // is counted from here.
    // Message itself never releases this pointer; the memory really belongs to a packet ring or
    // a file buffer.
    const std::uint8_t* body;
    // len is the length of the message's body, not counting the two byte length prefix.
    // It is 16 bits like the length field on the wire, and lets a caller check what length a
    // message type ought to have.
    // The small methods below trust that framing and the type check are done, and do not look at
    // the bounds again on every field.
    // len can be compared with kBodyLen[type()] to spot a stream being interpreted from the
    // wrong place.
    // This field only describes what is valid and says nothing about who owns body.
    std::uint16_t len;

    // type() reads the single byte message type of the common header.
    // The order book uses the character it returns to tell apart the paths for A, F, E, C, X, D
    // and U.
    // This field is one byte and needs no conversion of byte order.
    // The method is const, because reading the type changes neither the Message nor the
    // underlying buffer.
    // noexcept says any byte value comes back as a char and an unknown type is not reported by
    // an exception.
    [[nodiscard]] char type() const noexcept {
        // kTypeOff comes from types.hpp; the uint8_t it is stored as becomes a char that can be
        // compared with a character constant.
        // A strict checking tool uses kBodyLen afterwards to judge whether that character
        // belongs to ITCH 5.0.
        return static_cast<char>(body[kTypeOff]);
    }
    // stock_locate() reads the two byte security number of the common header.
    // The trader uses it to choose a shard and the order book to choose that security's price
    // levels.
    // The field is big-endian and its address may be unaligned, so it goes through
    // read_be<uint16_t>.
    // A security number is an integer within that day's file and is not turned into an eight
    // byte ticker here.
    // The method reads from body every time and keeps no copy of its own inside Message.
    [[nodiscard]] std::uint16_t stock_locate() const noexcept {
        // The pointer moves to the first wire byte of the security number and an integer that
        // can be used directly as an index comes back.
        // read_be copies two bytes and swaps them, and does not require body+kLocateOff to be
        // aligned to two bytes.
        return read_be<std::uint16_t>(body + kLocateOff);
    }
    // timestamp() reads the six byte exchange timestamp of the common header.
    // It is nanoseconds from midnight of that day, not the hardware timestamp the card stamped
    // when a packet arrived.
    // The sender uses it to restore the spacing between messages, and window::Tracker to tell
    // the full speed stretch, the warm up, a window and the tail apart.
    // A six byte field has to go through read_be48; reading eight would run past the field's
    // end.
    // The uint64_t that comes back is still nanoseconds, with no conversion of time zone and no
    // subtraction against this machine's clock.
    // The method only decodes a field; whether it is in session is decided by the session and
    // window logic afterwards.
    [[nodiscard]] std::uint64_t timestamp() const noexcept {
        // Move to the start of the timestamp, copy six bytes, and return nanoseconds as a
        // uint64_t.
        // The packet underneath does not change in any way from this read.
        return read_be48(body + kTimestampOff);
    }

    // event_code() should only be called on a message whose type() has been confirmed to be a
    // System Event.
    // The event character follows the 11 byte common header, so its offset is kHeaderLen.
    // window::note_session uses Q and M to open and close the in-session window logic, and other
    // tools recognise O, S, E and C.
    // This method only returns a character; it changes no session state itself and does not
    // check the message type again.
    // Called on a message that is not an S, what it reads is that message's ordinary field at
    // offset 11, so the type check has to be done outside.
    // An event code is one byte and needs neither read_be nor a byte swap.
    [[nodiscard]] char event_code() const noexcept {
        // Read the first byte after the common header and turn it into a char that can be
        // compared with the event characters.
        // kHeaderLen is defined in one place in types.hpp, so a change to the common header
        // carries through here as well.
        return static_cast<char>(body[kHeaderLen]);
    }
// The end of Message; it has nothing to destroy, because the buffer body points at never belongs
// to it.
};

}
