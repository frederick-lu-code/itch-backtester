#pragma once

// The simulated exchange: the strategy's own orders, and the fill model.
//
// Simulated orders never enter the historical book, and historical order flow is never
// changed by them. Instead each simulated order watches the historical book go by:
//
//   - A marketable order fills on arrival against displayed liquidity, level by level, up to
//     its limit price.
//   - A passive order joins the back of its price level. It records the shares resting there
//     on arrival (shares_ahead) and the newest historical order's sequence number.
//   - When a historical order at that level that arrived earlier (lower sequence) is executed,
//     cancelled, deleted or replaced, shares_ahead falls by that amount.
//   - Once shares_ahead is zero, each further historical execution at that price fills the
//     simulated order by up to the executed quantity.
//   - A historical execution at a price through the simulated order's limit fills it in full:
//     someone traded at a worse price than this order was offering.
//
// Known biases, all favouring the strategy: no market impact, fills after the queue is
// exhausted are optimistic (the same aggressor is counted for the historical order and for
// ours), and hidden liquidity is ignored. See README, "Simulation limits".
//
// Storage is allocated once at construction. Nothing here allocates afterwards.

#include "itchbt/book/types.hpp"
#include "itchbt/sim/config.hpp"
#include "itchbt/sim/order_state.hpp"
#include "itchbt/sim/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace itchbt::sim {

struct SimOrder {
    OrderId id = kInvalidOrderId;
    Timestamp submit_ts = 0;
    book::LevelQty shares_ahead = 0;
    book::LevelQty shares_ahead_at_arrival = 0;
    /// The newest historical sequence when the order went live: older orders are ahead of it.
    book::Sequence arrival_seq = 0;
    Price price = 0;
    Qty qty = 0;
    Qty leaves = 0;
    Locate locate = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    OrderState state = OrderState::PendingNew;
    /// The strategy asked to cancel. Kept separately because a fill can move the state from
    /// PendingCancel back to PartiallyFilled before the cancel arrives.
    bool cancel_requested = false;
};

class SimExchange {
public:
    static constexpr std::int32_t kNotTraded = -1;

    /// An exchange that trades nothing, to be set up later with configure().
    SimExchange() : index_of_(65536, kNotTraded) {}

    SimExchange(const ExchangeConfig& config, std::span<const Locate> symbols) : SimExchange() {
        configure(config, symbols);
    }

