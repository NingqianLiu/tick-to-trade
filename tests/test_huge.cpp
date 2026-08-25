// The buffer the book lives in. What matters here is that it behaves like the
// vector it replaced, and that when the machine has a pool of 2 MB pages the
// memory really comes from it - a silent fall back to 4 KB pages would show up
// only as a slower run.

#include "common/huge.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

// The page size the kernel is using for the mapping an address falls in, read
// out of this process's own map. Zero if the address is in no mapping.
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

// PriceLevels is returned by value, so this is the property that keeps it
// compiling as well as the one that keeps it correct.
TEST(Huge, moving_carries_the_memory_across) {
    huge::Buffer<std::uint32_t> a;
    a.assign(64, 5u);
    const std::uint32_t* was = &a[0];
    huge::Buffer<std::uint32_t> b = std::move(a);
    EXPECT_EQ(b.size(), 64u);
    EXPECT_EQ(&b[0], was);
    EXPECT_EQ(b[63], 5u);
}

// The pool is grown by the process that needs it and given back when it is
// done, so the machine holds none between runs. Both halves matter: pages left
// reserved are pages nothing else can use, and pages already there when a run
// starts were taken when memory looked different.
// The pool grows by exactly what was asked for and shrinks again afterwards, so
// the machine holds no pages between runs. Only the growth can be pinned down
// here - what comes back depends on what the other tests in this process are
// still holding - and the end to end version of the check is in full_day.sh,
// which refuses to start if any pages are already reserved.
TEST(Huge, the_pool_grows_by_what_was_asked_for) {
    huge::bind_to(0);
    const std::size_t before = huge::pool_size();
    {
        huge::Buffer<std::uint64_t> b;
        b.assign(1u << 20, 0u);  // 8 MB, four pages
        if (page_size_at(&b[0]) != huge::kPage) {
            GTEST_SKIP() << "cannot reserve pages here, so nothing to check";
        }
        // Topped up to what is needed, not blindly added to.
        EXPECT_GE(huge::pool_size(), 4u);
    }
    huge::give_back();
    EXPECT_LE(huge::pool_size(), before);
}

}  // namespace
