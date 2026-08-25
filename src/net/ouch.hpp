#pragma once

// What the order sent when a signal fires looks like.
// Inside is one OUCH 5.0 Enter Order.
// Around it is a SoupBinTCP envelope of three bytes: a two byte length and a one byte type.
// The field positions of both layers are copied from Nasdaq's own documents rather than guessed.
// And the unit tests pin down every one of the offsets.
//
//   the three SoupBinTCP bytes           OUCH 5.0 Enter Order (with no appendage)
//   0   2  how many bytes follow          0   1  message type 'O'
//   2   1  type 'U'                       1   4  our own order number
//   3   n  the message itself             5   1  buy or sell
//                                         6   4  shares
//   That length counts the bytes         10   8  ticker
//   after it and not itself,             18   8  price
//   so it is 1 + 47.                     26   1  time in force
//                                        27   1  whether it shows on the book
//                                        28   1  in what capacity it is entered
//                                        29   1  whether it may sweep other markets
//                                        30   1  whether it is an auction order
//                                        31  14  the client order id, which we make up
//                                        45   2  the appendage length, zero here
//
// Fifty bytes go on the wire altogether.
// The headers Onload adds on this link measure 66 bytes, so a whole frame is 116.
//
// That fifty appears all over the send path:
// a frame gathers at most 28 orders, which is 1,400 bytes;
// and the room check before sending multiplies "what is gathered plus this one" by 50 to compare
// against the far end's window.
//
// One convenient coincidence: the price is an unsigned integer with four implied decimals.
// It is the same scale ITCH uses.
// So a price read out of the book goes straight into an order with not one conversion.
//
// The highest price the exchange will accept is 0x7735939C, which is $199,999.99.
// The highest price that appeared in ITCH that day happens to be exactly that.

#include <cstdint>
#include <cstring>

