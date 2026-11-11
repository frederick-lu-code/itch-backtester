// The engine end to end on synthetic streams: event ordering, latency, the strategy API,
// risk rejects, and determinism of the recorded output.

#include "passive_maker.hpp"
#include "support/itch_writer.hpp"
#include "support/random_stream.hpp"
#include "support/scripted_strategy.hpp"
#include "support/temp_file.hpp"

#include "itchbt/record/recorder.hpp"
#include "itchbt/sim/engine.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace itchbt::sim {
namespace {

using test::ItchWriter;
using test::ScriptedStrategy;

constexpr Locate kAapl = 7;
constexpr Locate kMsft = 8;
constexpr Timestamp kOpen = 1'000;

/// Directory, both symbols trading, market open at kOpen.
ItchWriter session() {
    ItchWriter w;
    w.system_event(1, 'O')
        .stock_directory(2, kAapl, "AAPL")
        .stock_directory(3, kMsft, "MSFT")
        .trading_action(4, kAapl, "AAPL", 'T')
        .trading_action(5, kMsft, "MSFT", 'T')
        .system_event(kOpen, 'Q');
    return w;
}

Config config_for(std::vector<std::string> symbols = {"AAPL"}) {
    Config c;
    c.symbols = std::move(symbols);
    c.expected_orders = 1024;
    c.exchange.max_orders_per_symbol = 1024;
    return c;
}

struct Result {
    RunStats stats;
    std::int64_t position = 0;
    std::int64_t pnl2 = 0;
    bool killed = false;
};

/// Runs `strategy` over `stream` and returns what the engine reports afterwards.
template <typename S>
Result run(const ItchWriter& stream, const Config& config, S& strategy,
           record::Recorder* recorder = nullptr) {
    record::Recorder discard;
    Backtester<S> engine(config, strategy, recorder != nullptr ? *recorder : discard);
    std::string error;
    EXPECT_TRUE(engine.prepare(stream.bytes(), error)) << error;
    const itch::ParseResult result = engine.run(stream.bytes());
    EXPECT_TRUE(result.ok());
    Result out;
    out.stats = engine.stats();
    out.position = engine.position(kAapl);
    out.pnl2 = engine.risk().total_pnl2();
    out.killed = engine.risk().tripped();
    return out;
}

std::vector<OrderUpdate> of_kind(const ScriptedStrategy& s, UpdateKind kind) {
    std::vector<OrderUpdate> out;
    for (const OrderUpdate& u : s.order_updates) {
        if (u.kind == kind) {
            out.push_back(u);
        }
    }
    return out;
}

TEST(Engine, CallsStartAndEndAndResolvesSymbolsBeforeStart) {
    ScriptedStrategy s;
    std::vector<Locate> seen;
    std::string name;
    s.start = [&](Context& ctx) {
        seen.assign(ctx.symbols().begin(), ctx.symbols().end());
        name = std::string(ctx.symbol_name(kAapl));
        EXPECT_EQ(ctx.find_symbol("MSFT"), kMsft);
        EXPECT_EQ(ctx.now(), 0U);
    };
    run(session(), config_for(), s);
    EXPECT_TRUE(s.started);
    EXPECT_TRUE(s.ended);
    EXPECT_EQ(seen, (std::vector<Locate>{kAapl}));
    EXPECT_EQ(name, "AAPL");
}

TEST(Engine, UnknownOrRepeatedSymbolFailsPreparation) {
    ScriptedStrategy s;
    record::Recorder discard;
    const ItchWriter stream = session();
    std::string error;
    Backtester<ScriptedStrategy> unknown(config_for({"NOPE"}), s, discard);
    EXPECT_FALSE(unknown.prepare(stream.bytes(), error));
    EXPECT_NE(error.find("NOPE"), std::string::npos);
    Backtester<ScriptedStrategy> twice(config_for({"AAPL", "AAPL"}), s, discard);
    EXPECT_FALSE(twice.prepare(stream.bytes(), error));
    EXPECT_NE(error.find("twice"), std::string::npos);
}

TEST(Engine, BookUpdatesComeOnlyForSubscribedSymbolsAndOnlyWhenTheTopChanges) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Buy, 100, "AAPL", 1000)    // new best bid
        .add(1200, kMsft, 2, Side::Buy, 100, "MSFT", 5000) // not subscribed
        .add(1300, kAapl, 3, Side::Buy, 100, "AAPL", 990)  // behind the best: no update
        .add(1400, kAapl, 4, Side::Sell, 200, "AAPL", 1010)
        .cancel(1500, kAapl, 1, 40); // size at the best bid changes
    ScriptedStrategy s;
    const Result r = run(w, config_for(), s);
    ASSERT_EQ(s.book_updates.size(), 3U);
    EXPECT_EQ(s.book_updates[0].ts, 1100U);
    EXPECT_TRUE(s.book_updates[0].has_bid);
    EXPECT_FALSE(s.book_updates[0].has_ask);
    EXPECT_EQ(s.book_updates[1].ask_price, 1010U);
    EXPECT_EQ(s.book_updates[1].ask_qty, 200U);
    EXPECT_EQ(s.book_updates[2].bid_price, 1000U);
    EXPECT_EQ(s.book_updates[2].bid_qty, 60U);
    EXPECT_EQ(s.book_updates[2].locate, kAapl);
    EXPECT_EQ(r.stats.book_updates, 3U);
}

