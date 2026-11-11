#include "itchbt/util/histogram.hpp"
#include "itchbt/util/ticks.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <random>

namespace itchbt {
namespace {

TEST(Histogram, SmallValuesAreExact) {
    for (std::uint64_t v = 0; v < Histogram::kSubCount * 2; ++v) {
        EXPECT_EQ(Histogram::upper_bound_of(Histogram::index_of(v)), v) << v;
    }
}

TEST(Histogram, EveryValueFallsInsideItsBucketWithBoundedError) {
    std::mt19937_64 rng(3);
    for (int i = 0; i < 200'000; ++i) {
        const std::uint64_t v = rng() >> (rng() % 64);
        const std::size_t index = Histogram::index_of(v);
        ASSERT_LT(index, Histogram::kBuckets);
        const std::uint64_t upper = Histogram::upper_bound_of(index);
        ASSERT_GE(upper, v);
        // Relative error at most 1/32.
        ASSERT_LE(upper - v, v / Histogram::kSubCount + 1) << v;
    }
}

TEST(Histogram, BucketsAreContiguousAndIncreasing) {
    std::uint64_t previous = Histogram::upper_bound_of(0);
    for (std::size_t i = 1; i < Histogram::kBuckets - 1; ++i) {
        const std::uint64_t upper = Histogram::upper_bound_of(i);
        ASSERT_GT(upper, previous) << i;
        // The value just past one bucket's bound belongs to the next bucket.
        ASSERT_EQ(Histogram::index_of(previous + 1), i) << i;
        previous = upper;
    }
    EXPECT_EQ(Histogram::index_of(~std::uint64_t{0}), Histogram::kBuckets - 1);
}

TEST(Histogram, PercentilesOfAKnownDistribution) {
    const auto h = std::make_unique<Histogram>();
    for (std::uint64_t v = 1; v <= 1000; ++v) {
        h->record(v);
    }
    EXPECT_EQ(h->count(), 1000U);
    EXPECT_EQ(h->min(), 1U);
    EXPECT_EQ(h->max(), 1000U);
    EXPECT_DOUBLE_EQ(h->mean(), 500.5);
    const auto near = [](std::uint64_t actual, std::uint64_t expected) {
        return actual >= expected && actual <= expected + expected / 32 + 1;
    };
    EXPECT_TRUE(near(h->percentile(0.50), 500)) << h->percentile(0.50);
    EXPECT_TRUE(near(h->percentile(0.90), 900)) << h->percentile(0.90);
    EXPECT_TRUE(near(h->percentile(0.99), 990)) << h->percentile(0.99);
    EXPECT_EQ(h->percentile(1.0), 1000U); // never reported above the true maximum
    EXPECT_EQ(h->percentile(0.0), 1U);
}

TEST(Histogram, EmptyAndSingleSample) {
    const auto h = std::make_unique<Histogram>();
    EXPECT_EQ(h->percentile(0.99), 0U);
    EXPECT_EQ(h->max(), 0U);
    EXPECT_EQ(h->min(), 0U);
    h->record(12345);
    EXPECT_EQ(h->percentile(0.5), 12345U);
    EXPECT_EQ(h->percentile(0.999), 12345U);
}

TEST(Histogram, MergeAddsCounts) {
    const auto a = std::make_unique<Histogram>();
    const auto b = std::make_unique<Histogram>();
    for (std::uint64_t v = 0; v < 100; ++v) {
        a->record(10);
        b->record(1'000'000);
    }
    a->merge(*b);
    EXPECT_EQ(a->count(), 200U);
    EXPECT_EQ(a->min(), 10U);
    EXPECT_EQ(a->max(), 1'000'000U);
    EXPECT_EQ(a->percentile(0.5), 10U);
    EXPECT_EQ(a->percentile(0.75), 1'000'000U);
}

TEST(Ticks, CounterAdvancesAndCalibratesToAPlausibleRate) {
    const double ns_per_tick = calibrate_ns_per_tick();
    // Between a 10 GHz counter and a 1 MHz one.
    EXPECT_GT(ns_per_tick, 0.1);
    EXPECT_LT(ns_per_tick, 1000.0);
    const std::uint64_t t0 = ticks_begin();
    std::uint64_t t1 = ticks_end();
    for (int i = 0; i < 10'000'000 && t1 == t0; ++i) {
        t1 = ticks_end();
    }
    EXPECT_GT(t1, t0);
}

} // namespace
} // namespace itchbt
