#pragma once

// Value types of the simulation: orders, their states, and the events a strategy sees.

#include "itchbt/book/types.hpp"
#include "itchbt/util/types.hpp"

#include <cstdint>

namespace itchbt::sim {

/// Identifies one simulated order. The stock locate sits in the top bits and a per-symbol
/// counter in the low 40, so ids depend only on the symbol's own history. That keeps output
/// identical whether symbols are replayed together or in separate shards.
using OrderId = std::uint64_t;

inline constexpr OrderId kInvalidOrderId = 0;
inline constexpr unsigned kOrderIdLocateShift = 40;

[[nodiscard]] constexpr OrderId make_order_id(Locate locate, std::uint64_t counter) noexcept {
    return (OrderId{locate} << kOrderIdLocateShift) | counter;
}
[[nodiscard]] constexpr Locate locate_of(OrderId id) noexcept {
    return static_cast<Locate>(id >> kOrderIdLocateShift);
}
[[nodiscard]] constexpr std::uint64_t counter_of(OrderId id) noexcept {
    return id & ((OrderId{1} << kOrderIdLocateShift) - 1);
}

enum class OrderType : std::uint8_t { Limit, Ioc };

enum class OrderState : std::uint8_t {
    PendingNew,
    Live,
    PartiallyFilled,
    PendingCancel,
    Filled,
    Cancelled,
    Rejected,
};
inline constexpr int kOrderStateCount = 7;

/// Why an order was refused, by the risk gate before it left or by the simulated exchange.
enum class RejectReason : std::uint8_t {
    None,
    // Risk gate (pre-trade)
    KillSwitch,
    InvalidOrder,
    MarketClosed,
    SymbolHalted,
    MaxOrderQty,
    MaxOrderNotional,
    NoReferencePrice,
    PriceCollar,
    MaxPosition,
    MaxOpenOrders,
    MaxGrossNotional,
    RateLimit,
    // Engine limits
    EventQueueFull,
    // Simulated exchange, on arrival
    ExchangeClosed,
    ExchangeHalted,
    ExchangeOpenOrderLimit,
    // Cancel requests
    TooLateToCancel,
};

[[nodiscard]] constexpr const char* to_string(RejectReason r) noexcept {
    switch (r) {
    case RejectReason::None: return "none";
    case RejectReason::KillSwitch: return "kill_switch";
    case RejectReason::InvalidOrder: return "invalid_order";
    case RejectReason::MarketClosed: return "market_closed";
    case RejectReason::SymbolHalted: return "symbol_halted";
    case RejectReason::MaxOrderQty: return "max_order_qty";
    case RejectReason::MaxOrderNotional: return "max_order_notional";
    case RejectReason::NoReferencePrice: return "no_reference_price";
    case RejectReason::PriceCollar: return "price_collar";
    case RejectReason::MaxPosition: return "max_position";
    case RejectReason::MaxOpenOrders: return "max_open_orders";
    case RejectReason::MaxGrossNotional: return "max_gross_notional";
    case RejectReason::RateLimit: return "rate_limit";
    case RejectReason::EventQueueFull: return "event_queue_full";
    case RejectReason::ExchangeClosed: return "exchange_closed";
    case RejectReason::ExchangeHalted: return "exchange_halted";
    case RejectReason::ExchangeOpenOrderLimit: return "exchange_open_order_limit";
    case RejectReason::TooLateToCancel: return "too_late_to_cancel";
    }
    return "?";
}

/// Top of book for one symbol, delivered when it changes.
struct BookUpdate {
    Timestamp ts = 0;
    book::LevelQty bid_qty = 0;
    book::LevelQty ask_qty = 0;
    Price bid_price = 0;
    Price ask_price = 0;
    Locate locate = 0;
    bool has_bid = false;
    bool has_ask = false;
};

/// A historical execution.
struct Trade {
    Timestamp ts = 0;
    Price price = 0;
    Qty qty = 0;
    Locate locate = 0;
    /// Side of the resting order that was executed. Meaningless if !displayed.
    Side resting_side = Side::Buy;
    /// False for trades against hidden orders ('P'), where the feed does not say which side
    /// was resting.
    bool displayed = true;
    /// False for executions that belong to a bulk cross print.
    bool printable = true;
};

enum class UpdateKind : std::uint8_t {
    Accepted,       // a limit order is now live on the simulated exchange
    Fill,           // some or all of the order traded
    Cancelled,      // the remainder was cancelled (by request, IOC remainder, or end of day)
    Rejected,       // the order never became live
    CancelRejected, // a cancel request arrived too late; the order is unchanged
};

/// What happened to one of the strategy's own orders.
struct OrderUpdate {
    /// When it happened at the exchange (or at the risk gate, for a pre-trade reject).
    Timestamp ts = 0;
    OrderId id = kInvalidOrderId;
    /// Shares ahead in the queue when the order went live (Accepted only).
    book::LevelQty shares_ahead = 0;
    Price price = 0;      // the order's limit price
    Price fill_price = 0; // Fill only
    Qty fill_qty = 0;     // Fill only
    Qty leaves = 0;       // shares still working after this event
    Qty removed_qty = 0;  // shares that will now never trade (Cancelled, Rejected)
    Locate locate = 0;
    UpdateKind kind = UpdateKind::Accepted;
    OrderState state = OrderState::PendingNew; // state after this event
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    RejectReason reason = RejectReason::None;
    /// Fill only: true if the order took liquidity on arrival, false if it was resting.
    bool aggressive = false;
};

} // namespace itchbt::sim
