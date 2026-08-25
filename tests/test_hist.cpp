#include <gtest/gtest.h>

#include "common/hist.hpp"

namespace {

using hist::Hist;

TEST(Hist, a_sample_lands_in_the_bucket_that_covers_it) {
    Hist h;
    h.add(0);
    h.add(4);   // same bucket as zero
    h.add(5);   // the next one
    h.add(12);
    EXPECT_EQ(h.samples(), 4u);
    EXPECT_EQ(h.buckets()[0], 2u);
    EXPECT_EQ(h.buckets()[1], 1u);
    EXPECT_EQ(h.buckets()[2], 1u);
    EXPECT_EQ(h.largest(), 12u);
}

TEST(Hist, quantiles_come_out_where_the_samples_are) {
    Hist h;
    // A thousand samples at 100 ns and one at 10 us. The median sits with the
    // crowd, and so does the ninety-nine point nine: one sample in a thousand
    // and one is not past it. Only the very top reaches the straggler.
    for (int i = 0; i < 1000; ++i) h.add(100);
    h.add(10000);
    EXPECT_EQ(h.quantile(0.5), 105u);
    EXPECT_EQ(h.quantile(0.999), 105u);
    EXPECT_EQ(h.quantile(0.9999), 10005u);
    EXPECT_EQ(h.largest(), 10000u);
}

// A percentile taken over a bucket that quietly swallowed everything past the
// range would read far too low, so samples past it are held apart.
TEST(Hist, samples_past_the_range_are_kept_apart) {
    Hist h;
    for (int i = 0; i < 99; ++i) h.add(1000);
    h.add(500000000);  // 500 ms, past the 100 ms the buckets cover
    EXPECT_EQ(h.samples(), 100u);
    EXPECT_EQ(h.over_range(), 1u);
    EXPECT_EQ(h.largest(), 500000000u);
    EXPECT_EQ(h.quantile(0.5), 1005u);
    EXPECT_EQ(h.quantile(0.999), 500000000u);  // not the top of the range
}

// The first end to end run put more than one sample in a hundred past twenty
// microseconds, which the fine buckets alone could not name. The coarse half
// has to give a real number there rather than falling off the end.
TEST(Hist, a_tail_past_the_fine_buckets_still_gets_a_number) {
    Hist h;
    for (int i = 0; i < 990; ++i) h.add(8000);   // 8 us, in the fine half
    for (int i = 0; i < 10; ++i) h.add(1761012); // 1.76 ms, in the coarse half
    EXPECT_EQ(h.quantile(0.5), 8005u);
    const std::uint64_t tail = h.quantile(0.999);
    EXPECT_GT(tail, 1761000u);
    EXPECT_LT(tail, 1763100u);  // named to within one coarse bucket
    EXPECT_EQ(h.over_range(), 0u);
}

// Windows are pooled and then measured, never measured and then averaged: a
// quiet window would otherwise count for as much as a busy one.
TEST(Hist, merging_pools_the_samples) {
    Hist quiet, busy, all;
    for (int i = 0; i < 10; ++i) quiet.add(100);
    for (int i = 0; i < 990; ++i) busy.add(5000);
    all.merge(quiet);
    all.merge(busy);
    EXPECT_EQ(all.samples(), 1000u);
    EXPECT_EQ(all.quantile(0.5), 5005u);  // the busy window decides the median
    EXPECT_EQ(all.quantile(0.001), 105u);
}

TEST(Hist, an_empty_histogram_answers_zero) {
    Hist h;
    EXPECT_EQ(h.samples(), 0u);
    EXPECT_EQ(h.quantile(0.5), 0u);
    h.add(7);
    h.clear();
    EXPECT_EQ(h.samples(), 0u);
    EXPECT_EQ(h.largest(), 0u);
}

}  // namespace
