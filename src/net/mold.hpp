#pragma once

// The MoldUDP64 layer. The first twenty bytes of a packet are:
//   the session name, 10, the sequence number of the first message in this packet, 8, and how
//   many messages the packet holds, 2
// After those twenty bytes come the ITCH records one after another, each with a two byte length
// in front - exactly as they are laid out in the raw market data file, so the bytes read from
// the file are moved across as they are, with no parsing and no rewriting.
//
// There is one thing here that is very easy to confuse. Confusing it makes the whole book
// worthless.
//
// The sequence number in this header is the transport layer's own counter. Replaying, we number
// it afresh from the beginning.
// The eight byte order id inside an ITCH message body is an entirely different field.
// It has to pass through untouched, not one bit changed.
//
// Both are eight bytes and their names sound alike. But one may be changed and the other never.

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

// itch::read_be reads an integer out of bytes that are most significant first.
// Reading the sequence number and the count below both use it, rather than writing the byte
// order conversion again here.
#include "itch/reader.hpp"

namespace mold {

// The session name is ten bytes. Every packet of one replay carries the same ten, and the
// receiving side uses them to confirm it is the same stream rather than a packet leaking in from
// another replay.
inline constexpr std::size_t kSessionLen = 10;
// The sequence number starts at byte 10 and takes eight.
inline constexpr std::size_t kSeqOff = 10;
// The count starts at byte 18 and takes two.
inline constexpr std::size_t kCountOff = 18;
// The whole header is twenty bytes. Stepping over that many on the receiving side gives the
// first message.
inline constexpr std::size_t kHeaderLen = 20;

// Writes the twenty byte header at p.
// It takes the session name, the sequence number of the first message in this packet, and how
// many messages it holds.
inline void write_header(std::uint8_t* p, const char* session, std::uint64_t seq,
                         std::uint16_t count) noexcept {
    // The session name is ten ASCII characters with no byte order to worry about, so it is
    // copied as one piece.
    std::memcpy(p, session, kSessionLen);
    // The sequence number has to go in most significant byte first. So it is turned round in a
    // register and then copied in as a piece.
    // That is quicker than shifting a byte at a time.
    // The compiler turns byteswap into a single bswap instruction.
    const std::uint64_t s = std::byteswap(seq);
    // What is written is the swapped local s rather than the original seq.
    // memcpy rather than a plain assignment, because the destination here is not guaranteed to
    // be aligned to eight bytes.
    std::memcpy(p + kSeqOff, &s, sizeof(s));
    // The count has to be turned round as well, two bytes being no exception.
    const std::uint16_t c = std::byteswap(count);
    // Written at offset 18. After this line the twenty bytes are complete.
    std::memcpy(p + kCountOff, &c, sizeof(c));
}

// Reads the sequence number out of a packet received.
// p points at the first byte of the MoldUDP64 header.
// That is, the position past the forty two bytes of Ethernet, IP and UDP.
// Given the wrong position it reads rubbish, and nothing reports it.
//
// The receiving side uses it to check the numbering.
// Lower than expected is a duplicate, the same copy sent down the other path.
// Higher than expected means something was lost in between.
[[nodiscard]] inline std::uint64_t sequence(const std::uint8_t* p) noexcept {
    return itch::read_be<std::uint64_t>(p + kSeqOff);
}

// Reads how many messages a packet received holds.
// The receiving side uses it to work out the next packet's sequence number: this number plus
// this count.
[[nodiscard]] inline std::uint16_t count(const std::uint8_t* p) noexcept {
    return itch::read_be<std::uint16_t>(p + kCountOff);
}

// Gathers one packet at a time.
// There is only one rule and it is easy to get wrong: the sequence numbers count messages, not
// packets.
// A packet carries the number of the first message inside it.
// So the next packet's number is the previous number plus how many the previous packet held.
// Keeping to that leaves the stream continuous with no holes.
// And "continuous with no holes" is the only thing the receiving side has to judge a loss by.
// Adding one per packet would make the receiving side believe it was losing all the time.
class Packer {
public:
    // It takes the session name, which number the first packet starts at, at most how many
    // messages a packet holds, and how many bytes to leave in front of a packet.
    // That last one is room for the three headers: Ethernet, IP and UDP.
    // The room is left first, and sealing a packet fills the headers into it.
    // Not one byte of the content has to be moved along.
    Packer(const char* session, std::uint64_t first_seq, std::uint16_t max_messages,
           std::size_t headroom = 0)
        : max_(max_messages), headroom_(headroom), seq_(first_seq) {
        // The session name is kept, to be written into every packet sealed.
        std::memcpy(session_, session, kSessionLen);
        // The buffer is put into the shape of an empty packet: the room in front and the place
        // for the twenty byte header are taken, and the count is zero.
        // As the last line of the constructor, so add can be called straight away and the caller
        // does not have to remember some other call first.
        clear();
    }

