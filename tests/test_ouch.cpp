#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "net/ouch.hpp"

namespace {

std::uint64_t be(const std::uint8_t* p, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

TEST(Ouch, the_fields_land_where_the_specification_puts_them) {
    std::vector<std::uint8_t> buf(ouch::kOrderPacketLen);
    ouch::prefill(buf.data());
    const std::uint8_t symbol[8] = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    ouch::fill(buf.data(), 12345, ouch::kBuy, 1, symbol, 1234500);
    ouch::set_cl_ord_id(buf.data(), 987);

    EXPECT_EQ(be(buf.data(), 2), 48u);
    EXPECT_EQ(buf[2], 'U');

    const std::uint8_t* m = buf.data() + ouch::kSoupHeaderLen;
    EXPECT_EQ(m[ouch::kTypeOff], 'O');
    EXPECT_EQ(be(m + ouch::kUserRefOff, 4), 12345u);
    EXPECT_EQ(m[ouch::kSideOff], 'B');
    EXPECT_EQ(be(m + ouch::kQuantityOff, 4), 1u);
    EXPECT_EQ(std::memcmp(m + ouch::kSymbolOff, symbol, 8), 0);
    EXPECT_EQ(be(m + ouch::kPriceOff, 8), 1234500u);
    EXPECT_EQ(m[ouch::kTimeInForceOff], '3');
    EXPECT_EQ(m[ouch::kDisplayOff], 'Y');
    EXPECT_EQ(m[ouch::kCapacityOff], 'P');
    EXPECT_EQ(m[ouch::kSweepOff], 'N');
    EXPECT_EQ(m[ouch::kCrossTypeOff], 'N');
    EXPECT_EQ(be(m + ouch::kAppendageLenOff, 2), 0u);
    EXPECT_EQ(ouch::kOrderPacketLen, 50u);
}

TEST(Ouch, the_client_order_id_is_right_justified_and_padded) {
    std::vector<std::uint8_t> buf(ouch::kOrderPacketLen);
    ouch::prefill(buf.data());
    ouch::set_cl_ord_id(buf.data(), 987);
    const std::uint8_t* id = buf.data() + ouch::kSoupHeaderLen + ouch::kClOrdIdOff;
    EXPECT_EQ(std::memcmp(id, "           987", 14), 0);

    ouch::set_cl_ord_id(buf.data(), 1304894064);
    EXPECT_EQ(std::memcmp(id, "    1304894064", 14), 0);
}

TEST(Ouch, a_price_off_the_book_needs_no_conversion) {
    std::vector<std::uint8_t> buf(ouch::kOrderPacketLen);
    ouch::prefill(buf.data());
    const std::uint8_t symbol[8] = {'S', 'P', 'C', 'X', ' ', ' ', ' ', ' '};
    ouch::fill(buf.data(), 1, ouch::kSell, 1, symbol, ouch::kHighestPrice);
    const std::uint8_t* m = buf.data() + ouch::kSoupHeaderLen;
    EXPECT_EQ(be(m + ouch::kPriceOff, 8), 1999999900u);
    EXPECT_EQ(ouch::kHighestPrice, 1999999900u);
    EXPECT_EQ(m[ouch::kSideOff], 'S');
}

TEST(Ouch, a_second_order_overwrites_only_what_changes) {
    std::vector<std::uint8_t> buf(ouch::kOrderPacketLen);
    ouch::prefill(buf.data());
    const std::uint8_t a[8] = {'A', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    const std::uint8_t b[8] = {'M', 'S', 'F', 'T', ' ', ' ', ' ', ' '};
    ouch::fill(buf.data(), 1, ouch::kBuy, 1, a, 100);
    ouch::fill(buf.data(), 2, ouch::kSell, 1, b, 200);

    const std::uint8_t* m = buf.data() + ouch::kSoupHeaderLen;
    EXPECT_EQ(be(m + ouch::kUserRefOff, 4), 2u);
    EXPECT_EQ(m[ouch::kSideOff], 'S');
    EXPECT_EQ(std::memcmp(m + ouch::kSymbolOff, b, 8), 0);
    EXPECT_EQ(be(m + ouch::kPriceOff, 8), 200u);
    EXPECT_EQ(m[ouch::kTimeInForceOff], '3');
    EXPECT_EQ(m[ouch::kCapacityOff], 'P');
    EXPECT_EQ(be(buf.data(), 2), 48u);
}

TEST(Ouch, the_login_says_how_long_it_is) {
    std::vector<std::uint8_t> buf(ouch::kLoginLen);
    ouch::login(buf.data(), "user", "secret");
    EXPECT_EQ(be(buf.data(), 2), 47u);
    EXPECT_EQ(buf[2], 'L');
    EXPECT_EQ(std::memcmp(buf.data() + 3, "user  ", 6), 0);
    EXPECT_EQ(std::memcmp(buf.data() + 9, "secret    ", 10), 0);
    EXPECT_EQ(std::memcmp(buf.data() + 19, "          ", 10), 0);
    EXPECT_EQ(buf[38], '1');
}

}
