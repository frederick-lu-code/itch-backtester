// The sample strategies on small hand-built streams.

#include "imbalance_taker.hpp"
#include "noop.hpp"
#include "passive_maker.hpp"
#include "support/itch_writer.hpp"
#include "support/random_stream.hpp"

#include "itchbt/record/recorder.hpp"
#include "itchbt/sim/engine.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace itchbt::strategies {
namespace {

using test::ItchWriter;

constexpr Locate kLoc = 7;

ItchWriter session() {
    ItchWriter w;
    w.system_event(1, 'O')
        .stock_directory(2, kLoc, "AAPL")
        .trading_action(3, kLoc, "AAPL", 'T')
        .system_event(1000, 'Q');
    return w;
}

sim::Config base_config() {
    sim::Config c;
    c.symbols = {"AAPL"};
    c.expected_orders = 1024;
    c.exchange.max_orders_per_symbol = 4096;
    return c;
}

/// Runs a strategy and returns the engine's view afterwards.
template <typename S>
struct Outcome {
    sim::RunStats stats;
    std::int64_t position = 0;
    risk::SymbolRisk risk;
};

template <typename S>
Outcome<S> run(const ItchWriter& stream, const sim::Config& config, S& s) {
    record::Recorder discard;
    sim::Backtester<S> engine(config, s, discard);
    std::string error;
    EXPECT_TRUE(engine.prepare(stream.bytes(), error)) << error;
    EXPECT_TRUE(engine.run(stream.bytes()).ok());
    return {engine.stats(), engine.position(kLoc), engine.risk().symbol(0)};
}

TEST(NoOpStrategy, ReplaysWithoutTrading) {
    test::RandomStreamOptions options;
    options.symbols = {"AAPL"};
    options.first_locate = kLoc;
    options.events = 5'000;
    NoOp noop;
    const auto out = run(test::random_stream(options), base_config(), noop);
    EXPECT_EQ(out.stats.orders_submitted, 0U);
    EXPECT_GT(out.stats.book_updates, 100U);
    EXPECT_GT(out.stats.trades, 100U);
}

TEST(PassiveMakerStrategy, QuotesBothSidesAtTheTouch) {
    ItchWriter w = session();
    w.add(1100, kLoc, 1, Side::Buy, 500, "AAPL", 1'000'000)
        .add(1200, kLoc, 2, Side::Sell, 500, "AAPL", 1'000'100);
    PassiveMaker maker{sim::PassiveMakerConfig{}};
    const auto out = run(w, base_config(), maker);
    // Nothing to quote against until both sides exist, then one order each side.
    EXPECT_EQ(out.stats.orders_submitted, 2U);
    EXPECT_EQ(out.risk.open_buy_qty, 100);
    EXPECT_EQ(out.risk.open_sell_qty, 100);
    EXPECT_EQ(out.risk.open_orders, 2U);
}

TEST(PassiveMakerStrategy, RequotesWhenTheTouchMoves) {
    ItchWriter w = session();
    w.add(1100, kLoc, 1, Side::Buy, 500, "AAPL", 1'000'000)
        .add(1200, kLoc, 2, Side::Sell, 500, "AAPL", 1'000'100)
        .add(1300, kLoc, 3, Side::Buy, 500, "AAPL", 1'000'050) // bid improves
        .add(1400, kLoc, 4, Side::Sell, 1, "AAPL", 1'000'300); // deep: the touch is unchanged
    PassiveMaker maker{sim::PassiveMakerConfig{}};
    const auto out = run(w, base_config(), maker);
    EXPECT_EQ(out.stats.cancels_requested, 1U); // only the bid
    EXPECT_EQ(out.stats.orders_submitted, 3U);  // bid, ask, new bid
    EXPECT_EQ(out.risk.open_buy_qty, 100);
}

TEST(PassiveMakerStrategy, StopsBuyingAtItsInventoryLimitAndSkewsTheBid) {
    ItchWriter w = session();
    w.add(1100, kLoc, 1, Side::Buy, 100, "AAPL", 1'000'000)
        .add(1200, kLoc, 2, Side::Sell, 5000, "AAPL", 1'000'100)
        .add(1300, kLoc, 3, Side::Buy, 5000, "AAPL", 1'000'000) // behind the maker's bid
        .execute(1400, kLoc, 1, 100, 1)                         // queue ahead clears
        .execute(1500, kLoc, 3, 100, 2)                         // maker buys 100
        .execute(1600, kLoc, 3, 100, 3);                        // and, after re-quoting, no more
    sim::Config c = base_config();
    c.passive_maker.max_inventory = 100;
    c.passive_maker.skew_per_lot = 300;
    PassiveMaker maker(c.passive_maker);
    const auto out = run(w, c, maker);
    EXPECT_EQ(out.position, 100);
    EXPECT_EQ(out.risk.open_buy_qty, 0); // at the limit: no bid
    EXPECT_EQ(out.risk.open_sell_qty, 100);
}

TEST(PassiveMakerStrategy, DoesNotQuoteBeforeTheOpen) {
    ItchWriter w;
    w.system_event(1, 'O')
        .stock_directory(2, kLoc, "AAPL")
        .trading_action(3, kLoc, "AAPL", 'T')
        .add(100, kLoc, 1, Side::Buy, 500, "AAPL", 1'000'000)
        .add(200, kLoc, 2, Side::Sell, 500, "AAPL", 1'000'100);
    PassiveMaker maker{sim::PassiveMakerConfig{}};
    const auto out = run(w, base_config(), maker);
    EXPECT_EQ(out.stats.orders_submitted, 0U);
}

TEST(PassiveMakerStrategy, SurvivesARandomMarketWithinItsLimits) {
    test::RandomStreamOptions options;
    options.symbols = {"AAPL"};
    options.first_locate = kLoc;
    options.events = 30'000;
    options.seed = 11;
    sim::Config c = base_config();
    c.exchange.max_orders_per_symbol = 100'000;
    c.risk.max_position = 400;
    c.passive_maker.max_inventory = 300;
    PassiveMaker maker(c.passive_maker);
    const auto out = run(test::random_stream(options), c, maker);
    EXPECT_GT(out.stats.fills, 50U);
    EXPECT_EQ(out.stats.orders_rejected_by_risk, 0U); // its own cap is inside the risk limit
    EXPECT_LE(out.position, 400);
    EXPECT_GE(out.position, -400);
    EXPECT_EQ(out.risk.open_orders, 0U); // day orders expired at the close
    EXPECT_EQ(out.risk.open_buy_qty, 0);
    EXPECT_EQ(out.risk.open_sell_qty, 0);
    EXPECT_EQ(out.risk.open_notional, 0);
}

TEST(ImbalanceTakerStrategy, BuysWhenTheBidDominatesAndRespectsTheCooldown) {
    ItchWriter w = session();
    w.add(1100, kLoc, 1, Side::Sell, 100, "AAPL", 1'000'100)
        .add(1200, kLoc, 2, Side::Buy, 900, "AAPL", 1'000'000) // 90% bid: buy
        .add(1300, kLoc, 3, Side::Buy, 100, "AAPL", 1'000'000) // still lopsided, cooling down
        .add(1400 + 5000, kLoc, 4, Side::Buy, 100, "AAPL", 1'000'000); // cooldown over: buy
    sim::Config c = base_config();
    c.imbalance_taker.threshold_pct = 80;
    c.imbalance_taker.cooldown_ns = 5000;
    c.imbalance_taker.lot = 100;
    ImbalanceTaker taker(c.imbalance_taker);
    const auto out = run(w, c, taker);
    EXPECT_EQ(out.stats.orders_submitted, 2U);
    EXPECT_EQ(out.stats.fills, 2U);
    EXPECT_EQ(out.position, 200);
}

TEST(ImbalanceTakerStrategy, SellsWhenTheAskDominatesAndStaysQuietWhenBalanced) {
    ItchWriter w = session();
    w.add(1100, kLoc, 1, Side::Buy, 500, "AAPL", 1'000'000)
        .add(1200, kLoc, 2, Side::Sell, 500, "AAPL", 1'000'100)   // balanced: nothing
        .add(1300, kLoc, 3, Side::Sell, 4500, "AAPL", 1'000'100); // ask is now ~91%: sell
    sim::Config c = base_config();
    c.imbalance_taker.threshold_pct = 90;
    ImbalanceTaker taker(c.imbalance_taker);
    const auto out = run(w, c, taker);
    EXPECT_EQ(out.stats.orders_submitted, 1U);
    EXPECT_EQ(out.position, -100);
}

TEST(ImbalanceTakerStrategy, StopsAtItsInventoryLimit) {
    ItchWriter w = session();
    w.add(1100, kLoc, 1, Side::Sell, 100, "AAPL", 1'000'100);
    for (OrderRef i = 0; i < 6; ++i) {
        w.add(2000 + i * 100, kLoc, 10 + i, Side::Buy, 1000, "AAPL", 1'000'000);
    }
    sim::Config c = base_config();
    c.imbalance_taker.cooldown_ns = 0;
    c.imbalance_taker.max_inventory = 300;
    ImbalanceTaker taker(c.imbalance_taker);
    const auto out = run(w, c, taker);
    EXPECT_EQ(out.position, 300);
    EXPECT_EQ(out.stats.orders_submitted, 3U);
}

} // namespace
} // namespace itchbt::strategies
