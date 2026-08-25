// This header only has to be included once.
#pragma once

// A very small SHA-256.
//
// What it is for in this project, so it is not read as cryptography: SHA-256 squeezes a string of
// bytes of any length into a fixed thirty two. The same input always gives the same result, and
// one bit of difference in the input makes the result unrecognisable.
// So it has one use here - comparing two very large things at a glance.
// Two books hold millions of orders each, and comparing them order by order takes a pile of
// code; working out a thirty two byte number for each and comparing those is the end of it.
//
// Why not an existing library: so that this repository carries not one external dependency.
// Somebody who clones it can build it without installing OpenSSL first.
//
// It is used in two places:
// squeezing the reference book into a string of hexadecimal to compare against the book being
// tested (see book/ref_book.hpp);
// and later, hashing the configuration files as well so that both ends print a line - a mismatch
// meaning the configurations differ.
//
// The second is worth doing because it has already gone wrong: one round had the parameters of
// the two ends out of step, and that mistake gives no warning and quietly runs two different
// experiments while looking afterwards like a comparison of the same thing.
//
// What follows is the standard algorithm with not one word changed. So it must not be read as
// this project's logic: those constants and rounds all come from the SHA-256 specification, and
// changing one makes it not SHA-256.
// The only parts worth looking at are how update and digest gather a whole block before working
// on it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Put in the crypto namespace, which holds only this one class at present.
namespace crypto {

// Works out one digest.
// It is used by feeding update as many times as wanted and taking digest or hex once at the end.
// One object can only be taken once - finishing changes the internal state and feeding it
// afterwards means nothing.
class Sha256 {
// What follows is the three things for outside use: construction, update to feed data, and
// digest or hex to take the result.
public:
    // The starting state is written into the members' default values (the eight numbers at the
    // end of the class), so the constructor has nothing to do.
    Sha256() = default;

    // Feeds in a piece of data. It can be fed any number of times, each of any length.
    //
    // This is one of the two main pieces. It looks as though every call works out a hash - it
    // does not.
    // This algorithm can only take sixty four bytes at a time, so what this function really does
    // is gather: sixty four gathered works out one block, and less than that stays in the holding
    // area until the next call's data joins it.
    // So the caller may cut it up however it likes, and the cutting does not affect the result.
    // The reference book feeds nineteen bytes a row (nineteen times something rarely coming to
    // exactly sixty four), and this is what lets it.
    void update(const void* data, std::size_t len) {
        // Whatever type the caller gave, it is treated as bytes throughout.
        const auto* p = static_cast<const std::uint8_t*>(data);
        // How many bytes have been fed altogether. The number of bits of it goes into the digest
        // at the end, see digest.
        total_ += len;
        // Keep moving until this call's data is used up.
        while (len > 0) {
            // How much this turn can move: either the rest of it or exactly what fills the
            // holding area, whichever is smaller.
            const std::size_t take = std::min(len, sizeof(buf_) - buf_len_);
            // Joined behind what the holding area already has.
            std::memcpy(buf_ + buf_len_, p, take);
            // The number of bytes in the holding area goes up.
            buf_len_ += take;
            // The source pointer moves along, so the next turn carries on from what has not been
            // moved.
            p += take;
            // How much is left to move. The loop ends when this reaches zero.
            len -= take;
            // The holding area is exactly full at sixty four bytes.
            if (buf_len_ == sizeof(buf_)) {
                // This block is worked out. The result is added into the eight state words in
                // h_.
                block(buf_);
                // The holding area starts gathering again. Only "how many bytes are in it" has
                // to go to zero - the sixty four old bytes need no wiping, because the next
                // memcpy covers them from the start.
                buf_len_ = 0;
            }
        }
    }

