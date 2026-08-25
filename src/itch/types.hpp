// This file gathers the ITCH 5.0 message lengths, the common header, the event codes and
// the order field offsets into one place.
// reader.hpp reads a Message through the common offsets, and whoever calls framing uses
// the length table to check that a message boundary was read correctly.
// order_book.hpp and the offline tools use the order offsets to read the order id, the
// shares, the side and the price, so that none of them hard codes a number of its own.
// There is no parsing state here and nothing is read from a file; it only turns the fixed
// numbers of the protocol into compile time constants.
// Changing any one value changes how several tools read the same bytes, so every one of
// them has to match the ITCH 5.0 format exactly.

// These inline constexpr constants are defined in a header; expanding it once avoids
// defining them twice in the same translation unit.
#pragma once

// <array> gives the fixed 256 entry message length table, so an ASCII type can be turned
// straight into an index.
// A fixed size lets the whole table be built at compile time, and looking a type up at run
// time is a single array access.
#include <array>
// <cstddef> gives std::size_t, used here for byte lengths and field offsets.
// size_t also adds to a body pointer directly, with no conversion at each call site.
#include <cstddef>
// <cstdint> gives uint8_t; every ITCH body length fits in one byte.
// The explicit one byte type also keeps the whole length table at exactly 256 bytes.
#include <cstdint>

