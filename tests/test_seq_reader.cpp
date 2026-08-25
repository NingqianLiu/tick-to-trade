#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "io/seq_reader.hpp"
#include "itch/framing.hpp"

namespace {

std::string write_sample(std::size_t records) {
    const std::string path =
        (std::filesystem::temp_directory_path() / "seq_reader_test.bin").string();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return {};
    for (std::size_t i = 0; i < records; ++i) {
        const std::uint16_t len = static_cast<std::uint16_t>(12 + (i % 29));
        std::fputc(len >> 8, f);
        std::fputc(len & 0xff, f);
        for (std::uint16_t b = 0; b < len; ++b) {
            std::fputc(b == 0 ? 'A' : static_cast<int>(i & 0xff), f);
        }
    }
    std::fclose(f);
    return path;
}

std::size_t count_with_buffer(const std::string& path, std::size_t cap,
                              std::uint64_t* bytes) {
    io::SeqReader reader(path.c_str(), cap);
    EXPECT_TRUE(reader.ok());
    std::size_t seen = 0;
    while (reader.fill()) {
        const auto r = itch::for_each_message(
            reader.data(), reader.size(), [&](const itch::Message& m) {
                EXPECT_EQ(m.type(), 'A');
                ++seen;
                return true;
            });
        reader.consume(r.consumed);
        if (r.consumed == 0) break;
    }
    EXPECT_EQ(reader.size(), 0);
    *bytes = reader.total_bytes();
    return seen;
}

TEST(SeqReader, buffer_sizes_agree) {
    const std::size_t records = 2000;
    const std::string path = write_sample(records);
    EXPECT_FALSE(path.empty());
    const auto file_size = std::filesystem::file_size(path);

    for (std::size_t cap : {48u, 64u, 4096u, 1u << 20}) {
        std::uint64_t bytes = 0;
        EXPECT_EQ(count_with_buffer(path, cap, &bytes), records);
        EXPECT_EQ(bytes, file_size);
    }
    std::filesystem::remove(path);
}

TEST(SeqReader, buffer_must_exceed_the_longest_record) {
    const std::string path = write_sample(200);
    io::SeqReader reader(path.c_str(), 16);
    EXPECT_TRUE(reader.ok());
    EXPECT_TRUE(reader.fill());
    const auto r = itch::for_each_message(reader.data(), reader.size(),
                                          [](const itch::Message&) { return true; });
    reader.consume(r.consumed);
    std::size_t stalled_at = 0;
    while (reader.fill()) {
        const auto step = itch::for_each_message(
            reader.data(), reader.size(), [](const itch::Message&) { return true; });
        reader.consume(step.consumed);
        if (step.consumed == 0) {
            stalled_at = reader.size();
            break;
        }
    }
    EXPECT_TRUE(stalled_at > 0);
    std::filesystem::remove(path);
}

TEST(SeqReader, missing_file) {
    io::SeqReader reader("/nonexistent/itch/file", 4096);
    EXPECT_FALSE(reader.ok());
}

}
