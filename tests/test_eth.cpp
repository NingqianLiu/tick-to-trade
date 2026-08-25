#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "net/eth.hpp"

namespace {

const eth::Endpoint kSrc{{0x00, 0x0f, 0x53, 0x42, 0xfd, 0x50},
                         eth::ipv4(10, 0, 0, 1), 40000};
const eth::Endpoint kDst{{0x01, 0x00, 0x5e, 0x36, 0x0c, 0x6f},
                         eth::ipv4(233, 54, 12, 111), 26477};

TEST(Eth, the_checksum_matches_the_published_example) {
    const std::uint8_t header[20] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40,
                                     0x00, 0x40, 0x11, 0x00, 0x00, 0xc0, 0xa8,
                                     0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7};
    EXPECT_EQ(eth::checksum(header, sizeof(header)), 0xb861);
}

TEST(Eth, a_written_header_checks_out_as_zero) {
    std::uint8_t frame[eth::kHeaderBytes] = {};
    for (std::size_t payload : {0u, 1u, 34u, 511u, 1452u}) {
        eth::write(frame, kSrc, kDst, payload);
        EXPECT_EQ(eth::checksum(frame + eth::kEthernetBytes, eth::kIpBytes), 0)
            << "payload " << payload;
    }
}

TEST(Eth, the_lengths_count_what_follows_each_header) {
    std::uint8_t frame[eth::kHeaderBytes] = {};
    eth::write(frame, kSrc, kDst, 1452);
    const std::uint8_t* ip = frame + eth::kEthernetBytes;
    const std::uint8_t* udp = ip + eth::kIpBytes;
    EXPECT_EQ((ip[2] << 8) | ip[3], 20 + 8 + 1452);
    EXPECT_EQ((udp[4] << 8) | udp[5], 8 + 1452);
    EXPECT_EQ(((ip[2] << 8) | ip[3]), 1480);
}

TEST(Eth, the_fields_land_where_the_wire_format_puts_them) {
    std::uint8_t frame[eth::kHeaderBytes] = {};
    eth::write(frame, kSrc, kDst, 100);
    EXPECT_EQ(std::memcmp(frame, kDst.mac, eth::kMacBytes), 0);
    EXPECT_EQ(std::memcmp(frame + 6, kSrc.mac, eth::kMacBytes), 0);
    EXPECT_EQ((frame[12] << 8) | frame[13], 0x0800);

    const std::uint8_t* ip = frame + eth::kEthernetBytes;
    EXPECT_EQ(ip[0], 0x45);
    EXPECT_EQ(ip[6] & 0x40, 0x40);
    EXPECT_EQ(ip[9], 17);
    EXPECT_EQ(ip[12], 10);
    EXPECT_EQ(ip[19], 111);

    const std::uint8_t* udp = ip + eth::kIpBytes;
    EXPECT_EQ((udp[0] << 8) | udp[1], 40000);
    EXPECT_EQ((udp[2] << 8) | udp[3], 26477);
    EXPECT_EQ((udp[6] << 8) | udp[7], 0);
}

TEST(Eth, a_multicast_group_maps_onto_its_mac) {
    std::uint8_t mac[eth::kMacBytes] = {};
    eth::multicast_mac(eth::ipv4(233, 54, 12, 111), mac);
    const std::uint8_t want[] = {0x01, 0x00, 0x5e, 0x36, 0x0c, 0x6f};
    EXPECT_EQ(std::memcmp(mac, want, sizeof(want)), 0);

    std::uint8_t other[eth::kMacBytes] = {};
    eth::multicast_mac(eth::ipv4(233, 182, 12, 111), other);
    EXPECT_EQ(std::memcmp(mac, other, sizeof(mac)), 0);
}

TEST(Eth, the_two_feeds_differ_only_in_the_destination) {
    std::uint8_t a[eth::kHeaderBytes] = {};
    std::uint8_t b[eth::kHeaderBytes] = {};
    eth::Endpoint dst_b = kDst;
    dst_b.port = kDst.port + 1;
    eth::write(a, kSrc, kDst, 200);
    eth::write(b, kSrc, dst_b, 200);
    EXPECT_NE(std::memcmp(a, b, sizeof(a)), 0);
    EXPECT_EQ(std::memcmp(a, b, eth::kEthernetBytes + eth::kIpBytes + 2), 0);
}

}
