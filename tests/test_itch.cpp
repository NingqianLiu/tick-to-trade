#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "itch/framing.hpp"
#include "itch/reader.hpp"
#include "itch/types.hpp"

namespace {

void append(std::vector<std::uint8_t>& out, char type, std::uint16_t locate,
            std::uint64_t ts, std::uint16_t len) {
    out.push_back(static_cast<std::uint8_t>(len >> 8));
    out.push_back(static_cast<std::uint8_t>(len));
    const std::size_t start = out.size();
    out.resize(start + len, 0);
    out[start + itch::kTypeOff] = static_cast<std::uint8_t>(type);
    out[start + itch::kLocateOff] = static_cast<std::uint8_t>(locate >> 8);
    out[start + itch::kLocateOff + 1] = static_cast<std::uint8_t>(locate);
    for (int i = 0; i < 6; ++i) {
        out[start + itch::kTimestampOff + i] =
            static_cast<std::uint8_t>(ts >> (8 * (5 - i)));
    }
}

TEST(Itch, readers) {
    const std::uint8_t buf[] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    EXPECT_EQ(itch::read_be<std::uint16_t>(buf), 0x1234);
    EXPECT_EQ(itch::read_be<std::uint32_t>(buf), 0x12345678u);
    EXPECT_EQ(itch::read_be<std::uint64_t>(buf), 0x123456789abcdef0ull);
    EXPECT_EQ(itch::read_be48(buf), 0x123456789abcull);

    const std::uint64_t open_ns = 34200ull * 1000000000ull;
    std::uint8_t ts[6];
    for (int i = 0; i < 6; ++i) {
        ts[i] = static_cast<std::uint8_t>(open_ns >> (8 * (5 - i)));
    }
    EXPECT_EQ(itch::read_be48(ts), open_ns);

    const std::uint8_t holes[6] = {0xaa, 0x00, 0xbb, 0x00, 0xcc, 0x00};
    EXPECT_EQ(itch::read_be48(holes), 0xaa00bb00cc00ull);
}

TEST(Itch, body_len_table) {
    EXPECT_EQ(itch::kBodyLen['S'], 12);
    EXPECT_EQ(itch::kBodyLen['R'], 39);
    EXPECT_EQ(itch::kBodyLen['A'], 36);
    EXPECT_EQ(itch::kBodyLen['D'], 19);
    EXPECT_EQ(itch::kBodyLen['P'], 44);
    EXPECT_EQ(itch::kBodyLen['z'], 0);
}

TEST(Itch, framing) {
    std::vector<std::uint8_t> buf;
    append(buf, 'S', 0, 10892008976642ull, 12);
    append(buf, 'R', 7, 11097195265907ull, 39);
    append(buf, 'A', 4242, 34200000000000ull, 36);

    std::vector<char> types;
    std::vector<std::uint64_t> stamps;
    const auto r = itch::for_each_message(
        buf.data(), buf.size(), [&](const itch::Message& m) {
            types.push_back(m.type());
            stamps.push_back(m.timestamp());
            EXPECT_EQ(m.len, itch::kBodyLen[static_cast<unsigned char>(m.type())]);
            return true;
        });

    EXPECT_EQ(r.messages, 3);
    EXPECT_EQ(r.consumed, buf.size());
    EXPECT_EQ(r.stop, itch::FrameStop::kEndOfBuffer);
    EXPECT_TRUE(types.size() == 3 && types[0] == 'S' && types[2] == 'A');
    EXPECT_EQ(stamps[0], 10892008976642ull);
    EXPECT_EQ(stamps[2], 34200000000000ull);
}

TEST(Itch, partial_tail) {
    std::vector<std::uint8_t> buf;
    append(buf, 'S', 0, 1, 12);
    append(buf, 'A', 1, 2, 36);
    const std::size_t whole = 2 + 12;

    for (std::size_t cut : {whole + 1, whole + 2, whole + 20}) {
        const auto r = itch::for_each_message(buf.data(), cut,
                                              [](const itch::Message&) { return true; });
        EXPECT_EQ(r.messages, 1);
        EXPECT_EQ(r.consumed, whole);
        EXPECT_EQ(r.stop, itch::FrameStop::kPartialTail);
    }

    const auto r = itch::for_each_message(buf.data(), whole,
                                          [](const itch::Message&) { return true; });
    EXPECT_EQ(r.consumed, whole);
    EXPECT_EQ(r.stop, itch::FrameStop::kEndOfBuffer);
}

TEST(Itch, stop_and_zero_length) {
    std::vector<std::uint8_t> buf;
    append(buf, 'S', 0, 1, 12);
    append(buf, 'A', 1, 2, 36);

    const auto r = itch::for_each_message(buf.data(), buf.size(),
                                          [](const itch::Message&) { return false; });
    EXPECT_EQ(r.messages, 1);
    EXPECT_EQ(r.consumed, 2 + 12);
    EXPECT_EQ(r.stop, itch::FrameStop::kCallerStopped);

    const std::uint8_t zero[] = {0x00, 0x00, 0x41};
    const auto z = itch::for_each_message(zero, sizeof(zero),
                                          [](const itch::Message&) { return true; });
    EXPECT_EQ(z.messages, 0);
    EXPECT_EQ(z.stop, itch::FrameStop::kZeroLength);
}

}