namespace ouch {

// The Enter Order message itself is forty seven bytes.
inline constexpr std::size_t kEnterOrderLen = 47;
// The envelope around it is three bytes.
inline constexpr std::size_t kSoupHeaderLen = 3;
// The total length of an order on the wire, fifty bytes. The send path works out room and
// offsets from this number throughout.
inline constexpr std::size_t kOrderPacketLen = kSoupHeaderLen + kEnterOrderLen;
// A ticker takes eight bytes, padded with spaces rather than zeros.
inline constexpr std::size_t kSymbolLen = 8;
// The client order id takes fourteen bytes.
inline constexpr std::size_t kClOrdIdLen = 14;
// The highest price the exchange will accept. Above it an order is rejected, so a higher price
// read from the book should not be sent.
inline constexpr std::uint64_t kHighestPrice = 0x7735939Cull;  // $199,999.9900

// What follows is where each field starts inside the Enter Order body.
// They are constants rather than numbers written into the code so that the unit tests can pin
// each of them down - if somebody ever changes an offset by mistake, a test goes red at once
// rather than the exchange rejecting an order later.
//
// Read them in two halves by who writes them, which is the most important thing in this file:
//   written once before the run  the type, time in force, display, capacity, sweep, auction,
//                                appendage length
//   written when a signal fires  the order number, side, shares, ticker, price (and the client
//                                order id, written separately below)
// The first half is written once in prefill and never touched again.
// The second half is written once per order in fill.

// prefill writes 'O' here. The exchange tells an entry from a cancel or a replace by this one
// byte.
inline constexpr std::size_t kTypeOff = 0;
// The number we make up, going up by one per order. The exchange's reports carry it back
// unchanged, so it is the key from a report to the order it belongs to.
inline constexpr std::size_t kUserRefOff = 1;
// fill writes it for every order. It sits next to the order number above with no gap - so the
// writes to those two fields land on the same cache line and do not fetch memory twice.
inline constexpr std::size_t kSideOff = 5;
// We always send one share. That is a choice of this test bench: the number of shares does not
// affect what this path costs, and fixing it at one keeps "how many orders went out" and "how
// many shares traded" from interfering with each other.
inline constexpr std::size_t kQuantityOff = 6;
// The eight byte ticker. Its content was pulled out of the market data before the open and
// arranged by security number, so on the hot path this is one eight byte copy with no string
// work at all.
inline constexpr std::size_t kSymbolOff = 10;
// What goes here is directly the price read out of the book.
// ITCH and OUCH use the same scale, both being integers in hundredths of a cent.
// So there is not one conversion in between.
inline constexpr std::size_t kPriceOff = 18;
// From here down, six fields belong to prefill and the hot path touches not one of their bytes.
inline constexpr std::size_t kTimeInForceOff = 26;
// The same, written once before the run.
inline constexpr std::size_t kDisplayOff = 27;
// The same.
inline constexpr std::size_t kCapacityOff = 28;
// The same.
inline constexpr std::size_t kSweepOff = 29;
// The same.
inline constexpr std::size_t kCrossTypeOff = 30;
// Fourteen bytes whose content we make up. It is a text field and has to be right aligned with
// spaces on the left, which is why set_cl_ord_id below fills it from the right.
inline constexpr std::size_t kClOrdIdOff = 31;
// Always zero. It is precisely because it is always zero that an order is a fixed fifty bytes -
// every place on the send path that works out room or offsets rests on that.
inline constexpr std::size_t kAppendageLenOff = 45;

// The character for a buy.
inline constexpr char kBuy = 'B';
// The character for a sell.
inline constexpr char kSell = 'S';
// The time in force is immediate or cancel. Because ours is a taking strategy: either it trades
// at once or it should not rest on the book - resting means having to manage it, and it changes
// what other people see.
inline constexpr char kImmediateOrCancel = '3';
// It shows on the book. With immediate or cancel chosen, this makes practically no difference.
inline constexpr char kVisible = 'Y';
// Principal, meaning trading our own money.
inline constexpr char kPrincipal = 'P';         // trading our own money
// Do not sweep other exchanges. Trading only here is the simplest behaviour and the easiest
// result to explain.
inline constexpr char kNotSweepEligible = 'N';
// An ordinary order in continuous trading, not one of the opening or closing auctions.
inline constexpr char kContinuousMarket = 'N';  // not one of the auctions

// Writes an integer into bytes, most significant first.
// The pointer cannot simply be cast and written through.
// x86 memory is least significant first, and the order written that way is exactly the opposite
// of what the protocol asks for.
// It is a template because widths of 2, 4 and 8 all have to be written.
template <typename T>
void put_be(std::uint8_t* p, T v) {
    // Starting from the most significant byte and working along.
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        // Byte i wants section i counting down from the top.
        // So it shifts right by (total bytes - 1 - i) times eight.
        p[i] = static_cast<std::uint8_t>(v >> (8 * (sizeof(T) - 1 - i)));
    }
}

// This function is the reason sending an order can be quick, which the name does not show, so it
// is worth saying plainly:
// it seems natural that a signal firing assembles a fifty byte order there and then. That is not
// what happens here.
// Most of the bytes of an order are the same every time - the type, time in force, capacity,
// appendage length and the envelope - and they are written into that entry of the card's buffer
// before the run and never touched again, because fill below writes only the fields that change.
// So the moment a signal fires, what is left to do is four writes and one eight byte copy.
inline void prefill(std::uint8_t* p) {
    // The whole piece is filled with spaces first. Not zeros.
    // OUCH says text fields are padded with spaces, and zeros are not valid.
    std::memset(p, ' ', kOrderPacketLen);
    // The envelope's length field: it counts the bytes after it, which is one byte of type and
    // forty seven of message.
    put_be<std::uint16_t>(p, static_cast<std::uint16_t>(1 + kEnterOrderLen));
    // The envelope's type. 'U' means unsequenced data, an ordinary message from a client to the
    // exchange.
    p[2] = 'U';
    // Past the three envelope bytes is the OUCH message itself.
    // Every offset below is counted from m, matching the table in the document one for one.
    std::uint8_t* m = p + kSoupHeaderLen;
    // The OUCH message type 'O' is Enter Order.
    m[kTypeOff] = 'O';
    // Time in force: immediate or cancel.
    m[kTimeInForceOff] = kImmediateOrCancel;
    // It shows on the book.
    m[kDisplayOff] = kVisible;
    // Entered as principal.
    m[kCapacityOff] = kPrincipal;
    // Not sweeping other exchanges.
    m[kSweepOff] = kNotSweepEligible;
    // An ordinary order in continuous trading.
    m[kCrossTypeOff] = kContinuousMarket;
    // The appendage length is zero, meaning the message ends here.
    // After this line every unchanging byte of an order is lying in the card's buffer.
    put_be<std::uint16_t>(m + kAppendageLenOff, 0);
}

