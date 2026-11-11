#pragma once

// PassiveMaker: keeps one lot resting at the best bid and one at the best ask of each symbol.
//
// It exists to exercise the engine (queue position, passive fills, cancels, risk limits), not
// to make money. Per symbol and side it is a three-state machine:
//
//   Idle --submit--> Working --price moved: cancel--> Cancelling --terminal update--> Idle
//
// Inventory control: a side stops quoting once the position reaches max_inventory in that
// direction, and its quote backs away from the touch by skew_per_lot for every lot held.
//
// Each symbol's state depends only on that symbol's own events, which is what lets sharded
// runs reproduce single-threaded results exactly.

#include "itchbt/sim/engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace itchbt::strategies {

class PassiveMaker {
public:
    explicit PassiveMaker(const sim::PassiveMakerConfig& config)
        : config_(config), quotes_(65536) {}

    void on_start(sim::Context& /*ctx*/) noexcept {}

    void on_book_update(const sim::BookUpdate& update, sim::Context& ctx) {
        requote(update.locate, ctx);
    }

    void on_trade(const sim::Trade& /*trade*/, sim::Context& /*ctx*/) noexcept {}

    void on_order_update(const sim::OrderUpdate& update, sim::Context& ctx) {
        Quote& quote = quotes_[update.locate].side[index(update.side)];
        if (update.id != quote.id) {
            return;
        }
        if (sim::is_terminal(update.state)) {
            const bool rejected = update.kind == sim::UpdateKind::Rejected;
            quote = Quote{};
            // After a reject, wait for the next book update before trying again: resubmitting
            // from inside the reject would spin at one timestamp.
            if (!rejected) {
                requote(update.locate, ctx);
            }
        }
    }

    void on_timer(Timestamp /*ts*/, sim::Context& /*ctx*/) noexcept {}
    void on_end(sim::Context& /*ctx*/) noexcept {}

private:
    enum class Phase : std::uint8_t { Idle, Working, Cancelling };

    struct Quote {
        sim::OrderId id = sim::kInvalidOrderId;
        Price price = 0;
        Phase phase = Phase::Idle;
    };
    struct SymbolQuotes {
        std::array<Quote, 2> side;
    };

    static std::size_t index(Side side) noexcept { return side == Side::Buy ? 0 : 1; }

    void requote(Locate locate, sim::Context& ctx) {
        const auto& book = ctx.book(locate);
        const auto bid = book.best_bid();
        const auto ask = book.best_ask();
        if (!ctx.tradable(locate) || !bid || !ask || bid->price >= ask->price) {
            return; // closed, halted, or no clean two-sided market to quote against
        }
        const std::int64_t position = ctx.position(locate);
        const std::int64_t lots = position / static_cast<std::int64_t>(config_.lot);
        const auto skew = static_cast<Price>((lots < 0 ? -lots : lots) * config_.skew_per_lot);

        // Long: the bid backs off. Short: the offer backs off. The other side stays at the touch.
        const Price bid_target = lots > 0 && bid->price > skew ? bid->price - skew : bid->price;
        const Price ask_target = lots < 0 ? ask->price + skew : ask->price;
        manage(locate, Side::Buy, bid_target, position < config_.max_inventory, ctx);
        manage(locate, Side::Sell, ask_target, position > -config_.max_inventory, ctx);
    }

    void manage(Locate locate, Side side, Price target, bool allowed, sim::Context& ctx) {
        Quote& quote = quotes_[locate].side[index(side)];
        switch (quote.phase) {
        case Phase::Idle:
            if (allowed) {
                quote.id = ctx.submit_limit(locate, side, target, config_.lot);
                if (quote.id != sim::kInvalidOrderId) {
                    quote.price = target;
                    quote.phase = Phase::Working;
                }
            }
            break;
        case Phase::Working:
            if (!allowed || quote.price != target) {
                ctx.cancel(quote.id);
                quote.phase = Phase::Cancelling;
            }
            break;
        case Phase::Cancelling: break; // wait for the terminal update
        }
    }

    sim::PassiveMakerConfig config_;
    std::vector<SymbolQuotes> quotes_; // by locate
};

static_assert(sim::Strategy<PassiveMaker>);

} // namespace itchbt::strategies
