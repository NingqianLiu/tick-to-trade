#pragma once

#include <cstdint>
#include <cstring>

namespace ouch {

inline constexpr std::size_t kEnterOrderLen = 47;
inline constexpr std::size_t kSoupHeaderLen = 3;
inline constexpr std::size_t kOrderPacketLen = kSoupHeaderLen + kEnterOrderLen;
inline constexpr std::size_t kSymbolLen = 8;
inline constexpr std::size_t kClOrdIdLen = 14;
inline constexpr std::uint64_t kHighestPrice = 0x7735939Cull;

inline constexpr std::size_t kTypeOff = 0;
inline constexpr std::size_t kUserRefOff = 1;
inline constexpr std::size_t kSideOff = 5;
inline constexpr std::size_t kQuantityOff = 6;
inline constexpr std::size_t kSymbolOff = 10;
inline constexpr std::size_t kPriceOff = 18;
inline constexpr std::size_t kTimeInForceOff = 26;
inline constexpr std::size_t kDisplayOff = 27;
inline constexpr std::size_t kCapacityOff = 28;
inline constexpr std::size_t kSweepOff = 29;
inline constexpr std::size_t kCrossTypeOff = 30;
inline constexpr std::size_t kClOrdIdOff = 31;
inline constexpr std::size_t kAppendageLenOff = 45;

inline constexpr char kBuy = 'B';
inline constexpr char kSell = 'S';
inline constexpr char kImmediateOrCancel = '3';
inline constexpr char kVisible = 'Y';
inline constexpr char kPrincipal = 'P';
inline constexpr char kNotSweepEligible = 'N';
inline constexpr char kContinuousMarket = 'N';

template <typename T>
void put_be(std::uint8_t* p, T v) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * (sizeof(T) - 1 - i)));
    }
}

inline void prefill(std::uint8_t* p) {
    std::memset(p, ' ', kOrderPacketLen);
    put_be<std::uint16_t>(p, static_cast<std::uint16_t>(1 + kEnterOrderLen));
    p[2] = 'U';
    std::uint8_t* m = p + kSoupHeaderLen;
    m[kTypeOff] = 'O';
    m[kTimeInForceOff] = kImmediateOrCancel;
    m[kDisplayOff] = kVisible;
    m[kCapacityOff] = kPrincipal;
    m[kSweepOff] = kNotSweepEligible;
    m[kCrossTypeOff] = kContinuousMarket;
    put_be<std::uint16_t>(m + kAppendageLenOff, 0);
}

inline void fill(std::uint8_t* p, std::uint32_t user_ref, char side,
                 std::uint32_t quantity, const std::uint8_t* symbol,
                 std::uint64_t price) {
    std::uint8_t* m = p + kSoupHeaderLen;
    put_be<std::uint32_t>(m + kUserRefOff, user_ref);
    m[kSideOff] = side;
    put_be<std::uint32_t>(m + kQuantityOff, quantity);
    std::memcpy(m + kSymbolOff, symbol, kSymbolLen);
    put_be<std::uint64_t>(m + kPriceOff, price);
}

inline void set_cl_ord_id(std::uint8_t* p, std::uint64_t n) {
    std::uint8_t* d = p + kSoupHeaderLen + kClOrdIdOff;
    std::memset(d, ' ', kClOrdIdLen);
    for (std::size_t i = kClOrdIdLen; i-- > 0 && n != 0; n /= 10) {
        d[i] = static_cast<std::uint8_t>('0' + n % 10);
    }
}

inline constexpr std::size_t kLoginLen = 49;
inline void login(std::uint8_t* p, const char* user, const char* password) {
    std::memset(p, ' ', kLoginLen);
    put_be<std::uint16_t>(p, static_cast<std::uint16_t>(kLoginLen - 2));
    p[2] = 'L';
    std::memcpy(p + 3, user, std::strlen(user) < 6 ? std::strlen(user) : 6);
    std::memcpy(p + 9, password, std::strlen(password) < 10 ? std::strlen(password) : 10);
    p[38] = '1';
}

}
