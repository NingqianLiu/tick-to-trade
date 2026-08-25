#pragma once

// The card's own counters, read the way ethtool reads them.
//
// There are two checks for whether a round lost packets, and this is the second.
// The first is the MoldUDP64 sequence number, which sees that a message never arrived at all.
// This one answers why it never arrived.
//
// The one really worth watching is port_rx_nodesc_drops.
// It counts packets the card received with nowhere to put them, because the host did not return
// free buffers in time.
// On this link that is the only way a packet can be lost:
// in between is a direct cable with no switch, and the sender cannot outrun the wire itself.
// In other words, this number moving means we did not keep up; it is nobody else's problem.
//
// port_rx_dp_di_dropped_packets is deliberately left out.
// It counts packets that matched no filter, which is background traffic.
// An idle port already has a thousand of them, and including it would mark every round dirty.
//
// This is one of the numbers checked at the end of every round.
// A clean round is: gaps 0, lapped 0, orphan 0, discards 0, and the card's own drop counters
// unmoved.
// With any one of them missing, that round's percentiles cannot be compared against anything.

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

// A lost packet shows up in these four counters.
// They are read once before a round and once after. Any one of them moving makes the round
// unusable.
inline constexpr const char* kDropCounters[] = {
    // Received with no free buffer to put it in. The only one that really happens on this path.
    "port_rx_nodesc_drops",
    // The card's internal receive buffer overflowed.
    "port_rx_overflow",
    // The packet was longer than the buffer and was truncated.
    "rx_nodesc_trunc",
    // No skb could be had on the kernel's path. We bypass the kernel, so it should always be
    // zero.
    "rx_noskb_drops",
};

// One snapshot of the four counters.
struct Drops {
    // In the same order as the table of names above.
    std::uint64_t v[sizeof(kDropCounters) / sizeof(kDropCounters[0])] = {};

    // Whether two snapshots are the same.
    // The test is exact equality rather than "did it grow" - counters that somebody cleared do
    // not count as clean either.
    [[nodiscard]] bool operator==(const Drops& o) const noexcept {
        for (std::size_t i = 0; i < sizeof(v) / sizeof(v[0]); ++i) {
            if (v[i] != o.v[i]) return false;
        }
        return true;
    }
};

// This is the body of the file: read those four drop counters of a port.
//
// A false means one thing only: this card cannot be asked at all.
// That lets the caller tell "nothing was lost" apart from "nobody looked", and the second must
// not count as clean.
[[nodiscard]] inline bool read_drops(const char* intf, Drops* out) {
    // An ioctl needs a file descriptor to be sent on. Any kind of socket will do; it never
    // really communicates.
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    // This structure tells the kernel which port is being asked about, and doubles as the
    // envelope for the arguments.
    ifreq ifr{};
    // The port name goes in. snprintf rather than strcpy, so an over long name is truncated
    // rather than writing past the end.
    std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", intf);

    // Three ioctls follow, each more specific than the last:
    //   1 how many counters this card has
    //   2 what they are called
    //   3 what they hold now
    // All three structures end with an array of a length known only at run time, so each buffer
    // has to be sized as the structure plus the answer rather than declared on the stack.
    std::vector<std::uint8_t> info_buf(sizeof(ethtool_sset_info) + sizeof(std::uint32_t));
    auto* info = reinterpret_cast<ethtool_sset_info*>(info_buf.data());
    // First: how many entries there are in the statistics set.
    info->cmd = ETHTOOL_GSSET_INFO;
    // A bit mask says which set is being asked about, here the statistics counters.
    info->sset_mask = 1ull << ETH_SS_STATS;
    // The buffer goes into the envelope.
    ifr.ifr_data = reinterpret_cast<char*>(info);
    // Sent. Besides the return value the mask has to be checked - the kernel says "I do not
    // support this set" by clearing the corresponding bit.
    if (::ioctl(fd, SIOCETHTOOL, &ifr) != 0 || info->sset_mask == 0) {
        ::close(fd);
        return false;
    }
    // How many counters there are. On this card it is several hundred.
    const std::uint32_t n = info->data[0];

    // Second: fetch all several hundred names. Each name takes a fixed length.
    std::vector<std::uint8_t> raw(sizeof(ethtool_gstrings) + n * ETH_GSTRING_LEN);
    auto* names = reinterpret_cast<ethtool_gstrings*>(raw.data());
    names->cmd = ETHTOOL_GSTRINGS;
    // Again saying it is the statistics set that is wanted.
    names->string_set = ETH_SS_STATS;
    // Telling the kernel how many we are ready to receive.
    names->len = n;
    ifr.ifr_data = reinterpret_cast<char*>(names);
    if (::ioctl(fd, SIOCETHTOOL, &ifr) != 0) {
        ::close(fd);
        return false;
    }

    // Third: fetch all several hundred values.
    // The names and the values are two separate calls lined up by index - name i goes with value
    // i.
    std::vector<std::uint8_t> vals(sizeof(ethtool_stats) + n * sizeof(std::uint64_t));
    auto* stats = reinterpret_cast<ethtool_stats*>(vals.data());
    stats->cmd = ETHTOOL_GSTATS;
    stats->n_stats = n;
    ifr.ifr_data = reinterpret_cast<char*>(stats);
    if (::ioctl(fd, SIOCETHTOOL, &ifr) != 0) {
        ::close(fd);
        return false;
    }
    // All three questions are answered and the descriptor can be closed. What follows uses only
    // the two buffers already in hand.
    ::close(fd);

    // Pick our four out of the several hundred.
    for (std::uint32_t i = 0; i < n; ++i) {
        // Name i. The names are laid out at a fixed length, so it is found by index times
        // length.
        const char* name =
            reinterpret_cast<const char*>(names->data) + i * ETH_GSTRING_LEN;
        // Compared against each of the four.
        for (std::size_t k = 0; k < sizeof(kDropCounters) / sizeof(kDropCounters[0]); ++k) {
            // strncmp with a limit, because that name slot is not guaranteed to end with a zero.
            if (std::strncmp(name, kDropCounters[k], ETH_GSTRING_LEN) == 0) {
                // A match copies the value into the matching place of the snapshot.
                out->v[k] = stats->data[i];
            }
        }
    }
    // A true here only means the card could be asked, not that nothing was lost.
    // Whether it was clean is decided by comparing two snapshots, below.
    return true;
}

// Prints whichever counters moved and answers whether this round was clean.
// A true means not one of them moved.
[[nodiscard]] inline bool report_drops(const Drops& before, const Drops& after) {
    bool clean = true;
    for (std::size_t k = 0; k < sizeof(kDropCounters) / sizeof(kDropCounters[0]); ++k) {
        // Only the ones that moved are printed, so that every round does not carry four useless
        // lines.
        if (before.v[k] != after.v[k]) {
            // Both values are printed. Saying only that it moved would leave how much to be
            // looked up.
            std::printf("               %s %llu -> %llu\n", kDropCounters[k],
                        static_cast<unsigned long long>(before.v[k]),
                        static_cast<unsigned long long>(after.v[k]));
            clean = false;
        }
    }
    // The caller uses it to decide whether this round's numbers can be used.
    return clean;
}

}  // namespace nic