    // Finishes and takes out the thirty two byte digest.
    //
    // This is the second of the two main pieces. What finishing does is called padding.
    //
    // It looks as though padding is only "fill up a block and it does not matter with what". It
    // is not - what goes in is laid down, and for a reason: first a 0x80, then as many zeros as
    // it takes until a block is eight bytes short of full; and those last eight bytes hold how
    // many bits the original data was.
    // Those last eight bytes are the point: with them, "abc" and "abc with a pile of zeros after
    // it" cannot work out to the same result - the lengths differ, so the last block fed in
    // differs.
    //
    // The number of bits has to be recorded before anything is padded, which is why it is the
    // first line - the padding bytes go in through update as well and would add to total_.
    [[nodiscard]] std::array<std::uint8_t, 32> digest() {
        // How many bits the original data was. Times eight because a byte is eight bits.
        const std::uint64_t bits = total_ * 8;
        // The first padding byte is always 0x80, which is a one followed by seven zeros.
        // It is static because its address is passed to update, and update takes a pointer.
        static const std::uint8_t pad_start = 0x80;
        // Fed in. This may fill a block exactly, in which case update works it out on the way.
        update(&pad_start, 1);
        // The byte used for padding with zeros.
        static const std::uint8_t zero = 0;
        // Zeros go in until the holding area has exactly fifty six bytes - eight short of a full
        // block.
        //
        // A byte at a time looks clumsy, but finishing happens once in a lifetime and is not
        // worth optimising.
        // There is another case that is easy to miss: the 0x80 may have filled a block exactly,
        // update worked it out on the way, and buf_len_ became 0. Then this loop pads fifty six
        // zeros from the start - which is exactly what the specification asks for, so it needs no
        // branch of its own.
        while (buf_len_ != 56) update(&zero, 1);
        // The last eight bytes hold the number of bits, most significant first.
        // Eight bytes on the stack, gone once used.
        std::uint8_t len_be[8];
        // Laid out from the most significant byte.
        for (int i = 0; i < 8; ++i) {
            // Byte 0 shifts right by fifty six and the last by zero.
            len_be[i] = static_cast<std::uint8_t>(bits >> (8 * (7 - i)));
        }
        // Fed in. These eight bytes fill the holding area exactly, so update works out the last
        // block.
        update(len_be, 8);

        // What follows spreads the eight thirty two bit state words into thirty two bytes, most
        // significant first.
        // The braces clear it all first, so that missing an entry below cannot carry a stray
        // number out.
        std::array<std::uint8_t, 32> out{};
        // One state word gives four bytes.
        for (int i = 0; i < 8; ++i) {
            // The most significant byte.
            out[4 * i + 0] = static_cast<std::uint8_t>(h_[i] >> 24);
            // The next.
            out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
            // The next again.
            out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
            // The least significant byte, which needs no shift.
            out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
        }
        // The digest goes to the caller.
        // It can only be taken once: the padding above has already changed the internal state,
        // and what an update after it works out is the hash of nothing at all. Another digest
        // means another object.
        return out;
    }

    // The same digest as sixty four hexadecimal characters.
    // Thirty two bytes, two characters each, so sixty four.
    // What is printed in a log and compared is this form, because the raw bytes cannot be
    // printed directly.
    [[nodiscard]] std::string hex() {
        // The digest is worked out first. That line uses digest up, so hex can only be called
        // once as well.
        const auto d = digest();
        // A string of sixty four characters is prepared. What is in it does not matter, since
        // every place is covered below; preparing it avoids growing while assembling.
        std::string s(64, '0');
        // Thirty two bytes, two characters each.
        for (int i = 0; i < 32; ++i) {
            // The third argument being 3 looks like a mistake - is a byte not two characters?
            // It is not a mistake: snprintf always puts a zero after what it wrote, and that zero
            // needs a place of its own.
            // Given 2 it would only write one character and the string would come out wrong.
            //
            // That extra zero covers the next place, which the next turn rewrites at once, so it
            // does not matter.
            // On the last turn (i = 31) what it covers is the string's own terminating zero -
            // writing a zero over a zero, which changes nothing.
            std::snprintf(s.data() + 2 * i, 3, "%02x", d[i]);
        }
        // The sixty four characters are handed over.
        return s;
    }

// What follows is the internals. The outside does not need to know how it works block by block.
private:
    // Rotate right: every bit moves right by n, and the n bits pushed out of the right join back
    // on at the far left.
    // Like turning a ring of beads, with not one falling off.
    // This algorithm is full of it. Why not an ordinary shift right: an ordinary shift throws the
    // bits pushed out away, while a hash requires every bit of the input to affect the final
    // result, with not one lost.
    static std::uint32_t ror(std::uint32_t v, int n) {
        // Shift right by n and join the n bits shifted out back on at the far left.
        return (v >> n) | (v << (32 - n));
    }

