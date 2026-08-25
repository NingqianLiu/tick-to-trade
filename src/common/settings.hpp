#pragma once

#include <cstddef>
#include <cstdint>

namespace cfg {

inline constexpr std::size_t kHugePageBytes = 2u << 20;

inline constexpr std::size_t kRxDescriptors = 4096;

inline constexpr std::size_t kRxRingHugePages = 2048;
inline constexpr std::size_t kRxSlotBytes = 2048;
inline constexpr std::size_t kRxPrefixBytes = 14;
inline constexpr std::size_t kRxRingBytes = kRxRingHugePages * kHugePageBytes;
inline constexpr std::size_t kRxRingSlots = kRxRingBytes / kRxSlotBytes;

static_assert(kRxRingBytes % kHugePageBytes == 0);
static_assert((kRxRingSlots & (kRxRingSlots - 1)) == 0, "the ring wraps by mask");
static_assert(kRxRingSlots > kRxDescriptors, "there must be spare buffers to rotate through");

inline constexpr std::size_t kOrderSlotBytes = 2048;

static_assert(1514 <= kOrderSlotBytes, "a full Ethernet frame must fit one transmit slot");

inline constexpr std::uint64_t kCoalesceNs = 300;

inline constexpr std::size_t kEthMtu = 1486;
inline constexpr std::size_t kIpUdpBytes = 20 + 8;
inline constexpr std::size_t kMoldHeaderBytes = 20;
inline constexpr std::size_t kMaxPacketPayload = kEthMtu - kIpUdpBytes - kMoldHeaderBytes;
inline constexpr std::size_t kMaxFrameBytes = 14 + kEthMtu;

static_assert(kMaxPacketPayload == 1438);
static_assert(kMaxFrameBytes == 1500);
static_assert(kMaxFrameBytes + kRxPrefixBytes <= kRxSlotBytes);

}
