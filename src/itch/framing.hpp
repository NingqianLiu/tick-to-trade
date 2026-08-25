#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "itch/reader.hpp"
#include "itch/types.hpp"

namespace itch {

enum class FrameStop : std::uint8_t {
    kEndOfBuffer,
    kPartialTail,
    kZeroLength,
    kCallerStopped
};

struct FrameResult {
    std::size_t consumed;
    std::size_t messages;
    FrameStop stop;
};

template <typename Fn>
[[nodiscard]] inline FrameResult for_each_message(const std::uint8_t* data,
                                                  std::size_t size, Fn&& fn) {
    std::size_t off = 0;
    std::size_t count = 0;
    for (;;) {
        if (off + kLenPrefix > size) {
            return {off, count,
                    off == size ? FrameStop::kEndOfBuffer : FrameStop::kPartialTail};
        }
        const std::uint16_t len = read_be<std::uint16_t>(data + off);
        if (len == 0) return {off, count, FrameStop::kZeroLength};
        if (off + kLenPrefix + len > size) {
            return {off, count, FrameStop::kPartialTail};
        }
        const bool go_on = fn(Message{data + off + kLenPrefix, len});
        off += kLenPrefix + len;
        ++count;
        if (!go_on) return {off, count, FrameStop::kCallerStopped};
    }
}

}
