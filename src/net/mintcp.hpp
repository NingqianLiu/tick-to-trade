#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "net/eth.hpp"

namespace mintcp {

inline constexpr std::size_t kEthLen = 14;
inline constexpr std::size_t kIpLen = 20;
inline constexpr std::size_t kTcpLen = 20;
inline constexpr std::size_t kHeaderLen = kEthLen + kIpLen + kTcpLen;
inline constexpr std::size_t kSynOptLen = 4;

inline constexpr std::size_t kIpTotalLenOff = kEthLen + 2;
inline constexpr std::size_t kIpSumOff = kEthLen + 10;
inline constexpr std::size_t kTcpSeqOff = kEthLen + kIpLen + 4;
inline constexpr std::size_t kTcpAckOff = kEthLen + kIpLen + 8;
inline constexpr std::size_t kTcpFlagsOff = kEthLen + kIpLen + 13;
inline constexpr std::size_t kTcpWinOff = kEthLen + kIpLen + 14;
inline constexpr std::size_t kTcpSumOff = kEthLen + kIpLen + 16;

inline constexpr std::uint8_t kFin = 0x01;
inline constexpr std::uint8_t kSyn = 0x02;
inline constexpr std::uint8_t kRst = 0x04;
inline constexpr std::uint8_t kPsh = 0x08;
inline constexpr std::uint8_t kAck = 0x10;

inline void put16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

inline void put32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

[[nodiscard]] inline std::uint16_t get16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((std::uint32_t{p[0]} << 8) | p[1]);
}

[[nodiscard]] inline std::uint32_t get32(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | p[3];
}

[[nodiscard]] inline std::uint32_t sum16(const std::uint8_t* p,
                                         std::size_t n) noexcept {
    std::uint32_t s = 0;
    std::size_t i = 0;
    for (; i + 1 < n; i += 2) s += (std::uint32_t{p[i]} << 8) | p[i + 1];
    if (i < n) s += std::uint32_t{p[i]} << 8;
    return s;
}

[[nodiscard]] inline std::uint16_t fold(std::uint32_t s) noexcept {
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return static_cast<std::uint16_t>(~s);
}

[[nodiscard]] inline bool before(std::uint32_t a, std::uint32_t b) noexcept {
    return static_cast<std::int32_t>(a - b) < 0;
}

[[nodiscard]] inline bool before_eq(std::uint32_t a, std::uint32_t b) noexcept {
    return static_cast<std::int32_t>(a - b) <= 0;
}

