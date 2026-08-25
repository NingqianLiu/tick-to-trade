// This file cuts a run of records, each a 2 byte big-endian length followed by an ITCH body, out
// of a stretch of buffer.
// It only checks boundaries and zero lengths, and does not judge what length a type ought to
// have; a caller wanting a strict check looks at kBodyLen afterwards.
// Every complete record cut out becomes a Message handed to a callback, and how many bytes were
// completely handled and why it stopped go back to the layer above.
// The same logic reads a 39 GB file in order and cuts the several messages out of a MoldUDP64
// packet.
// A Message is only a view of the current buffer, so a callback has to be finished with it before
// that buffer is overwritten or moved.

// The template for_each_message is defined in this header; expanding it once avoids a duplicate
// definition of the other types inside one translation unit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

// reader.hpp provides Message and read_be, the first handing a body to a callback and the second
// reading the big-endian length prefix safely.
#include "itch/reader.hpp"
// types.hpp defines kLenPrefix plainly; reader.hpp includes it indirectly as well, but framing's
// own dependency is still stated here.
#include "itch/types.hpp"

// Both the cutting logic and the return type belong to parsing the ITCH wire, so they go in the
// itch namespace.
// Writing itch::FrameStop makes it plain that this is the outcome of an ITCH record boundary and
// not an error code of the file system.
namespace itch {

// FrameStop says why for_each_message stopped.
// A caller has to tell a normal finish, a tail that has to be kept, damaged input and a
// deliberate stop apart, to handle the next block of data correctly.
// enum class does not turn into an integer by itself, and the layer above has to compare against
// a particular reason plainly.
// A uint8_t holds four values easily and keeps this field of FrameResult at a fixed size.
enum class FrameStop : std::uint8_t {
    // The current position is exactly the end of the buffer, every byte visible made a complete
    // record, and this is a normal end.
    // The layer above can read the next block straight away, without moving a tail of the old
    // buffer across.
    kEndOfBuffer,
    // The end of the buffer holds only part of a length prefix or part of a body; the caller has
    // to move those bytes in front of the next read.
    // consumed points at the start of that unfinished record, and keeping only the part of the
    // body that arrived is not enough.
    kPartialTail,
    // A length prefix of zero cannot be a valid ITCH record and means the stream is being
    // interpreted from the wrong place.
    // The position returned does not step over those two zero bytes, or the layer above would
    // hide where the real misalignment is.
    kZeroLength,
    // The callback returned false having handled the current message; that message still counts
    // as consumed, and the layer above should decide whether to carry on from after this
    // position.
    // If the layer above means to stop reading altogether, it has to break on seeing this value
    // rather than feeding the rest back again and again.
    kCallerStopped
// The end of the four reasons for stopping.
};

// FrameResult hands the progress of the cutting back to the caller in one piece.
// SeqReader and the various tools use consumed to keep a tail, messages to count what this call
// produced, and stop to decide what to do next.
// The three fields come back together, so that a normal end and an error are not mixed into one
// special size_t value.
// It owns no data and keeps no last Message; it lives as long as any ordinary return value.
struct FrameResult {
    // consumed is how many bytes from data have been completely handled; an unfinished tail
    // starts at data+consumed.
    // On a deliberate stop it includes the complete record that made the callback return false.
    // Neither a zero length nor a partial tail counts the record at fault, so the layer above can
    // check or join from the same place.
    std::size_t consumed;
    // messages is how many complete messages were handed to the callback this time, not counting
    // a partial tail or a zero length record.
    // It is separate from consumed, because an ITCH body has no fixed length and a count of
    // messages cannot be worked out from a count of bytes.
    std::size_t messages;
    // stop holds one of the four reasons above, so a caller does not have to work out what
    // happened from consumed and size.
    // A normal return always has a plain value too and never leaves it uninitialised.
    FrameStop stop;
// The end of FrameResult's three returned fields.
};

// Fn is the type of callback the caller provides; it takes a const Message& or something
// compatible and returns something that turns into a bool.
// Fn&& accepts an ordinary function object and a temporary lambda alike; this function calls it
// during the walk and keeps no reference to it.
// The definition of a template has to stay in the header, so that the compiler sees the concrete
// Fn at the call site and can inline the callback.
// A callback may check a length, build a book, assemble a packet or count things on a message,
// and framing cares about none of that.
template <typename Fn>
// This is the body of cutting a stretch of buffer up.
// data points at the first byte that may be read and size is how many bytes may really be read
// this time.
// The loop confirms the two byte length prefix is complete, reads len, and confirms the whole
// body is complete.
// A complete record becomes a Message handed to fn; a true carries on and a false stops after
// that record.
// No branch reads a byte past data+size, and a partial tail is left as it is for the layer above
// to join next time.
// The consumed, messages and stop that come back are enough for reading a file, for MoldUDP64 and
// for the tests to use one way of handling it.
// size may be zero; the first check then returns kEndOfBuffer normally and data is never
// dereferenced.
// A non zero len with an unknown type still calls fn, because whether a protocol length is valid
// is the callback's responsibility.
// The message whose callback returned false has been handled completely, so the position returned
// is after it rather than at its start.
// The function catches no exception; the callbacks on this project's hot path do not use them
// either.
[[nodiscard]] inline FrameResult for_each_message(const std::uint8_t* data,
                                                  // size together with data limits what may be
                                                  // reached this time to the half-open range
                                                  // [data,data+size).
                                                  // fn is the callback for this walk; a named
                                                  // variable is called directly in the body.
                                                  // Fn&& keeps the caller's concrete type, but fn
                                                  // is not passed on to anything that keeps it
                                                  // for long.
                                                  std::size_t size, Fn&& fn) {
    // off is where the next length prefix is relative to data, which is also how many bytes have
    // been completely handled so far.
    // It only goes up once a complete message has been handed to the callback, so it always lands
    // on a record boundary.
    // On a partial tail return, data+off is exactly the first byte the layer above has to keep.
    std::size_t off = 0;
    // count records how many messages were successfully handed to fn; only a complete body raises
    // it.
    // Even where fn returns false, the current message has already been called, so count still
    // goes up by one first.
    std::size_t count = 0;
    // Every turn handles at most one message, and every condition for stopping returns a
    // FrameResult from inside the loop.
    // An endless loop lets every way out give an accurate reason for stopping at the same time,
    // without having to guess the state again after the loop.
    // Every normal message raises off by at least three bytes, two of prefix and at least one of
    // body, so it cannot spin in place.
    // The only thing that cannot move on, a len of 0, is recognised plainly below and returns
    // kZeroLength.
    // kEndOfBuffer, kPartialTail and kCallerStopped all return directly as well and rest on no
    // shared state after a break.
    for (;;) {
        // Before a length can be read, at least kLenPrefix=2 bytes have to be left from off.
        // A true may be exactly the end, or only part of a length prefix left; the two need
        // different reasons returned.
        // A false means the whole length prefix can be read and only then may the next line call
        // read_be<uint16_t> safely.
        // It uses > rather than >=: with exactly two bytes left the length prefix is complete and
        // len has to be read.
        // Both off and size are size_t; off only moves forward from zero and never points before
        // data.
        // Without a complete prefix for this record, len must not be guessed and this part must
        // not be counted into consumed.
        if (off + kLenPrefix > size) {
            // consumed stays at off, because an incomplete prefix was not handled.
            // off==size means no bytes are left and it is a normal kEndOfBuffer.
            // off<size means a tail of a byte or so is left and kPartialTail tells the layer
            // above to keep it.
            // count holds only the complete messages already handed to the callback.
            // Aggregate initialisation fills consumed, messages and stop in that order, matching
            // FrameResult's fields.
            // This return ends the whole walk and fn never sees an incomplete record.
            return {off, count,
                    // A true in the conditional chooses a normal end and a false the partial tail
                    // that has to be joined to the next block.
                    // Both keep the same off and count and differ only in the reason for
                    // stopping.
                    off == size ? FrameStop::kEndOfBuffer : FrameStop::kPartialTail};
        }
        // Read the two byte big-endian body length from data+off.
        // read_be allows that address to be unaligned; len does not include the length prefix
        // itself.
        // The condition before the read has already proved that data+off and the byte after it
        // are inside the buffer.
        // len is const, because once this turn's record boundary is settled it should not change
        // before or after the callback.
        const std::uint16_t len = read_be<std::uint16_t>(data + off);
        // A zero length is not a valid ITCH message, and off cannot move on to the next record by
        // it either.
        // A true returns kZeroLength where it stands, calling neither fn nor raising count.
        // A false carries on to check whether all len bytes of the body are in the current
        // buffer.
        // Were a zero length allowed to carry on, off would rise by two bytes with no type to
        // hand to a callback, and a broken stream could be swallowed quietly.
        // Returning consumed=off lets a log or a test point exactly at the length prefix that
        // went wrong.
        // The complete messages before it stay in count, so the caller knows how many were
        // handled before the error.
        if (len == 0) return {off, count, FrameStop::kZeroLength};
        // A complete record needs kLenPrefix+len bytes.
        // A true means the body runs past the end of the buffer, and the length prefix and the
        // body that did arrive both stay at off for the next read.
        // A false means the whole record is there and the body pointer and the Message can be
        // built safely.
        // It does not require len to equal kBodyLen[type]; a strict protocol check is done by
        // whoever received the Message.
        // Where they are equal the last body byte lands exactly on size-1, which is a complete
        // record, so only a > counts as partial.
        // On the return off still points at the length prefix, and the layer above moves the
        // prefix and the body it has across together.
        // fn is not called in this branch, because any of Message's common fields may not have
        // arrived complete.
        // count does not go up either, keeping "how many times the callback ran" and messages
        // always in step.
        if (off + kLenPrefix + len > size) {
            // No incomplete byte is counted into consumed, and count does not include this
            // message that has not been handed to fn.
            // stop is plainly kPartialTail, using the same way of recovering above as the case of
            // a single prefix byte left.
            return {off, count, FrameStop::kPartialTail};
        }
        // body starts after the length prefix, and len is the length of a body already confirmed
        // to be there in full.
        // fn may read the Message but must not keep the view past the point where the buffer data
        // belongs to goes away.
        // Returning true means another one is wanted, and false means stop having handled this
        // one.
        // body plus kLenPrefix makes sure Message::type() sees the first byte of the body rather
        // than the top byte of the length.
        // len goes into the Message as it is, and the callback can compare it against
        // kBodyLen[type].
        // A local bool keeps the callback's result, so that off and count are both updated as
        // "handled" whether it was true or false.
        // The callback finishes on this line and never runs alongside the next message.
        const bool go_on = fn(Message{data + off + kLenPrefix, len});
        // The current record has been handed to the callback in full, so off steps over the two
        // byte prefix and the len bytes of body.
        // Even where go_on is false this record counts as consumed, so recovering above does not
        // handle it twice.
        // The new off still points at a record boundary: either the next length prefix or exactly
        // size.
        // The addition uses the same three terms as the boundary check above, so off cannot move
        // outside the buffer.
        off += kLenPrefix + len;
        // The callback really did see one complete message, so the count of messages goes up by
        // one.
        // It goes up before go_on is looked at, so a message that stopped things is included in
        // the messages returned.
        ++count;
        // A true means the callback asks to carry on, and the loop goes back to the top to check
        // the next length prefix.
        // A false means a deliberate stop, and the position returned is already after the record
        // just handled.
        // The caller has to really stop or break at its own level, and must not feed the rest of
        // a kCallerStopped buffer back again and again.
        // With go_on true the if does not run and control returns to the top of the for to check
        // the next prefix.
        // With go_on false it returns kCallerStopped, which is not an error but the callback
        // deliberately ending the task at hand.
        // off and count already include the current message, so a caller resuming should start at
        // data+off.
        if (!go_on) return {off, count, FrameStop::kCallerStopped};
    }
}

}