TEST(Engine, TradesAreReportedWithPriceSizeAndRestingSide) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Sell, 500, "AAPL", 1010)
        .execute(1200, kAapl, 1, 100, 1)
        .execute_with_price(1300, kAapl, 1, 50, 2, false, 1005)
        .trade(1400, kAapl, Side::Buy, 70, "AAPL", 1007, 3)
        .execute(1500, kMsft, 99, 1, 4); // other symbol (and unknown order): no callback
    ScriptedStrategy s;
    run(w, config_for(), s);
    ASSERT_EQ(s.trades.size(), 3U);
    EXPECT_EQ(s.trades[0].price, 1010U); // 'E' prints at the resting order's price
    EXPECT_EQ(s.trades[0].qty, 100U);
    EXPECT_EQ(s.trades[0].resting_side, Side::Sell);
    EXPECT_TRUE(s.trades[0].displayed);
    EXPECT_TRUE(s.trades[0].printable);
    EXPECT_EQ(s.trades[1].price, 1005U); // 'C' prints at its own price
    EXPECT_FALSE(s.trades[1].printable);
    EXPECT_EQ(s.trades[2].price, 1007U);
    EXPECT_FALSE(s.trades[2].displayed); // 'P': hidden order, side unknown
    EXPECT_EQ(s.trades[2].ts, 1400U);
}

TEST(Engine, AnOrderTakesEffectOnlyAfterTheOrderLatency) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Buy, 300, "AAPL", 1000)    // strategy submits here
        .add(1300, kAapl, 2, Side::Buy, 200, "AAPL", 1000) // lands before our order does
        .add(1700, kAapl, 3, Side::Buy, 900, "AAPL", 1000) // lands after
        .add(9000, kMsft, 4, Side::Buy, 1, "MSFT", 1);     // something to advance the clock
    Config c = config_for();
    c.latency.order_ns = 500;
    c.latency.ack_ns = 70;
    ScriptedStrategy s;
    OrderId id = kInvalidOrderId;
    s.book = [&](const BookUpdate& u, Context& ctx) {
        if (u.ts == 1100) {
            id = ctx.submit_limit(kAapl, Side::Buy, 1000, 100);
        }
    };
    run(w, c, s);
    ASSERT_EQ(s.order_updates.size(), 1U);
    const OrderUpdate& accepted = s.order_updates[0];
    EXPECT_EQ(accepted.id, id);
    EXPECT_EQ(accepted.kind, UpdateKind::Accepted);
    EXPECT_EQ(accepted.ts, 1600U);             // 1100 + order latency
    EXPECT_EQ(accepted.shares_ahead, 500U);    // orders 1 and 2, not 3
    EXPECT_EQ(s.order_update_times[0], 1670U); // exchange time + ack latency
}

