#include "common/huge.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

std::size_t page_size_at(const void* p) {
    const auto want = reinterpret_cast<std::uintptr_t>(p);
    std::FILE* f = std::fopen("/proc/self/smaps", "r");
    if (f == nullptr) return 0;
    char line[512];
    bool inside = false;
    std::size_t kb = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long long lo = 0, hi = 0;
        if (std::sscanf(line, "%llx-%llx", &lo, &hi) == 2) {
            inside = want >= lo && want < hi;
        } else if (inside && std::strncmp(line, "KernelPageSize:", 15) == 0) {
            std::sscanf(line + 15, "%zu", &kb);
            break;
        }
    }
    std::fclose(f);
    return kb * 1024;
}

std::size_t free_huge_pages() {
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (f == nullptr) return 0;
    char line[256];
    std::size_t n = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::sscanf(line, "HugePages_Free: %zu", &n) == 1) break;
    }
    std::fclose(f);
    return n;
}

TEST(Huge, assign_puts_the_value_in_every_slot) {
    huge::Buffer<std::uint32_t> b;
    b.assign(1000, 7u);
    ASSERT_EQ(b.size(), 1000u);
    EXPECT_EQ(b[0], 7u);
    EXPECT_EQ(b[999], 7u);
    EXPECT_EQ(b.back(), 7u);
}

TEST(Huge, resize_starts_everything_at_zero) {
    huge::Buffer<std::uint64_t> b;
    b.resize(4096);
    for (std::size_t i = 0; i < b.size(); ++i) ASSERT_EQ(b[i], 0u) << "at " << i;
}

TEST(Huge, asking_again_replaces_what_was_there) {
    huge::Buffer<std::uint32_t> b;
    b.assign(10, 1u);
    b.assign(1u << 20, 2u);
    ASSERT_EQ(b.size(), 1u << 20);
    EXPECT_EQ(b[0], 2u);
    EXPECT_EQ(b.back(), 2u);
}

TEST(Huge, nothing_asked_for_is_not_an_error) {
    huge::Buffer<std::uint32_t> b;
    b.assign(0, 1u);
    EXPECT_EQ(b.size(), 0u);
}

TEST(Huge, moving_carries_the_memory_across) {
    huge::Buffer<std::uint32_t> a;
    a.assign(64, 5u);
    const std::uint32_t* was = &a[0];
    huge::Buffer<std::uint32_t> b = std::move(a);
    EXPECT_EQ(b.size(), 64u);
    EXPECT_EQ(&b[0], was);
    EXPECT_EQ(b[63], 5u);
}

TEST(Huge, the_pool_grows_by_what_was_asked_for) {
    huge::bind_to(0);
    const std::size_t before = huge::pool_size();
    {
        huge::Buffer<std::uint64_t> b;
        b.assign(1u << 20, 0u);
        if (page_size_at(&b[0]) != huge::kPage) {
            GTEST_SKIP() << "cannot reserve pages here, so nothing to check";
        }
        EXPECT_GE(huge::pool_size(), 4u);
    }
    huge::give_back();
    EXPECT_LE(huge::pool_size(), before);
}

}
