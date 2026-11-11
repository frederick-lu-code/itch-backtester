// Property tests: on random event streams every book implementation must equal the baseline
// after every single message, and the flat order map must behave exactly like
// std::unordered_map under random inserts, erases and lookups.

#include "support/random_stream.hpp"

#include "itchbt/book/book.hpp"
#include "itchbt/itch/parser.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace itchbt::book {
namespace {

/// Applies each book message to a baseline book and a candidate, comparing after every one.
template <typename Candidate>
struct Differ : itch::NullHandler {
    using itch::NullHandler::on;

    BaselineBooks baseline;
    Candidate candidate;
    std::vector<Locate> locates;
    std::uint64_t compared = 0;
    std::string failure;

    template <typename Message>
    void both(const Message& m) {
        const auto expected = apply(baseline, m);
        const auto actual = apply(candidate, m);
        check_change(expected, actual);
        compare_books();
    }
    void on(const itch::AddOrder& m) { both(m); }
    void on(const itch::OrderExecuted& m) { both(m); }
    void on(const itch::OrderExecutedWithPrice& m) { both(m); }
    void on(const itch::OrderCancel& m) { both(m); }
    void on(const itch::OrderDelete& m) { both(m); }
    void on(const itch::OrderReplace& m) { both(m); }

    void check_change(const BookChange& e, const BookChange& a) {
        if (e.applied != a.applied || e.top_changed != a.top_changed ||
            e.order_removed != a.order_removed || e.price != a.price || e.delta != a.delta ||
            e.level_before != a.level_before || e.level_after != a.level_after || e.seq != a.seq ||
            e.side != a.side || e.locate != a.locate) {
            fail("change record differs");
        }
    }
    void check_change(const ReplaceChange& e, const ReplaceChange& a) {
        check_change(e.removed, a.removed);
        check_change(e.added, a.added);
    }

    void compare_books() {
        ++compared;
        for (const Locate locate : locates) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                std::vector<LevelView> expected;
                std::vector<LevelView> actual;
                baseline.book(locate).side(side).for_each([&](const LevelView& v) {
                    expected.push_back(v);
                    return true;
                });
                candidate.book(locate).side(side).for_each([&](const LevelView& v) {
                    actual.push_back(v);
                    return true;
                });
                if (expected != actual ||
                    baseline.book(locate).side(side).best() !=
                        candidate.book(locate).side(side).best() ||
                    baseline.book(locate).side(side).level_count() !=
                        candidate.book(locate).side(side).level_count()) {
                    fail("levels differ");
                }
            }
        }
        if (baseline.live_orders() != candidate.live_orders()) {
            fail("live order count differs");
        }
    }

    void fail(const char* what) {
        if (failure.empty()) {
            failure = std::string(what) + " after " + std::to_string(compared) + " messages";
        }
    }
};

template <typename Candidate>
class BookEquivalence : public ::testing::Test {};
using Candidates = ::testing::Types<OptimisedBooks, FlatOrderBooks, BinaryLevelBooks>;
TYPED_TEST_SUITE(BookEquivalence, Candidates);

TYPED_TEST(BookEquivalence, MatchesTheBaselineAfterEveryMessage) {
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 4ULL}) {
        test::RandomStreamOptions options;
        options.seed = seed;
        options.events = 15'000;
        // A wide price range on odd seeds for deep books, a narrow one on even seeds so
        // levels are created and destroyed constantly.
        options.price_range_ticks = seed % 2 == 1 ? 200 : 4;
        const test::ItchWriter stream = test::random_stream(options);

        Differ<TypeParam> differ;
        for (std::size_t i = 0; i < options.symbols.size(); ++i) {
            differ.locates.push_back(static_cast<Locate>(options.first_locate + i));
        }
        ASSERT_TRUE(itch::parse(stream.bytes(), differ).ok());
        EXPECT_TRUE(differ.failure.empty()) << "seed " << seed << ": " << differ.failure;
        EXPECT_GT(differ.compared, 10'000U);
        std::string error;
        EXPECT_TRUE(differ.candidate.verify(error)) << error;
        EXPECT_EQ(differ.candidate.anomalies().total(), 0U);
    }
}

