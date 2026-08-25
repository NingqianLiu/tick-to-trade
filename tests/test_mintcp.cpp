#include "net/mintcp.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

using mintcp::Conn;

eth::Endpoint us() {
    eth::Endpoint e{};
    const std::uint8_t mac[6] = {0x00, 0x0f, 0x53, 0x11, 0x22, 0x33};
    std::memcpy(e.mac, mac, 6);
    e.ip = eth::ipv4(10, 9, 9, 2);
    e.port = 51000;
    return e;
}

eth::Endpoint them() {
    eth::Endpoint e{};
    const std::uint8_t mac[6] = {0x00, 0x0f, 0x53, 0x44, 0x55, 0x66};
    std::memcpy(e.mac, mac, 6);
    e.ip = eth::ipv4(10, 9, 9, 1);
    e.port = 46200;
    return e;
}

Conn opened(std::uint32_t seq = 1000) {
    Conn c;
    const std::uint8_t peer[6] = {0x00, 0x0f, 0x53, 0x44, 0x55, 0x66};
    c.open(us(), them(), peer, seq, 65535);
    return c;
}

std::uint32_t fold_of(std::uint32_t s) {
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return s;
}

std::uint32_t sum_of(const std::uint8_t* p, std::size_t n) {
    std::uint32_t s = mintcp::sum16(p, n);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return s;
}

TEST(MinTcp, the_header_is_the_right_shape) {
    Conn c = opened();
    std::vector<std::uint8_t> f(200, 0xee);
    const std::size_t n = c.build(f.data(), nullptr, 0, mintcp::kAck, 1000, 7);
    EXPECT_EQ(n, mintcp::kHeaderLen);
    EXPECT_EQ(mintcp::get16(f.data() + 12), 0x0800);
    EXPECT_EQ(f[14], 0x45);
    EXPECT_EQ(f[14 + 9], 6);
    EXPECT_EQ(mintcp::get32(f.data() + 14 + 12), eth::ipv4(10, 9, 9, 2));
    EXPECT_EQ(mintcp::get32(f.data() + 14 + 16), eth::ipv4(10, 9, 9, 1));
    EXPECT_EQ(mintcp::get16(f.data() + 34), 51000);
    EXPECT_EQ(mintcp::get16(f.data() + 36), 46200);
    EXPECT_EQ(mintcp::get16(f.data() + mintcp::kIpTotalLenOff), 40);
    EXPECT_EQ(f[mintcp::kTcpFlagsOff], mintcp::kAck);
    EXPECT_EQ(mintcp::get32(f.data() + mintcp::kTcpSeqOff), 1000u);
    EXPECT_EQ(mintcp::get32(f.data() + mintcp::kTcpAckOff), 7u);
}

TEST(MinTcp, the_ip_checksum_checks_out) {
    Conn c = opened();
    std::vector<std::uint8_t> f(200, 0);
    for (std::size_t len : {std::size_t{0}, std::size_t{1}, std::size_t{50},
                            std::size_t{100}}) {
        std::vector<std::uint8_t> body(len, 0xa5);
        (void)c.build(f.data(), body.data(), len, mintcp::kAck, 5, 6);
        EXPECT_EQ(sum_of(f.data() + mintcp::kEthLen, mintcp::kIpLen), 0xffffu)
            << "body length " << len;
    }
}

