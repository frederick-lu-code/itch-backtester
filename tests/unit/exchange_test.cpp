// The fill model, case by case, driven directly: a historical book, the simulated exchange,
// and a list of the updates it emits.

#include "itchbt/book/book.hpp"
#include "itchbt/sim/exchange.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace itchbt::sim {
namespace {

constexpr Locate kLoc = 7;
constexpr Locate kOther = 9;
constexpr RejectReason kOpen = RejectReason::None;

class ExchangeTest : public ::testing::Test {
protected:
    book::BaselineBooks books;
    std::array<Locate, 2> traded{kLoc, kOther};
    ExchangeConfig config;
    SimExchange exchange{config, traded};
    std::vector<OrderUpdate> updates;
    Timestamp now = 1000;

    auto emit() {
        return [this](const OrderUpdate& u) { updates.push_back(u); };
    }

    /// Creates an order and delivers it to the exchange at once.
    SimOrder& send(Side side, OrderType type, Price price, Qty qty, RejectReason refusal = kOpen) {
        SimOrder* const order = exchange.create(kLoc, side, type, price, qty, now);
        exchange.on_new_order(*order, now, books, refusal, emit());
        return *order;
    }
    SimOrder& rest(Side side, Price price, Qty qty) {
        return send(side, OrderType::Limit, price, qty);
    }

    // Historical events, applied to the book and then shown to the exchange, as the engine does.
    void hist_add(OrderRef ref, Side side, Qty qty, Price price) {
        books.add(ref, kLoc, side, qty, price);
    }
    void hist_execute(OrderRef ref, Qty qty, bool fills_allowed = true) {
        const book::BookChange change = books.reduce(ref, qty);
        if (fills_allowed) {
            exchange.on_trade_price(kLoc, change.price, now, emit());
        }
        exchange.on_reduction(change, true, fills_allowed, now, emit());
    }
    void hist_cancel(OrderRef ref, Qty qty) {
        exchange.on_reduction(books.reduce(ref, qty), false, false, now, emit());
    }
    void hist_delete(OrderRef ref) {
        exchange.on_reduction(books.remove(ref), false, false, now, emit());
    }
    void hist_replace(OrderRef old_ref, OrderRef new_ref, Qty qty, Price price) {
        exchange.on_reduction(books.replace(old_ref, new_ref, qty, price).removed, false, false,
                              now, emit());
    }

    [[nodiscard]] std::vector<OrderUpdate> fills() const {
        std::vector<OrderUpdate> out;
        for (const OrderUpdate& u : updates) {
            if (u.kind == UpdateKind::Fill) {
                out.push_back(u);
            }
        }
        return out;
    }
};

TEST_F(ExchangeTest, OrderIdsCarryTheLocateAndCountPerSymbol) {
    const SimOrder* const a = exchange.create(kLoc, Side::Buy, OrderType::Limit, 100, 1, now);
    const SimOrder* const b = exchange.create(kOther, Side::Buy, OrderType::Limit, 100, 1, now);
    const SimOrder* const c = exchange.create(kLoc, Side::Buy, OrderType::Limit, 100, 1, now);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->id, make_order_id(kLoc, 1));
    EXPECT_EQ(b->id, make_order_id(kOther, 1)); // another symbol's orders do not shift the count
    EXPECT_EQ(c->id, make_order_id(kLoc, 2));
    EXPECT_EQ(locate_of(c->id), kLoc);
    EXPECT_EQ(counter_of(c->id), 2U);
    EXPECT_EQ(exchange.find(c->id), c);
    EXPECT_EQ(a->state, OrderState::PendingNew);
    EXPECT_EQ(a->leaves, 1U);
}

TEST_F(ExchangeTest, UnknownSymbolsAndIdsAreRefused) {
    EXPECT_EQ(exchange.create(1234, Side::Buy, OrderType::Limit, 100, 1, now), nullptr);
    EXPECT_EQ(exchange.find(make_order_id(1234, 1)), nullptr);
    EXPECT_EQ(exchange.find(make_order_id(kLoc, 1)), nullptr); // none created yet
    EXPECT_EQ(exchange.find(make_order_id(kLoc, 0)), nullptr);
    EXPECT_EQ(exchange.find(kInvalidOrderId), nullptr);
}

