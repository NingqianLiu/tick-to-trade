#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hist {

class Hist {
public:
    static constexpr std::uint64_t kBucketNs = 5;
    static constexpr std::size_t kFine = 4000;
    static constexpr std::uint64_t kFineNs = kBucketNs * kFine;
    static constexpr std::uint64_t kCoarseNs = 2000;
    static constexpr std::size_t kCoarse = 49900;
    static constexpr std::size_t kBuckets = kFine + kCoarse;
    static constexpr std::uint64_t kRangeNs = kFineNs + kCoarseNs * kCoarse;

    void add(std::uint64_t ns) noexcept {
        if (ns > max_) max_ = ns;
        if (ns >= kRangeNs) {
            ++over_;
            return;
        }
        ++count_[ns < kFineNs ? ns / kBucketNs
                              : kFine + (ns - kFineNs) / kCoarseNs];
        ++total_;
    }

    void clear() noexcept {
        count_.fill(0);
        total_ = 0;
        over_ = 0;
        max_ = 0;
    }

    [[nodiscard]] std::uint64_t samples() const noexcept { return total_ + over_; }
    [[nodiscard]] std::uint64_t over_range() const noexcept { return over_; }
    [[nodiscard]] std::uint64_t largest() const noexcept { return max_; }

    [[nodiscard]] std::uint64_t quantile(double q) const noexcept {
        const std::uint64_t all = samples();
        if (all == 0) return 0;
        std::uint64_t want = static_cast<std::uint64_t>(q * static_cast<double>(all));
        if (want >= all) want = all - 1;
        std::uint64_t seen = 0;
        for (std::size_t b = 0; b < kBuckets; ++b) {
            seen += count_[b];
            if (seen <= want) continue;
            return b < kFine ? (b + 1) * kBucketNs
                             : kFineNs + (b - kFine + 1) * kCoarseNs;
        }
        return max_;
    }

    void merge(const Hist& other) noexcept {
        for (std::size_t b = 0; b < kBuckets; ++b) count_[b] += other.count_[b];
        total_ += other.total_;
        over_ += other.over_;
        if (other.max_ > max_) max_ = other.max_;
    }

    [[nodiscard]] const std::array<std::uint64_t, kBuckets>& buckets() const noexcept {
        return count_;
    }

private:
    std::array<std::uint64_t, kBuckets> count_{};
    std::uint64_t total_ = 0, over_ = 0, max_ = 0;
};

}
