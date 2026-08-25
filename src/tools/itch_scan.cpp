#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "itch/reader.hpp"
#include "itch/types.hpp"

namespace {

constexpr std::size_t kReadBuffer = 32u << 20;
constexpr std::size_t kSecondsPerDay = 86400;
constexpr std::uint64_t kNsPerSec = 1000000000ull;

constexpr unsigned kBurstShift = 20;

struct Stats {
    std::uint64_t messages = 0;
    std::uint64_t body_bytes = 0;
    std::uint64_t type_count[256] = {};
    std::uint64_t type_bytes[256] = {};

    std::vector<std::uint64_t> sec_msgs = std::vector<std::uint64_t>(kSecondsPerDay);
    std::vector<std::uint64_t> sec_bytes = std::vector<std::uint64_t>(kSecondsPerDay);
    std::uint64_t out_of_day = 0;

    std::uint64_t burst_bucket = 0;
    std::uint64_t burst_msgs = 0;
    std::uint64_t peak_burst_msgs = 0;
    std::uint64_t peak_burst_at = 0;
    std::uint64_t burst_hist[32] = {};

    std::uint64_t first_ts = 0;
    std::uint64_t last_ts = 0;
    std::uint64_t ts_regressions = 0;

    std::uint64_t bad_length = 0;
    std::uint64_t unknown_type = 0;
    std::uint64_t before_system_hours = 0;

    std::uint64_t event_ts[256] = {};
    bool event_seen[256] = {};

