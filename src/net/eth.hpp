#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace eth {

inline constexpr std::size_t kEthernetBytes = 14;
inline constexpr std::size_t kIpBytes = 20;
inline constexpr std::size_t kUdpBytes = 8;
inline constexpr std::size_t kHeaderBytes = kEthernetBytes + kIpBytes + kUdpBytes;
inline constexpr std::size_t kMacBytes = 6;

struct Endpoint {
    std::uint8_t mac[kMacBytes];
    std::uint32_t ip;
    std::uint16_t port;
};

[[nodiscard]] inline constexpr std::uint32_t ipv4(std::uint8_t a, std::uint8_t b,
                                                  std::uint8_t c,
                                                  std::uint8_t d) noexcept {
    return (std::uint32_t{a} << 24) | (std::uint32_t{b} << 16) |
           (std::uint32_t{c} << 8) | d;
}

[[nodiscard]] inline std::uint16_t checksum(const std::uint8_t* p,
                                            std::size_t n) noexcept {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        sum += (std::uint32_t{p[i]} << 8) | p[i + 1];
    }
    if (n % 2 != 0) sum += std::uint32_t{p[n - 1]} << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum);
}

inline void multicast_mac(std::uint32_t ip, std::uint8_t out[kMacBytes]) noexcept {
    out[0] = 0x01;
    out[1] = 0x00;
    out[2] = 0x5e;
    out[3] = static_cast<std::uint8_t>((ip >> 16) & 0x7f);
    out[4] = static_cast<std::uint8_t>((ip >> 8) & 0xff);
    out[5] = static_cast<std::uint8_t>(ip & 0xff);
}

namespace detail {

inline void put16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

inline void put32(std::uint8_t* p, std::uint32_t v) noexcept {
    put16(p, static_cast<std::uint16_t>(v >> 16));
    put16(p + 2, static_cast<std::uint16_t>(v));
}

}

inline void write(std::uint8_t* out, const Endpoint& src, const Endpoint& dst,
                  std::size_t payload) noexcept {
    std::memcpy(out, dst.mac, kMacBytes);
    std::memcpy(out + kMacBytes, src.mac, kMacBytes);
    detail::put16(out + 12, 0x0800);

    std::uint8_t* ip = out + kEthernetBytes;
    ip[0] = 0x45;
    ip[1] = 0;
    detail::put16(ip + 2, static_cast<std::uint16_t>(kIpBytes + kUdpBytes + payload));
    detail::put16(ip + 4, 0);
    detail::put16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = 17;
    detail::put16(ip + 10, 0);
    detail::put32(ip + 12, src.ip);
    detail::put32(ip + 16, dst.ip);
    detail::put16(ip + 10, checksum(ip, kIpBytes));

    std::uint8_t* udp = out + kEthernetBytes + kIpBytes;
    detail::put16(udp + 0, src.port);
    detail::put16(udp + 2, dst.port);
    detail::put16(udp + 4, static_cast<std::uint16_t>(kUdpBytes + payload));
    detail::put16(udp + 6, 0);
}

}
