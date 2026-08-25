#include <gtest/gtest.h>

#include "common/hist.hpp"

namespace {

using hist::Hist;

TEST(Hist, a_sample_lands_in_the_bucket_that_covers_it) {
    Hist h;
    h.add(0);
    h.add(4);
    h.add(5);
    h.add(12);
    EXPECT_EQ(h.samples(), 4u);
    EXPECT_EQ(h.buckets()[0], 2u);
    EXPECT_EQ(h.buckets()[1], 1u);
    EXPECT_EQ(h.buckets()[2], 1u);
    EXPECT_EQ(h.largest(), 12u);
}

TEST(Hist, quantiles_come_out_where_the_samples_are) {
    Hist h;
    for (int i = 0; i < 1000; ++i) h.add(100);
    h.add(10000);
    EXPECT_EQ(h.quantile(0.5), 105u);
    EXPECT_EQ(h.quantile(0.999), 105u);
    EXPECT_EQ(h.quantile(0.9999), 10005u);
    EXPECT_EQ(h.largest(), 10000u);
}

TEST(Hist, samples_past_the_range_are_kept_apart) {
    Hist h;
    for (int i = 0; i < 99; ++i) h.add(1000);
    h.add(500000000);
    EXPECT_EQ(h.samples(), 100u);
    EXPECT_EQ(h.over_range(), 1u);
    EXPECT_EQ(h.largest(), 500000000u);
    EXPECT_EQ(h.quantile(0.5), 1005u);
    EXPECT_EQ(h.quantile(0.999), 500000000u);
}

TEST(Hist, a_tail_past_the_fine_buckets_still_gets_a_number) {
    Hist h;
    for (int i = 0; i < 990; ++i) h.add(8000);
    for (int i = 0; i < 10; ++i) h.add(1761012);
    EXPECT_EQ(h.quantile(0.5), 8005u);
    const std::uint64_t tail = h.quantile(0.999);
    EXPECT_GT(tail, 1761000u);
    EXPECT_LT(tail, 1763100u);
    EXPECT_EQ(h.over_range(), 0u);
}

TEST(Hist, merging_pools_the_samples) {
    Hist quiet, busy, all;
    for (int i = 0; i < 10; ++i) quiet.add(100);
    for (int i = 0; i < 990; ++i) busy.add(5000);
    all.merge(quiet);
    all.merge(busy);
    EXPECT_EQ(all.samples(), 1000u);
    EXPECT_EQ(all.quantile(0.5), 5005u);
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

}