    std::vector<std::array<char, itch::kStockSymbolLen>> symbols =
        std::vector<std::array<char, itch::kStockSymbolLen>>(65536);
    std::vector<bool> symbol_seen = std::vector<bool>(65536, false);
    std::uint64_t directory_entries = 0;
};

void close_burst(Stats& s) {
    if (s.burst_msgs == 0) return;
    if (s.burst_msgs > s.peak_burst_msgs) {
        s.peak_burst_msgs = s.burst_msgs;
        s.peak_burst_at = s.burst_bucket << kBurstShift;
    }
    ++s.burst_hist[63 - __builtin_clzll(s.burst_msgs)];
}

void on_message(Stats& s, const itch::Message& m) {
    const auto type = static_cast<unsigned char>(m.type());
    const std::uint8_t want = itch::kBodyLen[type];
    if (want == 0) {
        ++s.unknown_type;
    } else if (want != m.len) {
        ++s.bad_length;
    }

    ++s.type_count[type];
    s.type_bytes[type] += m.len;
    ++s.messages;
    s.body_bytes += m.len;

    const std::uint64_t ts = m.timestamp();
    if (s.messages == 1) {
        s.first_ts = ts;
        s.burst_bucket = ts >> kBurstShift;
    } else if (ts < s.last_ts) {
        ++s.ts_regressions;
    }
    s.last_ts = ts;

    const std::uint64_t sec = ts / kNsPerSec;
    if (sec < kSecondsPerDay) {
        ++s.sec_msgs[sec];
        s.sec_bytes[sec] += m.len;
    } else {
        ++s.out_of_day;
    }

    const std::uint64_t bucket = ts >> kBurstShift;
    if (bucket != s.burst_bucket) {
        close_burst(s);
        s.burst_bucket = bucket;
        s.burst_msgs = 0;
    }
    ++s.burst_msgs;

    if (type == 'S') {
        const auto code = static_cast<unsigned char>(m.event_code());
        if (!s.event_seen[code]) {
            s.event_seen[code] = true;
            s.event_ts[code] = ts;
        }
    } else if (type == 'R' && m.len >= itch::kStockSymbolOff + itch::kStockSymbolLen) {
        const std::uint16_t locate = m.stock_locate();
        if (!s.symbol_seen[locate]) {
            s.symbol_seen[locate] = true;
            std::memcpy(s.symbols[locate].data(), m.body + itch::kStockSymbolOff,
                        itch::kStockSymbolLen);
            ++s.directory_entries;
        }
    }

    if (!s.event_seen[static_cast<unsigned char>(itch::kEventStartOfSystemHours)]) {
        ++s.before_system_hours;
    }
}

std::string trim_symbol(const std::array<char, itch::kStockSymbolLen>& raw) {
    std::size_t n = raw.size();
    while (n > 0 && raw[n - 1] == ' ') --n;
    return std::string(raw.data(), n);
}

void write_outputs(const Stats& s, const std::filesystem::path& dir,
                   const std::string& input, std::uint64_t file_bytes,
                   std::uint64_t leftover, double seconds) {
    std::filesystem::create_directories(dir);

    if (FILE* f = std::fopen((dir / "rate_per_second.csv").c_str(), "w")) {
        std::fprintf(f, "second_since_midnight,messages,body_bytes\n");
        for (std::size_t i = 0; i < kSecondsPerDay; ++i) {
            if (s.sec_msgs[i] == 0) continue;
            std::fprintf(f, "%zu,%" PRIu64 ",%" PRIu64 "\n", i, s.sec_msgs[i],
                         s.sec_bytes[i]);
        }
        std::fclose(f);
    }

    if (FILE* f = std::fopen((dir / "type_counts.csv").c_str(), "w")) {
        std::fprintf(f, "type,count,body_bytes,declared_len\n");
        for (int t = 0; t < 256; ++t) {
            if (s.type_count[t] == 0) continue;
            std::fprintf(f, "%c,%" PRIu64 ",%" PRIu64 ",%u\n", t, s.type_count[t],
                         s.type_bytes[t], itch::kBodyLen[t]);
        }
        std::fclose(f);
    }

    if (FILE* f = std::fopen((dir / "burst_windows.csv").c_str(), "w")) {
        std::fprintf(f, "min_messages,max_messages,windows\n");
        for (int i = 0; i < 32; ++i) {
            if (s.burst_hist[i] == 0) continue;
            std::fprintf(f, "%llu,%llu,%" PRIu64 "\n", 1ull << i,
                         (1ull << (i + 1)) - 1, s.burst_hist[i]);
        }
        std::fclose(f);
    }

    if (FILE* f = std::fopen((dir / "stock_directory.csv").c_str(), "w")) {
        std::fprintf(f, "stock_locate,symbol\n");
        for (std::size_t i = 0; i < s.symbol_seen.size(); ++i) {
            if (!s.symbol_seen[i]) continue;
            std::fprintf(f, "%zu,%s\n", i, trim_symbol(s.symbols[i]).c_str());
        }
        std::fclose(f);
    }

    std::uint64_t peak_sec_msgs = 0;
    std::size_t peak_sec = 0;
    for (std::size_t i = 0; i < kSecondsPerDay; ++i) {
        if (s.sec_msgs[i] > peak_sec_msgs) {
            peak_sec_msgs = s.sec_msgs[i];
            peak_sec = i;
        }
    }
    std::uint64_t active_seconds = 0;
    for (std::size_t i = 0; i < kSecondsPerDay; ++i) {
        if (s.sec_msgs[i] != 0) ++active_seconds;
    }

    FILE* f = std::fopen((dir / "summary.json").c_str(), "w");
    if (f == nullptr) return;
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"input\": \"%s\",\n", input.c_str());
    std::fprintf(f, "  \"bytes_read\": %" PRIu64 ",\n", file_bytes);
    std::fprintf(f, "  \"leftover_bytes\": %" PRIu64 ",\n", leftover);
    std::fprintf(f, "  \"messages\": %" PRIu64 ",\n", s.messages);
    std::fprintf(f, "  \"body_bytes\": %" PRIu64 ",\n", s.body_bytes);
    std::fprintf(f, "  \"framing_bytes\": %" PRIu64 ",\n",
                 s.body_bytes + s.messages * itch::kLenPrefix);
    std::fprintf(f, "  \"bad_length\": %" PRIu64 ",\n", s.bad_length);
    std::fprintf(f, "  \"unknown_type\": %" PRIu64 ",\n", s.unknown_type);
    std::fprintf(f, "  \"timestamp_regressions\": %" PRIu64 ",\n", s.ts_regressions);
    std::fprintf(f, "  \"messages_before_system_hours\": %" PRIu64 ",\n",
                 s.before_system_hours);
    std::fprintf(f, "  \"first_timestamp_ns\": %" PRIu64 ",\n", s.first_ts);
    std::fprintf(f, "  \"last_timestamp_ns\": %" PRIu64 ",\n", s.last_ts);
    std::fprintf(f, "  \"active_seconds\": %" PRIu64 ",\n", active_seconds);
    std::fprintf(f, "  \"mean_rate_per_active_second\": %.1f,\n",
                 active_seconds ? static_cast<double>(s.messages) / active_seconds : 0.0);
    std::fprintf(f, "  \"peak_second\": %zu,\n", peak_sec);
    std::fprintf(f, "  \"peak_messages_per_second\": %" PRIu64 ",\n", peak_sec_msgs);
    std::fprintf(f, "  \"burst_window_ns\": %u,\n", 1u << kBurstShift);
    std::fprintf(f, "  \"peak_messages_per_burst_window\": %" PRIu64 ",\n",
                 s.peak_burst_msgs);
    std::fprintf(f, "  \"peak_burst_at_ns\": %" PRIu64 ",\n", s.peak_burst_at);
    std::fprintf(f, "  \"stock_directory_entries\": %" PRIu64 ",\n",
                 s.directory_entries);
    std::fprintf(f, "  \"scan_seconds\": %.2f,\n", seconds);
    std::fprintf(f, "  \"system_events\": {");
    bool first = true;
    for (const char code : {'O', 'S', 'Q', 'M', 'E', 'C'}) {
        const auto c = static_cast<unsigned char>(code);
        if (!s.event_seen[c]) continue;
        std::fprintf(f, "%s\n    \"%c\": %" PRIu64, first ? "" : ",", code,
                     s.event_ts[c]);
        first = false;
    }
    std::fprintf(f, "%s}\n}\n", first ? "" : "\n  ");
    std::fclose(f);
}