TEST(MinTcp, the_tcp_checksum_checks_out) {
    Conn c = opened();
    std::vector<std::uint8_t> f(400, 0);
    for (std::size_t len : {std::size_t{0}, std::size_t{1}, std::size_t{50},
                            std::size_t{150}, std::size_t{301}}) {
        std::vector<std::uint8_t> body(len);
        for (std::size_t i = 0; i < len; ++i) {
            body[i] = static_cast<std::uint8_t>(i * 7 + 3);
        }
        (void)c.build(f.data(), body.data(), len, mintcp::kAck | mintcp::kPsh,
                      0x11223344, 0x55667788);
        const std::uint32_t tcp_len =
            static_cast<std::uint32_t>(mintcp::kTcpLen + len);
        std::uint32_t s = (eth::ipv4(10, 9, 9, 2) >> 16) +
                          (eth::ipv4(10, 9, 9, 2) & 0xffff) +
                          (eth::ipv4(10, 9, 9, 1) >> 16) +
                          (eth::ipv4(10, 9, 9, 1) & 0xffff) + 6u + tcp_len;
        s += mintcp::sum16(f.data() + mintcp::kEthLen + mintcp::kIpLen, tcp_len);
        while (s >> 16) s = (s & 0xffff) + (s >> 16);
        EXPECT_EQ(s, 0xffffu) << "body length " << len;
    }
}

TEST(MinTcp, sending_advances_the_stream_by_what_went) {
    Conn c = opened(1000);
    std::vector<std::uint8_t> f(200, 0);
    std::vector<std::uint8_t> body(50, 1);
    EXPECT_EQ(c.snd_nxt(), 1000u);
    (void)c.send(f.data(), body.data(), 50, mintcp::kAck | mintcp::kPsh);
    EXPECT_EQ(c.snd_nxt(), 1050u);
    EXPECT_EQ(mintcp::get32(f.data() + mintcp::kTcpSeqOff), 1000u);
    (void)c.send(f.data(), body.data(), 50, mintcp::kAck | mintcp::kPsh);
    EXPECT_EQ(c.snd_nxt(), 1100u);
    EXPECT_EQ(mintcp::get32(f.data() + mintcp::kTcpSeqOff), 1050u);
}

TEST(MinTcp, a_syn_takes_a_sequence_number_of_its_own) {
    Conn c = opened(7000);
    std::vector<std::uint8_t> f(200, 0);
    (void)c.send(f.data(), nullptr, 0, mintcp::kSyn);
    EXPECT_EQ(c.snd_nxt(), 7001u);
    (void)c.send(f.data(), nullptr, 0, mintcp::kFin | mintcp::kAck);
    EXPECT_EQ(c.snd_nxt(), 7002u);
}

TEST(MinTcp, an_ack_only_counts_once_and_only_for_what_went) {
    Conn c = opened(1000);
    std::vector<std::uint8_t> f(200, 0);
    std::vector<std::uint8_t> body(50, 1);
    (void)c.send(f.data(), body.data(), 50, mintcp::kAck);
    (void)c.send(f.data(), body.data(), 50, mintcp::kAck);
    EXPECT_EQ(c.in_flight(), 100u);
    EXPECT_TRUE(c.on_ack(1050, 4000));
    EXPECT_EQ(c.snd_una(), 1050u);
    EXPECT_EQ(c.in_flight(), 50u);
    EXPECT_EQ(c.peer_wnd(), 4000);
    EXPECT_FALSE(c.on_ack(1050, 4000));
    EXPECT_FALSE(c.on_ack(1000, 4000));
    EXPECT_FALSE(c.on_ack(9999, 4000));
    EXPECT_EQ(c.snd_una(), 1050u);
}

TEST(MinTcp, the_sequence_space_wraps_without_going_backwards) {
    const std::uint32_t start = 0xffffffffu - 20;
    Conn c = opened(start);
    std::vector<std::uint8_t> f(200, 0);
    std::vector<std::uint8_t> body(50, 1);
    (void)c.send(f.data(), body.data(), 50, mintcp::kAck);
    EXPECT_EQ(c.snd_nxt(), start + 50);
    EXPECT_EQ(c.in_flight(), 50u);
    EXPECT_TRUE(c.on_ack(start + 50, 4000));
    EXPECT_EQ(c.in_flight(), 0u);
    EXPECT_TRUE(mintcp::before(0xfffffff0u, 0x00000010u));
    EXPECT_FALSE(mintcp::before(0x00000010u, 0xfffffff0u));
    EXPECT_TRUE(mintcp::before_eq(0x00000010u, 0x00000010u));
}