    // This packet holds no messages yet. The sending side uses it to ask whether there is
    // anything to send.
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    // Whether it is full. In practice this returns false essentially always.
    // What really ends a packet is one more message going past a frame, not the count reaching
    // its limit.
    // The limit here comes from the protocol rather than being a performance setting of ours -
    // the count field on the wire is two bytes and goes to sixty odd thousand. What really
    // decides how large a packet is, is the frame the caller has to fill; reaching the frame's
    // limit is when to seal, and this limit usually never gets a chance to fire.
    [[nodiscard]] bool full() const noexcept { return count_ >= max_; }
    // How many bytes this packet is altogether, including the room in front and the twenty byte
    // header.
    [[nodiscard]] std::size_t bytes() const noexcept { return buf_.size(); }
    // How many messages so far. It is also how far the next packet's sequence number jumps -
    // the numbers count messages, so next() uses this number the moment the packet is sealed.
    [[nodiscard]] std::uint16_t count() const noexcept { return count_; }
    // Which sequence number this packet will use. Sealing writes it into the header.
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }

    // Add one message to the packet.
    // The caller has to have asked whether to seal first.
    // This function does not check whether it fits.
    // record means the whole thing, the two byte length and the message body.
    // Exactly as it appears in the raw file.
    // It is neither parsed nor rewritten but moved across as it is.
    // That way the bytes replayed are the same bytes the exchange sent on the day.
    void add(const std::uint8_t* record, std::size_t record_len) {
        // Appended to the end of the buffer. The room in front and the place for the header were
        // taken in clear(), so these bytes land behind the header and cannot cover it.
        buf_.insert(buf_.end(), record, record + record_len);
        // The count goes up, to be written into the header on sealing.
        ++count_;
    }

    // Seal: write the twenty byte header into the place kept for it and hand the whole packet
    // over.
    [[nodiscard]] std::span<const std::uint8_t> seal() noexcept {
        // The header goes after the room rather than at the very start of the buffer - that
        // first piece is for the three headers and the MoldUDP64 header follows it.
        write_header(buf_.data() + headroom_, session_, seq_, count_);
        // What is handed over is the whole piece from the first byte of the buffer, room
        // included, to the end.
        // The caller fills the three headers into the room and hands the whole piece to the
        // card.
        // What comes back is a view rather than a copy, so the caller has to be finished with it
        // before the next next(), whose clear() wipes the content.
        return {buf_.data(), buf_.size()};
    }

    // Turn the page and start gathering the next packet.
    void next() noexcept {
        // This line is the rule above: what is added is how many messages this packet held, not
        // one.
        seq_ += count_;
        // The buffer goes back to the shape of an empty packet.
        clear();
    }

private:
    // Puts the buffer back to an empty packet: the room in front and the twenty byte header
    // filled with zeros to hold the places, nothing behind them, and the count at zero.
    void clear() {
        // assign sets the length to that many and fills them all with zeros. The header's
        // content is only written on sealing, and this merely keeps the place - so that the
        // bytes add appends land behind the header.
        // The same vector is reused throughout, so once the run is steady nothing more is asked
        // of the system.
        buf_.assign(headroom_ + kHeaderLen, 0);
        // The count goes to zero and the next packet counts again.
        count_ = 0;
    }

    // The buffer a packet is gathered in.
    std::vector<std::uint8_t> buf_;
    // The session name, copied at construction, written as the same ten bytes into every packet.
    char session_[kSessionLen];
    // At most how many messages a packet holds, from the constructor's argument.
    std::uint16_t max_;
    // How many messages this packet holds so far.
    std::uint16_t count_ = 0;
    // How many bytes to leave in front of a packet for the three headers.
    std::size_t headroom_;
    // This packet's sequence number. Every next() moves it on by the count.
    std::uint64_t seq_;
};

}  // namespace mold
