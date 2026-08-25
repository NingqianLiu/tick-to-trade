#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "itch/framing.hpp"
#include "net/mold.hpp"

namespace {

const char kSession[] = "ITCHBENCH0";

std::vector<std::uint8_t> record(char type, std::uint64_t order_ref) {
    const std::uint16_t len = itch::kBodyLen[static_cast<unsigned char>(type)];
    std::vector<std::uint8_t> r(itch::kLenPrefix + len, 0);
    r[0] = static_cast<std::uint8_t>(len >> 8);
    r[1] = static_cast<std::uint8_t>(len);
    r[itch::kLenPrefix + itch::kTypeOff] = static_cast<std::uint8_t>(type);
    for (int i = 0; i < 8; ++i) {
        r[itch::kLenPrefix + itch::kAddRefOff + i] =
            static_cast<std::uint8_t>(order_ref >> (8 * (7 - i)));
    }
    return r;
}

TEST(Mold, header_roundtrip) {
    std::uint8_t p[mold::kHeaderLen];
    mold::write_header(p, kSession, 0x0123456789abcdefull, 16);
    EXPECT_EQ(std::memcmp(p, kSession, mold::kSessionLen), 0);
    EXPECT_EQ(mold::sequence(p), 0x0123456789abcdefull);
    EXPECT_EQ(mold::count(p), 16);
    // Big endian on the wire, whatever the host does.
    EXPECT_TRUE(p[mold::kSeqOff] == 0x01 && p[mold::kSeqOff + 7] == 0xef);
    EXPECT_TRUE(p[mold::kCountOff] == 0x00 && p[mold::kCountOff + 1] == 0x10);
}

// A packet carries the sequence of its first message, so the stream is gapless
// only if the next packet starts at seq + count.
TEST(Mold, sequence_has_no_holes) {
    mold::Packer packer(kSession, 1, 4);
    std::uint64_t expect = 1;
    std::uint64_t messages = 0;
    for (int packet = 0; packet < 5; ++packet) {
        const std::uint16_t n = static_cast<std::uint16_t>(packet == 4 ? 3 : 4);
        for (std::uint16_t i = 0; i < n; ++i) {
            auto r = record('A', 1000 + messages);
            packer.add(r.data(), r.size());
            ++messages;
        }
        const auto bytes = packer.seal();
        EXPECT_EQ(mold::sequence(bytes.data()), expect);
        EXPECT_EQ(mold::count(bytes.data()), n);
        expect += n;
        packer.next();
        EXPECT_EQ(packer.seq(), expect);
    }
    EXPECT_EQ(expect, 1 + messages);
}

TEST(Mold, packer_fills_and_reparses) {
    mold::Packer packer(kSession, 7, 3);
    EXPECT_TRUE(packer.empty());
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(packer.full());
        auto r = record(i == 1 ? 'D' : 'A', 500 + i);
        packer.add(r.data(), r.size());
    }
    EXPECT_TRUE(packer.full());

    const auto bytes = packer.seal();
    std::vector<char> types;
    const auto res = itch::for_each_message(
        bytes.data() + mold::kHeaderLen, bytes.size() - mold::kHeaderLen,
        [&](const itch::Message& m) {
            types.push_back(m.type());
            return true;
        });
    EXPECT_EQ(res.messages, 3);
    EXPECT_EQ(res.consumed, bytes.size() - mold::kHeaderLen);
    EXPECT_TRUE(types.size() == 3 && types[0] == 'A' && types[1] == 'D');
}

// The headroom is where the Ethernet, IP and UDP headers go, so the MoldUDP64
// header has to start after it and the payload must not shift.
TEST(Mold, headroom_is_reserved_ahead_of_the_header) {
    const std::size_t headroom = 42;
    mold::Packer packer(kSession, 1, 2, headroom);
    auto r = record('A', 1);
    packer.add(r.data(), r.size());
    const auto bytes = packer.seal();
    EXPECT_EQ(bytes.size(), headroom + mold::kHeaderLen + r.size());
    EXPECT_EQ(mold::sequence(bytes.data() + headroom), 1);
    EXPECT_EQ(std::memcmp(bytes.data() + headroom + mold::kHeaderLen, r.data(),
                      r.size()), 0);
}

// Renumbering the transport sequence must leave the order reference inside the
// body alone.
TEST(Mold, order_reference_is_copied_through) {
    mold::Packer packer(kSession, 999, 2);
    auto r = record('A', 0xdeadbeefcafe1234ull);
    packer.add(r.data(), r.size());
    const auto bytes = packer.seal();
    const std::uint8_t* body = bytes.data() + mold::kHeaderLen + itch::kLenPrefix;
    EXPECT_EQ(itch::read_be<std::uint64_t>(body + itch::kAddRefOff), 0xdeadbeefcafe1234ull);
    EXPECT_EQ(mold::sequence(bytes.data()), 999);  // the two 8-byte fields differ
}

}  // namespace