TYPED_TEST(BookEquivalence, HandlesAnomaliesExactlyLikeTheBaseline) {
    // Unknown references, duplicates and over-reductions, mixed into valid flow.
    BaselineBooks baseline;
    TypeParam candidate;
    std::mt19937_64 rng(99);
    for (int i = 0; i < 20'000; ++i) {
        const OrderRef ref = rng() % 300; // small space: frequent duplicates and misses
        const auto qty = static_cast<Qty>(rng() % 500 + 1);
        const auto price = static_cast<Price>(1000 + rng() % 20);
        const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        switch (rng() % 4) {
        case 0:
            EXPECT_EQ(baseline.add(ref, 5, side, qty, price).applied,
                      candidate.add(ref, 5, side, qty, price).applied);
            break;
        case 1: EXPECT_EQ(baseline.reduce(ref, qty).delta, candidate.reduce(ref, qty).delta); break;
        case 2: EXPECT_EQ(baseline.remove(ref).applied, candidate.remove(ref).applied); break;
        default: {
            const OrderRef new_ref = rng() % 300;
            EXPECT_EQ(baseline.replace(ref, new_ref, qty, price).added.applied,
                      candidate.replace(ref, new_ref, qty, price).added.applied);
            break;
        }
        }
        ASSERT_EQ(baseline.live_orders(), candidate.live_orders()) << "step " << i;
        ASSERT_EQ(baseline.book(5).best_bid(), candidate.book(5).best_bid()) << "step " << i;
        ASSERT_EQ(baseline.book(5).best_ask(), candidate.book(5).best_ask()) << "step " << i;
    }
    EXPECT_EQ(baseline.anomalies().unknown_refs, candidate.anomalies().unknown_refs);
    EXPECT_EQ(baseline.anomalies().duplicate_refs, candidate.anomalies().duplicate_refs);
    EXPECT_EQ(baseline.anomalies().over_reductions, candidate.anomalies().over_reductions);
    EXPECT_GT(baseline.anomalies().total(), 1000U);
    std::string error;
    EXPECT_TRUE(candidate.verify(error)) << error;
}

TEST(FlatOrderMap, BehavesLikeUnorderedMapUnderRandomOperations) {
    FlatOrderMap flat; // starts at minimum capacity: growth is exercised many times
    std::unordered_map<OrderRef, Order> reference;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 200'000; ++i) {
        // Clustered keys (sequential, like real references) and a few scattered ones.
        const OrderRef ref = rng() % 8 == 0 ? rng() : rng() % 5'000;
        Order order;
        order.seq = static_cast<Sequence>(i);
        order.qty = static_cast<Qty>(rng());
        switch (rng() % 3) {
        case 0: ASSERT_EQ(flat.insert(ref, order), reference.emplace(ref, order).second); break;
        case 1:
            flat.erase(ref);
            reference.erase(ref);
            break;
        default: {
            const Order* const found = flat.find(ref);
            const auto it = reference.find(ref);
            ASSERT_EQ(found != nullptr, it != reference.end());
            if (found != nullptr) {
                ASSERT_EQ(found->seq, it->second.seq);
                ASSERT_EQ(found->qty, it->second.qty);
            }
            break;
        }
        }
        ASSERT_EQ(flat.size(), reference.size());
    }
    EXPECT_GT(flat.rehashes(), 3U);
    std::size_t visited = 0;
    flat.for_each([&](OrderRef ref, const Order& order) {
        ++visited;
        const auto it = reference.find(ref);
        ASSERT_NE(it, reference.end());
        EXPECT_EQ(it->second.seq, order.seq);
    });
    EXPECT_EQ(visited, reference.size());
}

TEST(FlatOrderMap, EraseLeavesNoTombstonesBehind) {
    // Fill one probe run, erase from its middle, and check everything is still reachable.
    FlatOrderMap flat(64);
    const std::size_t capacity = flat.capacity();
    std::vector<OrderRef> refs;
    for (OrderRef ref = 0; refs.size() < capacity / 2; ++ref) {
        ASSERT_TRUE(flat.insert(ref, Order{}));
        refs.push_back(ref);
    }
    for (std::size_t i = 0; i < refs.size(); i += 2) {
        flat.erase(refs[i]);
    }
    for (std::size_t i = 0; i < refs.size(); ++i) {
        EXPECT_EQ(flat.find(refs[i]) != nullptr, i % 2 == 1) << refs[i];
    }
    EXPECT_EQ(flat.capacity(), capacity);
    EXPECT_EQ(flat.rehashes(), 0U);
    // Erasing everything returns the table to empty, so a fresh probe ends at once.
    for (const OrderRef ref : refs) {
        flat.erase(ref);
    }
    EXPECT_EQ(flat.size(), 0U);
    EXPECT_EQ(flat.find(12345), nullptr);
}

TEST(FlatOrderMap, SizedUpFrontItNeverRehashes) {
    FlatOrderMap flat(10'000);
    for (OrderRef ref = 1; ref <= 10'000; ++ref) {
        ASSERT_TRUE(flat.insert(ref, Order{}));
    }
    EXPECT_EQ(flat.rehashes(), 0U);
    EXPECT_LE(flat.size() * 2, flat.capacity());
}

TEST(FlatOrderMap, ExtremeReferences) {
    FlatOrderMap flat;
    EXPECT_TRUE(flat.insert(0, Order{}));
    EXPECT_FALSE(flat.insert(0, Order{}));
    EXPECT_TRUE(flat.insert(0xFFFFFFFFFFFFFFFEULL, Order{}));
    // The one unrepresentable reference is refused rather than corrupting the table.
    EXPECT_FALSE(flat.insert(0xFFFFFFFFFFFFFFFFULL, Order{}));
    EXPECT_EQ(flat.find(0xFFFFFFFFFFFFFFFFULL), nullptr);
    flat.erase(0xFFFFFFFFFFFFFFFFULL); // must not disturb the count
    EXPECT_NE(flat.find(0), nullptr);
    EXPECT_EQ(flat.size(), 2U);
}

} // namespace
} // namespace itchbt::book
