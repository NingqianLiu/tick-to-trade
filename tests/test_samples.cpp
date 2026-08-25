#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "common/samples.hpp"

namespace {

TEST(Samples, samples_come_back_in_the_order_they_arrived) {
    sample::Log log;
    log.reserve(4);
    log.add(10);
    log.add(20);
    log.add(30);
    EXPECT_EQ(log.size(), 3u);
    EXPECT_EQ(log.data()[0], 10u);
    EXPECT_EQ(log.data()[2], 30u);
    EXPECT_EQ(log.over(), 0u);
}

// A run that quietly stopped recording would have its percentiles taken over
// its early part alone, so the ones that did not fit have to be counted.
TEST(Samples, what_did_not_fit_is_counted_rather_than_dropped_silently) {
    sample::Log log;
    log.reserve(2);
    log.add(1);
    log.add(2);
    log.add(3);
    log.add(4);
    EXPECT_EQ(log.size(), 2u);
    EXPECT_EQ(log.over(), 2u);
    EXPECT_EQ(log.data()[1], 2u);  // the ones that fit are untouched
}

TEST(Samples, the_file_has_one_heading_and_one_number_per_line) {
    const std::string dir = std::string(::testing::TempDir()) + "samples_test/deeper";
    sample::Log log;
    log.reserve(8);
    log.add(5165);
    log.add(105800);

    ASSERT_TRUE(sample::write_csv(dir, "latency.csv", log));
    std::FILE* f = std::fopen((dir + "/latency.csv").c_str(), "r");
    ASSERT_NE(f, nullptr);
    char line[64];
    ASSERT_NE(std::fgets(line, sizeof(line), f), nullptr);
    EXPECT_STREQ(line, "latency_ns\n");
    ASSERT_NE(std::fgets(line, sizeof(line), f), nullptr);
    EXPECT_STREQ(line, "5165\n");
    ASSERT_NE(std::fgets(line, sizeof(line), f), nullptr);
    EXPECT_STREQ(line, "105800\n");
    EXPECT_EQ(std::fgets(line, sizeof(line), f), nullptr);
    std::fclose(f);
}

TEST(Samples, an_empty_run_still_writes_a_file_with_its_heading) {
    const std::string dir = std::string(::testing::TempDir()) + "samples_empty";
    sample::Log log;
    log.reserve(1);
    ASSERT_TRUE(sample::write_csv(dir, "latency.csv", log));
    std::FILE* f = std::fopen((dir + "/latency.csv").c_str(), "r");
    ASSERT_NE(f, nullptr);
    char line[64];
    ASSERT_NE(std::fgets(line, sizeof(line), f), nullptr);
    EXPECT_STREQ(line, "latency_ns\n");
    EXPECT_EQ(std::fgets(line, sizeof(line), f), nullptr);
    std::fclose(f);
}

}  // namespace