    /// Sets the traded symbols and allocates all order storage. Discards any existing orders.
    void configure(const ExchangeConfig& config, std::span<const Locate> symbols) {
        index_of_.assign(index_of_.size(), kNotTraded);
        symbols_.assign(symbols.size(), Symbol{});
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            index_of_[symbols[i]] = static_cast<std::int32_t>(i);
            symbols_[i].orders.reserve(config.max_orders_per_symbol);
            symbols_[i].open.reserve(config.max_open_orders_per_symbol);
        }
    }

    /// Index of a traded symbol, or kNotTraded.
    [[nodiscard]] std::int32_t symbol_index(Locate locate) const noexcept {
        return index_of_[locate];
    }

    // --- Strategy side (takes effect immediately) --------------------------------------------

    /// Creates an order in PendingNew. Returns nullptr if the symbol is not traded or its
    /// order pool is exhausted.
    [[nodiscard]] SimOrder* create(Locate locate, Side side, OrderType type, Price price, Qty qty,
                                   Timestamp now) {
        const std::int32_t index = index_of_[locate];
        if (index == kNotTraded) {
            return nullptr;
        }
        std::vector<SimOrder>& orders = symbols_[static_cast<std::size_t>(index)].orders;
        if (orders.size() == orders.capacity()) {
            return nullptr;
        }
        SimOrder order;
        order.id = make_order_id(locate, orders.size() + 1);
        order.submit_ts = now;
        order.price = price;
        order.qty = qty;
        order.leaves = qty;
        order.locate = locate;
        order.side = side;
        order.type = type;
        orders.push_back(order); // never reallocates: size < capacity
        return &orders.back();
    }

    [[nodiscard]] SimOrder* find(OrderId id) noexcept {
        const std::int32_t index = index_of_[locate_of(id)];
        if (index == kNotTraded) {
            return nullptr;
        }
        std::vector<SimOrder>& orders = symbols_[static_cast<std::size_t>(index)].orders;
        const std::uint64_t counter = counter_of(id);
        return counter == 0 || counter > orders.size() ? nullptr : &orders[counter - 1];
    }

    /// The strategy asked to cancel `order`. A live order moves to PendingCancel at once; the
    /// cancel itself takes effect when it arrives (on_cancel).
    static void request_cancel(SimOrder& order) noexcept {
        order.cancel_requested = true;
        if (order.state == OrderState::Live || order.state == OrderState::PartiallyFilled) {
            transition(order.state, OrderState::PendingCancel);
        }
    }

    // --- Exchange side -------------------------------------------------------------------------

    /// A new order reaches the exchange. `refusal` is None if the symbol can trade now, or the
    /// reason it cannot. `emit(const OrderUpdate&)` receives every resulting update.
    template <typename Books, typename Emit>
    void on_new_order(SimOrder& order, Timestamp now, const Books& books, RejectReason refusal,
                      Emit&& emit) {
        assert(order.state == OrderState::PendingNew);
        Symbol& symbol = symbols_[static_cast<std::size_t>(index_of_[order.locate])];
        if (refusal == RejectReason::None && order.type == OrderType::Limit &&
            symbol.open.size() == symbol.open.capacity()) {
            refusal = RejectReason::ExchangeOpenOrderLimit;
        }
        if (refusal != RejectReason::None) {
            transition(order.state, OrderState::Rejected);
            OrderUpdate u = update_for(order, now, UpdateKind::Rejected);
            u.reason = refusal;
            u.removed_qty = order.leaves;
            u.leaves = 0;
            order.leaves = 0;
            emit(u);
            return;
        }

        const auto& book = books.book(order.locate);
        if (order.type == OrderType::Limit) {
            // Queue position is taken before the sweep: both are measured against the book as
            // it stands when the order arrives.
            order.shares_ahead = book.side(order.side).level(order.price).qty;
            order.shares_ahead_at_arrival = order.shares_ahead;
            order.arrival_seq = books.last_sequence();
            transition(order.state, OrderState::Live);
            OrderUpdate u = update_for(order, now, UpdateKind::Accepted);
            u.shares_ahead = order.shares_ahead;
            emit(u);
        }

        // Sweep displayed liquidity on the other side, best level first, up to the limit.
        const Side other = order.side == Side::Buy ? Side::Sell : Side::Buy;
        book.side(other).for_each([&](const book::LevelView& level) {
            const bool crosses =
                order.side == Side::Buy ? level.price <= order.price : level.price >= order.price;
            if (!crosses || order.leaves == 0) {
                return false;
            }
            const Qty take = level.qty < order.leaves ? static_cast<Qty>(level.qty) : order.leaves;
            fill(order, take, level.price, true, now, emit);
            return true;
        });

        if (order.leaves == 0) {
            return; // fill() made the final transition
        }
        if (order.type == OrderType::Ioc) {
            transition(order.state, OrderState::Cancelled);
            OrderUpdate u = update_for(order, now, UpdateKind::Cancelled);
            u.removed_qty = order.leaves;
            u.leaves = 0;
            order.leaves = 0;
            emit(u);
            return;
        }
        symbol.open.push_back(static_cast<std::uint32_t>(counter_of(order.id) - 1));
    }

    /// A cancel request reaches the exchange.
    template <typename Emit>
    void on_cancel(SimOrder& order, Timestamp now, Emit&& emit) {
        Symbol& symbol = symbols_[static_cast<std::size_t>(index_of_[order.locate])];
        if (!remove_open(symbol, order)) {
            // Not resting: never arrived yet, already filled, or already cancelled.
            OrderUpdate u = update_for(order, now, UpdateKind::CancelRejected);
            u.reason = RejectReason::TooLateToCancel;
            emit(u);
            return;
        }
        cancel_resting(order, now, emit);
    }

    /// A historical order lost shares (executed, cancelled, deleted, or the old half of a
    /// replace). `executed` distinguishes a trade from a withdrawal; `fills_allowed` is false
    /// while the symbol cannot trade, or for executions that belong to a cross.
    template <typename Emit>
    void on_reduction(const book::BookChange& change, bool executed, bool fills_allowed,
                      Timestamp now, Emit&& emit) {
        const std::int32_t index = index_of_[change.locate];
        if (index == kNotTraded) {
            return;
        }
        Symbol& symbol = symbols_[static_cast<std::size_t>(index)];
        for (std::size_t i = 0; i < symbol.open.size();) {
            SimOrder& order = symbol.orders[symbol.open[i]];
            if (order.side == change.side && order.price == change.price) {
                book::LevelQty remaining = change.delta;
                if (change.seq <= order.arrival_seq) {
                    // The historical order was ahead of ours in the queue.
                    const book::LevelQty ahead =
                        remaining < order.shares_ahead ? remaining : order.shares_ahead;
                    order.shares_ahead -= ahead;
                    remaining -= ahead;
                } else if (order.shares_ahead != 0) {
                    remaining = 0; // behind us, and we are not yet at the front
                }
                if (executed && fills_allowed && order.shares_ahead == 0 && remaining != 0) {
                    const Qty take =
                        remaining < order.leaves ? static_cast<Qty>(remaining) : order.leaves;
                    fill(order, take, order.price, false, now, emit);
                    if (order.leaves == 0) {
                        erase_open(symbol, i);
                        continue;
                    }
                }
            }
            ++i;
        }
    }

    /// A historical trade printed at `price`. Any resting simulated order priced through it
    /// (a buy above it, a sell below it) fills in full at its own limit.
    template <typename Emit>
    void on_trade_price(Locate locate, Price price, Timestamp now, Emit&& emit) {
        const std::int32_t index = index_of_[locate];
        if (index == kNotTraded) {
            return;
        }
        Symbol& symbol = symbols_[static_cast<std::size_t>(index)];
        for (std::size_t i = 0; i < symbol.open.size();) {
            SimOrder& order = symbol.orders[symbol.open[i]];
            const bool through =
                order.side == Side::Buy ? price < order.price : price > order.price;
            if (through) {
                fill(order, order.leaves, order.price, false, now, emit);
                erase_open(symbol, i);
            } else {
                ++i;
            }
        }
    }

    /// End of the trading day: every resting day order is cancelled.
    template <typename Emit>
    void expire_all(Timestamp now, Emit&& emit) {
        for (Symbol& symbol : symbols_) {
            for (const std::uint32_t slot : symbol.open) {
                cancel_resting(symbol.orders[slot], now, emit);
            }
            symbol.open.clear();
        }
    }

    /// Visits every order resting on the simulated exchange for one traded symbol.
    template <typename F>
    void for_each_open(std::size_t symbol_index, F&& f) {
        Symbol& symbol = symbols_[symbol_index];
        for (const std::uint32_t slot : symbol.open) {
            f(symbol.orders[slot]);
        }
    }

    [[nodiscard]] std::size_t symbol_count() const noexcept { return symbols_.size(); }
    [[nodiscard]] std::size_t open_orders(std::size_t symbol_index) const noexcept {
        return symbols_[symbol_index].open.size();
    }
    [[nodiscard]] std::size_t orders_created(std::size_t symbol_index) const noexcept {
        return symbols_[symbol_index].orders.size();
    }

