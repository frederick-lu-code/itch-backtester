// The risk gate: each pre-trade check at, just under and just over its limit, position and
// PnL arithmetic, and the kill switch.

#include "itchbt/risk/risk.hpp"

#include <cstdint>

#include <gtest/gtest.h>

namespace itchbt::risk {
namespace {

using sim::RejectReason;
using sim::RiskConfig;

constexpr Timestamp kSecond = 1'000'000'000;

/// A two-sided market around 100.0000, in regular hours, trading.
MarketView market(Price bid = 999'900, Price ask = 1'000'100) {
    MarketView m;
    m.regular_hours = true;
    m.trading = true;
    m.has_bid = true;
    m.has_ask = true;
    m.bid = bid;
    m.ask = ask;
    return m;
}

RejectReason check(RiskGate& gate, Side side, Price price, Qty qty, Timestamp now = 0,
                   const MarketView& view = market()) {
    return gate.check_and_reserve(0, side, price, qty, now, view);
}

TEST(RiskChecks, DefaultsAllowAnySaneOrder) {
    RiskGate gate(RiskConfig{}, false, 1);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 4'000'000'000U), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Sell, 4'000'000'000U, 4'000'000'000U), RejectReason::None);
}

TEST(RiskChecks, ZeroQuantityOrPriceIsInvalid) {
    RiskGate gate(RiskConfig{}, false, 1);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 0), RejectReason::InvalidOrder);
    EXPECT_EQ(check(gate, Side::Buy, 0, 100), RejectReason::InvalidOrder);
}

TEST(RiskChecks, MaxOrderQuantityBoundary) {
    RiskConfig c;
    c.max_order_qty = 500;
    RiskGate gate(c, false, 1);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 499), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 500), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 501), RejectReason::MaxOrderQty);
}

TEST(RiskChecks, MaxOrderNotionalBoundary) {
    RiskConfig c;
    c.max_order_notional = 100 * 1'000'000; // 100 shares at 100.0000
    RiskGate gate(c, false, 1);
    EXPECT_EQ(check(gate, Side::Buy, 999'999, 100), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 100), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'001, 100), RejectReason::MaxOrderNotional);
    EXPECT_EQ(check(gate, Side::Sell, 1'000'000, 101), RejectReason::MaxOrderNotional);
}

TEST(RiskChecks, PriceCollarBoundaryOnBothSides) {
    RiskConfig c;
    c.price_collar_bps = 100; // 1% of the 100.0000 mid is 1.0000
    RiskGate gate(c, false, 1);
    EXPECT_EQ(check(gate, Side::Buy, 1'009'999, 1), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'010'000, 1), RejectReason::None); // exactly 1% away
    EXPECT_EQ(check(gate, Side::Buy, 1'010'001, 1), RejectReason::PriceCollar);
    EXPECT_EQ(check(gate, Side::Sell, 990'001, 1), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Sell, 990'000, 1), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Sell, 989'999, 1), RejectReason::PriceCollar);
}

TEST(RiskChecks, PriceCollarHandlesAHalfUnitMidExactly) {
    RiskConfig c;
    c.price_collar_bps = 5'000; // 50%
    RiskGate gate(c, false, 1);
    // Mid is 10.5 price units: 50% of it is 5.25, so 15 is inside (4.5 away) and 16 is not.
    const MarketView m = market(10, 11);
    EXPECT_EQ(check(gate, Side::Buy, 15, 1, 0, m), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 16, 1, 0, m), RejectReason::PriceCollar);
    EXPECT_EQ(check(gate, Side::Sell, 6, 1, 0, m), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Sell, 5, 1, 0, m), RejectReason::PriceCollar);
}

