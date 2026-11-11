#pragma once

// Risk: pre-trade checks, position and PnL tracking, and the kill switch.
//
// Everything is integer arithmetic. Prices are in units of 1/10000 USD, so notional and PnL
// are in (1/10000 USD) x shares. The mid price can fall on a half unit, so marks are kept as
// bid + ask (twice the mid) and PnL as twice its value; the division by two happens once, when
// a figure is reported.
//
// The risk gate is the firm's view: it learns about fills and cancels when the order update
// reaches the strategy side, after the acknowledgement latency, exactly as the strategy does.

#include "itchbt/sim/config.hpp"
#include "itchbt/sim/types.hpp"
#include "itchbt/util/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace itchbt::risk {

using sim::RejectReason;

enum class KillReason : std::uint8_t { None, LossLimit, RejectRate };

/// Everything the pre-trade checks need to know about the market at the moment of the order.
struct MarketView {
    bool regular_hours = false;
    bool trading = false;
    bool has_bid = false;
    bool has_ask = false;
    Price bid = 0;
    Price ask = 0;
};

/// Risk state for one traded symbol.
struct SymbolRisk {
    std::int64_t position = 0;     // signed shares
    std::int64_t open_cost = 0;    // signed cost of the open position, price units x shares
    std::int64_t cash = 0;         // minus the signed value of every fill
    std::int64_t open_buy_qty = 0; // shares in buy orders not yet filled or closed
    std::int64_t open_sell_qty = 0;
    std::int64_t open_notional = 0; // limit price x open shares, both sides
    std::int64_t mark2 = 0;         // bid + ask at the last two-sided top of book
    std::int64_t gross2 = 0;        // |position| x mark2
    std::int64_t pnl2 = 0;          // 2 x (cash + position x mid)
    std::uint32_t open_orders = 0;
    bool has_mark = false;

    /// Realised PnL: what closing trades have locked in. Average cost, with the per-fill
    /// rounding (under one price unit) falling in the realised/unrealised split, never in the
    /// total.
    [[nodiscard]] std::int64_t realised() const noexcept { return cash + open_cost; }
    /// Unrealised PnL marked to mid, times two.
    [[nodiscard]] std::int64_t unrealised2() const noexcept {
        return position * mark2 - 2 * open_cost;
    }
};

class RiskGate {
public:
    /// A gate with every limit off and no symbols, to be set up later with configure().
    RiskGate() = default;

    RiskGate(const sim::RiskConfig& config, bool extended_hours, std::size_t symbols) {
        configure(config, extended_hours, symbols);
    }

    /// Sets the limits and the number of traded symbols. Discards all positions and history.
    void configure(const sim::RiskConfig& config, bool extended_hours, std::size_t symbols) {
        *this = RiskGate();
        config_ = config;
        extended_hours_ = extended_hours;
        symbols_.assign(symbols, SymbolRisk{});
        bucket_capacity_ = std::int64_t{config.rate_limit_burst} * kNanos;
        tokens_ = bucket_capacity_;
        recent_rejects_.assign(config.reject_threshold == 0 ? 0 : config.reject_threshold + 1U, 0);
    }

    // --- Pre-trade ---------------------------------------------------------------------------

    /// Runs every pre-trade check in a fixed order and returns the first that fails, or None.
    /// On success the order's exposure is counted as open and a rate-limit token is spent.
    [[nodiscard]] RejectReason check_and_reserve(std::size_t symbol, Side side, Price price,
                                                 Qty qty, Timestamp now,
                                                 const MarketView& market) noexcept {
        const RejectReason reason = check(symbol, side, price, qty, now, market);
        if (reason == RejectReason::None) {
            SymbolRisk& s = symbols_[symbol];
            const std::int64_t notional = std::int64_t{price} * qty;
            (side == Side::Buy ? s.open_buy_qty : s.open_sell_qty) += qty;
            s.open_notional += notional;
            total_open_notional_ += notional;
            ++s.open_orders;
            if (config_.rate_limit_per_second != 0) {
                tokens_ -= kNanos;
            }
        }
        return reason;
    }

    /// Records a rejection for the kill switch's rolling window. Returns true if this
    /// rejection tripped the switch.
    bool note_reject(Timestamp now) noexcept {
        ++rejects_;
        if (recent_rejects_.empty() || tripped()) {
            return false;
        }
        // The ring holds the last threshold + 1 reject times. Once it is full, the oldest of
        // them being inside the window means more than `threshold` rejects in the window.
        recent_rejects_[reject_head_] = now;
        reject_head_ = (reject_head_ + 1) % recent_rejects_.size();
        reject_count_ = std::min(reject_count_ + 1, recent_rejects_.size());
        if (reject_count_ == recent_rejects_.size() &&
            now - recent_rejects_[reject_head_] <= config_.reject_window_ns) {
            kill_reason_ = KillReason::RejectRate;
            return true;
        }
        return false;
    }

