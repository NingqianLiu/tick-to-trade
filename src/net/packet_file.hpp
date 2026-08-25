#pragma once

// What the file the sender maps directly into memory looks like.
//
// There is a division of labour to explain first, or it seems odd that the file holds no send
// times: working out which messages go in a packet and what its sequence number is takes a long
// time, so it is done offline and written into the file; when a packet goes out is deliberately
// not written, and the sender works it out as it runs from the ITCH time each packet carries.
// That way changing the pace does not mean generating forty five gigabytes again.
//
// The layout: a file header first, then one directory entry per packet, and only then the bytes
// of all the packets.
// The bytes of every packet have room left in front of them for the three headers, so handing a
// packet to the card moves not one byte.

#include <cstdint>

namespace pkt {

// The identifying number at the start of the file, four bytes that spell "PKT1".
// The first thing reading a file does is check it; a mismatch means the wrong file and the
// program exits.
inline constexpr std::uint32_t kMagic = 0x31544b50;  // "PKT1"
// How many bytes are left in front of every packet for the three headers: Ethernet 14, IP 20,
// UDP 8.
inline constexpr std::uint32_t kHeadroom = 42;       // Ethernet 14 + IP 20 + UDP 8

// The piece at the very start of the file, describing the whole of it.
struct FileHeader {
    // The identifying number, which has to equal kMagic.
    std::uint32_t magic;
    // How many bytes were left in front of every packet. Stored so that an old file is still
    // recognisable if this number ever changes.
    std::uint32_t headroom;
    // How many packets the file holds, which is also how many directory entries follow.
    std::uint64_t packets;
    // How many ITCH messages there are across all of them, for checking the numbers afterwards.
    std::uint64_t messages;
    // The MoldUDP64 sequence number of the first packet. The receiving side counts from it.
    std::uint64_t first_seq;
    // How many bytes all the packets come to together, which mapping the file needs to work out
    // the total length.
    std::uint64_t body_bytes;
    // The time of the last packet less the time of the first.
    // That is, how long a stretch of trading this file covers.
    // Before a run it gives an estimate of how much wall clock this round will take.
    std::uint64_t span_ns;  // ITCH timestamp of the last packet minus the first
    // The session name, sixteen bytes, written into the MoldUDP64 header of every packet.
    char session[16];
};

// One directory entry per packet. The sender walks this array, one entry being one packet.
struct Entry {
    // The ITCH time of the last message in this packet.
    // The last rather than the first, because a packet cannot possibly go out before its own
    // last message has happened - with the first, a replayed packet would be earlier than it was
    // on the day.
    // The sender turns it into when to send.
    // In the 1:1 stretches it keeps the original spacing, and elsewhere it uses a fixed pace.
    std::uint64_t ts_ns;
    // Which byte of the file's data area this packet's bytes start at.
    // It points at the start of the room left for the three headers, not at the MoldUDP64
    // header.
    std::uint64_t offset;  // into the body, points at the headroom
    // How many bytes this packet is altogether: the room for the three headers, the MoldUDP64
    // header, and all the messages.
    std::uint32_t len;     // headroom + MoldUDP64 header + messages
    // How many messages this packet holds. It is what the sequence number advances by, rather
    // than one.
    std::uint32_t messages;
};

}  // namespace pkt
