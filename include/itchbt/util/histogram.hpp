#pragma once

// A fixed-size log-linear histogram for latency samples, in the style of HdrHistogram.
//
// Values below 2^kSubBits are counted exactly. Above that, each power of two is split into
// 2^kSubBits equal buckets, so the relative error is at most 1 / 2^kSubBits (about 3%) at any
// magnitude. The whole thing is one array of counters: recording a sample is a count-leading-
// zeros, a shift and an increment, and it never allocates.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace itchbt {

class Histogram {
public:
    static constexpr unsigned kSubBits = 5;
    static constexpr std::uint64_t kSubCount = std::uint64_t{1} << kSubBits;
    /// One linear group for values under 2^kSubBits, then one group per further power of two.
    static constexpr std::size_t kGroups = 64 - kSubBits + 1;
    static constexpr std::size_t kBuckets = kGroups * kSubCount;

    void record(std::uint64_t value) noexcept {
        ++counts_[index_of(value)];
        ++total_;
        max_ = value > max_ ? value : max_;
        min_ = value < min_ ? value : min_;
        sum_ += value;
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return total_ == 0 ? 0 : max_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return total_ == 0 ? 0 : min_; }
    [[nodiscard]] double mean() const noexcept {
        return total_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(total_);
    }

    /// The smallest recorded bucket value v such that at least `fraction` of samples are <= v.
    /// Reported as the bucket's upper bound, so it never understates a latency.
    [[nodiscard]] std::uint64_t percentile(double fraction) const noexcept {
        if (total_ == 0) {
            return 0;
        }
        const auto wanted = static_cast<std::uint64_t>(fraction * static_cast<double>(total_));
        // At least one sample, at most all of them.
        const std::uint64_t target = std::clamp<std::uint64_t>(wanted, 1, total_);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= target) {
                const std::uint64_t upper = upper_bound_of(i);
                return upper > max_ ? max_ : upper;
            }
        }
        return max_;
    }

    void merge(const Histogram& other) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) {
            counts_[i] += other.counts_[i];
        }
        total_ += other.total_;
        sum_ += other.sum_;
        max_ = other.max_ > max_ ? other.max_ : max_;
        min_ = other.min_ < min_ ? other.min_ : min_;
    }

    /// Bucket index for a value. Exposed for tests.
    [[nodiscard]] static constexpr std::size_t index_of(std::uint64_t value) noexcept {
        if (value < kSubCount) {
            return static_cast<std::size_t>(value);
        }
        // The top set bit picks the group; the next kSubBits bits pick the bucket within it.
        const unsigned top = 63U - static_cast<unsigned>(std::countl_zero(value));
        const unsigned shift = top - kSubBits;
        const std::uint64_t sub = (value >> shift) & (kSubCount - 1);
        return static_cast<std::size_t>((std::uint64_t{shift} + 1) * kSubCount + sub);
    }

    /// Largest value that falls in bucket `index`. Exposed for tests.
    [[nodiscard]] static constexpr std::uint64_t upper_bound_of(std::size_t index) noexcept {
        const std::uint64_t group = index / kSubCount;
        const std::uint64_t sub = index % kSubCount;
        if (group == 0) {
            return sub;
        }
        const auto shift = static_cast<unsigned>(group - 1);
        const std::uint64_t low = (kSubCount + sub) << shift;
        return low + ((std::uint64_t{1} << shift) - 1);
    }

private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t total_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t max_ = 0;
    std::uint64_t min_ = ~std::uint64_t{0};
};

} // namespace itchbt