TEST(RiskChecks, PriceCollarNeedsATwoSidedMarketOnlyWhenEnabled) {
    MarketView one_sided = market();
    one_sided.has_ask = false;
    RiskConfig c;
    c.price_collar_bps = 100;
    RiskGate collared(c, false, 1);
    EXPECT_EQ(check(collared, Side::Buy, 1'000'000, 1, 0, one_sided),
              RejectReason::NoReferencePrice);
    RiskGate open(RiskConfig{}, false, 1);
    EXPECT_EQ(check(open, Side::Buy, 1'000'000, 1, 0, one_sided), RejectReason::None);
}

TEST(RiskChecks, MaxPositionCountsOpenOrdersAtWorstCase) {
    RiskConfig c;
    c.max_position = 300;
    RiskGate gate(c, false, 1);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 200), RejectReason::None); // open buys: 200
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 101), RejectReason::MaxPosition);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 100), RejectReason::None); // exactly 300
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1), RejectReason::MaxPosition);
    // The sell side is judged on its own worst case: open buys do not offset it.
    EXPECT_EQ(check(gate, Side::Sell, 1'000'000, 300), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Sell, 1'000'000, 1), RejectReason::MaxPosition);
}

TEST(RiskChecks, MaxPositionFollowsFillsAndCancels) {
    RiskConfig c;
    c.max_position = 300;
    RiskGate gate(c, false, 1);
    ASSERT_EQ(check(gate, Side::Buy, 1'000'000, 300), RejectReason::None);
    gate.on_fill(0, Side::Buy, 1'000'000, 1'000'000, 100); // position 100, 200 still open
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1), RejectReason::MaxPosition);
    gate.on_closed(0, Side::Buy, 1'000'000, 200, true);    // remainder cancelled
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 200), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1), RejectReason::MaxPosition);
    // Long 100: a sell of 400 would end short 300, which is allowed; 401 is not.
    EXPECT_EQ(check(gate, Side::Sell, 1'000'000, 401), RejectReason::MaxPosition);
    EXPECT_EQ(check(gate, Side::Sell, 1'000'000, 400), RejectReason::None);
}

TEST(RiskChecks, MaxOpenOrdersBoundaryAndRelease) {
    RiskConfig c;
    c.max_open_orders = 2;
    RiskGate gate(c, false, 1);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 10), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Sell, 1'000'100, 10), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 10), RejectReason::MaxOpenOrders);
    gate.on_closed(0, Side::Sell, 1'000'100, 10, true); // one cancelled
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 10), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 10), RejectReason::MaxOpenOrders);
    gate.on_fill(0, Side::Buy, 1'000'000, 1'000'000, 10); // one filled in full
    gate.on_order_done(0);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 10), RejectReason::None);
}

TEST(RiskChecks, PartialFillDoesNotFreeAnOpenOrderSlot) {
    RiskConfig c;
    c.max_open_orders = 1;
    RiskGate gate(c, false, 1);
    ASSERT_EQ(check(gate, Side::Buy, 1'000'000, 100), RejectReason::None);
    gate.on_fill(0, Side::Buy, 1'000'000, 1'000'000, 40);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 10), RejectReason::MaxOpenOrders);
}

TEST(RiskChecks, MaxGrossNotionalCountsPositionsOpenOrdersAndTheNewOrder) {
    RiskConfig c;
    c.max_gross_notional = 1'000 * 1'000'000; // 1,000 shares' worth at 100.0000
    RiskGate gate(c, false, 2);
    // Symbol 0: long 400 marked at the 100.0000 mid.
    ASSERT_EQ(gate.check_and_reserve(0, Side::Buy, 1'000'000, 400, 0, market()),
              RejectReason::None);
    gate.on_fill(0, Side::Buy, 1'000'000, 1'000'000, 400);
    gate.on_order_done(0);
    gate.on_top_of_book(0, market());
    // Symbol 1: an open sell of 300 at 100.0000.
    ASSERT_EQ(gate.check_and_reserve(1, Side::Sell, 1'000'000, 300, 0, market()),
              RejectReason::None);
    EXPECT_EQ(gate.gross_position_notional2(), 2 * 400 * 1'000'000LL);
    EXPECT_EQ(gate.open_notional(), 300 * 1'000'000LL);
    // 700 used, so 300 more is exactly the limit.
    EXPECT_EQ(gate.check_and_reserve(1, Side::Buy, 1'000'000, 301, 0, market()),
              RejectReason::MaxGrossNotional);
    EXPECT_EQ(gate.check_and_reserve(1, Side::Buy, 1'000'000, 300, 0, market()),
              RejectReason::None);
    EXPECT_EQ(gate.check_and_reserve(0, Side::Sell, 1, 1, 0, market()),
              RejectReason::MaxGrossNotional);
}