TEST(Engine, ZeroLatencyOrderIsLiveBeforeTheNextMessage) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Buy, 300, "AAPL", 1000)
        .add(1100, kAapl, 2, Side::Buy, 200, "AAPL", 1000); // same timestamp, later message
    ScriptedStrategy s;
    s.book = [&](const BookUpdate& u, Context& ctx) {
        if (u.bid_qty == 300) {
            static_cast<void>(ctx.submit_limit(kAapl, Side::Buy, 1000, 100));
        }
    };
    run(w, config_for(), s);
    ASSERT_EQ(s.order_updates.size(), 1U);
    EXPECT_EQ(s.order_updates[0].shares_ahead, 300U); // order 2 arrived after ours
    EXPECT_EQ(s.order_updates[0].ts, 1100U);
}

TEST(Engine, OrdersWithTheSameReleaseTimeArriveInSubmissionOrder) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Sell, 100, "AAPL", 1010)
        .add(5000, kMsft, 2, Side::Buy, 1, "MSFT", 1);
    Config c = config_for();
    c.latency.order_ns = 100;
    ScriptedStrategy s;
    std::vector<OrderId> submitted;
    s.book = [&](const BookUpdate& /*u*/, Context& ctx) {
        // Three marketable orders against 100 displayed shares: all "fill" (no impact), and
        // the fills must come back in the order the orders were sent.
        for (int i = 0; i < 3; ++i) {
            submitted.push_back(ctx.submit_ioc(kAapl, Side::Buy, 1010, 100));
        }
    };
    run(w, c, s);
    const auto fills = of_kind(s, UpdateKind::Fill);
    ASSERT_EQ(fills.size(), 3U);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(fills[i].id, submitted[i]);
        EXPECT_EQ(fills[i].ts, 1200U);
    }
    EXPECT_EQ(counter_of(submitted[0]), 1U);
    EXPECT_EQ(counter_of(submitted[2]), 3U);
}

TEST(Engine, TimersFireAtTheirTimeBetweenMessages) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Buy, 100, "AAPL", 1000)
        .add(2000, kAapl, 2, Side::Buy, 100, "AAPL", 1001)
        .add(3000, kAapl, 3, Side::Buy, 100, "AAPL", 1002);
    ScriptedStrategy s;
    std::vector<Price> best_bid_at_timer;
    s.start = [](Context& ctx) {
        ctx.set_timer(2500);
        ctx.set_timer(1500);
        ctx.set_timer(2000); // same time as a message: fires before it is applied
    };
    s.timer = [&](Timestamp /*ts*/, Context& ctx) {
        best_bid_at_timer.push_back(ctx.book(kAapl).best_bid()->price);
    };
    const Result r = run(w, config_for(), s);
    EXPECT_EQ(s.timers, (std::vector<Timestamp>{1500, 2000, 2500}));
    EXPECT_EQ(best_bid_at_timer, (std::vector<Price>{1000, 1000, 1001}));
    EXPECT_EQ(r.stats.timers_fired, 3U);
}

TEST(Engine, TimerInThePastFiresAtTheCurrentTime) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Buy, 100, "AAPL", 1000)
        .add(1200, kAapl, 2, Side::Buy, 1, "AAPL", 1);
    ScriptedStrategy s;
    s.book = [](const BookUpdate& u, Context& ctx) {
        if (u.ts == 1100) {
            ctx.set_timer(5);
        }
    };
    run(w, config_for(), s);
    EXPECT_EQ(s.timers, (std::vector<Timestamp>{1100}));
}

