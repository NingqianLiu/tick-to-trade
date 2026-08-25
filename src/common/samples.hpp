#pragma once

#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sample {

class Log {
public:
    void reserve(std::size_t n) { buf_.assign(n, 0); }

    void add(std::uint64_t ns) noexcept {
        if (n_ < buf_.size()) {
            buf_[n_++] = ns;
        } else {
            ++over_;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] std::uint64_t over() const noexcept { return over_; }
    [[nodiscard]] const std::uint64_t* data() const noexcept { return buf_.data(); }

private:
    std::vector<std::uint64_t> buf_;
    std::size_t n_ = 0;
    std::uint64_t over_ = 0;
};

[[nodiscard]] inline bool make_dir(const std::string& dir) {
    for (std::size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        const std::string part = dir.substr(0, i);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

[[nodiscard]] inline bool write_csv(const std::string& dir, const std::string& name,
                                    const Log& log) {
    if (!make_dir(dir)) return false;
    std::FILE* f = std::fopen((dir + "/" + name).c_str(), "w");
    if (f == nullptr) return false;
    std::fputs("latency_ns\n", f);
    for (std::size_t i = 0; i < log.size(); ++i) {
        std::fprintf(f, "%llu\n", static_cast<unsigned long long>(log.data()[i]));
    }
    return std::fclose(f) == 0;
}

}