void print_summary(const Stats& s, std::uint64_t file_bytes, std::uint64_t leftover,
                   double seconds) {
    std::printf("messages          %" PRIu64 "\n", s.messages);
    std::printf("bytes read        %" PRIu64 "\n", file_bytes);
    std::printf("framing bytes     %" PRIu64 "  (leftover %" PRIu64 ")\n",
                s.body_bytes + s.messages * itch::kLenPrefix, leftover);
    std::printf("bad length        %" PRIu64 "\n", s.bad_length);
    std::printf("unknown type      %" PRIu64 "\n", s.unknown_type);
    std::printf("ts regressions    %" PRIu64 "\n", s.ts_regressions);
    std::printf("directory entries %" PRIu64 "\n", s.directory_entries);
    std::printf("peak msgs / %.3f ms  %" PRIu64 "\n",
                static_cast<double>(1u << kBurstShift) / 1e6, s.peak_burst_msgs);
    for (const char code : {'O', 'S', 'Q', 'M', 'E', 'C'}) {
        const auto c = static_cast<unsigned char>(code);
        if (!s.event_seen[c]) continue;
        const std::uint64_t ts = s.event_ts[c];
        const std::uint64_t sec = ts / kNsPerSec;
        std::printf("event %c           %" PRIu64 " ns  (%02" PRIu64 ":%02" PRIu64
                    ":%02" PRIu64 " ET)\n",
                    code, ts, sec / 3600, (sec / 60) % 60, sec % 60);
    }
    std::printf("scan              %.2f s  (%.2f GB/s)\n", seconds,
                seconds > 0 ? static_cast<double>(file_bytes) / seconds / 1e9 : 0.0);
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: itch_scan <file> [--max-messages N] [--out DIR]\n");
        return 2;
    }
    const std::string input = argv[1];
    std::uint64_t max_messages = 0;
    std::filesystem::path out;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--max-messages") == 0 && i + 1 < argc) {
            max_messages = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (out.empty()) {
        out = std::filesystem::path("results") /
              ("exp0_" + std::filesystem::path(input).stem().string());
    }

    io::SeqReader reader(input.c_str(), kReadBuffer);
    if (!reader.ok()) {
        std::fprintf(stderr, "cannot open %s\n", input.c_str());
        return 1;
    }

    Stats stats;
    const auto started = std::chrono::steady_clock::now();
    bool stopped = false;
    while (!stopped && reader.fill()) {
        const auto r = itch::for_each_message(
            reader.data(), reader.size(), [&](const itch::Message& m) {
                on_message(stats, m);
                return max_messages == 0 || stats.messages < max_messages;
            });
        reader.consume(r.consumed);
        if (r.stop == itch::FrameStop::kZeroLength) {
            std::fprintf(stderr, "zero-length frame after %" PRIu64 " messages\n",
                         stats.messages);
            stopped = true;
        } else if (r.stop == itch::FrameStop::kCallerStopped) {
            stopped = true;
        } else if (r.consumed == 0) {
            break;
        }
    }
    close_burst(stats);

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
            .count();
    const std::uint64_t leftover = stopped ? 0 : reader.size();

    print_summary(stats, reader.total_bytes(), leftover, seconds);
    write_outputs(stats, out, input, reader.total_bytes(), leftover, seconds);
    std::printf("wrote             %s\n", out.c_str());

    if (!stopped && (leftover != 0 || stats.bad_length != 0 || stats.unknown_type != 0)) {
        return 1;
    }
    return 0;
}
