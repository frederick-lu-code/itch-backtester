#pragma once

// ImbalanceTaker: sends an immediate-or-cancel order when the top of book is lopsided.
//
// If the bid holds at least threshold_pct of the combined best bid and ask size, it buys one
// lot at the ask; if the ask does, it sells one lot at the bid. One order per symbol at a
// time, a cooldown between orders, and an inventory cap. It exists to exercise marketable
// orders and the IOC path, not to make money.

#include "itchbt/sim/engine.hpp"

#include <cstdint>
#include <vector>

namespace itchbt::strategies {

class ImbalanceTaker {
public:
    explicit ImbalanceTaker(const sim::ImbalanceTakerConfig& config)
        : config_(config), state_(65536) {}

    void on_start(sim::Context& /*ctx*/) noexcept {}

    void on_book_update(const sim::BookUpdate& update, sim::Context& ctx) {
        State& state = state_[update.locate];
        if (state.pending != sim::kInvalidOrderId || !ctx.tradable(update.locate) ||
            !update.has_bid || !update.has_ask || update.bid_price >= update.ask_price ||
            (state.traded && update.ts - state.last_order_ts < config_.cooldown_ns)) {
            return;
        }
        const book::LevelQty total = update.bid_qty + update.ask_qty;
        if (total == 0) {
            return;
        }
        const std::int64_t position = ctx.position(update.locate);
        // Integer form of bid_qty / total >= threshold_pct / 100.
        if (update.bid_qty * 100 >= total * config_.threshold_pct &&
            position < config_.max_inventory) {
            send(state, update, Side::Buy, update.ask_price, ctx);
        } else if (update.ask_qty * 100 >= total * config_.threshold_pct &&
                   position > -config_.max_inventory) {
            send(state, update, Side::Sell, update.bid_price, ctx);
        }
    }

    void on_trade(const sim::Trade& /*trade*/, sim::Context& /*ctx*/) noexcept {}

    void on_order_update(const sim::OrderUpdate& update, sim::Context& /*ctx*/) noexcept {
        State& state = state_[update.locate];
        if (update.id == state.pending && sim::is_terminal(update.state)) {
            state.pending = sim::kInvalidOrderId;
        }
    }

    void on_timer(Timestamp /*ts*/, sim::Context& /*ctx*/) noexcept {}
    void on_end(sim::Context& /*ctx*/) noexcept {}

private:
    struct State {
        sim::OrderId pending = sim::kInvalidOrderId;
        Timestamp last_order_ts = 0;
        bool traded = false;
    };

    void send(State& state, const sim::BookUpdate& update, Side side, Price price,
              sim::Context& ctx) const {
        state.pending = ctx.submit_ioc(update.locate, side, price, config_.lot);
        state.last_order_ts = update.ts;
        state.traded = true;
    }

    sim::ImbalanceTakerConfig config_;
    std::vector<State> state_; // by locate
};

static_assert(sim::Strategy<ImbalanceTaker>);

} // namespace itchbt::strategies
