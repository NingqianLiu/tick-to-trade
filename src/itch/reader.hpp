
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include "itch/types.hpp"

namespace itch {

static_assert(std::endian::native == std::endian::little,
              "the byte-swapping helpers assume a little-endian host");

template <typename T>
[[nodiscard]] inline T read_be(const std::uint8_t* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return std::byteswap(v);
}

[[nodiscard]] inline std::uint64_t read_be48(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, p, 6);
    return std::byteswap(v) >> 16;
}

struct Message {
    const std::uint8_t* body;
    std::uint16_t len;

    [[nodiscard]] char type() const noexcept {
        return static_cast<char>(body[kTypeOff]);
    }
    [[nodiscard]] std::uint16_t stock_locate() const noexcept {
        return read_be<std::uint16_t>(body + kLocateOff);
    }
    [[nodiscard]] std::uint64_t timestamp() const noexcept {
        return read_be48(body + kTimestampOff);
    }

    [[nodiscard]] char event_code() const noexcept {
        return static_cast<char>(body[kHeaderLen]);
    }
};

}