TEST_F(ExchangeTest, OrderPoolIsFixedSize) {
    ExchangeConfig small;
    small.max_orders_per_symbol = 2;
    SimExchange tiny(small, traded);
    EXPECT_NE(tiny.create(kLoc, Side::Buy, OrderType::Limit, 100, 1, now), nullptr);
    EXPECT_NE(tiny.create(kLoc, Side::Buy, OrderType::Limit, 100, 1, now), nullptr);
    EXPECT_EQ(tiny.create(kLoc, Side::Buy, OrderType::Limit, 100, 1, now), nullptr);
    EXPECT_NE(tiny.create(kOther, Side::Buy, OrderType::Limit, 100, 1, now), nullptr);
}

TEST_F(ExchangeTest, PassiveOrderJoinsTheBackOfItsLevel) {
    hist_add(1, Side::Buy, 300, 1000);
    hist_add(2, Side::Buy, 200, 1000);
    hist_add(3, Side::Buy, 900, 990); // another level: not ahead of us
    hist_add(4, Side::Sell, 100, 1010);
    const SimOrder& order = rest(Side::Buy, 1000, 100);

    EXPECT_EQ(order.state, OrderState::Live);
    EXPECT_EQ(order.shares_ahead, 500U);
    EXPECT_EQ(order.arrival_seq, 4U);
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].kind, UpdateKind::Accepted);
    EXPECT_EQ(updates[0].shares_ahead, 500U);
    EXPECT_EQ(updates[0].leaves, 100U);
    EXPECT_EQ(updates[0].state, OrderState::Live);
    EXPECT_EQ(updates[0].ts, now);
    EXPECT_EQ(exchange.open_orders(0), 1U);
    // The historical book never contains the simulated order.
    EXPECT_EQ(books.book(kLoc).bids.level(1000).qty, 500U);
}

TEST_F(ExchangeTest, AloneAtANewPriceThereIsNobodyAhead) {
    hist_add(1, Side::Buy, 300, 1000);
    hist_add(2, Side::Sell, 100, 1010);
    const SimOrder& order = rest(Side::Buy, 1005, 100); // inside the spread
    EXPECT_EQ(order.shares_ahead, 0U);
    EXPECT_EQ(order.state, OrderState::Live);
    EXPECT_TRUE(fills().empty());
}

TEST_F(ExchangeTest, ExecutionsAheadShortenTheQueueWithoutFilling) {
    hist_add(1, Side::Buy, 300, 1000);
    hist_add(2, Side::Buy, 200, 1000);
    const SimOrder& order = rest(Side::Buy, 1000, 100);
    hist_execute(1, 100);
    EXPECT_EQ(order.shares_ahead, 400U);
    hist_execute(1, 200); // order 1 gone
    EXPECT_EQ(order.shares_ahead, 200U);
    EXPECT_TRUE(fills().empty());
    EXPECT_EQ(order.state, OrderState::Live);
}

TEST_F(ExchangeTest, CancelsAndDeletesAheadShortenTheQueue) {
    hist_add(1, Side::Buy, 300, 1000);
    hist_add(2, Side::Buy, 200, 1000);
    const SimOrder& order = rest(Side::Buy, 1000, 100);
    hist_cancel(1, 50);
    EXPECT_EQ(order.shares_ahead, 450U);
    hist_delete(2);
    EXPECT_EQ(order.shares_ahead, 250U);
    EXPECT_TRUE(fills().empty());
}

TEST_F(ExchangeTest, ActivityBehindUsDoesNotMoveTheQueue) {
    hist_add(1, Side::Buy, 300, 1000);
    const SimOrder& order = rest(Side::Buy, 1000, 100);
    hist_add(2, Side::Buy, 500, 1000); // arrives after us
    hist_cancel(2, 100);
    EXPECT_EQ(order.shares_ahead, 300U);
    hist_delete(2);
    EXPECT_EQ(order.shares_ahead, 300U);
    // An execution of an order behind us while others are still ahead: not ours.
    hist_add(3, Side::Buy, 500, 1000);
    hist_execute(3, 200);
    EXPECT_EQ(order.shares_ahead, 300U);
    EXPECT_TRUE(fills().empty());
}

