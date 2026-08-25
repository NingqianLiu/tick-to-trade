#pragma once

// A TCP connection of our own, used only for sending orders.
//
// Why write TCP at all when a stack already exists? The stack is correct. It also costs
// 870 nanoseconds a send, 810 of which go into one call: telling the stack what was sent.
// It needs that in order to keep a copy in case the data has to go again, and the copy is
// what is being paid for.
//
// Could the telling be deferred? Only moved, not removed - and the worst case is a burst
// with no gap in it, which is exactly when there is nowhere to move it to. Owning the
// connection takes the cost away instead.
//
// Three things make that safe to do: there is one connection, the peer is a machine of
// ours, and between them is a direct cable with no other traffic. So congestion control,
// reordering and managing many connections are all beside the point.
//
// This header holds only the part that can be checked without a card: laying out the bytes
// of a frame, the two checksums, and the arithmetic on sequence numbers. What needs the
// card - finding the peer's address, the handshake, polling for acknowledgements - lives in
// the tool that drives it. Split that way, this half is pinned down by unit tests.

// std::atomic. The thread that sends orders and the thread that reads acknowledgements each
// write different fields of this connection: one moves "how far we have sent", the other
// moves "how far they have acknowledged" and "how much they can still take". The fields do
// not overlap, but each side reads what the other writes, so those have to be atomic.
//
// The most relaxed ordering is enough: all that is needed is reading a whole value rather
// than half of one, with no ordering implied - and on x86 a relaxed atomic load or store is
// an ordinary move, costing nothing.
#include <atomic>
#include <cstddef>
// Fixed widths, including the signed 32 bit type the sequence comparison needs - see
// before() below.
#include <cstdint>
// memcpy for the template, memset for clearing it in open().
#include <cstring>
// std::move, for moving a connection.
#include <utility>

// eth::Endpoint - one end's MAC, IP and port - and eth::kMacBytes. Only the types and the
// constant are borrowed: the three headers are laid out here, because the ones in eth.hpp
// are for UDP and a TCP header is not the same.
#include "net/eth.hpp"

namespace mintcp {

// Where each field sits, counted from the first byte of the frame.
//
// Offsets rather than a struct, because the hot path does not fill in a structure and send
// it: it copies a template that is already laid out and rewrites the few fields that
// change, and at that point there are offsets, not members.
inline constexpr std::size_t kEthLen = 14;
inline constexpr std::size_t kIpLen = 20;
// A TCP header is 20 bytes. Only the handshake packet is 24, because it carries four bytes
// of options.
inline constexpr std::size_t kTcpLen = 20;
// Fifty four bytes of headers, copied whole for every frame - the one fixed copy on the hot
// path.
inline constexpr std::size_t kHeaderLen = kEthLen + kIpLen + kTcpLen;  // 54
// The options that follow the handshake packet: one padding byte for alignment plus the
// three bytes of the window scale option. No other packet carries any.
inline constexpr std::size_t kSynOptLen = 4;

// The seven offsets below are the complete list of what changes from frame to frame.
// Everything else in the template is written once and never touched again.
//
// The total length in the IP header, which changes because each frame carries a different
// number of orders.
inline constexpr std::size_t kIpTotalLenOff = kEthLen + 2;   // 16
// The IP header checksum. It only follows the length, so it can be worked out as a constant
// part plus this frame's length rather than by adding twenty bytes again.
inline constexpr std::size_t kIpSumOff = kEthLen + 10;       // 24
// How far this stream has sent.
inline constexpr std::size_t kTcpSeqOff = kEthLen + kIpLen + 4;   // 38
// How far we have received from them.
inline constexpr std::size_t kTcpAckOff = kEthLen + kIpLen + 8;   // 42
// The flags byte: SYN, ACK, PSH and the rest.
inline constexpr std::size_t kTcpFlagsOff = kEthLen + kIpLen + 13;  // 47
// The receive window we advertise. It is fixed in the template, since almost no data comes
// back on this connection.
inline constexpr std::size_t kTcpWinOff = kEthLen + kIpLen + 14;    // 48
// The TCP checksum. It covers the data, so it cannot avoid changing every frame.
inline constexpr std::size_t kTcpSumOff = kEthLen + kIpLen + 16;    // 50

// The flags, all in the one byte at kTcpFlagsOff.
//
// The peer wants to close. On this path, receiving one means something went wrong.
inline constexpr std::uint8_t kFin = 0x01;
// The first packet of the handshake, sent once in the life of a connection.
inline constexpr std::uint8_t kSyn = 0x02;
// The peer tore the connection down. Like FIN, receiving one means something went wrong.
inline constexpr std::uint8_t kRst = 0x04;
// "Do not sit on this, hand it up now", on every order frame, because being acted on
// immediately is the entire point.
inline constexpr std::uint8_t kPsh = 0x08;
// "The acknowledgement number in this packet is real", on everything except the first
// handshake packet.
inline constexpr std::uint8_t kAck = 0x10;

// Reading and writing multi byte numbers in the order the wire uses. The wire is most
// significant byte first and this machine is not, so every multi byte field goes through
// these rather than being read through a cast.
inline void put16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    // The narrowing keeps the low byte, which is what is wanted.
    p[1] = static_cast<std::uint8_t>(v);
}