class Conn {
public:
    Conn() = default;
    Conn(Conn&& o) noexcept { *this = std::move(o); }
    Conn& operator=(Conn&& o) noexcept {
        us_ = o.us_;
        them_ = o.them_;
        std::memcpy(peer_mac_, o.peer_mac_, eth::kMacBytes);
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

    void open(const eth::Endpoint& us, const eth::Endpoint& them,
              const std::uint8_t peer_mac[eth::kMacBytes],
              std::uint32_t initial_seq, std::uint16_t window) noexcept {
        us_ = us;
        them_ = them;
        std::memcpy(peer_mac_, peer_mac, eth::kMacBytes);
        snd_una_.store(initial_seq, std::memory_order_relaxed);
        snd_nxt_.store(initial_seq, std::memory_order_relaxed);
        rcv_nxt_ = 0;
        window_ = window;
        peer_wnd_.store(0, std::memory_order_relaxed);
        peer_shift_ = 0;
        std::memset(hdr_, 0, sizeof(hdr_));

        std::memcpy(hdr_ + 0, peer_mac_, eth::kMacBytes);
        std::memcpy(hdr_ + 6, us_.mac, eth::kMacBytes);
        put16(hdr_ + 12, 0x0800);

        hdr_[kEthLen + 0] = 0x45;
        hdr_[kEthLen + 1] = 0;
        put16(hdr_ + kEthLen + 4, 0);
        put16(hdr_ + kEthLen + 6, 0x4000);
        hdr_[kEthLen + 8] = 64;
        hdr_[kEthLen + 9] = 6;
        put32(hdr_ + kEthLen + 12, us_.ip);
        put32(hdr_ + kEthLen + 16, them_.ip);

        put16(hdr_ + kEthLen + kIpLen + 0, us_.port);
        put16(hdr_ + kEthLen + kIpLen + 2, them_.port);
        hdr_[kEthLen + kIpLen + 12] = 0x50;
        put16(hdr_ + kTcpWinOff, window_);

        ip_base_ = sum16(hdr_ + kEthLen, kIpLen) -
                   ((std::uint32_t{hdr_[kIpTotalLenOff]} << 8) | hdr_[kIpTotalLenOff + 1]);
        tcp_base_ = (us_.ip >> 16) + (us_.ip & 0xffff) + (them_.ip >> 16) +
                    (them_.ip & 0xffff) + 6u + us_.port + them_.port +
                    (std::uint32_t{0x50} << 8) + window_;
    }

    [[nodiscard]] std::size_t build(std::uint8_t* frame, const std::uint8_t* body,
                                    std::size_t body_len, std::uint8_t flags,
                                    std::uint32_t seq, std::uint32_t ack) const noexcept {
        std::memcpy(frame, hdr_, kHeaderLen);
        if (body_len != 0 && body != nullptr) {
            std::memcpy(frame + kHeaderLen, body, body_len);
        }
        const std::uint16_t ip_total =
            static_cast<std::uint16_t>(kIpLen + kTcpLen + body_len);
        put16(frame + kIpTotalLenOff, ip_total);
        put16(frame + kIpSumOff, fold(ip_base_ + ip_total));
        put32(frame + kTcpSeqOff, seq);
        put32(frame + kTcpAckOff, ack);
        frame[kTcpFlagsOff] = flags;
        const std::uint32_t tcp_len = static_cast<std::uint32_t>(kTcpLen + body_len);
        std::uint32_t s = tcp_base_ + tcp_len + (seq >> 16) + (seq & 0xffff) +
                          (ack >> 16) + (ack & 0xffff) + flags;
        if (body_len != 0 && body != nullptr) s += sum16(body, body_len);
        put16(frame + kTcpSumOff, fold(s));
        return kHeaderLen + body_len;
    }

    [[nodiscard]] std::size_t send(std::uint8_t* frame, const std::uint8_t* body,
                                   std::size_t body_len, std::uint8_t flags) noexcept {
        std::uint32_t nxt = snd_nxt_.load(std::memory_order_relaxed);
        const std::size_t n = build(frame, body, body_len, flags, nxt, rcv_nxt_);
        nxt += static_cast<std::uint32_t>(body_len);
        if ((flags & kSyn) != 0 || (flags & kFin) != 0) ++nxt;
        snd_nxt_.store(nxt, std::memory_order_relaxed);
        return n;
    }

    [[nodiscard]] std::size_t send_syn(std::uint8_t* frame, std::uint8_t shift) noexcept {
        std::memcpy(frame, hdr_, kHeaderLen);
        std::uint8_t* opt = frame + kHeaderLen;
        opt[0] = 1;
        opt[1] = 3;
        opt[2] = 3;
        opt[3] = shift;
        frame[kEthLen + kIpLen + 12] = 0x60;
        const std::uint16_t ip_total =
            static_cast<std::uint16_t>(kIpLen + kTcpLen + kSynOptLen);
        put16(frame + kIpTotalLenOff, ip_total);
        put16(frame + kIpSumOff, fold(ip_base_ + ip_total));
        const std::uint32_t seq = snd_nxt_.load(std::memory_order_relaxed);
        put32(frame + kTcpSeqOff, seq);
        put32(frame + kTcpAckOff, 0);
        frame[kTcpFlagsOff] = kSyn;
        const std::uint32_t tcp_len = kTcpLen + kSynOptLen;
        const std::uint32_t s = tcp_base_ + (std::uint32_t{0x10} << 8) + tcp_len +
                                (seq >> 16) + (seq & 0xffff) + kSyn + sum16(opt, kSynOptLen);
        put16(frame + kTcpSumOff, fold(s));
        snd_nxt_.store(seq + 1, std::memory_order_relaxed);
        return kHeaderLen + kSynOptLen;
    }

    [[nodiscard]] static std::uint8_t shift_in(const std::uint8_t* frame,
                                               std::size_t len) noexcept {
        if (len < kHeaderLen) return 0;
        const std::size_t hlen =
            static_cast<std::size_t>(frame[kEthLen + kIpLen + 12] >> 4) * 4;
        if (hlen <= kTcpLen || kEthLen + kIpLen + hlen > len) return 0;
        const std::uint8_t* p = frame + kEthLen + kIpLen + kTcpLen;
        const std::uint8_t* end = frame + kEthLen + kIpLen + hlen;
        while (p < end) {
            if (p[0] == 0) break;
            if (p[0] == 1) { ++p; continue; }
            if (p + 1 >= end || p[1] < 2) break;
            if (p[0] == 3 && p[1] == 3 && p + 2 < end) return p[2];
            p += p[1];
        }
        return 0;
    }

    void set_peer_shift(std::uint8_t shift) noexcept { peer_shift_ = shift; }
    [[nodiscard]] std::uint8_t peer_shift() const noexcept { return peer_shift_; }

    bool on_ack(std::uint32_t ack, std::uint16_t peer_window) noexcept {
        peer_wnd_.store(static_cast<std::uint32_t>(peer_window) << peer_shift_,
                        std::memory_order_relaxed);
        if (before(snd_nxt_.load(std::memory_order_relaxed), ack)) return false;
        const std::uint32_t una = snd_una_.load(std::memory_order_relaxed);
        if (before_eq(ack, una)) return false;
        snd_una_.store(ack, std::memory_order_relaxed);
        return true;
    }

    bool on_data(std::uint32_t seq, std::size_t len) noexcept {
        if (seq != rcv_nxt_) return false;
        rcv_nxt_ += static_cast<std::uint32_t>(len);
        return true;
    }

    void set_rcv_nxt(std::uint32_t v) noexcept { rcv_nxt_ = v; }
    [[nodiscard]] std::uint32_t snd_una() const noexcept {
        return snd_una_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t snd_nxt() const noexcept {
        return snd_nxt_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t rcv_nxt() const noexcept { return rcv_nxt_; }
    [[nodiscard]] std::uint32_t peer_wnd() const noexcept {
        return peer_wnd_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t in_flight() const noexcept {
        return snd_nxt() - snd_una();
    }
    [[nodiscard]] const std::uint8_t* header() const noexcept { return hdr_; }

private:
    eth::Endpoint us_{};
    eth::Endpoint them_{};
    std::uint8_t peer_mac_[eth::kMacBytes]{};
    std::uint8_t hdr_[kHeaderLen]{};
    std::uint32_t ip_base_ = 0;
    std::uint32_t tcp_base_ = 0;
    std::uint32_t rcv_nxt_ = 0;
    std::uint16_t window_ = 0;
    std::uint8_t peer_shift_ = 0;

    alignas(64) std::atomic<std::uint32_t> snd_nxt_{0};
    alignas(64) std::atomic<std::uint32_t> snd_una_{0};
    std::atomic<std::uint32_t> peer_wnd_{0};
};

}