    // --- Post-trade --------------------------------------------------------------------------

    /// A fill reached the firm. Returns true if the resulting PnL tripped the kill switch.
    bool on_fill(std::size_t symbol, Side side, Price order_price, Price fill_price,
                 Qty qty) noexcept {
        SymbolRisk& s = symbols_[symbol];
        release(s, side, order_price, qty);

        const std::int64_t signed_qty = side == Side::Buy ? std::int64_t{qty} : -std::int64_t{qty};
        const std::int64_t value = signed_qty * std::int64_t{fill_price};
        const bool reduces = s.position != 0 && (s.position > 0) != (signed_qty > 0);
        if (!reduces) {
            s.open_cost += value;
        } else if (std::llabs(signed_qty) <= std::llabs(s.position)) {
            // Closing part of the position: remove the same fraction of its cost.
            s.open_cost -= s.open_cost * std::llabs(signed_qty) / std::llabs(s.position);
        } else {
            // Through zero: the old position is closed and the excess opens a new one.
            s.open_cost = (s.position + signed_qty) * std::int64_t{fill_price};
        }
        s.position += signed_qty;
        s.cash -= value;
        if (s.position == 0) {
            s.open_cost = 0;
        }
        if (!s.has_mark) {
            // No two-sided market seen yet: mark at the fill price rather than at zero, which
            // would book the whole purchase as a loss.
            s.mark2 = 2 * std::int64_t{fill_price};
            s.has_mark = true;
        }
        ++fills_;
        refresh(s);
        return check_loss();
    }

    /// An order's remaining shares will never trade (cancelled, or rejected by the exchange).
    void on_closed(std::size_t symbol, Side side, Price order_price, Qty removed_qty,
                   bool order_done) noexcept {
        SymbolRisk& s = symbols_[symbol];
        release(s, side, order_price, removed_qty);
        if (order_done && s.open_orders > 0) {
            --s.open_orders;
        }
    }

    /// An order filled completely: it no longer counts against the open-order limit.
    void on_order_done(std::size_t symbol) noexcept {
        if (symbols_[symbol].open_orders > 0) {
            --symbols_[symbol].open_orders;
        }
    }

    /// The top of book changed. Marks need both sides; a one-sided book keeps the last mark.
    /// Returns true if the resulting PnL tripped the kill switch.
    bool on_top_of_book(std::size_t symbol, const MarketView& market) noexcept {
        if (!market.has_bid || !market.has_ask) {
            return false;
        }
        SymbolRisk& s = symbols_[symbol];
        s.mark2 = std::int64_t{market.bid} + std::int64_t{market.ask};
        s.has_mark = true;
        if (s.position == 0) {
            return false; // PnL cannot have moved
        }
        refresh(s);
        return check_loss();
    }

    // --- State -------------------------------------------------------------------------------