    // Works out one block of sixty four bytes.
    // This is the heart of the algorithm, copied entirely from the specification, and must not be
    // read as this project's logic - there is no choice of ours here and every step is what the
    // specification lays down.
    void block(const std::uint8_t* p) {
        // Sixty four round constants, one per round.
        // They come from the fractional parts of the cube roots of the first sixty four primes.
        // The specification fixes them and changing one makes it not SHA-256. As static constexpr
        // they are fixed in read only data at compile time.
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        // This block is expanded into sixty four thirty two bit words, one per round.
        // Two hundred and fifty six bytes on the stack. It is opened afresh for every block, but
        // opening an array on the stack only moves the stack pointer and asks for no memory, so
        // it is not worth caching.
        std::uint32_t w[64];
        // The first sixteen come straight from the sixty four input bytes, four bytes to a word,
        // most significant first.
        for (int i = 0; i < 16; ++i) {
            // The first byte is the top eight bits.
            w[i] = (static_cast<std::uint32_t>(p[4 * i]) << 24) |
                   // The second byte.
                   (static_cast<std::uint32_t>(p[4 * i + 1]) << 16) |
                   // The third byte.
                   (static_cast<std::uint32_t>(p[4 * i + 2]) << 8) |
                   // The fourth byte, the lowest eight bits, which needs no shift.
                   static_cast<std::uint32_t>(p[4 * i + 3]);
        }
        // The other forty eight are worked out from the ones already there.
        // What this step does is spread: it lets every bit of the input affect all the rounds
        // that follow.
        // The names s0 and s1 below appear again in the sixty four round loop after this one, but
        // those are two different expressions shifting by completely different amounts. The two
        // must not be taken as the same thing.
        for (int i = 16; i < 64; ++i) {
            // Mix in the fifteenth word back. Three terms exclusive-ored, the first two rotations
            // and the third an ordinary shift right.
            const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            // Mix in the second word back, the same shape with different amounts.
            const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            // Four terms added give this new word. The addition here wraps at thirty two bits,
            // and the overflow is both normal and wanted by the algorithm - wrapping is itself a
            // kind of mixing.
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        // What follows copies the current eight state words into eight working variables.
        // The first four.
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        // The last four.
        std::uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        // Sixty four rounds. Every round has exactly the same shape, differing only in the round
        // constant k[i] and the word w[i].
        for (int i = 0; i < 64; ++i) {
            // Mix e: rotate it right by three different amounts and exclusive-or the three.
            const std::uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            // Choose: where a bit of e is 1 take f's, where it is 0 take g's. Done bit by bit.
            const std::uint32_t ch = (e & f) ^ (~e & g);
            // The first intermediate of this round, five terms added. The round constant and the
            // word come in here.
            const std::uint32_t t1 = h + s1 + ch + k[i] + w[i];
            // Mix a, the same shape as the line above but with a and three different amounts.
            const std::uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            // Majority: for each bit, whichever value a, b and c mostly hold. Done bit by bit.
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            // The second intermediate of this round.
            const std::uint32_t t2 = s0 + maj;
            // The two lines below move all eight variables along one place, with the newly worked
            // out values inserted in two places.
            // h takes g's, g takes f's, f takes e's; and e is d plus t1 - the first insertion.
            h = g; g = f; f = e; e = d + t1;
            // d takes c's, c takes b's, b takes a's; and a is t1 plus t2 - the second insertion.
            d = c; c = b; b = a; a = t1 + t2;
        }
        // The two lines below fold this block's result back into the state.
        // It is added rather than assigned. That step is exactly what makes every block's result
        // depend on all the blocks before it rather than on the last one alone - assigned, every
        // byte fed in earlier would have been fed for nothing.
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        // The last four the same way.
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    // The eight state words, which are how far it has got.
    // Their starting values come from the fractional parts of the square roots of the first eight
    // primes, which the specification fixes as well.
    std::uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    // The bytes that do not fill a block are held here. Sixty four bytes, exactly one block.
    std::uint8_t buf_[64] = {};
    // How many bytes the holding area has now. Sixty four works out a block and goes back to
    // zero.
    std::size_t buf_len_ = 0;
    // How many bytes were fed in altogether. Finishing writes the number of bits of it into the
    // last eight bytes.
    std::uint64_t total_ = 0;
// The end of the class.
};

}  // namespace crypto