inline void put32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

// Widened to 32 bits before the shift: shifting a uint8_t promotes it to int, which is
// undefined for some values.
[[nodiscard]] inline std::uint16_t get16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((std::uint32_t{p[0]} << 8) | p[1]);
}

// Used to read the peer's acknowledgement number.
[[nodiscard]] inline std::uint32_t get32(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | p[3];
}

// Adds a stretch of bytes together in pairs, deliberately without the final fold.
//
// This is the design that makes the file fast: unfolded sums can be added to one another,
// so the parts of the headers that never change are summed once before the run and each
// frame only adds what is different about it.
[[nodiscard]] inline std::uint32_t sum16(const std::uint8_t* p,
                                         std::size_t n) noexcept {
    // Accumulated in 32 bits, with the carries collecting in the top half.
    std::uint32_t s = 0;
    // Declared outside the loop because an odd length leaves one byte over.
    std::size_t i = 0;
    for (; i + 1 < n; i += 2) s += (std::uint32_t{p[i]} << 8) | p[i + 1];
    // That last byte counts as the high half, with zero below it.
    if (i < n) s += std::uint32_t{p[i]} << 8;
    // Still unfolded: the caller either adds more or folds once at the end.
    return s;
}

// Folds the carries back into the low sixteen bits and inverts, which is the value that
// goes into the packet.
//
// The algorithm has a convenient property: summing a header that already carries a correct
// checksum gives zero, so the receiver does not have to take the checksum out first.
[[nodiscard]] inline std::uint16_t fold(std::uint32_t s) noexcept {
    // A while and not an if: folding once can produce another carry.
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    // Without the inversion the packet still goes out, and the peer drops it as corrupt.
    return static_cast<std::uint16_t>(~s);
}

// Sequence numbers are 32 bits and wrap.
//
// Writing a < b would be wrong once every four gigabytes: after a wrap, a very large number
// really does come before a very small one. Taking the difference and reading it as signed
// is right on both sides of the wrap.
[[nodiscard]] inline bool before(std::uint32_t a, std::uint32_t b) noexcept {
    // The subtraction has to happen unsigned, where wrapping is defined; only the result is
    // read as signed.
    return static_cast<std::int32_t>(a - b) < 0;
}

// The same, counting equality as well. Used for "has every byte of this frame been
// acknowledged" - an acknowledgement exactly at the end of a frame has acknowledged it.
[[nodiscard]] inline bool before_eq(std::uint32_t a, std::uint32_t b) noexcept {
    return static_cast<std::int32_t>(a - b) <= 0;
}

