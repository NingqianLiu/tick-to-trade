#pragma once

#include <cstdint>

namespace pkt {

inline constexpr std::uint32_t kMagic = 0x31544b50;
inline constexpr std::uint32_t kHeadroom = 42;

struct FileHeader {
    std::uint32_t magic;
    std::uint32_t headroom;
    std::uint64_t packets;
    std::uint64_t messages;
    std::uint64_t first_seq;
    std::uint64_t body_bytes;
    std::uint64_t span_ns;
    char session[16];
};

struct Entry {
    std::uint64_t ts_ns;
    std::uint64_t offset;
    std::uint32_t len;
    std::uint32_t messages;
};

}
