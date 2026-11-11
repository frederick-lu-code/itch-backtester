// Order book behaviour, written as a typed test so every book implementation runs the same
// cases. Add an implementation to BookTypes and it must pass all of them unchanged.

#include "book_types.hpp"

#include "itchbt/book/book.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace itchbt::book {
namespace {

constexpr Locate kLoc = 42;

template <typename Books>
class BookTest : public ::testing::Test {
protected:
    Books books;

    [[nodiscard]] std::vector<LevelView> bids(Locate locate = kLoc) const {
        return levels(books.book(locate).bids);
    }
    [[nodiscard]] std::vector<LevelView> asks(Locate locate = kLoc) const {
        return levels(books.book(locate).asks);
    }
    void expect_consistent() const {
        std::string error;
        EXPECT_TRUE(books.verify(error)) << error;
        EXPECT_EQ(books.anomalies().total(), 0U);
    }

private:
    template <typename Side>
    static std::vector<LevelView> levels(const Side& side) {
        std::vector<LevelView> out;
        side.for_each([&](const LevelView& v) {
            out.push_back(v);
            return true;
        });
        return out;
    }
};

TYPED_TEST_SUITE(BookTest, test::BookTypes);

TYPED_TEST(BookTest, StartsEmpty) {
    EXPECT_FALSE(this->books.book(kLoc).best_bid());
    EXPECT_FALSE(this->books.book(kLoc).best_ask());
    EXPECT_FALSE(this->books.book(kLoc).crossed_or_locked());
    EXPECT_EQ(this->books.live_orders(), 0U);
    EXPECT_EQ(this->books.book(kLoc).bids.level(100), (LevelView{100, 0, 0}));
    this->expect_consistent();
}

TYPED_TEST(BookTest, AddCreatesALevelAndReportsTheChange) {
    const BookChange c = this->books.add(1, kLoc, Side::Buy, 300, 1000);
    EXPECT_TRUE(c.applied);
    EXPECT_TRUE(c.top_changed);
    EXPECT_FALSE(c.order_removed);
    EXPECT_EQ(c.side, Side::Buy);
    EXPECT_EQ(c.locate, kLoc);
    EXPECT_EQ(c.price, 1000U);
    EXPECT_EQ(c.delta, 300U);
    EXPECT_EQ(c.level_before, 0U);
    EXPECT_EQ(c.level_after, 300U);
    EXPECT_EQ(c.seq, 1U);
    EXPECT_EQ(this->books.book(kLoc).best_bid(), (LevelView{1000, 300, 1}));
    EXPECT_FALSE(this->books.book(kLoc).best_ask());
    this->expect_consistent();
}

TYPED_TEST(BookTest, OrdersAtOnePriceAggregate) {
    this->books.add(1, kLoc, Side::Sell, 100, 2000);
    const BookChange c = this->books.add(2, kLoc, Side::Sell, 250, 2000);
    EXPECT_EQ(c.level_before, 100U);
    EXPECT_EQ(c.level_after, 350U);
    EXPECT_TRUE(c.top_changed); // size at the best price changed
    EXPECT_EQ(this->books.book(kLoc).best_ask(), (LevelView{2000, 350, 2}));
    this->expect_consistent();
}

TYPED_TEST(BookTest, BestBidIsHighestAndBestAskIsLowest) {
    this->books.add(1, kLoc, Side::Buy, 10, 990);
    this->books.add(2, kLoc, Side::Buy, 10, 1000);
    this->books.add(3, kLoc, Side::Buy, 10, 980);
    this->books.add(4, kLoc, Side::Sell, 10, 1020);
    this->books.add(5, kLoc, Side::Sell, 10, 1010);
    this->books.add(6, kLoc, Side::Sell, 10, 1030);
    EXPECT_EQ(this->books.book(kLoc).best_bid()->price, 1000U);
    EXPECT_EQ(this->books.book(kLoc).best_ask()->price, 1010U);
    EXPECT_EQ(this->bids(), (std::vector<LevelView>{{1000, 10, 1}, {990, 10, 1}, {980, 10, 1}}));
    EXPECT_EQ(this->asks(), (std::vector<LevelView>{{1010, 10, 1}, {1020, 10, 1}, {1030, 10, 1}}));
    this->expect_consistent();
}

TYPED_TEST(BookTest, TopChangedOnlyWhenTheBestLevelIsTouched) {
    EXPECT_TRUE(this->books.add(1, kLoc, Side::Buy, 10, 1000).top_changed);
    EXPECT_FALSE(this->books.add(2, kLoc, Side::Buy, 10, 990).top_changed); // behind the best
    EXPECT_TRUE(this->books.add(3, kLoc, Side::Buy, 10, 1010).top_changed); // new best price
    EXPECT_FALSE(this->books.reduce(2, 5).top_changed);
    EXPECT_FALSE(this->books.remove(1).top_changed);
    EXPECT_TRUE(this->books.reduce(3, 5).top_changed);
    EXPECT_TRUE(this->books.remove(3).top_changed); // best level disappears
    EXPECT_EQ(this->books.book(kLoc).best_bid(), (LevelView{990, 5, 1}));

    // Asks mirror bids: lower is better.
    EXPECT_TRUE(this->books.add(10, kLoc, Side::Sell, 10, 1100).top_changed);
    EXPECT_FALSE(this->books.add(11, kLoc, Side::Sell, 10, 1110).top_changed);
    EXPECT_TRUE(this->books.add(12, kLoc, Side::Sell, 10, 1090).top_changed);
    this->expect_consistent();
}

TYPED_TEST(BookTest, PartialExecutionReducesOrderAndLevel) {
    this->books.add(1, kLoc, Side::Buy, 300, 1000);
    this->books.add(2, kLoc, Side::Buy, 200, 1000);
    const BookChange c = this->books.reduce(1, 100);
    EXPECT_TRUE(c.applied);
    EXPECT_FALSE(c.order_removed);
    EXPECT_EQ(c.delta, 100U);
    EXPECT_EQ(c.level_before, 500U);
    EXPECT_EQ(c.level_after, 400U);
    EXPECT_EQ(c.seq, 1U);
    EXPECT_EQ(c.price, 1000U);
    EXPECT_EQ(this->books.find_order(1)->qty, 200U);
    EXPECT_EQ(this->books.book(kLoc).best_bid(), (LevelView{1000, 400, 2}));
    this->expect_consistent();
}

TYPED_TEST(BookTest, ExecutionToZeroRemovesTheOrder) {
    this->books.add(1, kLoc, Side::Buy, 300, 1000);
    this->books.add(2, kLoc, Side::Buy, 200, 1000);
    this->books.reduce(1, 100);
    const BookChange c = this->books.reduce(1, 200);
    EXPECT_TRUE(c.order_removed);
    EXPECT_EQ(c.level_after, 200U);
    EXPECT_EQ(this->books.find_order(1), nullptr);
    EXPECT_EQ(this->books.live_orders(), 1U);
    EXPECT_EQ(this->books.book(kLoc).best_bid(), (LevelView{1000, 200, 1}));
    this->expect_consistent();
}

TYPED_TEST(BookTest, LastOrderLeavingRemovesTheLevel) {
    this->books.add(1, kLoc, Side::Sell, 100, 2000);
    this->books.add(2, kLoc, Side::Sell, 100, 2010);
    const BookChange c = this->books.reduce(1, 100);
    EXPECT_TRUE(c.order_removed);
    EXPECT_TRUE(c.top_changed);
    EXPECT_EQ(c.level_after, 0U);
    EXPECT_EQ(this->books.book(kLoc).asks.level_count(), 1U);
    EXPECT_EQ(this->books.book(kLoc).best_ask()->price, 2010U);
    EXPECT_EQ(this->books.book(kLoc).asks.level(2000), (LevelView{2000, 0, 0}));
    this->expect_consistent();
}

TYPED_TEST(BookTest, CancelReducesAndDeleteRemoves) {
    this->books.add(1, kLoc, Side::Buy, 300, 1000);
    const BookChange cancel = this->books.reduce(1, 50); // X
    EXPECT_EQ(cancel.delta, 50U);
    EXPECT_FALSE(cancel.order_removed);
    EXPECT_EQ(this->books.find_order(1)->qty, 250U);

    const BookChange del = this->books.remove(1); // D
    EXPECT_TRUE(del.applied);
    EXPECT_TRUE(del.order_removed);
    EXPECT_EQ(del.delta, 250U); // whatever was left
    EXPECT_EQ(del.level_before, 250U);
    EXPECT_EQ(del.level_after, 0U);
    EXPECT_EQ(this->books.live_orders(), 0U);
    EXPECT_FALSE(this->books.book(kLoc).best_bid());
    this->expect_consistent();
}

TYPED_TEST(BookTest, ReplaceInheritsSideAndSymbolAndTakesANewSequence) {
    this->books.add(1, kLoc, Side::Sell, 100, 2000);
    this->books.add(2, kLoc, Side::Sell, 100, 2000);
    const ReplaceChange c = this->books.replace(1, 3, 400, 2010);
    EXPECT_TRUE(c.removed.applied);
    EXPECT_TRUE(c.removed.order_removed);
    EXPECT_EQ(c.removed.price, 2000U);
    EXPECT_EQ(c.removed.delta, 100U);
    EXPECT_EQ(c.removed.seq, 1U);
    EXPECT_TRUE(c.added.applied);
    EXPECT_EQ(c.added.side, Side::Sell);
    EXPECT_EQ(c.added.locate, kLoc);
    EXPECT_EQ(c.added.price, 2010U);
    EXPECT_EQ(c.added.delta, 400U);
    EXPECT_EQ(c.added.seq, 3U);

    EXPECT_EQ(this->books.find_order(1), nullptr);
    const Order* const replaced = this->books.find_order(3);
    ASSERT_NE(replaced, nullptr);
    EXPECT_EQ(replaced->side, Side::Sell);
    EXPECT_EQ(replaced->locate, kLoc);
    EXPECT_EQ(replaced->qty, 400U);
    EXPECT_EQ(this->asks(), (std::vector<LevelView>{{2000, 100, 1}, {2010, 400, 1}}));
    this->expect_consistent();
}

TYPED_TEST(BookTest, ReplaceAtTheSamePriceStillLosesQueuePriority) {
    this->books.add(1, kLoc, Side::Buy, 100, 1000);
    this->books.add(2, kLoc, Side::Buy, 100, 1000);
    const ReplaceChange c = this->books.replace(1, 3, 100, 1000);
    EXPECT_GT(c.added.seq, this->books.find_order(2)->seq);
    EXPECT_EQ(this->books.book(kLoc).best_bid(), (LevelView{1000, 200, 2}));
    this->expect_consistent();
}

TYPED_TEST(BookTest, UnknownReferencesAreIgnoredAndCounted) {
    this->books.add(1, kLoc, Side::Buy, 100, 1000);
    EXPECT_FALSE(this->books.reduce(99, 10).applied);
    EXPECT_FALSE(this->books.remove(99).applied);
    const ReplaceChange r = this->books.replace(99, 100, 10, 1000);
    EXPECT_FALSE(r.removed.applied);
    EXPECT_FALSE(r.added.applied);
    EXPECT_EQ(this->books.anomalies().unknown_refs, 3U);
    EXPECT_EQ(this->books.find_order(100), nullptr); // a failed replace adds nothing
    EXPECT_EQ(this->books.book(kLoc).best_bid(), (LevelView{1000, 100, 1}));
    std::string error;
    EXPECT_TRUE(this->books.verify(error)) << error;
}

TYPED_TEST(BookTest, OperationsOnARemovedOrderAreUnknown) {
    this->books.add(1, kLoc, Side::Buy, 100, 1000);
    this->books.reduce(1, 100); // executed to zero
    EXPECT_FALSE(this->books.reduce(1, 1).applied);
    EXPECT_FALSE(this->books.remove(1).applied);
    EXPECT_EQ(this->books.anomalies().unknown_refs, 2U);
}

TYPED_TEST(BookTest, DuplicateReferenceIsIgnoredAndCounted) {
    this->books.add(1, kLoc, Side::Buy, 100, 1000);
    EXPECT_FALSE(this->books.add(1, kLoc, Side::Sell, 5, 2000).applied);
    EXPECT_EQ(this->books.anomalies().duplicate_refs, 1U);
    EXPECT_EQ(this->books.book(kLoc).best_bid(), (LevelView{1000, 100, 1}));
    EXPECT_FALSE(this->books.book(kLoc).best_ask());
    std::string error;
    EXPECT_TRUE(this->books.verify(error)) << error;
}

TYPED_TEST(BookTest, OverReductionIsClampedAndCounted) {
    this->books.add(1, kLoc, Side::Buy, 100, 1000);
    this->books.add(2, kLoc, Side::Buy, 100, 1000);
    const BookChange c = this->books.reduce(1, 150);
    EXPECT_TRUE(c.applied);
    EXPECT_TRUE(c.order_removed);
    EXPECT_EQ(c.delta, 100U);
    EXPECT_EQ(this->books.anomalies().over_reductions, 1U);
    EXPECT_EQ(this->books.book(kLoc).best_bid(), (LevelView{1000, 100, 1}));
    std::string error;
    EXPECT_TRUE(this->books.verify(error)) << error;
}

TYPED_TEST(BookTest, SymbolsAreIndependentButShareTheOrderStore) {
    this->books.add(1, 10, Side::Buy, 100, 1000);
    this->books.add(2, 20, Side::Buy, 200, 5000);
    EXPECT_EQ(this->books.book(10).best_bid(), (LevelView{1000, 100, 1}));
    EXPECT_EQ(this->books.book(20).best_bid(), (LevelView{5000, 200, 1}));
    // A reduce carries no locate: the order store finds the right book.
    this->books.reduce(2, 50);
    EXPECT_EQ(this->books.book(20).best_bid(), (LevelView{5000, 150, 1}));
    EXPECT_EQ(this->books.book(10).best_bid(), (LevelView{1000, 100, 1}));
    EXPECT_EQ(this->books.live_orders(), 2U);
    this->expect_consistent();
}

TYPED_TEST(BookTest, ExtremeLocatesAndPricesWork) {
    this->books.add(1, 0, Side::Buy, 1, 0);
    this->books.add(2, 65535, Side::Sell, 1, 0xFFFFFFFFU);
    this->books.add(3, 65535, Side::Sell, 1, 0);
    this->books.add(4, 0, Side::Buy, 1, 0xFFFFFFFFU);
    EXPECT_EQ(this->books.book(0).best_bid()->price, 0xFFFFFFFFU);
    EXPECT_EQ(this->books.book(65535).best_ask()->price, 0U);
    this->expect_consistent();
}

TYPED_TEST(BookTest, DepthReturnsTheBestLevelsInOrder) {
    for (Price i = 0; i < 5; ++i) {
        this->books.add(i + 1, kLoc, Side::Buy, 10 * (i + 1), 1000 - i);
        this->books.add(i + 101, kLoc, Side::Sell, 10 * (i + 1), 1001 + i);
    }
    std::array<LevelView, 3> out{};
    EXPECT_EQ(this->books.book(kLoc).bids.depth(out), 3U);
    EXPECT_EQ(out[0], (LevelView{1000, 10, 1}));
    EXPECT_EQ(out[2], (LevelView{998, 30, 1}));
    EXPECT_EQ(this->books.book(kLoc).asks.depth(out), 3U);
    EXPECT_EQ(out[0], (LevelView{1001, 10, 1}));
    EXPECT_EQ(out[2], (LevelView{1003, 30, 1}));

    std::array<LevelView, 8> wide{};
    EXPECT_EQ(this->books.book(kLoc).bids.depth(wide), 5U); // fewer levels than asked for
}

TYPED_TEST(BookTest, CrossedAndLockedBooksAreDetected) {
    this->books.add(1, kLoc, Side::Buy, 10, 1000);
    this->books.add(2, kLoc, Side::Sell, 10, 1001);
    EXPECT_FALSE(this->books.book(kLoc).crossed_or_locked());
    this->books.add(3, kLoc, Side::Sell, 10, 1000);
    EXPECT_TRUE(this->books.book(kLoc).crossed_or_locked()); // locked
    this->books.add(4, kLoc, Side::Buy, 10, 1005);
    EXPECT_TRUE(this->books.book(kLoc).crossed_or_locked()); // crossed
}

TYPED_TEST(BookTest, TracksPeakLiveOrdersAndSequence) {
    this->books.add(1, kLoc, Side::Buy, 10, 1000);
    this->books.add(2, kLoc, Side::Buy, 10, 1000);
    this->books.remove(1);
    this->books.add(3, kLoc, Side::Buy, 10, 1000);
    EXPECT_EQ(this->books.live_orders(), 2U);
    EXPECT_EQ(this->books.peak_live_orders(), 2U);
    EXPECT_EQ(this->books.last_sequence(), 3U);
}

TYPED_TEST(BookTest, ApplyDrivesTheBookFromDecodedMessages) {
    itch::AddOrder add;
    add.h.locate = kLoc;
    add.ref = 7;
    add.side = Side::Buy;
    add.qty = 500;
    add.price = 1000;
    EXPECT_TRUE(apply(this->books, add).applied);

    itch::OrderExecuted executed;
    executed.ref = 7;
    executed.qty = 100;
    EXPECT_EQ(apply(this->books, executed).level_after, 400U);

    itch::OrderExecutedWithPrice priced;
    priced.ref = 7;
    priced.qty = 100;
    priced.price = 999; // the execution price does not move the resting order's level
    const BookChange c = apply(this->books, priced);
    EXPECT_EQ(c.price, 1000U);
    EXPECT_EQ(c.level_after, 300U);

    itch::OrderCancel cancel;
    cancel.ref = 7;
    cancel.qty = 100;
    EXPECT_EQ(apply(this->books, cancel).level_after, 200U);

    itch::OrderReplace replace;
    replace.old_ref = 7;
    replace.new_ref = 8;
    replace.qty = 50;
    replace.price = 1001;
    EXPECT_TRUE(apply(this->books, replace).added.applied);
    EXPECT_EQ(this->books.book(kLoc).best_bid(), (LevelView{1001, 50, 1}));

    itch::OrderDelete del;
    del.ref = 8;
    EXPECT_TRUE(apply(this->books, del).order_removed);
    EXPECT_FALSE(this->books.book(kLoc).best_bid());
    this->expect_consistent();
}

TEST(PriorityKey, LargerKeyIsAlwaysABetterPrice) {
    EXPECT_GT(priority_key(Side::Buy, 101), priority_key(Side::Buy, 100));
    EXPECT_GT(priority_key(Side::Sell, 100), priority_key(Side::Sell, 101));
    for (const Price p : {Price{0}, Price{1}, Price{1'234'500}, Price{0xFFFFFFFFU}}) {
        EXPECT_EQ(price_of_key(Side::Buy, priority_key(Side::Buy, p)), p);
        EXPECT_EQ(price_of_key(Side::Sell, priority_key(Side::Sell, p)), p);
    }
}

} // namespace
} // namespace itchbt::book
