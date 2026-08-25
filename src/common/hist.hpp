#pragma once

// Latency samples counted into buckets rather than kept.
//
// Keeping them sounds simpler, and it is what the sample log next door does at the end of
// a run - but not from inside the measured code. Millions of samples is about a gigabyte
// of writes, and those writes sweep through the cache the measured code is using, so the
// act of measuring changes what is being measured. Counting into a fixed set of buckets is
// an index and an increment, and the few tens of kilobytes it touches stay in the level
// two cache.
//
// The buckets come in two widths, and that decides which printed numbers can be trusted:
//   below 20 us   5 ns a bucket    where almost every sample lands, and percentiles are exact
//   above 20 us   2 us a bucket    only the tail lands here
//
// A percentile that lands in the coarse half is reported as the top edge of its bucket,
// not as a value that was measured. A suspiciously round number like 26,000 is the
// giveaway: the real value is somewhere within two microseconds below it. One reading of
// 26,000 was really 24,579. For an exact figure up there the raw samples have to be sorted
// instead.
//
// So the tick to trade line a run prints cannot be used above 20 us. Comparing two runs is
// where this bites hardest: if both p99.9 values land in the same two microsecond bucket
// the printed numbers are identical, which looks like nothing changed while the difference
// could be nineteen hundred nanoseconds.
//
// The coarse half has to exist at all because the first full run showed one sample in every
// hundred above twenty microseconds, and a percentile that falls outside the buckets is not
// a measurement - it is a shrug.

#include <array>
#include <cstddef>
#include <cstdint>

namespace hist {

// One distribution. Each thread keeps its own and they are merged at the end, so the hot
// path never writes across threads.
class Hist {
public:
    // Five nanoseconds a bucket, not one: finer means more buckets, and four thousand of
    // them is already thirty two kilobytes. Any finer would not fit the level two cache,
    // and then recording a sample would reach out to memory every time.
    static constexpr std::uint64_t kBucketNs = 5;
    // Four thousand of them is exactly twenty microseconds. The boundary is not arbitrary:
    // the target is a p99.9 inside forty microseconds while almost every sample is a few
    // microseconds, so everything that has to be exact is below here.
    static constexpr std::size_t kFine = 4000;  // out to 20 us
    static constexpr std::uint64_t kFineNs = kBucketNs * kFine;
    // Two microseconds a bucket, ten times wider than it used to be. Over a full day one
    // sample in a thousand was past ten milliseconds, and a percentile outside the buckets
    // can only be reported as the largest sample, which is not an answer. The resolution
    // given up costs nothing: two microseconds on a reading of a hundred thousand is two
    // percent, and the same reading moves twenty percent between runs anyway.
    static constexpr std::uint64_t kCoarseNs = 2000;
    // Which carries the range on to a hundred milliseconds. Anything larger is no longer a
    // latency - it is something else that happened in that run and needs looking at
    // separately.
    static constexpr std::size_t kCoarse = 49900;  // from there to 100 ms
    static constexpr std::size_t kBuckets = kFine + kCoarse;
    // Where the buckets end. A sample past this fits nowhere and is counted on its own.
    static constexpr std::uint64_t kRangeNs = kFineNs + kCoarseNs * kCoarse;

    // The only function here the hot path reaches, which is why it is barely more than an
    // increment.
    void add(std::uint64_t ns) noexcept {
        // The largest sample is kept separately. It is the only fallback for a sample past
        // the end of the buckets: without it such a percentile could only be reported as
        // the top of the range, which is a number nobody measured.
        if (ns > max_) max_ = ns;
        if (ns >= kRangeNs) {
            // Deliberately not counted in total_, which counts samples that have a bucket,
            // while samples() below includes it in the denominator - so a sample like this
            // cannot quietly pull the percentiles down.
            ++over_;
            // And no bucket is touched, since touching one would be a write past the end.
            return;
        }
        // Both halves in one expression: below the boundary divide by five; above it take
        // away what the fine half covers, divide by two thousand, and carry on after the
        // four thousandth bucket.
        ++count_[ns < kFineNs ? ns / kBucketNs
                              : kFine + (ns - kFineNs) / kCoarseNs];
        ++total_;
    }