TEST_F(ExchangeTest, OtherLevelsAndTheOtherSideDoNotMoveTheQueue) {
    hist_add(1, Side::Buy, 300, 1000);
    hist_add(2, Side::Buy, 300, 990);
    hist_add(3, Side::Sell, 300, 1010);
    const SimOrder& order = rest(Side::Buy, 1000, 100);
    hist_delete(2);
    hist_cancel(3, 100);
    EXPECT_EQ(order.shares_ahead, 300U);
}

TEST_F(ExchangeTest, TheExecutionThatEmptiesTheQueueDoesNotFill) {
    hist_add(1, Side::Buy, 300, 1000);
    const SimOrder& order = rest(Side::Buy, 1000, 100);
    hist_execute(1, 300);
    EXPECT_EQ(order.shares_ahead, 0U);
    EXPECT_TRUE(fills().empty()); // "each further execution" fills, not this one
}

TEST_F(ExchangeTest, AtTheFrontEachExecutionFillsUpToItsSize) {
    hist_add(1, Side::Buy, 300, 1000);
    const SimOrder& order = rest(Side::Buy, 1000, 100);
    hist_add(2, Side::Buy, 500, 1000); // behind us
    hist_execute(1, 300);              // queue ahead is now empty
    updates.clear();

    hist_execute(2, 40);
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0].fill_qty, 40U);
    EXPECT_EQ(fills()[0].fill_price, 1000U);
    EXPECT_EQ(fills()[0].leaves, 60U);
    EXPECT_FALSE(fills()[0].aggressive);
    EXPECT_EQ(fills()[0].state, OrderState::PartiallyFilled);
    EXPECT_EQ(fills()[0].shares_ahead, 300U); // queue position at arrival, for analysis
    EXPECT_EQ(order.state, OrderState::PartiallyFilled);

    hist_execute(2, 200); // more than we have left
    ASSERT_EQ(fills().size(), 2U);
    EXPECT_EQ(fills()[1].fill_qty, 60U);
    EXPECT_EQ(fills()[1].leaves, 0U);
    EXPECT_EQ(fills()[1].state, OrderState::Filled);
    EXPECT_EQ(order.state, OrderState::Filled);
    EXPECT_EQ(exchange.open_orders(0), 0U);

    hist_execute(2, 100); // a filled order takes no further part
    EXPECT_EQ(fills().size(), 2U);
}

TEST_F(ExchangeTest, CancelAtTheFrontDoesNotFill) {
    const SimOrder& order = rest(Side::Sell, 1010, 100);
    hist_add(1, Side::Sell, 500, 1010);
    hist_cancel(1, 100);
    hist_delete(1);
    EXPECT_TRUE(fills().empty());
    EXPECT_EQ(order.leaves, 100U);
}

TEST_F(ExchangeTest, TradeThroughFillsABuyInFull) {
    hist_add(1, Side::Buy, 5000, 1000);
    hist_add(2, Side::Buy, 300, 990);
    const SimOrder& order = rest(Side::Buy, 1000, 100); // 5000 shares ahead
    // A seller hits the 990 bid: he accepted less than our 1000.
    hist_execute(2, 50);
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0].fill_qty, 100U);
    EXPECT_EQ(fills()[0].fill_price, 1000U); // at our limit
    EXPECT_EQ(order.state, OrderState::Filled);
}

TEST_F(ExchangeTest, TradeThroughFillsASellInFull) {
    hist_add(1, Side::Sell, 5000, 1010);
    hist_add(2, Side::Sell, 300, 1020);
    const SimOrder& order = rest(Side::Sell, 1010, 100);
    hist_execute(2, 50); // a buyer paid 1020 while we offered 1010
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0].fill_qty, 100U);
    EXPECT_EQ(fills()[0].fill_price, 1010U);
    EXPECT_EQ(order.state, OrderState::Filled);
}