// One connection: who is at each end, how far the stream has got, and a template of a frame
// with every unchanging byte already in place.
//
// That template is why this is fast. Sending is not building a packet; it is copying a
// template and rewriting seven fields.
class Conn {
public:
    Conn() = default;
    // The three cross thread fields are atomic, and an atomic cannot itself be moved, so
    // the values are copied across one at a time. A connection may be moved before the two
    // threads have it - returning it from the function that built it, say - but never once
    // they are using it, because this copying is not atomic.
    Conn(Conn&& o) noexcept { *this = std::move(o); }
    Conn& operator=(Conn&& o) noexcept {
        us_ = o.us_;
        them_ = o.them_;
        // An array, so it is copied rather than assigned.
        std::memcpy(peer_mac_, o.peer_mac_, eth::kMacBytes);
        // The fifty four byte template, whole.
        std::memcpy(hdr_, o.hdr_, sizeof(hdr_));
        ip_base_ = o.ip_base_;
        tcp_base_ = o.tcp_base_;
        rcv_nxt_ = o.rcv_nxt_;
        window_ = o.window_;
        peer_shift_ = o.peer_shift_;
        snd_una_.store(o.snd_una_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        snd_nxt_.store(o.snd_nxt_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
        peer_wnd_.store(o.peer_wnd_.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
        return *this;
    }

    // Fills in the template: both addresses, both ports, every IP header field that never
    // moves, and the part of each checksum those fields decide. What is left for a send is
    // the length, the two sequence numbers, the flags and the data.
    void open(const eth::Endpoint& us, const eth::Endpoint& them,
              const std::uint8_t peer_mac[eth::kMacBytes],
              std::uint32_t initial_seq, std::uint16_t window) noexcept {
        us_ = us;
        them_ = them;
        // The peer's card address cannot be worked out from its IP - that only holds for
        // multicast - so it is looked up beforehand and passed in.
        std::memcpy(peer_mac_, peer_mac, eth::kMacBytes);
        // Nothing is in flight yet, so both start at the same sequence number.
        snd_una_.store(initial_seq, std::memory_order_relaxed);
        snd_nxt_.store(initial_seq, std::memory_order_relaxed);
        // Nothing received from them yet; the real value arrives with the handshake reply.
        rcv_nxt_ = 0;
        window_ = window;
        // Until the handshake says otherwise the peer can take nothing, so no order can go
        // out before it completes.
        peer_wnd_.store(0, std::memory_order_relaxed);
        // No scale agreed yet, which is the same as no scaling.
        peer_shift_ = 0;
        // Cleared first, so that every byte not written below stays zero - which is what
        // the TCP header fields this connection does not use should be.
        std::memset(hdr_, 0, sizeof(hdr_));

        // Ethernet: to whom, from whom, and what follows.
        //
        // The destination is first, so a switch can start forwarding after six bytes.
        std::memcpy(hdr_ + 0, peer_mac_, eth::kMacBytes);
        std::memcpy(hdr_ + 6, us_.mac, eth::kMacBytes);
        // 0x0800 means IPv4 follows.
        put16(hdr_ + 12, 0x0800);

        // IP: version four, a header of five words, which is twenty bytes with no options.
        hdr_[kEthLen + 0] = 0x45;
        hdr_[kEthLen + 1] = 0;  // no differentiated services
        // Total length is written per packet.
        // The identification field is only for reassembling fragments, and nothing here is
        // ever fragmented.
        put16(hdr_ + kEthLen + 4, 0);  // identification, left at zero
        // Do not fragment - the pair to the zero above. Holding both fixed is also what
        // leaves the header checksum depending on the length alone.
        put16(hdr_ + kEthLen + 6, 0x4000);
        // A direct cable needs no hops at all; 64 is the usual value.
        hdr_[kEthLen + 8] = 64;  // time to live
        hdr_[kEthLen + 9] = 6;   // TCP
        // Header checksum is written per packet, and the zero left here is exactly the state
        // the base sum below is computed against.
        put32(hdr_ + kEthLen + 12, us_.ip);
        put32(hdr_ + kEthLen + 16, them_.ip);

        // TCP: the two ports, a five word header, and the window we advertise.
        put16(hdr_ + kEthLen + kIpLen + 0, us_.port);
        // The filter on the card picks acknowledgements out by this pair of ports.
        put16(hdr_ + kEthLen + kIpLen + 2, them_.port);
        // The top four bits say which four byte word the data starts at: five words, no
        // options. The handshake packet raises it to 0x60 - see send_syn.
        hdr_[kEthLen + kIpLen + 12] = 0x50;  // five words, no options
        // Fixed in the template, because only acknowledgements come back on this connection
        // and what we can take never changes - one field fewer per frame.
        put16(hdr_ + kTcpWinOff, window_);

        // The unchanging part of each checksum, computed once.
        //
        // For IP: sum the whole header and subtract the total length, since that is the one
        // field that changes. The checksum field itself is zero at this moment, so it adds
        // nothing. A frame then costs one addition: this base plus its own length.
        ip_base_ = sum16(hdr_ + kEthLen, kIpLen) -
                   ((std::uint32_t{hdr_[kIpTotalLenOff]} << 8) | hdr_[kIpTotalLenOff + 1]);
        // TCP also covers a pseudo header - both addresses, the protocol number and the
        // length of the segment. The constant parts are added up here: the two addresses in
        // halves, the protocol, both ports, the 0x50 byte and the advertised window. What is
        // left per frame is the segment length, the two sequence numbers, the flags and the
        // data itself.
        tcp_base_ = (us_.ip >> 16) + (us_.ip & 0xffff) + (them_.ip >> 16) +
                    (them_.ip & 0xffff) + 6u + us_.port + them_.port +
                    (std::uint32_t{0x50} << 8) + window_;
    }

    // The hot path itself: lay a frame into the caller's buffer - copy the template, rewrite
    // what changes, append the data - and say how long it came to.
    //
    // Apart from summing the data, everything here writes bytes into memory that is already
    // in cache; nothing has to be fetched.
    //
    // The buffer belongs to the caller and its size cannot be checked here, so the single
    // call site pins that down with a static_assert instead.
    [[nodiscard]] std::size_t build(std::uint8_t* frame, const std::uint8_t* body,
                                    std::size_t body_len, std::uint8_t flags,
                                    std::uint32_t seq, std::uint32_t ack) const noexcept {
        // After this copy everything except the seven fields is already correct.
        std::memcpy(frame, hdr_, kHeaderLen);
        // A frame may carry nothing: a bare acknowledgement and the handshake do.
        if (body_len != 0 && body != nullptr) {
            std::memcpy(frame + kHeaderLen, body, body_len);
        }
        // What IP counts as the total: its own header, the TCP header and the data, without
        // the fourteen bytes of Ethernet.
        const std::uint16_t ip_total =
            static_cast<std::uint16_t>(kIpLen + kTcpLen + body_len);
        put16(frame + kIpTotalLenOff, ip_total);
        // The IP checksum is left at zero for the card to fill in before the frame goes out.
        //
        // The evidence for that is in the ef_vi header: the flags are called
        // EF_VI_TX_IP_CSUM_DIS and EF_VI_TX_TCPUDP_CSUM_DIS, where DIS is disable - so the
        // card computes them by default and overwrites whatever is here. The transmit
        // interface is opened without those flags, so it has been computing them all along,
        // and computing them here as well was work thrown away. Removing it took 23% off the
        // orders that landed past ten microseconds.
        put16(frame + kIpSumOff, 0);
        put32(frame + kTcpSeqOff, seq);
        // How much of their stream we have taken, which is how they know what arrived.
        put32(frame + kTcpAckOff, ack);
        frame[kTcpFlagsOff] = flags;
        // Zero here saves more than a fold: it removes walking the whole payload to add it
        // up, which for a frame of twenty eight orders is some seven hundred additions whose
        // result the card would have replaced anyway.
        put16(frame + kTcpSumOff, 0);
        return kHeaderLen + body_len;
    }

    // Sends the next packet of this stream. The only difference from build is that the
    // sequence numbers come from the connection itself and move on afterwards, so the caller
    // never has to think about them.
    [[nodiscard]] std::size_t send(std::uint8_t* frame, const std::uint8_t* body,
                                   std::size_t body_len, std::uint8_t flags) noexcept {
        // Only the sending thread writes this, so read, add, write back is safe: no other
        // writer can get in between. The acknowledgement thread only reads it, and reads a
        // whole value.
        std::uint32_t nxt = snd_nxt_.load(std::memory_order_relaxed);
        const std::size_t n = build(frame, body, body_len, flags, nxt, rcv_nxt_);
        nxt += static_cast<std::uint32_t>(body_len);
        // A SYN and a FIN each take a sequence number of their own, which is what lets a
        // handshake move the stream along while carrying no data.
        if ((flags & kSyn) != 0 || (flags & kFin) != 0) ++nxt;
        snd_nxt_.store(nxt, std::memory_order_relaxed);
        return n;
    }

    // Sends the first packet of the handshake, carrying the window scale option.
    //
    // Why it is needed: each packet tells us how many bytes may be outstanding, and the
    // field holding that number is sixteen bits, so it cannot say more than 65,535 however
    // much the peer can really take. Window scaling is an agreement made during the
    // handshake that the number in that field is to be shifted left by an agreed number of
    // bits, which widens the range without widening the field.
    //
    // Only this one packet carries options; every packet after it still has a twenty byte
    // header, so the hot path is untouched.
    //
    // The shift given here is our own side's. Almost nothing comes back on this connection,
    // so it is zero - our window means exactly what it says. The point of offering the
    // option is to let the peer state its own.
    [[nodiscard]] std::size_t send_syn(std::uint8_t* frame, std::uint8_t shift) noexcept {
        std::memcpy(frame, hdr_, kHeaderLen);
        // Four bytes of options. TCP wants the option area to be a multiple of four and the
        // window scale itself is three, so a do nothing byte pads it.
        std::uint8_t* opt = frame + kHeaderLen;
        opt[0] = 1;      // no operation, purely for alignment
        opt[1] = 3;      // the window scale option
        opt[2] = 3;      // three bytes including the kind and the length
        opt[3] = shift;  // how many bits to shift by
        // Twenty bytes of header plus four of options is six words, written in the top four
        // bits as 0x60, against 0x50 when there are no options.
        frame[kEthLen + kIpLen + 12] = 0x60;
        const std::uint16_t ip_total =
            static_cast<std::uint16_t>(kIpLen + kTcpLen + kSynOptLen);
        put16(frame + kIpTotalLenOff, ip_total);
        put16(frame + kIpSumOff, fold(ip_base_ + ip_total));
        const std::uint32_t seq = snd_nxt_.load(std::memory_order_relaxed);
        put32(frame + kTcpSeqOff, seq);
        put32(frame + kTcpAckOff, 0);
        frame[kTcpFlagsOff] = kSyn;
        // The base sum was computed with no options, which counted the 0x50 byte; this
        // packet has 0x60, so the difference of 0x10 is added into the high byte. The rest
        // is what is particular to this packet: the length of header plus options, the
        // sequence number, the flags, and the four option bytes.
        const std::uint32_t tcp_len = kTcpLen + kSynOptLen;
        const std::uint32_t s = tcp_base_ + (std::uint32_t{0x10} << 8) + tcp_len +
                                (seq >> 16) + (seq & 0xffff) + kSyn + sum16(opt, kSynOptLen);
        put16(frame + kTcpSumOff, fold(s));
        // A SYN takes a sequence number of its own.
        snd_nxt_.store(seq + 1, std::memory_order_relaxed);
        return kHeaderLen + kSynOptLen;
    }

    // Reads the peer's own window scale out of its handshake reply. A peer that did not
    // offer the option gives zero, which behaves exactly as before - a ceiling of 65,535.
    [[nodiscard]] static std::uint8_t shift_in(const std::uint8_t* frame,
                                               std::size_t len) noexcept {
        if (len < kHeaderLen) return 0;
        // The top four bits say which word the data starts at, so times four is the header
        // length.
        const std::size_t hlen =
            static_cast<std::size_t>(frame[kEthLen + kIpLen + 12] >> 4) * 4;
        // Twenty or less means no options; longer than what arrived means a broken packet.
        if (hlen <= kTcpLen || kEthLen + kIpLen + hlen > len) return 0;
        const std::uint8_t* p = frame + kEthLen + kIpLen + kTcpLen;
        const std::uint8_t* end = frame + kEthLen + kIpLen + hlen;
        // Options come one after another: a byte saying which, then a byte saying how long.
        while (p < end) {
            // Zero ends the list; what follows is padding.
            if (p[0] == 0) break;
            // One is do nothing, and has no length byte.
            if (p[0] == 1) { ++p; continue; }
            // No length byte to read, or a length too small to be real: the packet is wrong.
            if (p + 1 >= end || p[1] < 2) break;
            // Three is the window scale, its length must be three, and the third byte is the
            // shift.
            if (p[0] == 3 && p[1] == 3 && p + 2 < end) return p[2];
            p += p[1];
        }
        return 0;
    }

    // Called once the handshake is done, to record what the peer asked for. Every
    // acknowledgement after that has its sixteen bit window shifted by this much. The
    // handshake packet itself does not count - scaling only applies to what follows - so
    // this has to happen after that packet has been dealt with.
    void set_peer_shift(std::uint8_t shift) noexcept { peer_shift_ = shift; }
    [[nodiscard]] std::uint8_t peer_shift() const noexcept { return peer_shift_; }

    // This runs on the thread that reads acknowledgements, not the one that sends. It writes
    // two fields - the peer's window and how far they have acknowledged - and reads one the
    // other thread writes, so all three go through relaxed atomics.
    //
    // A true means this acknowledgement covered something that was not covered before, which
    // is how the caller knows a send slot can be freed.
    bool on_ack(std::uint32_t ack, std::uint16_t peer_window) noexcept {
        // The number in the sixteen bit field, shifted by what was agreed. With nothing
        // agreed the shift is zero and it means what it says.
        peer_wnd_.store(static_cast<std::uint32_t>(peer_window) << peer_shift_,
                        std::memory_order_relaxed);
        // An acknowledgement for bytes that were never sent: either it is not ours or it is
        // corrupt. Accepting it would put "acknowledged" ahead of "sent", the headroom
        // calculation would wrap to an enormous number, and every order would be let through
        // until the peer started dropping them.
        if (before(snd_nxt_.load(std::memory_order_relaxed), ack)) return false;
        const std::uint32_t una = snd_una_.load(std::memory_order_relaxed);
        // A repeat that moved nothing along: no slot to free.
        if (before_eq(ack, una)) return false;
        // Only this thread writes it, so there is no race here either.
        snd_una_.store(ack, std::memory_order_relaxed);
        return true;
    }

    // Bytes the peer sent us, which have to be acknowledged.
    bool on_data(std::uint32_t seq, std::size_t len) noexcept {
        // Only data in order is taken; anything else is dropped for the peer to send again.
        // On a direct cable with one sender that should never happen, so this false is less
        // a way of handling the case than a signal that something is wrong.
        if (seq != rcv_nxt_) return false;
        // Moved along, and written into the acknowledgement field of the next frame out.
        rcv_nxt_ += static_cast<std::uint32_t>(len);
        return true;
    }

    // The handshake reply carries the peer's starting sequence number; this is the one time
    // it is set.
    void set_rcv_nxt(std::uint32_t v) noexcept { rcv_nxt_ = v; }
    // How far the peer has acknowledged, compared against the end of each send slot when
    // slots are reclaimed.
    [[nodiscard]] std::uint32_t snd_una() const noexcept {
        return snd_una_.load(std::memory_order_relaxed);
    }
    // How far we have sent, recorded with each frame so it is known when that frame has been
    // fully acknowledged.
    [[nodiscard]] std::uint32_t snd_nxt() const noexcept {
        return snd_nxt_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t rcv_nxt() const noexcept { return rcv_nxt_; }
    // How many bytes the peer can still take, already scaled. The headroom check before an
    // order goes out reads this, and refuses the order if it does not fit.
    //
    // This number really did cost orders: without window scaling it stops at 65,535, and in
    // the last ten minutes of a session with the real strategy, 9,535 orders - 3.5% - were
    // turned away by our own check. The peer is an ordinary socket going through a kernel,
    // and how fast it reads is not ours to control.
    [[nodiscard]] std::uint32_t peer_wnd() const noexcept {
        return peer_wnd_.load(std::memory_order_relaxed);
    }
    // Bytes not yet acknowledged. The two values are read separately and the other thread
    // may move one in between, but only in the direction that makes the answer larger and so
    // more cautious, since "acknowledged" only ever increases - which is what makes it safe
    // as a check before sending.
    [[nodiscard]] std::uint32_t in_flight() const noexcept {
        return snd_nxt() - snd_una();
    }
    // The frame template, for the unit tests to compare byte by byte.
    [[nodiscard]] const std::uint8_t* header() const noexcept { return hdr_; }

private:
    eth::Endpoint us_{};
    eth::Endpoint them_{};
    // Kept separately because it is looked up in the system's address table rather than
    // chosen by us, as the IP and the port are.
    std::uint8_t peer_mac_[eth::kMacBytes]{};
    // The fifty four byte template: sending is copying this and rewriting seven fields.
    std::uint8_t hdr_[kHeaderLen]{};
    // The constant part of each checksum, computed once in open().
    std::uint32_t ip_base_ = 0;
    std::uint32_t tcp_base_ = 0;
    // How far we have taken their stream. Only the sending thread touches it, so it needs no
    // atomic.
    std::uint32_t rcv_nxt_ = 0;
    // The window we advertise. Nothing reads it after it goes into the template; it is kept
    // so that moving a connection carries it along.
    std::uint16_t window_ = 0;
    // The peer's shift, written once during the handshake and only read afterwards.
    std::uint8_t peer_shift_ = 0;

    // The two groups below are touched by both threads, so they are atomic - and each gets a
    // cache line of its own.
    //
    // Why a line of its own: a cache line of sixty four bytes is the smallest unit that
    // moves between two cores. With both groups on one line, a write by either side
    // invalidates the whole line for the other, which then has to fetch it back even though
    // the bytes it wants were never touched. Measured with them packed together, at offsets
    // 100, 104 and 114, the median cost 89 ns more and p90 208 ns more.
    //
    // Apart they have one writer each: one line written only by the sending thread, the
    // other only by the acknowledgement thread. Both still read the other's line, but that
    // is a message really being passed, which cannot and should not be avoided.
    //
    // It fixed something else as well: hdr_ above, read whole for every frame, used to share
    // a line with these, so every acknowledgement invalidated the template too.

    // Written only by the sending thread: how far the bytes have gone.
    alignas(64) std::atomic<std::uint32_t> snd_nxt_{0};
    // Written only by the acknowledgement thread: how far the peer has acknowledged, and how
    // much it says it can still take.
    alignas(64) std::atomic<std::uint32_t> snd_una_{0};
    std::atomic<std::uint32_t> peer_wnd_{0};
};

}  // namespace mintcp
