
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace itch {

inline constexpr std::size_t kLenPrefix = 2;

inline constexpr std::size_t kHeaderLen = 11;
inline constexpr std::size_t kTypeOff = 0;
inline constexpr std::size_t kLocateOff = 1;
inline constexpr std::size_t kTimestampOff = 5;

inline constexpr std::array<std::uint8_t, 256> kBodyLen = [] {
    std::array<std::uint8_t, 256> t{};
    t['S'] = 12;
    t['R'] = 39;
    t['H'] = 25;
    t['Y'] = 20;
    t['L'] = 26;
    t['V'] = 35;
    t['W'] = 12;
    t['K'] = 28;
    t['J'] = 35;
    t['h'] = 21;
    t['A'] = 36;
    t['F'] = 40;
    t['E'] = 31;
    t['C'] = 36;
    t['X'] = 23;
    t['D'] = 19;
    t['U'] = 35;
    t['P'] = 44;
    t['Q'] = 40;
    t['B'] = 19;
    t['I'] = 50;
    t['N'] = 20;
    t['O'] = 48;
    return t;
}();

inline constexpr char kEventStartOfSystemHours = 'S';
inline constexpr char kEventStartOfMarketHours = 'Q';
inline constexpr char kEventEndOfMarketHours = 'M';

inline constexpr std::size_t kStockSymbolOff = 11;
inline constexpr std::size_t kStockSymbolLen = 8;

inline constexpr std::size_t kAddRefOff = 11;
inline constexpr std::size_t kAddSideOff = 19;
inline constexpr std::size_t kAddSharesOff = 20;
inline constexpr std::size_t kAddPriceOff = 32;
inline constexpr std::size_t kExecRefOff = 11;
inline constexpr std::size_t kExecSharesOff = 19;
inline constexpr std::size_t kCancelRefOff = 11;
inline constexpr std::size_t kCancelSharesOff = 19;
inline constexpr std::size_t kDeleteRefOff = 11;
inline constexpr std::size_t kReplaceOldRefOff = 11;
inline constexpr std::size_t kReplaceNewRefOff = 19;
inline constexpr std::size_t kReplaceSharesOff = 27;
inline constexpr std::size_t kReplacePriceOff = 31;

}