TEST_F(ExchangeTest, TradeAtOurPriceIsNotATradeThrough) {
    hist_add(1, Side::Buy, 5000, 1000);
    const SimOrder& order = rest(Side::Buy, 1000, 100);
    exchange.on_trade_price(kLoc, 1000, now, emit());
    exchange.on_trade_price(kLoc, 1001, now, emit()); // better for the seller than our bid
    EXPECT_TRUE(fills().empty());
    EXPECT_EQ(order.leaves, 100U);
}

TEST_F(ExchangeTest, MarketableLimitSweepsLevelsUpToItsPriceThenRests) {
    hist_add(1, Side::Sell, 100, 1010);
    hist_add(2, Side::Sell, 200, 1020);
    hist_add(3, Side::Sell, 400, 1030);
    hist_add(4, Side::Sell, 400, 1040);
    const SimOrder& order = rest(Side::Buy, 1030, 500);

    ASSERT_EQ(updates.size(), 4U);
    EXPECT_EQ(updates[0].kind, UpdateKind::Accepted);
    EXPECT_EQ(updates[0].shares_ahead, 0U);
    ASSERT_EQ(fills().size(), 3U);
    EXPECT_EQ(fills()[0].fill_price, 1010U);
    EXPECT_EQ(fills()[0].fill_qty, 100U);
    EXPECT_EQ(fills()[1].fill_price, 1020U);
    EXPECT_EQ(fills()[1].fill_qty, 200U);
    EXPECT_EQ(fills()[2].fill_price, 1030U);
    EXPECT_EQ(fills()[2].fill_qty, 200U); // only what was left of the order
    EXPECT_TRUE(fills()[0].aggressive);
    EXPECT_EQ(order.state, OrderState::Filled);
    EXPECT_EQ(exchange.open_orders(0), 0U);
    // No market impact: the historical book is untouched.
    EXPECT_EQ(books.book(kLoc).asks.level(1010).qty, 100U);
    EXPECT_EQ(books.book(kLoc).asks.level(1030).qty, 400U);
}

TEST_F(ExchangeTest, MarketableLimitRemainderRests) {
    hist_add(1, Side::Sell, 100, 1010);
    hist_add(2, Side::Sell, 400, 1040);
    const SimOrder& order = rest(Side::Buy, 1020, 500);
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0].fill_qty, 100U);
    EXPECT_EQ(order.leaves, 400U);
    EXPECT_EQ(order.state, OrderState::PartiallyFilled);
    EXPECT_EQ(exchange.open_orders(0), 1U);
}

TEST_F(ExchangeTest, MarketableSellSweepsBidsDownward) {
    hist_add(1, Side::Buy, 100, 1000);
    hist_add(2, Side::Buy, 100, 990);
    hist_add(3, Side::Buy, 100, 980);
    rest(Side::Sell, 990, 300);
    ASSERT_EQ(fills().size(), 2U);
    EXPECT_EQ(fills()[0].fill_price, 1000U);
    EXPECT_EQ(fills()[1].fill_price, 990U);
}

TEST_F(ExchangeTest, IocFillsWhatItCanAndCancelsTheRest) {
    hist_add(1, Side::Sell, 100, 1010);
    hist_add(2, Side::Sell, 100, 1030);
    const SimOrder& order = send(Side::Buy, OrderType::Ioc, 1020, 300);

    ASSERT_EQ(updates.size(), 2U); // no Accepted for an IOC
    EXPECT_EQ(updates[0].kind, UpdateKind::Fill);
    EXPECT_EQ(updates[0].fill_qty, 100U);
    EXPECT_EQ(updates[0].state, OrderState::PendingNew); // per the table, until it ends
    EXPECT_EQ(updates[1].kind, UpdateKind::Cancelled);
    EXPECT_EQ(updates[1].removed_qty, 200U);
    EXPECT_EQ(updates[1].leaves, 0U);
    EXPECT_EQ(order.state, OrderState::Cancelled);
    EXPECT_EQ(exchange.open_orders(0), 0U);
}