TEST(Engine, RiskRejectIsImmediateAndNothingReachesTheExchange) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Sell, 100, "AAPL", 1010)
        .add(1200, kAapl, 2, Side::Sell, 1, "AAPL", 2000);
    Config c = config_for();
    c.latency.order_ns = 500;
    c.latency.ack_ns = 500;
    c.risk.max_order_qty = 50;
    ScriptedStrategy s;
    s.book = [](const BookUpdate& u, Context& ctx) {
        if (u.ts == 1100) {
            static_cast<void>(ctx.submit_ioc(kAapl, Side::Buy, 1010, 100));
        }
    };
    const Result r = run(w, c, s);
    ASSERT_EQ(s.order_updates.size(), 1U);
    EXPECT_EQ(s.order_updates[0].kind, UpdateKind::Rejected);
    EXPECT_EQ(s.order_updates[0].reason, RejectReason::MaxOrderQty);
    EXPECT_EQ(s.order_updates[0].state, OrderState::Rejected);
    EXPECT_EQ(s.order_updates[0].removed_qty, 100U);
    EXPECT_EQ(s.order_update_times[0], 1100U); // synchronous: no latency either way
    EXPECT_EQ(r.stats.orders_rejected_by_risk, 1U);
    EXPECT_EQ(r.stats.fills, 0U);
}

TEST(Engine, OrdersAreRejectedBeforeTheOpenAndWhileHalted) {
    ItchWriter w;
    w.system_event(1, 'O')
        .stock_directory(2, kAapl, "AAPL")
        .trading_action(3, kAapl, "AAPL", 'T')
        .add(500, kAapl, 1, Side::Sell, 100, "AAPL", 1010) // before the open
        .system_event(kOpen, 'Q')
        .trading_action(1500, kAapl, "AAPL", 'H')
        .add(1600, kAapl, 2, Side::Sell, 100, "AAPL", 1005) // halted
        .trading_action(1700, kAapl, "AAPL", 'T')
        .add(1800, kAapl, 3, Side::Sell, 100, "AAPL", 1000); // trading again
    ScriptedStrategy s;
    std::vector<bool> tradable;
    s.book = [&](const BookUpdate& u, Context& ctx) {
        tradable.push_back(ctx.tradable(kAapl));
        static_cast<void>(ctx.submit_ioc(kAapl, Side::Buy, u.ask_price, 100));
    };
    run(w, config_for(), s);
    ASSERT_EQ(s.order_updates.size(), 3U);
    EXPECT_EQ(s.order_updates[0].reason, RejectReason::MarketClosed);
    EXPECT_EQ(s.order_updates[1].reason, RejectReason::SymbolHalted);
    EXPECT_EQ(s.order_updates[2].kind, UpdateKind::Fill);
    EXPECT_EQ(tradable, (std::vector<bool>{false, false, true}));
}

TEST(Engine, HaltBetweenSendingAndArrivalIsAnExchangeReject) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Sell, 100, "AAPL", 1010)
        .trading_action(1200, kAapl, "AAPL", 'H')
        .add(5000, kMsft, 2, Side::Buy, 1, "MSFT", 1);
    Config c = config_for();
    c.latency.order_ns = 500;
    ScriptedStrategy s;
    s.book = [](const BookUpdate& /*u*/, Context& ctx) {
        static_cast<void>(ctx.submit_ioc(kAapl, Side::Buy, 1010, 100));
    };
    const Result r = run(w, c, s);
    ASSERT_EQ(s.order_updates.size(), 1U);
    EXPECT_EQ(s.order_updates[0].kind, UpdateKind::Rejected);
    EXPECT_EQ(s.order_updates[0].reason, RejectReason::ExchangeHalted);
    EXPECT_EQ(r.stats.orders_rejected_by_exchange, 1U);
    EXPECT_EQ(r.stats.orders_rejected_by_risk, 0U);
}

