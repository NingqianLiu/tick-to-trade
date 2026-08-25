// Walks a buffer of records, each one two big endian length bytes followed by an ITCH
// body, and hands every complete record to a callback.
//
// It checks boundaries and rejects a zero length; it does not check the length against
// the type. A caller that wants that stricter check looks the type up in kBodyLen once
// it has the message. The same walk serves both jobs in this project: reading a 39 GB
// file in order, and splitting the several messages inside one MoldUDP64 packet.
//
// A Message is only a view into the buffer, so the callback has to be finished with it
// before that buffer is overwritten or moved.
#pragma once

// std::size_t for buffer sizes, the position inside one, and the counts returned.
#include <cstddef>
// std::uint8_t for the raw bytes and std::uint16_t for the length prefix.
#include <cstdint>
#include <utility>

// Message, which is what the callback receives, and read_be, which reads the length
// prefix from an address that need not be aligned.
#include "itch/reader.hpp"
// kLenPrefix. reader.hpp pulls this in as well, but the dependency is stated here.
#include "itch/types.hpp"

namespace itch {

// Why the walk stopped. The caller has to tell these apart: three of them need
// different handling and only one of them means something is wrong.
enum class FrameStop : std::uint8_t {
    // The position landed exactly on the end of the buffer, so every byte formed a
    // complete record. The next block can be read straight in, with nothing carried over.
    kEndOfBuffer,
    // The buffer ends inside a record: part of a length prefix, or a body that is not all
    // there. consumed points at the start of that unfinished record, so the caller moves
    // those bytes to the front of the next read - the prefix included, not just the part
    // of the body that arrived.
    kPartialTail,
    // A length prefix of zero. No ITCH record can be zero bytes long, so the stream is
    // being read from the wrong place. The position does not step over those two bytes,
    // because that would hide where the misalignment began.
    kZeroLength,
    // The callback returned false. That message was handled in full and is counted as
    // consumed, so a caller that resumes starts after it - and a caller that meant to
    // stop for good has to stop, rather than feeding the rest of the buffer back in.
    kCallerStopped
};

// What one walk produced. The three come back together so that a special value of a size
// does not have to stand in for an error.
struct FrameResult {
    // Bytes handled in full, counted from the start of the buffer. Anything unfinished
    // begins at data + consumed. A record that stopped the walk, whether by returning
    // false or by being zero length, is treated differently: the caller's message is
    // included, the zero length prefix is not.
    std::size_t consumed;
    // Complete messages given to the callback. It is kept separate from consumed because
    // ITCH bodies vary in length, so one cannot be worked out from the other.
    std::size_t messages;
    // One of the four reasons above, so the caller does not have to infer what happened
    // by comparing consumed against the size it passed in.
    FrameStop stop;
};

// Fn is the caller's callback. It takes a const Message& and returns something that
// converts to bool: true to carry on, false to stop after this message. The template has
// to live in the header so the compiler can see the callback at the call site and inline
// it into the walk.
template <typename Fn>
// Reads records out of [data, data + size). Each turn of the loop checks that a length
// prefix is there, reads it, checks that the whole body is there, and only then builds a
// Message. No branch ever reads past data + size, and a record that is not all there is
// left untouched for the caller to join with the next block.
//
// A size of zero is fine: the first check returns kEndOfBuffer without dereferencing
// anything. A non zero length whose type is unknown still reaches the callback, because
// whether a length is legal for a type is the callback's business, not the walk's.
// Nothing here throws, and neither does anything on the hot path that uses it.
[[nodiscard]] inline FrameResult for_each_message(const std::uint8_t* data,
                                                  std::size_t size, Fn&& fn) {
    // Where the next length prefix starts, which is also how many bytes are done. It only
    // moves after a whole message has gone to the callback, so it always sits on a record
    // boundary.
    std::size_t off = 0;
    // Messages handed over so far. A body that is not all there never counts.
    std::size_t count = 0;
    // Every exit returns from inside the loop, so each one can name its own reason
    // instead of leaving the state to be worked out afterwards. Every ordinary record
    // moves off along by at least three bytes, and the one length that would not move it
    // at all, zero, is caught below, so the loop cannot spin in place.
    for (;;) {
        // Two bytes have to be there before a length can be read. Note the >: with
        // exactly two bytes left the prefix is complete and the walk goes on.
        if (off + kLenPrefix > size) {
            // Nothing incomplete is counted as consumed. Landing exactly on the end is
            // the ordinary finish; anything else is a tail for the caller to keep.
            return {off, count,
                    off == size ? FrameStop::kEndOfBuffer : FrameStop::kPartialTail};
        }
        // The body length, big endian, not counting the two bytes it is written in.
        const std::uint16_t len = read_be<std::uint16_t>(data + off);
        // Zero is not a legal length and gives nothing to advance by. Returning here
        // without touching off or count means a log or a test can point at the exact
        // prefix that went wrong, and at how many messages were sound before it.
        if (len == 0) return {off, count, FrameStop::kZeroLength};
        // A whole record is the prefix plus the body. Equality means the last body byte
        // is the last byte of the buffer, which is complete, so only > is a partial tail.
        // The callback is not called here: any field of the message might be missing.
        if (off + kLenPrefix + len > size) {
            return {off, count, FrameStop::kPartialTail};
        }
        // The body starts after the prefix, which is what makes Message::type() see the
        // first byte of the body rather than the high byte of the length. The length is
        // passed along so the callback can compare it with kBodyLen[type] if it wants to.
        const bool go_on = fn(Message{data + off + kLenPrefix, len});
        // The record is done either way, so it is stepped over and counted even when the
        // callback said stop; a caller that resumes will not see it twice.
        off += kLenPrefix + len;
        ++count;
        if (!go_on) return {off, count, FrameStop::kCallerStopped};
    }
}

}