TEST_F(ExchangeTest, IocFilledInFull) {
    hist_add(1, Side::Sell, 500, 1010);
    const SimOrder& order = send(Side::Buy, OrderType::Ioc, 1010, 300);
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].fill_qty, 300U);
    EXPECT_EQ(updates[0].state, OrderState::Filled);
    EXPECT_EQ(order.state, OrderState::Filled);
}

TEST_F(ExchangeTest, IocWithNothingToHitIsCancelledWhole) {
    hist_add(1, Side::Sell, 500, 1010);
    const SimOrder& order = send(Side::Buy, OrderType::Ioc, 1000, 300);
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].kind, UpdateKind::Cancelled);
    EXPECT_EQ(updates[0].removed_qty, 300U);
    EXPECT_EQ(order.state, OrderState::Cancelled);
}

TEST_F(ExchangeTest, OrdersAreRejectedWhenTheSymbolCannotTrade) {
    hist_add(1, Side::Sell, 500, 1010);
    for (const RejectReason reason : {RejectReason::ExchangeClosed, RejectReason::ExchangeHalted}) {
        updates.clear();
        const SimOrder& order = send(Side::Buy, OrderType::Limit, 1010, 100, reason);
        ASSERT_EQ(updates.size(), 1U);
        EXPECT_EQ(updates[0].kind, UpdateKind::Rejected);
        EXPECT_EQ(updates[0].reason, reason);
        EXPECT_EQ(updates[0].removed_qty, 100U);
        EXPECT_EQ(order.state, OrderState::Rejected);
    }
    EXPECT_EQ(exchange.open_orders(0), 0U);
}

TEST_F(ExchangeTest, NoFillsWhileHaltedButTheQueueStillMoves) {
    hist_add(1, Side::Buy, 300, 1000);
    const SimOrder& order = rest(Side::Buy, 1000, 100);
    hist_add(2, Side::Buy, 500, 1000);
    hist_execute(1, 300, false);
    EXPECT_EQ(order.shares_ahead, 0U);
    hist_execute(2, 100, false); // would fill us if the symbol were trading
    EXPECT_TRUE(fills().empty());
    hist_execute(2, 100, true);
    EXPECT_EQ(fills().size(), 1U);
}

TEST_F(ExchangeTest, CancelRemovesARestingOrder) {
    hist_add(1, Side::Buy, 300, 1000);
    SimOrder& order = rest(Side::Buy, 1000, 100);
    SimExchange::request_cancel(order);
    EXPECT_EQ(order.state, OrderState::PendingCancel);
    updates.clear();
    exchange.on_cancel(order, now + 5, emit());
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].kind, UpdateKind::Cancelled);
    EXPECT_EQ(updates[0].removed_qty, 100U);
    EXPECT_EQ(updates[0].ts, now + 5);
    EXPECT_EQ(order.state, OrderState::Cancelled);
    EXPECT_EQ(exchange.open_orders(0), 0U);
    hist_execute(1, 300); // nothing left to fill
    EXPECT_TRUE(fills().empty());
}

TEST_F(ExchangeTest, FillBeforeTheCancelArrivesStillHappens) {
    SimOrder& order = rest(Side::Buy, 1000, 100);
    hist_add(1, Side::Buy, 500, 1000);
    SimExchange::request_cancel(order);
    hist_execute(1, 30);
    EXPECT_EQ(order.state, OrderState::PartiallyFilled); // PendingCancel -> PartiallyFilled
    updates.clear();
    exchange.on_cancel(order, now, emit());
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].kind, UpdateKind::Cancelled);
    EXPECT_EQ(updates[0].removed_qty, 70U);
    EXPECT_EQ(order.state, OrderState::Cancelled);
}