// All of these constants describe the ITCH wire format, so they live in the itch
// namespace.
// Writing itch::kAddPriceOff at a call site makes it clear this is a protocol offset
// rather than the position of a field in some local struct.
namespace itch {

// In the file and in a MoldUDP64 payload, every ITCH body is preceded by two big endian
// length bytes.
// framing.hpp reads those two bytes first and then decides whether the whole body is
// already in the current buffer.
// The length is not part of the ITCH body, so Message::body skips it and Message::len does
// not include it.
// SeqReader also keeps a partial tail starting from the first of those two bytes.
// The value itself is stored big endian, so framing.hpp reads it with read_be<uint16_t>
// rather than dereferencing it as a native integer.
// One complete record occupies kLenPrefix plus the body length, and for_each_message moves
// its offset along by that total each time.
inline constexpr std::size_t kLenPrefix = 2;

// Every ITCH message starts with the same 11 byte header.
// The order is one byte of type, two bytes of stock locate, two bytes of tracking number
// and six bytes of timestamp.
// The fields specific to an order message, and the event code of a System Event, all begin
// after that point.
// 1+2+2+6 comes to exactly 11, and every offset below is counted from the first byte of the
// body.
// reader.hpp's event_code() reads body[kHeaderLen] directly, so it may only be used on an S
// message of at least 12 bytes.
// The order id of an add, an execution, a cancel, a delete and a replace also begins after
// the common header.
// Writing the length as one constant keeps the relationships of the shared part of the
// protocol consistent throughout the code.
// The tracking number has no reader of its own at the moment, but the two bytes it occupies
// still count towards the timestamp offset.
// kHeaderLen is where the shared part ends, not the length of a whole message; different
// types carry different fields after it.
// Every known message has a kBodyLen of at least 12, so the common header is always inside
// a complete body.
inline constexpr std::size_t kHeaderLen = 11;
// The type is the first byte of the body; Message::type() reads the ASCII message type at
// this offset.
// An offset of zero means the body pointer itself points at the type, without the length
// field in front of it.
inline constexpr std::size_t kTypeOff = 0;
// The stock locate follows the type and occupies two bytes; Message::stock_locate() reads
// it big endian.
// Once read it is used directly as an index in the range 0 to 65535.
inline constexpr std::size_t kLocateOff = 1;
// One plus two plus two is five bytes, so the six byte timestamp starts at offset 5 of the
// body.
// read_be48 copies exactly six bytes from there, and the last of them sits at offset 10 of
// the common header.
inline constexpr std::size_t kTimestampOff = 5;

// kBodyLen takes the message type as an unsigned char and gives the body length ITCH 5.0
// says it should have.
// The array has 256 entries, so any byte is a safe index; characters the protocol does not
// define are left at zero.
// itch_scan, wire_check and frame_check compare the length prefix against the value here.
// If the two disagree, the fixed offsets below may read the wrong message, so the whole
// stream has to be treated as invalid.
// This is the main body of building the length table: clear the whole thing first, then
// fill in only the types the protocol actually defines.
// The elements are uint8_t, because the longest ITCH 5.0 body is 50 bytes, far below 255.
// inline lets several translation units include the same definition, and constexpr
// guarantees the initialisation happens at compile time.
// The lambda is only there to make the entries readable one by one; what it produces is
// still an ordinary constant array.
// The indexes are character literals, so a length and a message type can be read together.
// Entries left at zero let a caller recognise an unknown character without keeping a
// separate set.
// The table only checks the shape of a record; it does not check whether each field is
// sensible.
// framing itself accepts any non zero length, because reading a file has to tell "the
// boundary is complete" apart from "the protocol is legal".
// A strict tool first gets a safe Message out of framing, then looks the type up in this
// table, so an error never reads a field out of bounds.
// The trader's input has already been checked by the acceptance tools, but sharing one
// table keeps the offline and the live interpretation the same.
// An array, not a map: there is no lookup allocation, no hashing and no inserting of
// unknown keys.
// An ASCII type is one byte, so 256 entries cover every possible input and no range check
// is needed.
// These lengths include the 11 byte common header and exclude the two byte length prefix
// in front of it.
inline constexpr std::array<std::uint8_t, 256> kBodyLen = [] {
    // Value initialisation sets all 256 entries to zero, and zero means "not a known ITCH
    // 5.0 type".
    // The local array only exists while the constexpr lambda is being evaluated, and is
    // copied whole into the value of kBodyLen.
    // Starting from all zeros means a new protocol type in future has to be written down
    // explicitly.
    // If an input contains something like z, the lookup reliably gives zero rather than an
    // uninitialised value.
    // 256 entries cover the full range of an unsigned char, so a converted index is never
    // out of bounds.
    // None of this costs anything at run time; the compiler puts the finished bytes
    // straight into read only data.
    std::array<std::uint8_t, 256> t{};
    // S is a System Event, 12 bytes: the common header plus one event code.
    // Message::event_code() reads exactly that twelfth byte, at offset 11.
    t['S'] = 12;
    // R is a Stock Directory, 39 bytes; the tools build the mapping from stock locate to
    // ticker out of it.
    // The eight character symbol starts at offset 11, and other directory fields follow it.
    t['R'] = 39;
    // H is a Stock Trading Action, 25 bytes.
    // It describes the trading state of a security and never adds or removes anything from
    // this project's book.
    t['H'] = 25;
    // Y is a Reg SHO Restriction, 20 bytes.
    // A scanner still has to know its length in order to step to the next message.
    t['Y'] = 20;
    // L is a Market Participant Position, 26 bytes.
    // Even though the current strategy does not use it, the length check after framing
    // still covers this message.
    t['L'] = 26;
    // V is an MWCB Decline Level, 35 bytes.
    // The length comes from the protocol, not from whether the current tools read the
    // fields.
    t['V'] = 35;
    // W is an MWCB Status, 12 bytes.
    // It is the same length as S but a different type, and the table keeps them apart by
    // character.
    t['W'] = 12;
    // K is an IPO Quoting Period Update, 28 bytes.
    // A tool may skip the contents, but it may not get the 28 bytes wrong.
    t['K'] = 28;
    // J is a LULD Auction Collar, 35 bytes.
    // This length is what keeps the next record starting at the right two byte prefix.
    t['J'] = 35;
    // Lower case h is an Operational Halt, 21 bytes; it is a different message from upper
    // case H.
    // The array is indexed by the raw byte, so the two naturally land in different entries.
    t['h'] = 21;
    // A is an Add Order without an MPID, 36 bytes, and it adds a resting order to the book.
    // kAddRefOff, kAddSideOff, kAddSharesOff and kAddPriceOff all lie within those 36 bytes.
    // It also repeats the eight character symbol in the body, but the hot path uses the
    // integer stock locate from the common header.
    // On an A the book first inserts into the order table, then adds the shares to the price
    // level of the matching side.
    // If the length is not 36 then the fixed price offset of 32 can no longer be trusted, and
    // the input has to be rejected.
    t['A'] = 36;
    // F is an Add Order with an MPID, 40 bytes; the order fields the book reads sit at the
    // same offsets as in A.
    // The extra MPID is at the end and does not move the order id, the side, the shares or
    // the price this project needs.
    t['F'] = 40;
    // E is an Order Executed, 31 bytes, and it reduces the shares left on an order by id.
    // The book reads the id at offset 11 and the executed shares at offset 19.
    t['E'] = 31;
    // C is an Order Executed with a price, 36 bytes; the book still takes the shares off at
    // the order's own price.
    // It shares the execute id and shares offsets with E, so take() can follow the same path.
    t['C'] = 36;
    // X is an Order Cancel, 23 bytes, and it reduces an order by id and cancelled shares.
    // This message never removes an order that still has shares left; reduce only takes off
    // the number given.
    t['X'] = 23;
    // D is an Order Delete, 19 bytes, and it carries only the id, so the shares left and the
    // price have to come back out of the order table.
    // Eleven bytes of header plus eight of id is exactly 19, with no shares field to read.
    // What erase gives back decides which side, which price and how many shares to take off.
    // If the id is not there, the book counts an orphan rather than guessing a quantity and
    // carrying on.
    // This is the direct reason the order table cannot leave out the shares and the price.
    t['D'] = 19;
    // U is an Order Replace, 35 bytes; the old id is deleted first, then a new order is
    // inserted with a new id, shares and price.
    // The two ids, four bytes of shares and four of price follow the common header.
    // U does not carry a side, so the new order has to keep the direction recovered from the
    // old id.
    // The shares left on the old order come off its old price level in full, and the new
    // shares go on to the new price level.
    // The two ids must not be confused: the old one is erased and the new one is inserted.
    t['U'] = 35;
    // P is a non cross Trade, 44 bytes; it does not change the resting book this project
    // keeps.
    // It is still a legal ITCH message and a scanner has to step over exactly 44 bytes of it.
    t['P'] = 44;
    // Q is a Cross Trade, 40 bytes; this Q is a message type, not the Q event code of a
    // System Event.
    // The two live at different levels: one is body[0], the other only appears at body[11] of
    // an S message.
    t['Q'] = 40;
    // B is a Broken Trade, 19 bytes.
    // This project does not undo trades, but the length table still guarantees framing has
    // not slipped.
    t['B'] = 19;
    // I is a Net Order Imbalance Indicator, 50 bytes.
    // 50 is the longest body in this table and still fits safely in a uint8_t.
    t['I'] = 50;
    // N is a Retail Price Improvement Indicator, 20 bytes.
    // This N has nothing to do with any run time parameter; it is only a message type.
    t['N'] = 20;
    // O is a Direct Listing with Capital Raise Price Discovery, 48 bytes.
    // It is message type O and should not be confused with the O of "Start of Messages" in a
    // System Event.
    t['O'] = 48;
    // Return the finished array as the compile time value of kBodyLen.
    // The lambda ends here, and no initialisation loop runs at start up to build the table.
    // The return is by value, but constexpr evaluation forms the final constant directly and
    // does not copy 256 bytes when the program starts.
    // The indexes still at zero go on meaning an unknown type.
    // A caller should convert a char to unsigned char before indexing, so that a signed char
    // cannot produce a negative index.
    // This table does not own its input and never changes after being queried.
    // A constexpr return requires every assignment to be possible at compile time, so there
    // is no dynamic memory and no system call here.
    // The finished table can be shared read only between threads, with no lock and no atomic.
    // If the protocol gains a type later, one more length before the return is all it takes.
    return t;
// Immediately call the constexpr lambda above; the closing semicolon completes the
// definition of kBodyLen.
// The braces only end the lambda; the parentheses are what run it and take the array it
// returns.
}();

// The three characters below are the System Event codes of an S message that this program
// acts on, and they are not message types.
// Message::type() has to say S before a caller may compare event_code() against them.
// The same character may also appear as another message type in body[0]; a different
// position means a different meaning.
// These events come from the file itself, so a session boundary does not have to be written
// down as a clock time.
// S means the start of system hours; itch_scan uses it to confirm the opening events of the
// day are present.
// The statistics still do not enter the regular trading window on this event alone.
inline constexpr char kEventStartOfSystemHours = 'S';
// Q means the start of regular market hours; window::note_session lets in session windows
// count from here.
// Taking the event from the message rather than hard coding 09:30 keeps a half day working
// on its own contents.
inline constexpr char kEventStartOfMarketHours = 'Q';
// M means the end of regular market hours; window::note_session stops counting new official
// windows from here.
// Messages after M are still parsed and still update the book; they simply no longer produce
// in session samples.
inline constexpr char kEventEndOfMarketHours = 'M';

// A Stock Directory puts the eight character ticker, padded with spaces, immediately after
// the common header.
// The trader and the scanning tools find the first character at offset 11.
// This is only used on the cold path, to map a stock locate to a string; the hot path
// carries on using the integer.
// The offset equals kHeaderLen, because the symbol is the first field particular to an R
// message.
inline constexpr std::size_t kStockSymbolOff = 11;
// A ticker occupies eight bytes on the wire; a reader trims the trailing spaces afterwards.
// It cannot rely on a C string terminator, because the protocol field has no extra zero
// byte.
inline constexpr std::size_t kStockSymbolLen = 8;

// Below are the field offsets of the messages that change the book, all counted from the
// start of the ITCH body.
// A strict acceptance tool checks the type and the body length against kBodyLen first; the
// trader trusts that the sender has given it legal ITCH.
// An offset only says where a field begins; a multi byte id, share count or price still has
// to be turned into a host integer by read_be.
// The same number is kept under different names so that a call site says whether it is
// handling an add, an execution, a cancel or a replace.
// The order id of both A and F follows the common header, starting at offset 11 and
// occupying eight bytes.
// That shared beginning is what lets the book handle both kinds of add with one add()
// method.
inline constexpr std::size_t kAddRefOff = 11;
// The side of an Add Order follows the id, as an ASCII B or S.
// The book turns the character into the index PriceLevels uses and the 0 or 1 the order
// table uses.
inline constexpr std::size_t kAddSideOff = 19;
// The shares of an Add Order follow the side, starting at offset 20, as four big endian
// bytes.
// reader::read_be<uint32_t> turns them into a host integer, which is then stored as the
// shares left.
inline constexpr std::size_t kAddSharesOff = 20;
// An Add Order carries the eight character ticker after the shares, which puts the four
// byte price at offset 32.
// The price keeps the integer units of ITCH; nothing turns it into a floating point number
// while parsing.
inline constexpr std::size_t kAddPriceOff = 32;
// E and C both give the id of the executed order after the common header, so they share
// offset 11.
// That id is what finds the original order's price, side and remaining shares in the order
// table.
inline constexpr std::size_t kExecRefOff = 11;
// The executed shares follow the eight byte id, starting at offset 19.
// reduce uses them to take shares off the order and hands the state from before back to the
// book so it can update the price level.
inline constexpr std::size_t kExecSharesOff = 19;
// X gives the id to reduce after the common header, in the same place as an execution.
// Keeping a separate name lets the calling code say it is handling a cancel, rather than
// relying on two numbers happening to be equal.
inline constexpr std::size_t kCancelRefOff = 11;
// The cancelled shares follow the id, starting at offset 19.
// They are the number being taken away, not the number left after the cancel.
inline constexpr std::size_t kCancelSharesOff = 19;
// The only content particular to D is the eight byte id, starting where the common header
// ends.
// With no price and no shares, erase has to recover those fields from the existing order.
inline constexpr std::size_t kDeleteRefOff = 11;
// U gives the old id that is being replaced first; the book erases it and takes its shares
// off the old price level.
// Only once that has succeeded does it read and insert the new id, so the old order cannot
// be left in the book.
inline constexpr std::size_t kReplaceOldRefOff = 11;
// The new id follows the old one, starting at offset 19; it becomes the key of the inserted
// order.
// A replace does not edit fields of the old id: the protocol explicitly gives a new
// reference number.
inline constexpr std::size_t kReplaceNewRefOff = 19;
// The replacement's new shares follow the two ids, starting at offset 27.
// This is the full initial size of the new order, not a change against the old one.
inline constexpr std::size_t kReplaceSharesOff = 27;
// The replacement's new price follows the four bytes of shares, starting at offset 31.
// The new order keeps the old side, but the price is read afresh from this field.
inline constexpr std::size_t kReplacePriceOff = 31;

}