TEST(RiskChecks, RateLimitIsATokenBucketOnSimulationTime) {
    RiskConfig c;
    c.rate_limit_per_second = 10; // one token every 100 ms
    c.rate_limit_burst = 3;
    RiskGate gate(c, false, 1);
    const Timestamp t0 = 5 * kSecond;
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1, t0), RejectReason::None) << i;
    }
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1, t0), RejectReason::RateLimit);
    // One nanosecond short of a new token, then exactly on it.
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1, t0 + kSecond / 10 - 1),
              RejectReason::RateLimit);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1, t0 + kSecond / 10), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1, t0 + kSecond / 10), RejectReason::RateLimit);
}

TEST(RiskChecks, RateLimitRefillStopsAtTheBurstSize) {
    RiskConfig c;
    c.rate_limit_per_second = 1'000'000;
    c.rate_limit_burst = 2;
    RiskGate gate(c, false, 1);
    // A gap of most of a day must not overflow or bank more than the burst.
    const Timestamp late = 80'000 * kSecond;
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1, late), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1, late), RejectReason::None);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1, late), RejectReason::RateLimit);
}

TEST(RiskChecks, RejectedOrdersDoNotSpendTokensOrReserveExposure) {
    RiskConfig c;
    c.rate_limit_per_second = 1;
    c.rate_limit_burst = 1;
    c.max_order_qty = 10;
    RiskGate gate(c, false, 1);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 11), RejectReason::MaxOrderQty);
    EXPECT_EQ(gate.symbol(0).open_buy_qty, 0);
    EXPECT_EQ(gate.symbol(0).open_orders, 0U);
    EXPECT_EQ(gate.open_notional(), 0);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 10), RejectReason::None); // token still there
}

TEST(RiskChecks, ClosedMarketAndHaltedSymbol) {
    RiskGate gate(RiskConfig{}, false, 1);
    MarketView closed = market();
    closed.regular_hours = false;
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1, 0, closed), RejectReason::MarketClosed);
    MarketView halted = market();
    halted.trading = false;
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 1, 0, halted), RejectReason::SymbolHalted);

    RiskGate extended(RiskConfig{}, true, 1);
    EXPECT_EQ(check(extended, Side::Buy, 1'000'000, 1, 0, closed), RejectReason::None);
    closed.trading = false;
    EXPECT_EQ(check(extended, Side::Buy, 1'000'000, 1, 0, closed), RejectReason::SymbolHalted);
}

TEST(RiskChecks, ChecksRunInAFixedOrder) {
    // An order that breaks several limits reports the first in the documented order.
    RiskConfig c;
    c.max_order_qty = 10;
    c.max_order_notional = 1;
    c.max_position = 1;
    RiskGate gate(c, false, 1);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 100), RejectReason::MaxOrderQty);
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 5), RejectReason::MaxOrderNotional);
    MarketView halted = market();
    halted.trading = false;
    EXPECT_EQ(check(gate, Side::Buy, 1'000'000, 100, 0, halted), RejectReason::SymbolHalted);
}

// --- Position and PnL ------------------------------------------------------------------------

/// Reserves and fills in one step, as a filled order would.
void trade(RiskGate& gate, Side side, Price price, Qty qty) {
    ASSERT_EQ(check(gate, side, price, qty), RejectReason::None);
    gate.on_fill(0, side, price, price, qty);
    gate.on_order_done(0);
}