TEST(Engine, PassiveFillFlowsThroughToPositionAfterTheAckLatency) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Buy, 300, "AAPL", 1000) // strategy joins behind this
        .add(1150, kAapl, 2, Side::Sell, 300, "AAPL", 1010)
        .add(1200, kAapl, 3, Side::Buy, 500, "AAPL", 1000) // behind the strategy's order
        .execute(1300, kAapl, 1, 300, 1)                   // clears the queue ahead
        .execute(1400, kAapl, 3, 60, 2)                    // fills 60 of ours
        .add(1410, kMsft, 4, Side::Buy, 1, "MSFT", 1)      // before the fill is acknowledged
        .add(1500, kMsft, 5, Side::Buy, 1, "MSFT", 1);     // after
    Config c = config_for();
    c.latency.ack_ns = 50;
    ScriptedStrategy s;
    std::vector<std::int64_t> position_seen;
    s.book = [&](const BookUpdate& u, Context& ctx) {
        if (u.ts == 1100) {
            static_cast<void>(ctx.submit_limit(kAapl, Side::Buy, 1000, 100));
        }
    };
    s.trade = [&](const Trade& t, Context& ctx) {
        if (t.ts == 1400) {
            position_seen.push_back(ctx.position(kAapl)); // filled at the exchange, not yet known
        }
    };
    s.order = [&](const OrderUpdate& u, Context& ctx) {
        if (u.kind == UpdateKind::Fill) {
            position_seen.push_back(ctx.position(kAapl));
        }
    };
    const Result r = run(w, c, s);
    const auto fills = of_kind(s, UpdateKind::Fill);
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills[0].fill_qty, 60U);
    EXPECT_EQ(fills[0].fill_price, 1000U);
    EXPECT_EQ(fills[0].ts, 1400U);
    EXPECT_FALSE(fills[0].aggressive);
    EXPECT_EQ(position_seen, (std::vector<std::int64_t>{0, 60}));
    EXPECT_EQ(r.position, 60);
    EXPECT_EQ(r.stats.shares_filled, 60U);
    // Bought 60 at 1000, marked at the mid of 1000 x 1010: 60 x 5 = 300, so twice that is 600.
    EXPECT_EQ(r.pnl2, 600);
}

TEST(Engine, CancelTakesEffectAfterTheCancelLatency) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Buy, 300, "AAPL", 1000)     // strategy rests behind this
        .add(1200, kAapl, 2, Side::Sell, 300, "AAPL", 1010) // strategy asks to cancel here
        .execute(1250, kAapl, 1, 300, 1)                    // queue ahead clears
        .add(1260, kAapl, 3, Side::Buy, 500, "AAPL", 1000)
        .execute(1300, kAapl, 3, 30, 2) // cancel not there yet: 30 fill
        .execute(1900, kAapl, 3, 30, 3) // cancelled by now: no fill
        .add(5000, kMsft, 4, Side::Buy, 1, "MSFT", 1);
    Config c = config_for();
    c.latency.cancel_ns = 400; // requested at 1200, effective at 1600
    ScriptedStrategy s;
    OrderId id = kInvalidOrderId;
    s.book = [&](const BookUpdate& u, Context& ctx) {
        if (u.ts == 1100) {
            id = ctx.submit_limit(kAapl, Side::Buy, 1000, 100);
        } else if (u.ts == 1200) {
            ctx.cancel(id);
            ctx.cancel(id); // asking twice sends one cancel
        }
    };
    const Result r = run(w, c, s);
    ASSERT_EQ(s.order_updates.size(), 3U);
    EXPECT_EQ(s.order_updates[0].kind, UpdateKind::Accepted);
    EXPECT_EQ(s.order_updates[1].kind, UpdateKind::Fill);
    EXPECT_EQ(s.order_updates[1].fill_qty, 30U);
    EXPECT_EQ(s.order_updates[1].state, OrderState::PartiallyFilled);
    EXPECT_EQ(s.order_updates[2].kind, UpdateKind::Cancelled);
    EXPECT_EQ(s.order_updates[2].ts, 1600U);
    EXPECT_EQ(s.order_updates[2].removed_qty, 70U);
    EXPECT_EQ(r.stats.cancels_requested, 1U);
    EXPECT_EQ(r.position, 30);
}

