#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "itch/reader.hpp"

namespace mold {

inline constexpr std::size_t kSessionLen = 10;
inline constexpr std::size_t kSeqOff = 10;
inline constexpr std::size_t kCountOff = 18;
inline constexpr std::size_t kHeaderLen = 20;

inline void write_header(std::uint8_t* p, const char* session, std::uint64_t seq,
                         std::uint16_t count) noexcept {
    std::memcpy(p, session, kSessionLen);
    const std::uint64_t s = std::byteswap(seq);
    std::memcpy(p + kSeqOff, &s, sizeof(s));
    const std::uint16_t c = std::byteswap(count);
    std::memcpy(p + kCountOff, &c, sizeof(c));
}

[[nodiscard]] inline std::uint64_t sequence(const std::uint8_t* p) noexcept {
    return itch::read_be<std::uint64_t>(p + kSeqOff);
}

[[nodiscard]] inline std::uint16_t count(const std::uint8_t* p) noexcept {
    return itch::read_be<std::uint16_t>(p + kCountOff);
}

class Packer {
public:
    Packer(const char* session, std::uint64_t first_seq, std::uint16_t max_messages,
           std::size_t headroom = 0)
        : max_(max_messages), headroom_(headroom), seq_(first_seq) {
        std::memcpy(session_, session, kSessionLen);
        clear();
    }

    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] bool full() const noexcept { return count_ >= max_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return buf_.size(); }
    [[nodiscard]] std::uint16_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }

    void add(const std::uint8_t* record, std::size_t record_len) {
        buf_.insert(buf_.end(), record, record + record_len);
        ++count_;
    }

    [[nodiscard]] std::span<const std::uint8_t> seal() noexcept {
        write_header(buf_.data() + headroom_, session_, seq_, count_);
        return {buf_.data(), buf_.size()};
    }

    void next() noexcept {
        seq_ += count_;
        clear();
    }

private:
    void clear() {
        buf_.assign(headroom_ + kHeaderLen, 0);
        count_ = 0;
    }

    std::vector<std::uint8_t> buf_;
    char session_[kSessionLen];
    std::uint16_t max_;
    std::uint16_t count_ = 0;
    std::size_t headroom_;
    std::uint64_t seq_;
};

}