// This is what really runs on the hot path: a signal fired, so the four fields of this order
// that change are filled in.
// It takes where the buffer is, our own order number, buy or sell, how many shares, the eight
// bytes of the ticker, and the price.
// Once filled the order can go out.
// The other bytes were written by prefill long ago and not one of them is touched here.
//
// The ticker was pulled apart into eight bytes and stored before the open, so this is one eight
// byte copy with no string work at all - no lookup, no comparison, no working out a length.
inline void fill(std::uint8_t* p, std::uint32_t user_ref, char side,
                 std::uint32_t quantity, const std::uint8_t* symbol,
                 std::uint64_t price) {
    // Past the envelope, pointing at the OUCH message itself.
    // The length and type inside the envelope were written by prefill.
    std::uint8_t* m = p + kSoupHeaderLen;
    // The number we gave this order, four bytes, most significant first.
    put_be<std::uint32_t>(m + kUserRefOff, user_ref);
    // Buy or sell. One byte, written directly, with no byte order to worry about.
    m[kSideOff] = side;
    // The number of shares, four bytes.
    put_be<std::uint32_t>(m + kQuantityOff, quantity);
    // The eight bytes of the ticker, copied as a piece. They come from the table arranged by
    // security number before the open.
    std::memcpy(m + kSymbolOff, symbol, kSymbolLen);
    // The price, eight bytes. After this line the order is complete - the caller goes on to fill
    // in the client order id and then gathers it into this frame.
    put_be<std::uint64_t>(m + kPriceOff, price);
}

// The client order id is a field we decide ourselves and the exchange merely returns unchanged.
// So which message brought this order about is encoded in it - a confirmation coming back can
// then be traced all the way to that tick of market data, without keeping a lookup table.
inline void set_cl_ord_id(std::uint8_t* p, std::uint64_t n) {
    // Pointing at the fourteen bytes of the client order id.
    std::uint8_t* d = p + kSoupHeaderLen + kClOrdIdOff;
    // The whole piece is filled with spaces first, and this step cannot be skipped: this buffer
    // is reused, and if the previous order's number was longer than this one, the extra digits
    // would stay behind and spell an order id that never existed.
    std::memset(d, ' ', kClOrdIdLen);
    // Decimal digits are filled from the right, until the number runs out or the fourteen places
    // are used up.
    // That leaves the number right aligned with spaces on the left, which is exactly what OUCH
    // asks of a text field.
    for (std::size_t i = kClOrdIdLen; i-- > 0 && n != 0; n /= 10) {
        // Take the lowest digit and add '0' to make it the matching character.
        d[i] = static_cast<std::uint8_t>('0' + n % 10);
    }
}

// The login message sent once the connection is up and before any order, forty nine bytes
// altogether.
inline constexpr std::size_t kLoginLen = 49;
// Assembles the login message. It takes the buffer, the user name and the password.
inline void login(std::uint8_t* p, const char* user, const char* password) {
    // Filled with spaces - as above, every text field is padded with spaces.
    std::memset(p, ' ', kLoginLen);
    // The envelope's length field counts the bytes after it, so it is the total less its own two
    // bytes.
    put_be<std::uint16_t>(p, static_cast<std::uint16_t>(kLoginLen - 2));
    // The type 'L' says this is a login request.
    p[2] = 'L';
    // The user name takes bytes 3 to 8, six of them. Longer is truncated and shorter leaves
    // spaces behind - which is why this takes the smaller of the name's length and six.
    std::memcpy(p + 3, user, std::strlen(user) < 6 ? std::strlen(user) : 6);
    // The password takes bytes 9 to 18, ten of them, truncated or left with spaces the same way.
    std::memcpy(p + 9, password, std::strlen(password) < 10 ? std::strlen(password) : 10);
    // The session name is left as spaces, meaning connect to whichever session is running.
    // And the '1' here is the sequence number to start from: from the first message of that
    // session.
    p[38] = '1';
}

}  // namespace ouch
