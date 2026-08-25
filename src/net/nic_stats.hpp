#pragma once

#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nic {

inline constexpr const char* kDropCounters[] = {
    "port_rx_nodesc_drops",
    "port_rx_overflow",
    "rx_nodesc_trunc",
    "rx_noskb_drops",
};

struct Drops {
    std::uint64_t v[sizeof(kDropCounters) / sizeof(kDropCounters[0])] = {};

    [[nodiscard]] bool operator==(const Drops& o) const noexcept {
        for (std::size_t i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
            if (v[i] != o.v[i]) return false;
        }
        return true;
    }
};

[[nodiscard]] inline bool read_drops(const char* intf, Drops* out) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    ifreq ifr{};
    std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", intf);

    std::vector<std::uint8_t> info_buf(sizeof(ethtool_sset_info) + sizeof(std::uint32_t));
    auto* info = reinterpret_cast<ethtool_sset_info*>(info_buf.data());
    info->cmd = ETHTOOL_GSSET_INFO;
    info->sset_mask = 1ull << ETH_SS_STATS;
    ifr.ifr_data = reinterpret_cast<char*>(info);
    if (::ioctl(fd, SIOCETHTOOL, &ifr) != 0 || info->sset_mask == 0) {
        ::close(fd);
        return false;
    }
    const std::uint32_t n = info->data[0];

    std::vector<std::uint8_t> raw(sizeof(ethtool_gstrings) + n * ETH_GSTRING_LEN);
    auto* names = reinterpret_cast<ethtool_gstrings*>(raw.data());
    names->cmd = ETHTOOL_GSTRINGS;
    names->string_set = ETH_SS_STATS;
    names->len = n;
    ifr.ifr_data = reinterpret_cast<char*>(names);
    if (::ioctl(fd, SIOCETHTOOL, &ifr) != 0) {
        ::close(fd);
        return false;
    }

    std::vector<std::uint8_t> vals(sizeof(ethtool_stats) + n * sizeof(std::uint64_t));
    auto* stats = reinterpret_cast<ethtool_stats*>(vals.data());
    stats->cmd = ETHTOOL_GSTATS;
    stats->n_stats = n;
    ifr.ifr_data = reinterpret_cast<char*>(stats);
    if (::ioctl(fd, SIOCETHTOOL, &ifr) != 0) {
        ::close(fd);
        return false;
    }
    ::close(fd);

    for (std::uint32_t i = 0; i < n; ++i) {
        const char* name =
            reinterpret_cast<const char*>(names->data) + i * ETH_GSTRING_LEN;
        for (std::size_t k = 0; k < sizeof(kDropCounters) / sizeof(kDropCounters[0]); ++k) {
            if (std::strncmp(name, kDropCounters[k], ETH_GSTRING_LEN) == 0) {
                out->v[k] = stats->data[i];
            }
        }
    }
    return true;
}

[[nodiscard]] inline bool report_drops(const Drops& before, const Drops& after) {
    bool clean = true;
    for (std::size_t k = 0; k < sizeof(kDropCounters) / sizeof(kDropCounters[0]); ++k) {
        if (before.v[k] != after.v[k]) {
            std::printf("               %s %llu -> %llu\n", kDropCounters[k],
                        static_cast<unsigned long long>(before.v[k]),
                        static_cast<unsigned long long>(after.v[k]));
            clean = false;
        }
    }
    return clean;
}

}