TEST(Engine, CancelOfAFilledOrderIsRejectedAsTooLate) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Sell, 100, "AAPL", 1010)
        .add(1200, kAapl, 2, Side::Sell, 100, "AAPL", 1020)
        .add(5000, kMsft, 3, Side::Buy, 1, "MSFT", 1);
    Config c = config_for();
    c.latency.order_ns = 50;
    c.latency.cancel_ns = 100;
    ScriptedStrategy s;
    OrderId id = kInvalidOrderId;
    s.book = [&](const BookUpdate& u, Context& ctx) {
        if (u.ts == 1100) {
            id = ctx.submit_limit(kAapl, Side::Buy, 1010, 100); // marketable: fills at 1150
            ctx.cancel(id);                                     // arrives at 1200
        }
    };
    run(w, c, s);
    ASSERT_EQ(s.order_updates.size(), 3U);
    EXPECT_EQ(s.order_updates[0].kind, UpdateKind::Accepted);
    EXPECT_EQ(s.order_updates[1].kind, UpdateKind::Fill);
    EXPECT_EQ(s.order_updates[1].state, OrderState::Filled);
    EXPECT_EQ(s.order_updates[2].kind, UpdateKind::CancelRejected);
    EXPECT_EQ(s.order_updates[2].reason, RejectReason::TooLateToCancel);
    EXPECT_EQ(s.order_updates[2].state, OrderState::Filled);
}

TEST(Engine, DayOrdersExpireAtTheEndOfMarketHours) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Buy, 300, "AAPL", 1000)
        .system_event(9000, 'M')
        .add(9100, kAapl, 2, Side::Buy, 500, "AAPL", 1000)
        .execute(9200, kAapl, 1, 300, 1)
        .execute(9300, kAapl, 2, 100, 2); // after hours: would have filled a live order
    ScriptedStrategy s;
    s.book = [](const BookUpdate& u, Context& ctx) {
        if (u.ts == 1100) {
            static_cast<void>(ctx.submit_limit(kAapl, Side::Buy, 1000, 100));
        }
    };
    const Result r = run(w, config_for(), s);
    ASSERT_EQ(s.order_updates.size(), 2U);
    EXPECT_EQ(s.order_updates[1].kind, UpdateKind::Cancelled);
    EXPECT_EQ(s.order_updates[1].ts, 9000U);
    EXPECT_EQ(s.order_updates[1].state, OrderState::Cancelled);
    EXPECT_EQ(r.stats.fills, 0U);
}

TEST(Engine, OrdersForUnsubscribedSymbolsGetNoId) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Buy, 300, "AAPL", 1000);
    ScriptedStrategy s;
    OrderId id = 1;
    s.book = [&](const BookUpdate& /*u*/, Context& ctx) {
        id = ctx.submit_limit(kMsft, Side::Buy, 1000, 100);
    };
    const Result r = run(w, config_for(), s);
    EXPECT_EQ(id, kInvalidOrderId);
    EXPECT_TRUE(s.order_updates.empty());
    EXPECT_EQ(r.stats.submits_refused, 1U);
}

TEST(Engine, UpdatesStillInFlightAtTheEndOfDataAreDelivered) {
    ItchWriter w = session();
    w.add(1100, kAapl, 1, Side::Sell, 100, "AAPL", 1010); // last message in the file
    Config c = config_for();
    c.latency.order_ns = 1'000'000;
    c.latency.ack_ns = 1'000'000;
    ScriptedStrategy s;
    s.book = [](const BookUpdate& /*u*/, Context& ctx) {
        static_cast<void>(ctx.submit_ioc(kAapl, Side::Buy, 1010, 100));
    };
    const Result r = run(w, c, s);
    ASSERT_EQ(s.order_updates.size(), 1U);
    EXPECT_EQ(s.order_updates[0].kind, UpdateKind::Fill);
    EXPECT_EQ(s.order_update_times[0], 1100U + 2'000'000U);
    EXPECT_EQ(r.position, 100);
    EXPECT_TRUE(s.ended);
}

// --- Determinism ---------------------------------------------------------------------------------

