#pragma once

// Every latency sample of a run, written out once the run is over.
//
// There is already a histogram, so why keep them all? Because a histogram only answers
// questions somebody thought of in advance, while the raw samples answer anything thought
// of afterwards: draw the distribution on a different scale, take a percentile nobody
// planned for, compare two runs directly - none of which needs the twenty minutes run
// again.
//
// There is a sharper reason as well. Above twenty microseconds the histogram's buckets are
// two microseconds wide, so what it prints up there is a bucket edge rather than a
// measurement. An exact tail percentile can only come from these samples.
//
// Is it too much data? An order goes out about every twenty packets, so a whole day is
// under two million samples, some fifteen megabytes. On the hot path this is one eight
// byte write into memory that was touched before the run started; the file is not opened
// until the last packet has gone by.

// mkdir. The output directory may not exist yet.
#include <sys/stat.h>

// errno and EEXIST: a directory that already exists is not a failure.
#include <cerrno>
// std::uint64_t - a sample is a count of nanoseconds.
#include <cstdint>
// std::FILE and the stdio calls that write the csv.
#include <cstdio>
// std::string, for joining a directory and a file name.
#include <string>
// std::vector, the buffer itself.
#include <vector>

namespace sample {

// One thread's samples.
class Log {
public:
    // assign rather than reserve, deliberately. reserve only claims the address space, so
    // the first write to each page would still have to ask the kernel for it. assign
    // writes every page, so the memory is really in hand before the run starts - the
    // difference being whether the hot path stalls for a few microseconds.
    void reserve(std::size_t n) { buf_.assign(n, 0); }

    // A full buffer does not stop the run, but it has to leave a trace, or the run would
    // quietly stop recording halfway and the percentiles would cover only the first half
    // with nobody the wiser.
    void add(std::uint64_t ns) noexcept {
        if (n_ < buf_.size()) {
            buf_[n_++] = ns;
        } else {
            // Counted and nothing more: the hot path can neither print nor grow.
            ++over_;
        }
    }

    // How many were recorded. Writing the file uses this rather than the buffer size,
    // since everything past it is still the zeros put there before the run.
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    // How many were dropped because the buffer filled. Anything other than zero makes the
    // run's percentiles unusable.
    [[nodiscard]] std::uint64_t over() const noexcept { return over_; }
    // The samples themselves, for writing them out and for sorting them into percentiles.
    [[nodiscard]] const std::uint64_t* data() const noexcept { return buf_.data(); }

private:
    // Claimed once before the run and never resized.
    std::vector<std::uint64_t> buf_;
    // Where the next sample goes, which is also how many there are.
    std::size_t n_ = 0;
    std::uint64_t over_ = 0;
};

// Creates a directory, and any level of it that is missing.
[[nodiscard]] inline bool make_dir(const std::string& dir) {
    // Walk the path and create each level as its separator comes up.
    for (std::size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        const std::string part = dir.substr(0, i);
        // Already existing is not a failure - going level by level, the first few usually
        // do exist.
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

// One number per line under a header, which is the shape the plotting tools want. It
// becomes <OUT>/latency.csv with a column called latency_ns, and the percentile script
// reads it directly - the only way to get the levels above 20 us right.
[[nodiscard]] inline bool write_csv(const std::string& dir, const std::string& name,
                                    const Log& log) {
    // The directory is usually named after the run, so it has to be created each time.
    if (!make_dir(dir)) return false;
    // A failure here is usually no permission, or a full disk.
    std::FILE* f = std::fopen((dir + "/" + name).c_str(), "w");
    if (f == nullptr) return false;
    std::fputs("latency_ns\n", f);
    // In the order they were recorded.
    for (std::size_t i = 0; i < log.size(); ++i) {
        std::fprintf(f, "%llu\n", static_cast<unsigned long long>(log.data()[i]));
    }
    // Closing is what actually flushes, so its result matters too: reporting success on
    // the strength of fopen alone would miss a full disk, which usually surfaces here.
    return std::fclose(f) == 0;
}

}  // namespace sample