TEST(MinTcp, only_data_that_arrives_in_order_is_acknowledged) {
    Conn c = opened();
    c.set_rcv_nxt(500);
    EXPECT_TRUE(c.on_data(500, 10));
    EXPECT_EQ(c.rcv_nxt(), 510u);
    EXPECT_FALSE(c.on_data(520, 10));
    EXPECT_EQ(c.rcv_nxt(), 510u);
    EXPECT_TRUE(c.on_data(510, 10));
    EXPECT_EQ(c.rcv_nxt(), 520u);
}

TEST(MinTcp, the_payload_is_copied_in_whole) {
    Conn c = opened();
    std::vector<std::uint8_t> f(400, 0);
    std::vector<std::uint8_t> body(300);
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<std::uint8_t>(i);
    }
    const std::size_t n = c.build(f.data(), body.data(), body.size(),
                                  mintcp::kAck, 1, 2);
    EXPECT_EQ(n, mintcp::kHeaderLen + body.size());
    EXPECT_EQ(std::memcmp(f.data() + mintcp::kHeaderLen, body.data(), body.size()), 0);
}

}

TEST(MinTcp, the_handshake_packet_carries_the_window_scale_option) {
    Conn c = opened(1000);
    std::vector<std::uint8_t> f(200, 0xee);
    const std::size_t n = c.send_syn(f.data(), 0);
    EXPECT_EQ(n, mintcp::kHeaderLen + 4);
    EXPECT_EQ(f[34 + 12] >> 4, 6u);
    EXPECT_EQ(f[mintcp::kHeaderLen + 0], 1);
    EXPECT_EQ(f[mintcp::kHeaderLen + 1], 3);
    EXPECT_EQ(f[mintcp::kHeaderLen + 2], 3);
    EXPECT_EQ(f[mintcp::kHeaderLen + 3], 0);
    EXPECT_EQ(mintcp::get16(f.data() + 16), mintcp::kIpLen + mintcp::kTcpLen + 4);
    EXPECT_EQ(sum_of(f.data() + 14, mintcp::kIpLen), 0xffffu);
    const std::uint32_t pseudo =
        (eth::ipv4(10, 9, 9, 2) >> 16) + (eth::ipv4(10, 9, 9, 2) & 0xffff) +
        (eth::ipv4(10, 9, 9, 1) >> 16) + (eth::ipv4(10, 9, 9, 1) & 0xffff) + 6u +
        (mintcp::kTcpLen + 4);
    EXPECT_EQ(fold_of(pseudo + mintcp::sum16(f.data() + 34, mintcp::kTcpLen + 4)),
              0xffffu);
    EXPECT_EQ(c.snd_nxt(), 1001u);
}

TEST(MinTcp, the_peer_shift_is_read_back_out_of_their_answer) {
    std::vector<std::uint8_t> f(200, 0);
    f[34 + 12] = 0x50;
    EXPECT_EQ(Conn::shift_in(f.data(), mintcp::kHeaderLen), 0);
    f[34 + 12] = 0x60;
    f[mintcp::kHeaderLen + 0] = 1;
    f[mintcp::kHeaderLen + 1] = 3;
    f[mintcp::kHeaderLen + 2] = 3;
    f[mintcp::kHeaderLen + 3] = 7;
    EXPECT_EQ(Conn::shift_in(f.data(), mintcp::kHeaderLen + 4), 7);
    EXPECT_EQ(Conn::shift_in(f.data(), mintcp::kHeaderLen), 0);
}

TEST(MinTcp, the_window_they_report_is_shifted_by_what_they_asked_for) {
    Conn c = opened(1000);
    (void)c.on_ack(1000, 65535);
    EXPECT_EQ(c.peer_wnd(), 65535u);
    c.set_peer_shift(7);
    (void)c.on_ack(1000, 65535);
    EXPECT_EQ(c.peer_wnd(), 65535u * 128u);
}
