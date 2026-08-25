#pragma once

// The forty two bytes at the front of every UDP packet: an Ethernet header of 14, an IPv4 header
// of 20, and a UDP header of 8.
// This file does one thing - fill those forty two bytes in as the protocols say.
//
// Ordinary socket programs never deal with any of this, so why fill it in here?
// Because we bypass the kernel's network stack.
// Ordinarily the kernel assembles the three headers for you. There is no kernel on this path any
// more.
//
// And they are filled in where they lie.
// Written straight into the card's memory this frame will later be sent from.
// So handing the packet to the card moves not one byte.
//
// Leaving the UDP checksum at zero throughout looks like laziness.
// In fact IPv4 says plainly that a zero means this packet has no UDP checksum.
// The exchange sends its own market data the same way.
//
// It is safe because the layers above and below both cover it.
// MoldUDP64 above carries a sequence number on every packet, so a loss is certainly seen.
// Ethernet's own frame check below stops a packet corrupted on the wire.
// And this link is a short direct cable anyway.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace eth {

// The Ethernet header is 14 bytes: destination MAC 6, source MAC 6, and the protocol above, 2.
inline constexpr std::size_t kEthernetBytes = 14;
// The IPv4 header is 20 bytes. Twenty is the shortest, with no options at all, and we never use
// options, so it is a constant.
inline constexpr std::size_t kIpBytes = 20;
// The UDP header is 8 bytes: source port 2, destination port 2, length 2, checksum 2.
inline constexpr std::size_t kUdpBytes = 8;
// The three together are 42 bytes.
// The receiving side steps over that many to reach the start of MoldUDP64.
// The sending side leaves that many in front of the data.
inline constexpr std::size_t kHeaderBytes = kEthernetBytes + kIpBytes + kUdpBytes;
// A MAC address is six bytes. This number is used below both as a length and as an offset, since
// the source MAC starts exactly at byte six, so both places have to use the same constant.
inline constexpr std::size_t kMacBytes = 6;

// One end of the conversation: card address, address and port. Sending a packet needs one at
// each end.
struct Endpoint {
    // The six byte card address, stored in the order it appears on the wire so it can be copied
    // straight into a packet.
    std::uint8_t mac[kMacBytes];
    // The address is stored as an ordinary number rather than in the order of the packet. That
    // is, 10.9.9.1 is stored as 0x0A090901 and turned round only when a packet is written. That
    // makes it plain to write in a configuration, and the turning round happens in one place
    // (put32 below) rather than being scattered about.
    std::uint32_t ip;    // host order
    // The port likewise, an ordinary number turned round when a packet is written.
    std::uint16_t port;  // host order
};

// Joins the four parts of a.b.c.d into one number.
// It is constexpr so that an address written into a configuration is worked out at compile time.
// Running costs not one instruction.
[[nodiscard]] inline constexpr std::uint32_t ipv4(std::uint8_t a, std::uint8_t b,
                                                  std::uint8_t c,
                                                  std::uint8_t d) noexcept {
    // The first part takes the top eight bits and the rest follow down, the last taking the
    // lowest eight.
    return (std::uint32_t{a} << 24) | (std::uint32_t{b} << 16) |
           (std::uint32_t{c} << 8) | d;
}

// The IP header's checksum.
//
// It is not ordinary addition and must not be read as such.
// The rule is: take two bytes at a time as a 16 bit number and add them all up.
// What overflows must not be dropped but folded back into the low sixteen bits and added on.
// Finally the whole thing is inverted.
//
// The algorithm has a rather neat property.
// Running it again over a header that already carries the right checksum always gives zero.
// That is what the receiving side checks by, without having to pull the checksum out first.
[[nodiscard]] inline std::uint16_t checksum(const std::uint8_t* p,
                                            std::size_t n) noexcept {
    // The sum is 32 bits so that the carries gather in the top sixteen and are folded back at
    // the end together.
    std::uint32_t sum = 0;
    // Two bytes at a time. The condition is i + 1 < n so that a last byte with no pair is not
    // read here - the line below deals with it.
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        // The first byte is the top eight bits and the second the low eight, which is the order
        // they are in inside the packet.
        sum += (std::uint32_t{p[i]} << 8) | p[i + 1];
    }
    // An odd length leaves one byte over, which counts as the top eight bits with zero below.
    // (The IP header is a fixed twenty bytes and never reaches this; it is here so the function
    // is right for any length.)
    if (n % 2 != 0) sum += std::uint32_t{p[n - 1]} << 8;
    // This line is the folding above: what went past sixteen bits is added back into the low
    // sixteen, until it no longer overflows. Without it the result is not the checksum this
    // protocol asks for.
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    // Inverting the whole thing gives the value to write into the packet. The caller writes it
    // back into bytes 10 and 11 of the IP header.
    return static_cast<std::uint16_t>(~sum);
}

