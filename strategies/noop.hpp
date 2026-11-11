#pragma once

// A strategy that does nothing. Used for replay benchmarks and as the smallest example of the
// Strategy concept.

#include "itchbt/sim/engine.hpp"

namespace itchbt::strategies {

struct NoOp {
    void on_start(sim::Context& /*ctx*/) noexcept {}
    void on_book_update(const sim::BookUpdate& /*update*/, sim::Context& /*ctx*/) noexcept {}
    void on_trade(const sim::Trade& /*trade*/, sim::Context& /*ctx*/) noexcept {}
    void on_order_update(const sim::OrderUpdate& /*update*/, sim::Context& /*ctx*/) noexcept {}
    void on_timer(Timestamp /*ts*/, sim::Context& /*ctx*/) noexcept {}
    void on_end(sim::Context& /*ctx*/) noexcept {}
};

static_assert(sim::Strategy<NoOp>);

} // namespace itchbt::strategies