std::vector<char> slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Config maker_config(bool build_all_books) {
    Config c = config_for({"AAA", "BBB"});
    c.build_all_books = build_all_books;
    c.latency.order_ns = 2'500;
    c.latency.cancel_ns = 2'500;
    c.latency.ack_ns = 1'500;
    c.exchange.max_orders_per_symbol = 100'000;
    c.risk.max_position = 600;
    c.risk.max_open_orders = 4;
    // Tight enough that some orders are refused, so risk.bin is exercised too.
    c.risk.rate_limit_per_second = 50'000;
    c.risk.rate_limit_burst = 5;
    c.passive_maker.lot = 100;
    c.passive_maker.max_inventory = 400;
    return c;
}

/// Runs PassiveMaker over a random stream into a fresh run directory and returns the bytes of
/// each output file.
std::vector<std::vector<char>> record_maker_run(const ItchWriter& stream, const Config& config,
                                                RunStats& stats) {
    const std::filesystem::path dir = test::TempFile::unique_path();
    record::Recorder recorder;
    std::string error;
    EXPECT_TRUE(recorder.open(dir, error)) << error;
    strategies::PassiveMaker maker(config.passive_maker);
    Backtester<strategies::PassiveMaker> engine(config, maker, recorder);
    EXPECT_TRUE(engine.prepare(stream.bytes(), error)) << error;
    EXPECT_TRUE(engine.run(stream.bytes()).ok());
    stats = engine.stats();
    EXPECT_TRUE(recorder.close());
    std::vector<std::vector<char>> files;
    for (const char* name : {"orders.bin", "fills.bin", "bbo.bin", "risk.bin"}) {
        files.push_back(slurp(dir / name));
    }
    std::filesystem::remove_all(dir);
    return files;
}

TEST(Determinism, TwoRunsProduceByteIdenticalFiles) {
    test::RandomStreamOptions options;
    options.seed = 42;
    options.events = 40'000;
    const ItchWriter stream = test::random_stream(options);
    RunStats first_stats;
    RunStats second_stats;
    const auto first = record_maker_run(stream, maker_config(true), first_stats);
    const auto second = record_maker_run(stream, maker_config(true), second_stats);

    // The run must actually have traded, or identical output proves nothing.
    EXPECT_GT(first_stats.orders_submitted, 100U);
    EXPECT_GT(first_stats.fills, 20U);
    EXPECT_GT(first_stats.cancels_requested, 20U);
    EXPECT_GT(first_stats.orders_rejected_by_risk, 5U);
    EXPECT_EQ(first_stats.events_dropped, 0U);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_GT(first[i].size(), 24U) << "file " << i << " holds only a header";
        EXPECT_EQ(first[i], second[i]) << "file " << i << " differs between runs";
    }
}

TEST(Determinism, BuildingOtherSymbolsBooksDoesNotChangeTheOutput) {
    test::RandomStreamOptions options;
    options.seed = 7;
    options.events = 40'000;
    const ItchWriter stream = test::random_stream(options); // AAA, BBB traded; CCC is not
    RunStats all_stats;
    RunStats traded_stats;
    const auto all = record_maker_run(stream, maker_config(true), all_stats);
    const auto traded_only = record_maker_run(stream, maker_config(false), traded_stats);
    EXPECT_GT(all_stats.fills, 20U);
    for (std::size_t i = 0; i < all.size(); ++i) {
        EXPECT_EQ(all[i], traded_only[i]) << "file " << i;
    }
}

TEST(Determinism, DifferentInputProducesDifferentOutput) {
    // Guards the two tests above against comparing files that never depend on the input.
    test::RandomStreamOptions options;
    options.events = 20'000;
    options.seed = 1;
    const ItchWriter a = test::random_stream(options);
    options.seed = 2;
    const ItchWriter b = test::random_stream(options);
    RunStats stats;
    EXPECT_NE(record_maker_run(a, maker_config(true), stats)[0],
              record_maker_run(b, maker_config(true), stats)[0]);
}

} // namespace
} // namespace itchbt::sim
