#pragma once

// The sizes of the hardware.
//
// Why constants rather than command line options: they describe this machine and this
// cable, not what a particular run is doing. Both ends also have to agree on every one of
// them, and compiling them in makes disagreeing impossible. What really changes from run
// to run - which day, how many shards, what speed - comes from the environment instead.

#include <cstddef>
#include <cstdint>

namespace cfg {

// A huge page is 2 MB. Not 1 GB: this processor has no room for 1 GB entries in its
// address translation cache, so using them would put every access on the slow path.
inline constexpr std::size_t kHugePageBytes = 2u << 20;  // 1 GB pages have no
                                                         // L2 TLB entries here

// How many receive descriptors the card will take. Measured rather than chosen: 4096 is
// accepted and 8192 is refused. "Buffer a bit more" ends here, at the hardware.
inline constexpr std::size_t kRxDescriptors = 4096;

// The memory the card writes packets into: 2048 huge pages, which is 4 GB.
//
// If the card only knows 4096 descriptors, why 4 GB? Because every packet received is
// given a brand new slot rather than the one just read being handed back. A slot is not
// written again until eight million packets later, and that is how far a shard can fall
// behind and still find its own packet where it left it. It is also the only way around
// the card's limit of 4096.
//
// Where 4 GB comes from: with 16 MB, thirty two thousand packets, a shard was lapped
// eleven times in one run, and the worst latency in that run was thirty seven
// milliseconds. Both paths together peak at about twelve million packets a second, so the
// deepest backlog is under half a million packets. Four gigabytes holds eight million,
// seven hundred milliseconds at that rate, with a wide margin. Nothing else pays for it:
// the pages are walked in order, one page covers four thousand packets, and the size
// makes no difference to that.
inline constexpr std::size_t kRxRingHugePages = 2048;
// 2048 bytes a slot, which has to hold the largest frame plus the short prefix the card
// writes in front of it - checked at the bottom of this file.
//
// It was raised from 512 along with the frame limit below: a full 1514 byte frame plus its
// prefix does not fit 512, the card would split it across two slots, and the receiving
// side has no code for that.
inline constexpr std::size_t kRxSlotBytes = 2048;
// With hardware timestamps on, the card writes a short prefix before each frame, carrying
// the moment it arrived. The real value is read from the card at start up; this copy only
// exists so the compile time check below can be worked out.
inline constexpr std::size_t kRxPrefixBytes = 14;
// The whole block, 4 GB.
inline constexpr std::size_t kRxRingBytes = kRxRingHugePages * kHugePageBytes;
// Eight million slots - the number of packets before one comes round again.
inline constexpr std::size_t kRxRingSlots = kRxRingBytes / kRxSlotBytes;

// A whole number of pages, or the tail of the block would fall on ordinary ones.
static_assert(kRxRingBytes % kHugePageBytes == 0);
// A power of two, so wrapping is a bitwise and rather than a modulo.
static_assert((kRxRingSlots & (kRxRingSlots - 1)) == 0, "the ring wraps by mask");
// More slots than the card has descriptors, or handing out a fresh slot per packet - and
// with it everything above - would not be possible.
static_assert(kRxRingSlots > kRxDescriptors, "there must be spare buffers to rotate through");

// 2048 bytes a slot on the send side as well, shared between the order path and the tool
// that measures the card's own floor. Orders need far less - a frame holds at most twenty
// eight of them, fourteen hundred bytes - but at 2048 the same slots also hold a full
// Ethernet frame, so one piece of memory registered with the card serves both.
inline constexpr std::size_t kOrderSlotBytes = 2048;

// The compile time guarantee of the sentence above.
static_assert(1514 <= kOrderSlotBytes, "a full Ethernet frame must fit one transmit slot");

// How long a packet may be held while more messages are gathered into it.
//
// Why time and not a count of messages: with a count, a quiet stretch never fills the
// packet and the messages already in it are simply held. Measured at sixteen messages a
// packet, the median message was held for 98.7 microseconds - against a whole latency of a
// few microseconds, which means the fixture was burying what it was supposed to measure.
// A time limit bounds that, and in a burst the packet still goes as soon as a frame is
// full rather than waiting for sixteen, so the peak came down too.
inline constexpr std::uint64_t kCoalesceNs = 300;

// At most 1486 bytes of IP packet, the largest the link allows.
//
// This was once lowered to 484, for a reason: the card timestamps a frame at its first
// byte, so the time the rest of the frame spends on the wire is counted in the latency of
// every message inside it. Measured over a full day, every percentile above p98.5 sat in
// the full frame bucket, paying 1248 ns for it, and at 498 bytes that fell to 418 ns.
//
// Putting it back was deliberate, and what it buys is fidelity: how the exchange packs
// messages into packets is the exchange's behaviour, not ours to choose because a smaller
// one measures better. More messages per packet also gives the batching something to
// batch.
inline constexpr std::size_t kEthMtu = 1486;         // the IP packet, not the frame
// 20 bytes of IP header and 8 of UDP. The figure above counts the IP packet, without the
// Ethernet header, so only these two come off.
inline constexpr std::size_t kIpUdpBytes = 20 + 8;
// The MoldUDP64 header.
inline constexpr std::size_t kMoldHeaderBytes = 20;  // session 10 + seq 8 + count 2
// How many bytes of messages a packet can hold, which is what the gathering code asks when
// deciding whether one more will fit - so it counts no headers at all.
inline constexpr std::size_t kMaxPacketPayload = kEthMtu - kIpUdpBytes - kMoldHeaderBytes;
// The largest frame on the wire: the Ethernet header plus the whole IP packet.
inline constexpr std::size_t kMaxFrameBytes = 14 + kEthMtu;  // ethernet header + all of it

// The two derived numbers are pinned down because they appear in comments and in measured
// results elsewhere. If somebody changes the MTU above and forgets, these fail at once.
static_assert(kMaxPacketPayload == 1438);
static_assert(kMaxFrameBytes == 1500);
// A frame that does not fit a slot would be split across two by the card, and the
// receiving side has no code for that - so a configuration that would fail at run time is
// stopped at compile time.
static_assert(kMaxFrameBytes + kRxPrefixBytes <= kRxSlotBytes);

}  // namespace cfg
