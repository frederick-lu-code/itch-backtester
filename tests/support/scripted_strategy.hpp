#pragma once

// A strategy for tests: it records everything it is told and runs caller-supplied hooks.

#include "itchbt/sim/engine.hpp"

#include <functional>
#include <vector>

namespace itchbt::test {

struct ScriptedStrategy {
    std::function<void(sim::Context&)> start;
    std::function<void(const sim::BookUpdate&, sim::Context&)> book;
    std::function<void(const sim::Trade&, sim::Context&)> trade;
    std::function<void(const sim::OrderUpdate&, sim::Context&)> order;
    std::function<void(Timestamp, sim::Context&)> timer;

    std::vector<sim::BookUpdate> book_updates;
    std::vector<sim::Trade> trades;
    std::vector<sim::OrderUpdate> order_updates;
    /// Simulation time at which each order update was delivered to the strategy.
    std::vector<Timestamp> order_update_times;
    std::vector<Timestamp> timers;
    bool started = false;
    bool ended = false;

    void on_start(sim::Context& ctx) {
        started = true;
        if (start) {
            start(ctx);
        }
    }
    void on_book_update(const sim::BookUpdate& u, sim::Context& ctx) {
        book_updates.push_back(u);
        if (book) {
            book(u, ctx);
        }
    }
    void on_trade(const sim::Trade& t, sim::Context& ctx) {
        trades.push_back(t);
        if (trade) {
            trade(t, ctx);
        }
    }
    void on_order_update(const sim::OrderUpdate& u, sim::Context& ctx) {
        order_updates.push_back(u);
        order_update_times.push_back(ctx.now());
        if (order) {
            order(u, ctx);
        }
    }
    void on_timer(Timestamp ts, sim::Context& ctx) {
        timers.push_back(ts);
        if (timer) {
            timer(ts, ctx);
        }
    }
    void on_end(sim::Context& /*ctx*/) { ended = true; }
};

static_assert(sim::Strategy<ScriptedStrategy>);

} // namespace itchbt::test