    [[nodiscard]] bool tripped() const noexcept { return kill_reason_ != KillReason::None; }
    [[nodiscard]] KillReason kill_reason() const noexcept { return kill_reason_; }
    [[nodiscard]] const SymbolRisk& symbol(std::size_t index) const noexcept {
        return symbols_[index];
    }
    [[nodiscard]] std::int64_t position(std::size_t index) const noexcept {
        return symbols_[index].position;
    }
    /// Total PnL (realised plus unrealised at mid) times two, in price units x shares.
    [[nodiscard]] std::int64_t total_pnl2() const noexcept { return total_pnl2_; }
    /// Sum over symbols of |position| x mid, times two.
    [[nodiscard]] std::int64_t gross_position_notional2() const noexcept { return total_gross2_; }
    [[nodiscard]] std::int64_t open_notional() const noexcept { return total_open_notional_; }
    [[nodiscard]] std::uint64_t rejects() const noexcept { return rejects_; }
    [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
    [[nodiscard]] std::size_t symbol_count() const noexcept { return symbols_.size(); }

private:
    static constexpr std::int64_t kNanos = 1'000'000'000;

    [[nodiscard]] RejectReason check(std::size_t symbol, Side side, Price price, Qty qty,
                                     Timestamp now, const MarketView& market) noexcept {
        if (tripped()) {
            return RejectReason::KillSwitch;
        }
        if (qty == 0 || price == 0) {
            return RejectReason::InvalidOrder;
        }
        if (!market.regular_hours && !extended_hours_) {
            return RejectReason::MarketClosed;
        }
        if (!market.trading) {
            return RejectReason::SymbolHalted;
        }
        if (qty > config_.max_order_qty) {
            return RejectReason::MaxOrderQty;
        }
        const std::int64_t notional = std::int64_t{price} * qty;
        if (notional > config_.max_order_notional) {
            return RejectReason::MaxOrderNotional;
        }
        if (config_.price_collar_bps != 0) {
            if (!market.has_bid || !market.has_ask) {
                return RejectReason::NoReferencePrice;
            }
            // |price - mid| / mid > bps / 10000, with mid = mid2 / 2, cleared of fractions.
            const std::int64_t mid2 = std::int64_t{market.bid} + std::int64_t{market.ask};
            const std::int64_t distance2 = std::llabs(2 * std::int64_t{price} - mid2);
            if (distance2 * 10'000 > std::int64_t{config_.price_collar_bps} * mid2) {
                return RejectReason::PriceCollar;
            }
        }
        const SymbolRisk& s = symbols_[symbol];
        // Worst case: every open order on this side fills and none on the other.
        const std::int64_t worst = side == Side::Buy ? s.position + s.open_buy_qty + qty
                                                     : -(s.position - s.open_sell_qty - qty);
        if (worst > config_.max_position) {
            return RejectReason::MaxPosition;
        }
        if (s.open_orders >= config_.max_open_orders) {
            return RejectReason::MaxOpenOrders;
        }
        // Positions at mid plus every open order at its limit price plus this order.
        if (total_gross2_ / 2 + total_open_notional_ > config_.max_gross_notional - notional) {
            return RejectReason::MaxGrossNotional;
        }
        if (config_.rate_limit_per_second != 0) {
            refill(now);
            if (tokens_ < kNanos) {
                return RejectReason::RateLimit;
            }
        }
        return RejectReason::None;
    }

    /// Adds the tokens earned since the last refill. One token is kNanos units, so the bucket
    /// earns `rate` units per nanosecond with no rounding.
    void refill(Timestamp now) noexcept {
        const auto rate = std::int64_t{config_.rate_limit_per_second};
        const auto elapsed = static_cast<std::int64_t>(now - last_refill_);
        last_refill_ = now;
        // Guard the multiplication: a long gap simply fills the bucket.
        if (elapsed >= bucket_capacity_ / rate + 1) {
            tokens_ = bucket_capacity_;
        } else {
            tokens_ = std::min(bucket_capacity_, tokens_ + elapsed * rate);
        }
    }

    void release(SymbolRisk& s, Side side, Price order_price, Qty qty) noexcept {
        const std::int64_t notional = std::int64_t{order_price} * qty;
        std::int64_t& open_qty = side == Side::Buy ? s.open_buy_qty : s.open_sell_qty;
        open_qty -= qty;
        s.open_notional -= notional;
        total_open_notional_ -= notional;
    }

    /// Recomputes a symbol's contribution to the totals after its position or mark changed.
    void refresh(SymbolRisk& s) noexcept {
        const std::int64_t gross2 = std::llabs(s.position) * s.mark2;
        const std::int64_t pnl2 = 2 * s.cash + s.position * s.mark2;
        total_gross2_ += gross2 - s.gross2;
        total_pnl2_ += pnl2 - s.pnl2;
        s.gross2 = gross2;
        s.pnl2 = pnl2;
    }

    bool check_loss() noexcept {
        if (config_.loss_limit != 0 && !tripped() && total_pnl2_ < -2 * config_.loss_limit) {
            kill_reason_ = KillReason::LossLimit;
            return true;
        }
        return false;
    }

    sim::RiskConfig config_;
    bool extended_hours_ = false;
    std::vector<SymbolRisk> symbols_;
    std::int64_t total_open_notional_ = 0;
    std::int64_t total_gross2_ = 0;
    std::int64_t total_pnl2_ = 0;
    std::int64_t bucket_capacity_ = 0;
    std::int64_t tokens_ = 0;
    Timestamp last_refill_ = 0;
    std::vector<Timestamp> recent_rejects_;
    std::size_t reject_head_ = 0;
    std::size_t reject_count_ = 0;
    std::uint64_t rejects_ = 0;
    std::uint64_t fills_ = 0;
    KillReason kill_reason_ = KillReason::None;
};

} // namespace itchbt::risk