TEST_F(ExchangeTest, CancelAfterTheOrderFilledIsTooLate) {
    SimOrder& order = rest(Side::Buy, 1000, 100);
    hist_add(1, Side::Buy, 500, 1000);
    SimExchange::request_cancel(order);
    hist_execute(1, 100);
    EXPECT_EQ(order.state, OrderState::Filled);
    updates.clear();
    exchange.on_cancel(order, now, emit());
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].kind, UpdateKind::CancelRejected);
    EXPECT_EQ(updates[0].reason, RejectReason::TooLateToCancel);
    EXPECT_EQ(order.state, OrderState::Filled);
}

TEST_F(ExchangeTest, CancelThatOvertakesItsOrderIsRejected) {
    SimOrder* const order = exchange.create(kLoc, Side::Buy, OrderType::Limit, 1000, 100, now);
    SimExchange::request_cancel(*order);
    EXPECT_EQ(order->state, OrderState::PendingNew); // not live yet, so not PendingCancel
    exchange.on_cancel(*order, now, emit());
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].kind, UpdateKind::CancelRejected);
    // The order still arrives and goes live.
    exchange.on_new_order(*order, now + 1, books, kOpen, emit());
    EXPECT_EQ(order->state, OrderState::Live);
}

TEST_F(ExchangeTest, ReplaceOfAnOrderAheadMovesItBehindUs) {
    hist_add(1, Side::Buy, 300, 1000);
    hist_add(2, Side::Buy, 200, 1000);
    const SimOrder& order = rest(Side::Buy, 1000, 100);
    hist_replace(1, 3, 300, 1000); // same price, but it loses its place
    EXPECT_EQ(order.shares_ahead, 200U);
    hist_execute(2, 200);
    EXPECT_EQ(order.shares_ahead, 0U);
    hist_execute(3, 60); // the replaced order is behind us now
    ASSERT_EQ(fills().size(), 1U);
    EXPECT_EQ(fills()[0].fill_qty, 60U);
}

TEST_F(ExchangeTest, EndOfDayCancelsEveryRestingOrder) {
    const SimOrder& a = rest(Side::Buy, 1000, 100);
    const SimOrder& b = rest(Side::Sell, 1010, 100);
    hist_add(1, Side::Sell, 50, 1010);
    updates.clear();
    exchange.expire_all(now + 9, emit());
    ASSERT_EQ(updates.size(), 2U);
    EXPECT_EQ(updates[0].kind, UpdateKind::Cancelled);
    EXPECT_EQ(updates[1].kind, UpdateKind::Cancelled);
    EXPECT_EQ(a.state, OrderState::Cancelled);
    EXPECT_EQ(b.state, OrderState::Cancelled);
    EXPECT_EQ(exchange.open_orders(0), 0U);
}

TEST_F(ExchangeTest, RestingOrderLimitIsEnforcedOnArrival) {
    ExchangeConfig small;
    small.max_open_orders_per_symbol = 2;
    SimExchange tiny(small, traded);
    const auto arrive = [&](Price price) {
        SimOrder* const order = tiny.create(kLoc, Side::Buy, OrderType::Limit, price, 100, now);
        tiny.on_new_order(*order, now, books, kOpen, emit());
        return order->state;
    };
    EXPECT_EQ(arrive(1000), OrderState::Live);
    EXPECT_EQ(arrive(999), OrderState::Live);
    EXPECT_EQ(arrive(998), OrderState::Rejected);
    EXPECT_EQ(updates.back().reason, RejectReason::ExchangeOpenOrderLimit);
}

TEST_F(ExchangeTest, TwoOrdersAtOnePriceAreBothServed) {
    hist_add(1, Side::Buy, 100, 1000);
    const SimOrder& first = rest(Side::Buy, 1000, 100);
    const SimOrder& second = rest(Side::Buy, 1000, 100);
    hist_add(2, Side::Buy, 500, 1000);
    hist_execute(1, 100);
    hist_execute(2, 100);
    // Simulated orders do not queue behind each other: a documented optimism of the model.
    EXPECT_EQ(first.state, OrderState::Filled);
    EXPECT_EQ(second.state, OrderState::Filled);
    ASSERT_EQ(fills().size(), 2U);
    EXPECT_EQ(fills()[0].id, first.id); // older order is reported first
}

} // namespace
} // namespace itchbt::sim