private:
    struct Symbol {
        std::vector<SimOrder> orders;    // every order ever created, by counter - 1
        std::vector<std::uint32_t> open; // slots of orders resting now, oldest first
    };

    static OrderUpdate update_for(const SimOrder& order, Timestamp now, UpdateKind kind) noexcept {
        OrderUpdate u;
        u.ts = now;
        u.id = order.id;
        u.price = order.price;
        u.leaves = order.leaves;
        u.locate = order.locate;
        u.kind = kind;
        u.state = order.state;
        u.side = order.side;
        u.type = order.type;
        return u;
    }

    /// Fills `qty` shares of `order` at `price` and makes the state transition that follows.
    template <typename Emit>
    static void fill(SimOrder& order, Qty qty, Price price, bool aggressive, Timestamp now,
                     Emit&& emit) {
        assert(qty != 0 && qty <= order.leaves);
        order.leaves -= qty;
        if (order.leaves == 0) {
            transition(order.state, OrderState::Filled);
        } else if (order.state != OrderState::PendingNew) {
            // An IOC stays PendingNew through partial fills; its end state comes afterwards.
            transition(order.state, OrderState::PartiallyFilled);
        }
        OrderUpdate u = update_for(order, now, UpdateKind::Fill);
        u.fill_qty = qty;
        u.fill_price = price;
        u.aggressive = aggressive;
        u.shares_ahead = order.shares_ahead_at_arrival;
        emit(u);
    }

    template <typename Emit>
    static void cancel_resting(SimOrder& order, Timestamp now, Emit&& emit) {
        // The table has no direct Live -> Cancelled edge: a cancel always passes through
        // PendingCancel, whether the strategy asked for it or the day ended.
        if (order.state != OrderState::PendingCancel) {
            transition(order.state, OrderState::PendingCancel);
        }
        transition(order.state, OrderState::Cancelled);
        OrderUpdate u = update_for(order, now, UpdateKind::Cancelled);
        u.removed_qty = order.leaves;
        u.leaves = 0;
        order.leaves = 0;
        emit(u);
    }

    static void erase_open(Symbol& symbol, std::size_t i) noexcept {
        // Order-preserving, so older orders keep being visited first. The list is short.
        symbol.open.erase(symbol.open.begin() + static_cast<std::ptrdiff_t>(i));
    }

    static bool remove_open(Symbol& symbol, const SimOrder& order) noexcept {
        const auto slot = static_cast<std::uint32_t>(counter_of(order.id) - 1);
        for (std::size_t i = 0; i < symbol.open.size(); ++i) {
            if (symbol.open[i] == slot) {
                erase_open(symbol, i);
                return true;
            }
        }
        return false;
    }

    std::vector<std::int32_t> index_of_;
    std::vector<Symbol> symbols_;
};

} // namespace itchbt::sim