TEST(RiskPnl, RoundTripRealisesTheDifference) {
    RiskGate gate(RiskConfig{}, false, 1);
    trade(gate, Side::Buy, 1'000'000, 100);
    EXPECT_EQ(gate.position(0), 100);
    EXPECT_EQ(gate.symbol(0).realised(), 0);
    trade(gate, Side::Sell, 1'010'000, 100);
    EXPECT_EQ(gate.position(0), 0);
    EXPECT_EQ(gate.symbol(0).realised(), 100 * 10'000); // 100 shares x 1.0000
    EXPECT_EQ(gate.symbol(0).unrealised2(), 0);
    EXPECT_EQ(gate.total_pnl2(), 2 * 100 * 10'000);
    EXPECT_EQ(gate.symbol(0).open_buy_qty, 0);
    EXPECT_EQ(gate.symbol(0).open_sell_qty, 0);
    EXPECT_EQ(gate.open_notional(), 0);
}

TEST(RiskPnl, UnrealisedIsMarkedToMidIncludingHalfUnits) {
    RiskGate gate(RiskConfig{}, false, 1);
    trade(gate, Side::Buy, 1'000'000, 100);
    gate.on_top_of_book(0, market(1'000'100, 1'000'201)); // mid 1,000,150.5
    EXPECT_EQ(gate.symbol(0).unrealised2(), 100 * 301);   // 2 x 100 x 150.5, exactly
    EXPECT_EQ(gate.total_pnl2(), 100 * 301);
    EXPECT_EQ(gate.symbol(0).realised(), 0);
}

TEST(RiskPnl, PartialCloseUsesAverageCost) {
    RiskGate gate(RiskConfig{}, false, 1);
    trade(gate, Side::Buy, 1'000'000, 100);
    trade(gate, Side::Buy, 1'020'000, 100); // average cost 1,010,000
    trade(gate, Side::Sell, 1'030'000, 50);
    EXPECT_EQ(gate.position(0), 150);
    EXPECT_EQ(gate.symbol(0).realised(), 50 * 20'000);
    gate.on_top_of_book(0, market(1'030'000, 1'030'000));
    EXPECT_EQ(gate.symbol(0).unrealised2(), 2 * 150 * 20'000);
    // Total is exact whatever the split: cash plus position at mid.
    EXPECT_EQ(gate.total_pnl2(), 2 * (50 * 20'000 + 150 * 20'000));
}

TEST(RiskPnl, ShortPositionsMirrorLongOnes) {
    RiskGate gate(RiskConfig{}, false, 1);
    trade(gate, Side::Sell, 1'000'000, 100);
    EXPECT_EQ(gate.position(0), -100);
    gate.on_top_of_book(0, market(990'000, 990'000));
    EXPECT_EQ(gate.symbol(0).unrealised2(), 2 * 100 * 10'000); // price fell: a short gains
    trade(gate, Side::Buy, 990'000, 100);
    EXPECT_EQ(gate.symbol(0).realised(), 100 * 10'000);
    EXPECT_EQ(gate.position(0), 0);
}

TEST(RiskPnl, TradingThroughZeroClosesAndReopens) {
    RiskGate gate(RiskConfig{}, false, 1);
    trade(gate, Side::Buy, 1'000'000, 100);
    trade(gate, Side::Sell, 1'010'000, 250); // closes 100 long, opens 150 short at 1,010,000
    EXPECT_EQ(gate.position(0), -150);
    EXPECT_EQ(gate.symbol(0).realised(), 100 * 10'000);
    gate.on_top_of_book(0, market(1'010'000, 1'010'000));
    EXPECT_EQ(gate.symbol(0).unrealised2(), 0); // the new short is at its entry price
    gate.on_top_of_book(0, market(1'000'000, 1'000'000));
    EXPECT_EQ(gate.symbol(0).unrealised2(), 2 * 150 * 10'000);
}

TEST(RiskPnl, RealisedPlusUnrealisedAlwaysEqualsTheTotal) {
    RiskGate gate(RiskConfig{}, false, 1);
    // Odd sizes and prices, so the average-cost split has rounding to absorb.
    trade(gate, Side::Buy, 1'000'003, 7);
    trade(gate, Side::Buy, 999'998, 11);
    trade(gate, Side::Sell, 1'000'007, 5);
    trade(gate, Side::Sell, 1'000'001, 4);
    trade(gate, Side::Sell, 999'990, 20);
    trade(gate, Side::Buy, 1'000'011, 3);
    gate.on_top_of_book(0, market(1'000'002, 1'000'005));
    const SymbolRisk& s = gate.symbol(0);
    EXPECT_EQ(2 * s.realised() + s.unrealised2(), gate.total_pnl2());
    EXPECT_EQ(gate.total_pnl2(), 2 * s.cash + s.position * s.mark2);
}

TEST(RiskPnl, FillBeforeAnyMarkIsMarkedAtTheFillPrice) {
    RiskGate gate(RiskConfig{}, false, 1);
    trade(gate, Side::Buy, 1'000'000, 100);
    EXPECT_EQ(gate.total_pnl2(), 0); // not minus the whole purchase
    MarketView one_sided = market();
    one_sided.has_bid = false;
    gate.on_top_of_book(0, one_sided); // a one-sided book keeps the last mark
    EXPECT_EQ(gate.total_pnl2(), 0);
}

TEST(RiskPnl, SymbolsAreTrackedSeparatelyAndSummed) {
    RiskGate gate(RiskConfig{}, false, 2);
    ASSERT_EQ(gate.check_and_reserve(0, Side::Buy, 1'000'000, 10, 0, market()),
              RejectReason::None);
    gate.on_fill(0, Side::Buy, 1'000'000, 1'000'000, 10);
    ASSERT_EQ(gate.check_and_reserve(1, Side::Sell, 500'000, 20, 0, market()),
              RejectReason::None);
    gate.on_fill(1, Side::Sell, 500'000, 500'000, 20);
    gate.on_top_of_book(0, market(1'000'100, 1'000'100));
    gate.on_top_of_book(1, market(500'100, 500'100));
    EXPECT_EQ(gate.position(0), 10);
    EXPECT_EQ(gate.position(1), -20);
    EXPECT_EQ(gate.total_pnl2(), 2 * (10 * 100 - 20 * 100));
    EXPECT_EQ(gate.gross_position_notional2(), 2 * (10 * 1'000'100LL + 20 * 500'100LL));
    EXPECT_EQ(gate.fills(), 2U);
}

TEST(RiskPnl, FillAtADifferentPriceThanTheLimitReleasesTheLimitNotional) {
    RiskGate gate(RiskConfig{}, false, 1);
    ASSERT_EQ(check(gate, Side::Buy, 1'000'500, 100), RejectReason::None);
    EXPECT_EQ(gate.open_notional(), 100 * 1'000'500LL);
    gate.on_fill(0, Side::Buy, 1'000'500, 1'000'100, 100); // price improvement
    EXPECT_EQ(gate.open_notional(), 0);
    EXPECT_EQ(gate.symbol(0).cash, -100 * 1'000'100LL);
}

// --- Kill switch -----------------------------------------------------------------------------

TEST(KillSwitch, TripsOnlyWhenTheLossExceedsTheLimit) {
    RiskConfig c;
    c.loss_limit = 100 * 10'000; // 100 dollars
    RiskGate gate(c, false, 1);
    trade(gate, Side::Buy, 1'000'000, 100);
    EXPECT_FALSE(gate.on_top_of_book(0, market(990'000, 990'000))); // down exactly 100 dollars
    EXPECT_FALSE(gate.tripped());
    EXPECT_TRUE(gate.on_top_of_book(0, market(989'999, 990'000)));  // half a unit a share more
    EXPECT_TRUE(gate.tripped());
    EXPECT_EQ(gate.kill_reason(), KillReason::LossLimit);
}

TEST(KillSwitch, OnceTrippedEveryNewOrderIsRejected) {
    RiskConfig c;
    c.loss_limit = 1;
    RiskGate gate(c, false, 1);
    trade(gate, Side::Buy, 1'000'000, 100);
    ASSERT_TRUE(gate.on_top_of_book(0, market(900'000, 900'000)));
    EXPECT_EQ(check(gate, Side::Sell, 900'000, 100), RejectReason::KillSwitch);
    // Recovery does not reset it: it stays tripped for the rest of the run.
    EXPECT_FALSE(gate.on_top_of_book(0, market(1'100'000, 1'100'000)));
    EXPECT_TRUE(gate.tripped());
    EXPECT_EQ(check(gate, Side::Sell, 1'100'000, 1), RejectReason::KillSwitch);
}

TEST(KillSwitch, ARealisedLossTripsItOnTheFill) {
    RiskConfig c;
    c.loss_limit = 50 * 10'000;
    RiskGate gate(c, false, 1);
    trade(gate, Side::Buy, 1'000'000, 100);
    ASSERT_EQ(check(gate, Side::Sell, 990'000, 100), RejectReason::None);
    EXPECT_TRUE(gate.on_fill(0, Side::Sell, 990'000, 990'000, 100)); // realises -100 dollars
    EXPECT_EQ(gate.kill_reason(), KillReason::LossLimit);
}

TEST(KillSwitch, RejectRateBoundary) {
    RiskConfig c;
    c.reject_threshold = 3;
    c.reject_window_ns = 1'000;
    RiskGate gate(c, false, 1);
    EXPECT_FALSE(gate.note_reject(100));
    EXPECT_FALSE(gate.note_reject(200));
    EXPECT_FALSE(gate.note_reject(300)); // three in the window: at the threshold, not over
    EXPECT_TRUE(gate.note_reject(1'100)); // a fourth, exactly one window after the first
    EXPECT_EQ(gate.kill_reason(), KillReason::RejectRate);
    EXPECT_EQ(gate.rejects(), 4U);
}

TEST(KillSwitch, RejectsSpreadWiderThanTheWindowDoNotTrip) {
    RiskConfig c;
    c.reject_threshold = 3;
    c.reject_window_ns = 1'000;
    RiskGate gate(c, false, 1);
    EXPECT_FALSE(gate.note_reject(100));
    EXPECT_FALSE(gate.note_reject(200));
    EXPECT_FALSE(gate.note_reject(300));
    EXPECT_FALSE(gate.note_reject(1'101)); // one nanosecond outside the window of the first
    EXPECT_FALSE(gate.note_reject(1'150));
    EXPECT_TRUE(gate.note_reject(1'201));  // 200, 300, 1101, 1150 and now 1201: over again
}

TEST(KillSwitch, RejectRateIsOffByDefault) {
    RiskGate gate(RiskConfig{}, false, 1);
    for (Timestamp t = 0; t < 1'000; ++t) {
        EXPECT_FALSE(gate.note_reject(t));
    }
    EXPECT_FALSE(gate.tripped());
    EXPECT_EQ(gate.rejects(), 1'000U);
}

TEST(RiskGateSetup, ConfigureResetsEverything) {
    RiskConfig c;
    c.loss_limit = 1;
    RiskGate gate(c, false, 1);
    trade(gate, Side::Buy, 1'000'000, 100);
    ASSERT_TRUE(gate.on_top_of_book(0, market(900'000, 900'000)));
    gate.configure(RiskConfig{}, false, 3);
    EXPECT_FALSE(gate.tripped());
    EXPECT_EQ(gate.symbol_count(), 3U);
    EXPECT_EQ(gate.position(0), 0);
    EXPECT_EQ(gate.total_pnl2(), 0);
    EXPECT_EQ(gate.fills(), 0U);
}

} // namespace
} // namespace itchbt::risk