// The MAC that goes with an IPv4 multicast address is not looked up but worked out by a fixed
// rule: the first three bytes are always 01:00:5e, and the last three are the lowest twenty
// three bits of the address.
// Note that the top bit of the second part is dropped.
// So two multicast addresses that differ by 128 in that bit work out to the same MAC.
// That is not our problem; the protocol simply is that way.
inline void multicast_mac(std::uint32_t ip, std::uint8_t out[kMacBytes]) noexcept {
    // The first byte of the fixed prefix. Its lowest bit is 1, and that bit is Ethernet's flag
    // for a multicast address - a switch seeing it knows to copy the frame to several ports.
    out[0] = 0x01;
    // This middle byte is always zero and has nothing to do with the address - only the last
    // three bytes of a multicast MAC come from it.
    out[1] = 0x00;
    // The third byte of the fixed prefix.
    // With this, 01:00:5e is complete, which is how every IPv4 multicast MAC begins.
    out[2] = 0x5e;
    // The second part of the address, with the top bit cleared by the rule, keeping the low
    // seven.
    out[3] = static_cast<std::uint8_t>((ip >> 16) & 0x7f);
    // The third part of the address, carried across as it is.
    out[4] = static_cast<std::uint8_t>((ip >> 8) & 0xff);
    // The fourth part, carried across as it is. That fills the six bytes, and the caller uses
    // them as the destination MAC.
    out[5] = static_cast<std::uint8_t>(ip & 0xff);
}

namespace detail {

// Writes a 16 bit number into two bytes, most significant first.
//
// Why not simply *(std::uint16_t*)p = v?
// Because x86 memory is least significant first.
// The two bytes written that way come out in exactly the opposite order from what the protocol
// asks for.
// And casting the pointer has an alignment problem as well.
inline void put16(std::uint8_t* p, std::uint16_t v) noexcept {
    // The top eight bits first. That order is the whole meaning of network byte order: the top
    // of a number goes on the wire first, so a receiver reading the first bytes already knows
    // the size of it.
    p[0] = static_cast<std::uint8_t>(v >> 8);
    // The low eight go into the second byte. The conversion here cuts the top off, which is
    // intended.
    p[1] = static_cast<std::uint8_t>(v);
}

// Writes a 32 bit number into four bytes, most significant first.
inline void put32(std::uint8_t* p, std::uint32_t v) noexcept {
    // The top sixteen bits go into the first two bytes through put16.
    put16(p, static_cast<std::uint16_t>(v >> 16));
    // The low sixteen go into the last two. Written as two 16 bit halves the order comes out
    // right by itself, without working through how four bytes should be laid out again.
    put16(p + 2, static_cast<std::uint16_t>(v));
}

}  // namespace detail

