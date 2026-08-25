#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "book/ref_book.hpp"
#include "itch/types.hpp"

namespace {

void put_be(std::uint8_t* p, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        p[i] = static_cast<std::uint8_t>(v >> (8 * (bytes - 1 - i)));
    }
}

std::vector<std::uint8_t> body(char type, std::uint16_t locate) {
    std::vector<std::uint8_t> b(itch::kBodyLen[static_cast<unsigned char>(type)], 0);
    b[itch::kTypeOff] = static_cast<std::uint8_t>(type);
    put_be(b.data() + itch::kLocateOff, locate, 2);
    put_be(b.data() + itch::kTimestampOff, 34200000000000ull, 6);
    return b;
}

void feed(book::RefBook& bk, std::vector<std::uint8_t>& b) {
    bk.apply(itch::Message{b.data(), static_cast<std::uint16_t>(b.size())});
}

void add(book::RefBook& bk, std::uint16_t locate, std::uint64_t ref, char side,
         std::uint32_t shares, std::uint32_t price, char type = 'A') {
    auto b = body(type, locate);
    put_be(b.data() + itch::kAddRefOff, ref, 8);
    b[itch::kAddSideOff] = static_cast<std::uint8_t>(side);
    put_be(b.data() + itch::kAddSharesOff, shares, 4);
    put_be(b.data() + itch::kAddPriceOff, price, 4);
    feed(bk, b);
}

void exec(book::RefBook& bk, std::uint64_t ref, std::uint32_t shares) {
    auto b = body('E', 1);
    put_be(b.data() + itch::kExecRefOff, ref, 8);
    put_be(b.data() + itch::kExecSharesOff, shares, 4);
    feed(bk, b);
}

void cancel(book::RefBook& bk, std::uint64_t ref, std::uint32_t shares) {
    auto b = body('X', 1);
    put_be(b.data() + itch::kCancelRefOff, ref, 8);
    put_be(b.data() + itch::kCancelSharesOff, shares, 4);
    feed(bk, b);
}

void del(book::RefBook& bk, std::uint64_t ref) {
    auto b = body('D', 1);
    put_be(b.data() + itch::kDeleteRefOff, ref, 8);
    feed(bk, b);
}

void replace(book::RefBook& bk, std::uint64_t old_ref, std::uint64_t new_ref,
             std::uint32_t shares, std::uint32_t price) {
    auto b = body('U', 1);
    put_be(b.data() + itch::kReplaceOldRefOff, old_ref, 8);
    put_be(b.data() + itch::kReplaceNewRefOff, new_ref, 8);
    put_be(b.data() + itch::kReplaceSharesOff, shares, 4);
    put_be(b.data() + itch::kReplacePriceOff, price, 4);
    feed(bk, b);
}

TEST(Book, add_and_execute) {
    book::RefBook bk(64);
    add(bk, 7, 1000, 'B', 500, 1234500);
    EXPECT_EQ(bk.live(), 1);
    const std::string after_add = bk.hash();

    exec(bk, 1000, 200);
    EXPECT_EQ(bk.live(), 1);
    EXPECT_NE(bk.hash(), after_add);

    exec(bk, 1000, 300);
    EXPECT_EQ(bk.live(), 0);
    EXPECT_EQ(bk.counters().executed, 2);
    EXPECT_EQ(bk.counters().orphan_ref, 0);
}

TEST(Book, cancel_and_delete) {
    book::RefBook bk(64);
    add(bk, 1, 10, 'S', 100, 500);
    cancel(bk, 10, 40);
    EXPECT_EQ(bk.live(), 1);
    cancel(bk, 10, 60);
    EXPECT_EQ(bk.live(), 0);

    add(bk, 1, 11, 'S', 100, 500);
    del(bk, 11);
    EXPECT_EQ(bk.live(), 0);
    EXPECT_EQ(bk.counters().orphan_ref, 0);
}

TEST(Book, replace_keeps_side_and_security) {
    book::RefBook bk(64);
    add(bk, 42, 200, 'S', 300, 999, 'F');
    replace(bk, 200, 201, 150, 1001);
    EXPECT_EQ(bk.live(), 1);
    EXPECT_EQ(bk.counters().replaced, 1);
    EXPECT_EQ(bk.counters().orphan_ref, 0);

    book::RefBook expect(64);
    add(expect, 42, 201, 'S', 150, 1001);
    EXPECT_EQ(bk.hash(), expect.hash());
}

TEST(Book, orphan_updates_are_counted) {
    book::RefBook bk(64);
    exec(bk, 5, 1);
    cancel(bk, 6, 1);
    del(bk, 7);
    replace(bk, 8, 9, 1, 1);
    EXPECT_EQ(bk.live(), 0);
    EXPECT_EQ(bk.counters().orphan_ref, 4);
}

TEST(Book, hash_is_insertion_order_independent) {
    book::RefBook a(64);
    add(a, 3, 100, 'B', 10, 700);
    add(a, 1, 101, 'S', 20, 800);
    add(a, 2, 102, 'B', 30, 900);

    book::RefBook b(64);
    add(b, 2, 102, 'B', 30, 900);
    add(b, 3, 100, 'B', 10, 700);
    add(b, 1, 101, 'S', 20, 800);

    EXPECT_EQ(a.hash(), b.hash());

    book::RefBook c(64);
    add(c, 3, 100, 'B', 10, 700);
    add(c, 1, 101, 'S', 20, 800);
    add(c, 2, 102, 'B', 31, 900);
    EXPECT_NE(a.hash(), c.hash());
}

TEST(Book, trades_do_not_move_the_book) {
    book::RefBook bk(64);
    add(bk, 1, 1, 'B', 100, 500);
    const std::string before = bk.hash();
    for (const char type : {'P', 'Q', 'B', 'H', 'Y'}) {
        auto b = body(type, 1);
        feed(bk, b);
    }
    EXPECT_EQ(bk.hash(), before);
    EXPECT_EQ(bk.live(), 1);
}

TEST(Book, peak_live_tracks_the_maximum) {
    book::RefBook bk(64);
    add(bk, 1, 1, 'B', 10, 100);
    add(bk, 1, 2, 'B', 10, 100);
    add(bk, 1, 3, 'B', 10, 100);
    del(bk, 1);
    del(bk, 2);
    EXPECT_EQ(bk.live(), 1);
    EXPECT_EQ(bk.peak_live(), 3);
}

TEST(Book, live_by_locate_splits_the_market) {
    book::RefBook bk(64);
    add(bk, 5, 1, 'B', 10, 100);
    add(bk, 5, 2, 'S', 10, 100);
    add(bk, 9, 3, 'B', 10, 100);
    del(bk, 3);
    add(bk, 9, 4, 'B', 10, 100);
    add(bk, 9, 5, 'B', 10, 100);

    const auto dist = bk.live_by_locate();
    EXPECT_EQ(dist[5], 2);
    EXPECT_EQ(dist[9], 2);
    EXPECT_EQ(dist[0], 0);
    std::uint64_t total = 0;
    for (const std::uint32_t n : dist) total += n;
    EXPECT_EQ(total, bk.live());
}

}
