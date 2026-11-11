#pragma once

// Runtime-polymorphic strategies, for when the strategy type is not known at compile time.
//
// The engine binds its strategy through the Strategy concept so callbacks inline. IStrategy is
// the classic alternative: an abstract base class. VirtualAdapter wraps a pointer to one so it
// satisfies the concept, at the cost of an indirect call per callback. That cost is a
// benchmark item (BENCHMARKS.md, "Dispatch").

#include "itchbt/sim/engine.hpp"

namespace itchbt::strategies {

class IStrategy {
public:
    IStrategy() = default;
    IStrategy(const IStrategy&) = delete;
    IStrategy& operator=(const IStrategy&) = delete;
    virtual ~IStrategy() = default;

    virtual void on_start(sim::Context& ctx) = 0;
    virtual void on_book_update(const sim::BookUpdate& update, sim::Context& ctx) = 0;
    virtual void on_trade(const sim::Trade& trade, sim::Context& ctx) = 0;
    virtual void on_order_update(const sim::OrderUpdate& update, sim::Context& ctx) = 0;
    virtual void on_timer(Timestamp ts, sim::Context& ctx) = 0;
    virtual void on_end(sim::Context& ctx) = 0;
};

/// Makes any concept-satisfying strategy available through IStrategy.
template <sim::Strategy S>
class Polymorphic final : public IStrategy {
public:
    template <typename... Args>
    explicit Polymorphic(Args&&... args) : strategy_(std::forward<Args>(args)...) {}

    void on_start(sim::Context& ctx) override { strategy_.on_start(ctx); }
    void on_book_update(const sim::BookUpdate& update, sim::Context& ctx) override {
        strategy_.on_book_update(update, ctx);
    }
    void on_trade(const sim::Trade& trade, sim::Context& ctx) override {
        strategy_.on_trade(trade, ctx);
    }
    void on_order_update(const sim::OrderUpdate& update, sim::Context& ctx) override {
        strategy_.on_order_update(update, ctx);
    }
    void on_timer(Timestamp ts, sim::Context& ctx) override { strategy_.on_timer(ts, ctx); }
    void on_end(sim::Context& ctx) override { strategy_.on_end(ctx); }

    [[nodiscard]] S& strategy() noexcept { return strategy_; }

private:
    S strategy_;
};

/// Satisfies the Strategy concept by forwarding every callback through a virtual call.
class VirtualAdapter {
public:
    explicit VirtualAdapter(IStrategy& strategy) noexcept : strategy_(&strategy) {}

    void on_start(sim::Context& ctx) { strategy_->on_start(ctx); }
    void on_book_update(const sim::BookUpdate& update, sim::Context& ctx) {
        strategy_->on_book_update(update, ctx);
    }
    void on_trade(const sim::Trade& trade, sim::Context& ctx) { strategy_->on_trade(trade, ctx); }
    void on_order_update(const sim::OrderUpdate& update, sim::Context& ctx) {
        strategy_->on_order_update(update, ctx);
    }
    void on_timer(Timestamp ts, sim::Context& ctx) { strategy_->on_timer(ts, ctx); }
    void on_end(sim::Context& ctx) { strategy_->on_end(ctx); }

private:
    IStrategy* strategy_;
};

static_assert(sim::Strategy<VirtualAdapter>);

} // namespace itchbt::strategies