// This is the body of the file: write all three headers in one go.
// It takes where to write - out being the first byte of this frame - the MAC, address and port
// of the sender and the receiver, and how many bytes of content follow the three headers.
// It fills the forty two bytes from front to back in the order Ethernet, IPv4, UDP.
// Once written the frame can be handed to the card whole and does not have to be touched again.
inline void write(std::uint8_t* out, const Endpoint& src, const Endpoint& dst,
                  std::size_t payload) noexcept {
    // The first six bytes are the destination MAC rather than the source. What appears on the
    // wire first is who it is for.
    // Because a switch can start forwarding as soon as it has read those six bytes.
    std::memcpy(out, dst.mac, kMacBytes);
    // The source MAC follows. Getting the two the wrong way round sends the packet back to
    // ourselves and dirties the switch's address table, which affects other traffic on this
    // link.
    std::memcpy(out + kMacBytes, src.mac, kMacBytes);
    // Bytes 12 and 13 say what follows, and 0x0800 is IPv4.
    detail::put16(out + 12, 0x0800);  // IPv4

    // The IP header starts at byte 14.
    // A local pointer is taken so that the offsets below are counted from the IP header itself.
    // That way they match the table in the protocol document one for one, without adding 14 in
    // one's head every time.
    std::uint8_t* ip = out + kEthernetBytes;
    // 0x45 is not a magic number. The top four bits are the version, 4.
    // The low four are how many 32 bit words this header is. Five words is 20 bytes.
    // Which is to say no options at all.
    ip[0] = 0x45;  // version 4, a header of five 32-bit words
    // Byte 1 is the type of service, the priority sort of thing. We mark nothing, so it is zero.
    ip[1] = 0;
    // Bytes 2 and 3 are how many bytes there are from the start of the IP header.
    // That is the IP header, the UDP header and the content.
    // It does not include the 14 bytes of the Ethernet header in front.
    detail::put16(ip + 2, static_cast<std::uint16_t>(kIpBytes + kUdpBytes + payload));
    // Bytes 4 and 5 are the identification, which only reassembling fragments needs. We never
    // fragment, so it is zero.
    detail::put16(ip + 4, 0);       // identification: nothing is ever fragmented
    // Bytes 6 and 7 are the flags and the fragment offset. The bit set in 0x4000 means do not
    // fragment.
    detail::put16(ip + 6, 0x4000);  // don't fragment
    // Byte 8 is how many more hops it may take. This is a direct link and needs none, so it
    // holds the common 64.
    ip[8] = 64;                     // hops
    // Byte 9 says what the layer above is, and 17 is UDP.
    ip[9] = 17;                     // UDP
    // Bytes 10 and 11 are the checksum, which starts as zero here.
    // This step cannot be skipped: the checksum is worked out over a header that holds zero in
    // its own place, and without clearing it the previous packet's value would be counted in and
    // the result would be wrong.
    detail::put16(ip + 10, 0);      // the checksum covers itself as zero
    // Bytes 12 to 15 are the source address. put32 turns the byte order round on the way.
    detail::put32(ip + 12, src.ip);
    // Bytes 16 to 19 are the destination address. With that the IP header is complete but for
    // the checksum.
    detail::put32(ip + 16, dst.ip);
    // The whole header is in place now, so the checksum is worked out and written back into
    // bytes 10 and 11.
    // Only these twenty bytes of the IP header are covered, not the content behind them.
    // An IPv4 checksum protects the header only.
    detail::put16(ip + 10, checksum(ip, kIpBytes));

    // The UDP header follows the IP header. A local pointer again, with offsets counted from the
    // UDP header itself.
    std::uint8_t* udp = out + kEthernetBytes + kIpBytes;
    // The source port. The far end's filter may select on it too, so it is not arbitrary.
    detail::put16(udp + 0, src.port);
    // Bytes 2 and 3 are the destination port. The filter on the receiving card selects packets
    // by exactly this port - wrong, and a packet reaches the far card but never enters our
    // queue.
    detail::put16(udp + 2, dst.port);
    // Bytes 4 and 5 are how many bytes the UDP header and the content come to.
    // The eight bytes of the UDP header itself are included.
    detail::put16(udp + 4, static_cast<std::uint16_t>(kUdpBytes + payload));
    // Bytes 6 and 7 are the UDP checksum, left at zero meaning no checksum. Why that is allowed
    // is at the top of this file.
    // With this line the forty two bytes are complete.
    // The caller carries on writing the MoldUDP64 content behind them.
    detail::put16(udp + 6, 0);
}

}  // namespace eth
