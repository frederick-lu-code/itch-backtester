#include "itchbt/util/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace itchbt {
namespace {

TEST(SpscRing, CapacityRoundsUpToAPowerOfTwo) {
    EXPECT_EQ(SpscRing<int>(1).capacity(), 2U);
    EXPECT_EQ(SpscRing<int>(2).capacity(), 2U);
    EXPECT_EQ(SpscRing<int>(3).capacity(), 4U);
    EXPECT_EQ(SpscRing<int>(1000).capacity(), 1024U);
    EXPECT_EQ(SpscRing<int>(1024).capacity(), 1024U);
}

TEST(SpscRing, IsFirstInFirstOut) {
    SpscRing<int> ring(8);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(ring.try_push(i));
    }
    EXPECT_EQ(ring.size_approx(), 5U);
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_EQ(ring.size_approx(), 0U);
}

TEST(SpscRing, PushFailsWhenFullAndPopFailsWhenEmpty) {
    SpscRing<int> ring(4);
    int out = 99;
    EXPECT_FALSE(ring.try_pop(out));
    EXPECT_EQ(out, 99); // untouched
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.try_push(i));
    }
    EXPECT_FALSE(ring.try_push(4));
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 0);
    EXPECT_TRUE(ring.try_push(4)); // room again
    EXPECT_FALSE(ring.try_push(5));
}

TEST(SpscRing, WrapsAroundManyTimes) {
    SpscRing<std::uint64_t> ring(4);
    std::uint64_t next_out = 0;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        ASSERT_TRUE(ring.try_push(i));
        if (i % 3 == 2) {
            for (int k = 0; k < 3; ++k) {
                std::uint64_t out = 0;
                ASSERT_TRUE(ring.try_pop(out));
                EXPECT_EQ(out, next_out++);
            }
        }
    }
}

struct Wide {
    std::uint64_t a;
    std::uint64_t b;
    std::uint64_t c;
};

TEST(SpscRing, TwoThreadsTransferEveryItemInOrderIntact) {
    // A small ring so both "full" and "empty" are hit constantly. Run under TSan in CI.
    SpscRing<Wide> ring(8);
    constexpr std::uint64_t kItems = 200'000;
    std::atomic<bool> failed{false};

    std::thread consumer([&] {
        Wide item{};
        for (std::uint64_t expected = 0; expected < kItems;) {
            if (ring.try_pop(item)) {
                // A torn or reordered item would break one of these.
                if (item.a != expected || item.b != expected * 2 || item.c != ~expected) {
                    failed.store(true);
                }
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
    });
    for (std::uint64_t i = 0; i < kItems;) {
        if (ring.try_push(Wide{i, i * 2, ~i})) {
            ++i;
        } else {
            std::this_thread::yield();
        }
    }
    consumer.join();
    EXPECT_FALSE(failed.load());
    EXPECT_EQ(ring.size_approx(), 0U);
}

} // namespace
} // namespace itchbt