    // This walks fifty thousand buckets, so it is only called on a window boundary and
    // never from the hot path.
    void clear() noexcept {
        count_.fill(0);
        total_ = 0;
        over_ = 0;
        // The largest has to go too, or one window's spike would follow every window after
        // it.
        max_ = 0;
    }

    // Every sample received, including the ones past the end of the buckets. Leaving those
    // out would make the denominator too small and put p99.9 at an earlier position than it
    // belongs.
    [[nodiscard]] std::uint64_t samples() const noexcept { return total_ + over_; }
    // How many were past the end. Clearly non zero means either the range needs raising or
    // the run was not normal - worth reading before any percentile.
    [[nodiscard]] std::uint64_t over_range() const noexcept { return over_; }
    // The largest sample seen, and the only figure here that was actually measured rather
    // than being a bucket edge.
    [[nodiscard]] std::uint64_t largest() const noexcept { return max_; }

    // The top edge of the bucket a given fraction of the samples falls in.
    //
    // The top edge, note, not a measured value. In the fine half a bucket is five
    // nanoseconds and the difference does not matter; in the coarse half it is two
    // microseconds and it shows.
    [[nodiscard]] std::uint64_t quantile(double q) const noexcept {
        const std::uint64_t all = samples();
        // An empty window is normal during a run, so this answers zero rather than failing.
        if (all == 0) return 0;
        // Which sample it is, counting from zero, once they are in order.
        std::uint64_t want = static_cast<std::uint64_t>(q * static_cast<double>(all));
        // A q of 1.0 lands one past the end.
        if (want >= all) want = all - 1;
        // Add up from the smallest bucket until the position is covered.
        std::uint64_t seen = 0;
        for (std::size_t b = 0; b < kBuckets; ++b) {
            seen += count_[b];
            if (seen <= want) continue;
            // The two halves convert differently, hence the two expressions.
            return b < kFine ? (b + 1) * kBucketNs
                             : kFineNs + (b - kFine + 1) * kCoarseNs;
        }
        // Every bucket was added and the position is still not covered, so the sample is
        // among those past the end. The largest seen is reported: the top of the range
        // would be defensible too, but it is a made up number and this one was measured.
        return max_;
    }

    // Folds another distribution in - one per window, into the one the report quotes.
    //
    // It cannot be done by taking a percentile per window and averaging those: a quiet
    // window with a handful of samples would weigh as much as a busy one with tens of
    // thousands, and the tail lives entirely in the busy ones.
    void merge(const Hist& other) noexcept {
        for (std::size_t b = 0; b < kBuckets; ++b) count_[b] += other.count_[b];
        total_ += other.total_;
        // The ones past the end as well, or the merged denominator would be short.
        over_ += other.over_;
        // The largest is not something that can be added.
        if (other.max_ > max_) max_ = other.max_;
    }

    // The buckets themselves, for the script that draws the distribution. A const
    // reference, so this does not copy the four hundred kilobytes.
    [[nodiscard]] const std::array<std::uint64_t, kBuckets>& buckets() const noexcept {
        return count_;
    }

private:
    // Fifty four thousand buckets of eight bytes, about 430 KB, which is larger than the
    // level two cache - but the hot path keeps touching only the stretch where the latency
    // usually lands, so what stays in cache is a few tens of kilobytes.
    std::array<std::uint64_t, kBuckets> count_{};
    // Samples with a bucket, samples past the end, and the largest seen. Together on one
    // line because they are always read and cleared together.
    std::uint64_t total_ = 0, over_ = 0, max_ = 0;
};

}  // namespace hist
