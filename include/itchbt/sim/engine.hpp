#pragma once

// The backtest engine: a single-threaded event loop over two time-ordered streams, the
// historical ITCH messages and the strategy's own delayed actions.
//
// For an ITCH message stamped t:
//   1. Release every queued event with release time <= t, in release order (ties by queue
//      order). Orders and cancels reach the exchange, order updates and timers reach the
//      strategy.
//   2. Apply the message to the book.
//   3. Let the simulated exchange check the change against the strategy's resting orders.
//   4. Call the strategy: book update first, then trade.
//
// Simulation time is the ITCH timestamp. The engine never reads the wall clock, so the same
// input, config and binary always produce the same output.
//
// EngineCore holds everything that does not depend on the strategy's type. Context is the
// strategy's handle onto it. Backtester<S> adds the strategy, bound at compile time through
// the Strategy concept so its callbacks inline into the loop.

#include "itchbt/book/default.hpp"
#include "itchbt/itch/directory.hpp"
#include "itchbt/itch/messages.hpp"
#include "itchbt/itch/parser.hpp"
#include "itchbt/record/recorder.hpp"
#include "itchbt/risk/risk.hpp"
#include "itchbt/sim/config.hpp"
#include "itchbt/sim/event_queue.hpp"
#include "itchbt/sim/exchange.hpp"
#include "itchbt/sim/types.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace itchbt::sim {

using Books = book::DefaultBooks;
using BookType = Books::BookType;

struct RunStats {
    std::uint64_t messages = 0;
    std::uint64_t orders_submitted = 0;
    std::uint64_t orders_rejected_by_risk = 0;
    std::uint64_t orders_rejected_by_exchange = 0;
    std::uint64_t fills = 0;
    std::uint64_t shares_filled = 0;
    std::uint64_t cancels_requested = 0;
    std::uint64_t timers_fired = 0;
    std::uint64_t book_updates = 0;
    std::uint64_t trades = 0;
    std::uint64_t submits_refused = 0; // no order id could be issued
    std::uint64_t events_dropped = 0;  // queue full; should always be zero
    std::size_t peak_queue_depth = 0;
};

class Context;

/// Everything in the engine that is independent of the strategy type.
class EngineCore {
public:
    EngineCore(const Config& config, record::Recorder& recorder)
        : config_(config), recorder_(recorder), books_(config.expected_orders),
          queue_(config.event_queue_capacity), subscribed_(65536, false) {}

    /// Reads the stock directory from the start of the data and resolves the configured
    /// symbols to locates. Directory messages all precede the first order of the day, so the
    /// scan stops at the first add. Returns false and sets `error` if a symbol is unknown.
    [[nodiscard]] bool prepare(std::span<const std::byte> data, std::string& error);

    // --- Strategy-facing operations (through Context) ----------------------------------------

    [[nodiscard]] OrderId submit(OrderType type, Locate locate, Side side, Price price, Qty qty);
    void cancel(OrderId id);
    void set_timer(Timestamp ts);

    // --- State ---------------------------------------------------------------------------------

    [[nodiscard]] Timestamp now() const noexcept { return now_; }
    [[nodiscard]] const Books& books() const noexcept { return books_; }
    [[nodiscard]] const itch::SymbolDirectory& directory() const noexcept { return directory_; }
    [[nodiscard]] const itch::MarketState& market() const noexcept { return market_; }
    [[nodiscard]] const risk::RiskGate& risk() const noexcept { return risk_; }
    [[nodiscard]] const RunStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] std::span<const Locate> symbols() const noexcept { return locates_; }
    [[nodiscard]] bool subscribed(Locate locate) const noexcept { return subscribed_[locate]; }
    [[nodiscard]] std::int64_t position(Locate locate) const noexcept {
        const std::int32_t index = exchange_.symbol_index(locate);
        return index < 0 ? 0 : risk_.position(static_cast<std::size_t>(index));
    }

    /// Whether orders for `locate` can trade right now, and if not, why.
    [[nodiscard]] RejectReason exchange_refusal(Locate locate) const noexcept {
        if (!market_.regular_hours() && !config_.exchange.extended_hours) {
            return RejectReason::ExchangeClosed;
        }
        return market_.trading(locate) ? RejectReason::None : RejectReason::ExchangeHalted;
    }
    [[nodiscard]] bool can_trade(Locate locate) const noexcept {
        return exchange_refusal(locate) == RejectReason::None;
    }

protected:
    [[nodiscard]] risk::MarketView market_view(Locate locate) const {
        risk::MarketView view;
        view.regular_hours = market_.regular_hours();
        view.trading = market_.trading(locate);
        const BookType& book = books_.book(locate);
        if (const auto bid = book.best_bid()) {
            view.has_bid = true;
            view.bid = bid->price;
        }
        if (const auto ask = book.best_ask()) {
            view.has_ask = true;
            view.ask = ask->price;
        }
        return view;
    }

    /// An event at the exchange: record it, then send it back to the strategy side after the
    /// acknowledgement latency.
    void emit_from_exchange(const OrderUpdate& u) {
        record_update(u);
        if (u.kind == UpdateKind::Fill) {
            ++stats_.fills;
            stats_.shares_filled += u.fill_qty;
        } else if (u.kind == UpdateKind::Rejected) {
            ++stats_.orders_rejected_by_exchange;
        }
        schedule_update(u, now_ + config_.latency.ack_ns);
    }

    void schedule_update(const OrderUpdate& u, Timestamp release) {
        Event event;
        event.release = release;
        event.kind = EventKind::Update;
        event.update = u;
        push(event);
    }

    /// Queues an event. Order updates must never be lost, so the queue keeps headroom for
    /// them: submit() refuses new work once the queue is nearly full.
    bool push(const Event& event) {
        const bool pushed = queue_.push(event);
        if (!pushed) {
            ++stats_.events_dropped;
        }
        stats_.peak_queue_depth = std::max(stats_.peak_queue_depth, queue_.size());
        return pushed;
    }

    /// Applies an order update on the strategy side: the firm now knows about it.
    void apply_update_to_risk(const OrderUpdate& u) {
        const auto index = static_cast<std::size_t>(exchange_.symbol_index(u.locate));
        bool tripped = false;
        switch (u.kind) {
        case UpdateKind::Fill:
            tripped = risk_.on_fill(index, u.side, u.price, u.fill_price, u.fill_qty);
            if (u.state == OrderState::Filled) {
                risk_.on_order_done(index);
            }
            break;
        case UpdateKind::Cancelled:
            risk_.on_closed(index, u.side, u.price, u.removed_qty, true);
            break;
        case UpdateKind::Rejected:
            // A risk reject never reserved anything; an exchange reject releases the lot.
            if (u.reason >= RejectReason::ExchangeClosed) {
                risk_.on_closed(index, u.side, u.price, u.removed_qty, true);
                tripped = note_reject(u);
            }
            break;
        case UpdateKind::Accepted:
        case UpdateKind::CancelRejected: break;
        }
        if (tripped) {
            trip_kill_switch();
        }
    }

    bool note_reject(const OrderUpdate& u) {
        record::RiskRecord r;
        r.ts = now_;
        r.order_id = u.id;
        r.locate = u.locate;
        r.event = static_cast<std::uint8_t>(record::RiskEventKind::Reject);
        r.reason = static_cast<std::uint8_t>(u.reason);
        recorder_.write(r);
        return risk_.note_reject(now_);
    }

    /// The kill switch tripped: record it and cancel everything still open. The risk gate
    /// rejects all new orders from here on.
    void trip_kill_switch() {
        record::RiskRecord r;
        r.ts = now_;
        r.value = risk_.total_pnl2();
        r.event = static_cast<std::uint8_t>(record::RiskEventKind::KillSwitch);
        r.reason = static_cast<std::uint8_t>(risk_.kill_reason());
        recorder_.write(r);
        // Any order not yet terminal gets a cancel, including ones still on their way to the
        // exchange: those cancels are rejected as too late only if the order never arrives.
        for (std::size_t s = 0; s < exchange_.symbol_count(); ++s) {
            const std::size_t created = exchange_.orders_created(s);
            for (std::size_t n = 1; n <= created; ++n) {
                const OrderId id = make_order_id(locates_[s], n);
                const SimOrder* const order = exchange_.find(id);
                if (order != nullptr && !is_terminal(order->state) && !order->cancel_requested) {
                    cancel(id);
                }
            }
        }
    }

    void record_update(const OrderUpdate& u) {
        if (!recorder_.enabled()) {
            return;
        }
        record::OrderRecord r;
        r.ts = u.ts;
        r.order_id = u.id;
        r.price = u.price;
        r.leaves = u.leaves;
        r.locate = u.locate;
        r.state = static_cast<std::uint8_t>(u.state);
        r.side = static_cast<std::uint8_t>(u.side);
        r.order_type = static_cast<std::uint8_t>(u.type);
        r.reason = static_cast<std::uint8_t>(u.reason);
        switch (u.kind) {
        case UpdateKind::Accepted:
            r.event = static_cast<std::uint8_t>(record::OrderEventKind::Accepted);
            r.shares_ahead = u.shares_ahead;
            r.qty = u.leaves;
            break;
        case UpdateKind::Fill: {
            r.event = static_cast<std::uint8_t>(record::OrderEventKind::Fill);
            r.qty = u.fill_qty;
            record::FillRecord f;
            f.ts = u.ts;
            f.order_id = u.id;
            f.shares_ahead_at_arrival = u.shares_ahead;
            f.price = u.fill_price;
            f.qty = u.fill_qty;
            f.leaves = u.leaves;
            f.order_price = u.price;
            f.locate = u.locate;
            f.side = static_cast<std::uint8_t>(u.side);
            f.aggressive = u.aggressive ? 1 : 0;
            recorder_.write(f);
            break;
        }
        case UpdateKind::Cancelled:
            r.event = static_cast<std::uint8_t>(record::OrderEventKind::Cancelled);
            r.qty = u.removed_qty;
            break;
        case UpdateKind::Rejected:
            r.event = static_cast<std::uint8_t>(record::OrderEventKind::Rejected);
            r.qty = u.removed_qty;
            break;
        case UpdateKind::CancelRejected:
            r.event = static_cast<std::uint8_t>(record::OrderEventKind::CancelRejected);
            break;
        }
        recorder_.write(r);
    }

    Config config_;
    record::Recorder& recorder_;
    Books books_;
    itch::SymbolDirectory directory_;
    itch::MarketState market_;
    EventQueue queue_;
    SimExchange exchange_; // configured in prepare(), once the locates are known
    risk::RiskGate risk_;
    std::vector<Locate> locates_;  // traded symbols, in config order
    std::vector<bool> subscribed_; // by locate
    RunStats stats_;
    Timestamp now_ = 0;
};

/// The strategy's handle onto the engine.
class Context {
public:
    explicit Context(EngineCore& core) noexcept : core_(&core) {}

    /// Simulation time: nanoseconds since midnight.
    [[nodiscard]] Timestamp now() const noexcept { return core_->now(); }
    /// The historical book for a symbol. Simulated orders are never in it.
    [[nodiscard]] const BookType& book(Locate symbol) const noexcept {
        return core_->books().book(symbol);
    }
    /// Signed position in shares, as of the fills the strategy has been told about.
    [[nodiscard]] std::int64_t position(Locate symbol) const noexcept {
        return core_->position(symbol);
    }
    /// Sends a day limit order. Returns its id at once; the outcome arrives through
    /// on_order_update. Returns kInvalidOrderId, with no update to follow, only if the symbol
    /// is not one of the run's configured symbols or its order pool is exhausted.
    [[nodiscard]] OrderId submit_limit(Locate symbol, Side side, Price price, Qty qty) {
        return core_->submit(OrderType::Limit, symbol, side, price, qty);
    }
    /// Sends an immediate-or-cancel order: it fills what it can on arrival and the rest is
    /// cancelled.
    [[nodiscard]] OrderId submit_ioc(Locate symbol, Side side, Price price, Qty qty) {
        return core_->submit(OrderType::Ioc, symbol, side, price, qty);
    }
    void cancel(OrderId id) { core_->cancel(id); }
    /// Asks for on_timer at simulation time `ts` (or as soon after as the next event).
    void set_timer(Timestamp ts) { core_->set_timer(ts); }

    /// True if an order for `symbol` sent now would be allowed to trade: the session is open
    /// (or extended hours are enabled) and the symbol is not halted.
    [[nodiscard]] bool tradable(Locate symbol) const noexcept { return core_->can_trade(symbol); }

    /// The run's configured symbols, as locates.
    [[nodiscard]] std::span<const Locate> symbols() const noexcept { return core_->symbols(); }
    [[nodiscard]] std::string_view symbol_name(Locate symbol) const noexcept {
        return core_->directory().symbol(symbol);
    }
    [[nodiscard]] std::optional<Locate> find_symbol(std::string_view name) const {
        return core_->directory().find(name);
    }
    [[nodiscard]] const Config& config() const noexcept { return core_->config(); }

private:
    EngineCore* core_;
};

/// What a strategy must provide. Callbacks are resolved at compile time.
template <typename S>
concept Strategy = requires(S s, Context& ctx, const BookUpdate& bu, const Trade& tr,
                            const OrderUpdate& ou, Timestamp ts) {
    s.on_start(ctx);
    s.on_book_update(bu, ctx);  // top of book changed for a subscribed symbol
    s.on_trade(tr, ctx);        // historical execution
    s.on_order_update(ou, ctx); // ack, fill, cancel, reject for the strategy's own orders
    s.on_timer(ts, ctx);
    s.on_end(ctx);
};

// --- EngineCore out-of-line members ------------------------------------------------------------

inline bool EngineCore::prepare(std::span<const std::byte> data, std::string& error) {
    struct DirectoryScan : itch::NullHandler {
        using itch::NullHandler::on;
        explicit DirectoryScan(itch::SymbolDirectory& d) : directory(d) {}
        void on(const itch::StockDirectory& m) { directory.on(m); }
        void on(const itch::AddOrder& /*m*/) noexcept { finished = true; }
        [[nodiscard]] bool done() const noexcept { return finished; }
        itch::SymbolDirectory& directory;
        bool finished = false;
    };
    itch::SymbolDirectory scanned;
    DirectoryScan scan(scanned);
    const itch::ParseResult result = itch::parse(data, scan);
    if (!result.ok()) {
        error = std::string(itch::to_string(result.error)) + " at file offset " +
                std::to_string(result.offset);
        return false;
    }
    locates_.clear();
    for (const std::string& symbol : config_.symbols) {
        const auto locate = scanned.find(symbol);
        if (!locate) {
            error = "symbol '" + symbol + "' is not in the file's stock directory";
            return false;
        }
        if (subscribed_[*locate]) {
            error = "symbol '" + symbol + "' is listed twice";
            return false;
        }
        subscribed_[*locate] = true;
        locates_.push_back(*locate);
    }
    // The run itself rebuilds the directory message by message; keep the scan's copy so that
    // on_start can already resolve names.
    directory_ = scanned;
    exchange_.configure(config_.exchange, locates_);
    risk_.configure(config_.risk, config_.exchange.extended_hours, locates_.size());
    return true;
}

inline OrderId EngineCore::submit(OrderType type, Locate locate, Side side, Price price, Qty qty) {
    SimOrder* const order = exchange_.create(locate, side, type, price, qty, now_);
    if (order == nullptr) {
        ++stats_.submits_refused;
        return kInvalidOrderId;
    }
    ++stats_.orders_submitted;
    const auto index = static_cast<std::size_t>(exchange_.symbol_index(locate));

    if (recorder_.enabled()) {
        record::OrderRecord r;
        r.ts = now_;
        r.order_id = order->id;
        r.price = price;
        r.qty = qty;
        r.leaves = qty;
        r.locate = locate;
        r.event = static_cast<std::uint8_t>(record::OrderEventKind::Submitted);
        r.state = static_cast<std::uint8_t>(order->state);
        r.side = static_cast<std::uint8_t>(side);
        r.order_type = static_cast<std::uint8_t>(type);
        recorder_.write(r);
    }

    // Keep a quarter of the queue free for the updates that orders already in flight will
    // generate; refusing new orders is recoverable, losing an update is not.
    RejectReason reason =
        queue_.size() >= queue_.capacity() - queue_.capacity() / 4
            ? RejectReason::EventQueueFull
            : risk_.check_and_reserve(index, side, price, qty, now_, market_view(locate));
    if (reason == RejectReason::None) {
        Event event;
        event.release = now_ + config_.latency.order_ns;
        event.kind = EventKind::NewOrder;
        event.update.id = order->id;
        if (!push(event)) {
            // Cannot happen given the headroom check; handled so nothing is silently dropped.
            risk_.on_closed(index, side, price, qty, true);
            reason = RejectReason::EventQueueFull;
        }
    }
    if (reason != RejectReason::None) {
        ++stats_.orders_rejected_by_risk;
        transition(order->state, OrderState::Rejected);
        order->leaves = 0;
        OrderUpdate u;
        u.ts = now_;
        u.id = order->id;
        u.price = price;
        u.removed_qty = qty;
        u.locate = locate;
        u.kind = UpdateKind::Rejected;
        u.state = OrderState::Rejected;
        u.side = side;
        u.type = type;
        u.reason = reason;
        record_update(u);
        const bool tripped = note_reject(u);
        // Pre-trade checks are synchronous: the strategy hears straight away, as the next
        // event, with no latency in either direction.
        schedule_update(u, now_);
        if (tripped) {
            trip_kill_switch();
        }
    }
    return order->id;
}

inline void EngineCore::cancel(OrderId id) {
    SimOrder* const order = exchange_.find(id);
    if (order == nullptr || is_terminal(order->state) || order->cancel_requested) {
        return; // nothing to cancel, or already asked
    }
    ++stats_.cancels_requested;
    SimExchange::request_cancel(*order);
    if (recorder_.enabled()) {
        record::OrderRecord r;
        r.ts = now_;
        r.order_id = id;
        r.price = order->price;
        r.leaves = order->leaves;
        r.locate = order->locate;
        r.event = static_cast<std::uint8_t>(record::OrderEventKind::CancelRequested);
        r.state = static_cast<std::uint8_t>(order->state);
        r.side = static_cast<std::uint8_t>(order->side);
        r.order_type = static_cast<std::uint8_t>(order->type);
        recorder_.write(r);
    }
    Event event;
    event.release = now_ + config_.latency.cancel_ns;
    event.kind = EventKind::Cancel;
    event.update.id = id;
    push(event);
}

inline void EngineCore::set_timer(Timestamp ts) {
    Event event;
    event.release = ts < now_ ? now_ : ts;
    event.kind = EventKind::Timer;
    push(event);
}

/// The backtester: EngineCore plus a strategy, driven as an ITCH parser handler.
template <Strategy S>
class Backtester : public EngineCore {
public:
    Backtester(const Config& config, S& strategy, record::Recorder& recorder)
        : EngineCore(config, recorder), strategy_(strategy), context_(*this) {}

    /// Replays `data`. Call prepare() first. Returns the parser's result; stats() and risk()
    /// describe what happened.
    [[nodiscard]] itch::ParseResult
    run(std::span<const std::byte> data,
        std::uint64_t max_messages = std::numeric_limits<std::uint64_t>::max()) {
        strategy_.on_start(context_);
        const itch::ParseResult result = itch::parse(data, *this, max_messages);
        stats_.messages = result.messages;
        // Deliver whatever is still in flight, in order, however far in the future.
        release_until(std::numeric_limits<Timestamp>::max());
        strategy_.on_end(context_);
        return result;
    }

    // --- Parser handler --------------------------------------------------------------------------

    void on(const itch::SystemEvent& m) {
        begin(m.h);
        market_.on(m);
        if (m.code == 'M') {
            // End of market hours: day orders expire.
            exchange_.expire_all(now_, [this](const OrderUpdate& u) { emit_from_exchange(u); });
        }
    }
    void on(const itch::StockDirectory& m) {
        begin(m.h);
        directory_.on(m);
    }
    void on(const itch::TradingAction& m) {
        begin(m.h);
        market_.on(m);
    }
    void on(const itch::AddOrder& m) {
        if (!wanted(m.h)) {
            return;
        }
        begin(m.h);
        const book::BookChange change = book::apply(books_, m);
        after_change(change, m.h);
    }
    void on(const itch::OrderExecuted& m) {
        if (!wanted(m.h)) {
            return;
        }
        begin(m.h);
        const book::BookChange change = book::apply(books_, m);
        // 'E' executes at the resting order's price.
        after_execution(change, m.h, change.price, m.qty, true);
    }
    void on(const itch::OrderExecutedWithPrice& m) {
        if (!wanted(m.h)) {
            return;
        }
        begin(m.h);
        const book::BookChange change = book::apply(books_, m);
        after_execution(change, m.h, m.price, m.qty, m.printable);
    }
    void on(const itch::OrderCancel& m) {
        if (!wanted(m.h)) {
            return;
        }
        begin(m.h);
        after_withdrawal(book::apply(books_, m), m.h);
    }
    void on(const itch::OrderDelete& m) {
        if (!wanted(m.h)) {
            return;
        }
        begin(m.h);
        after_withdrawal(book::apply(books_, m), m.h);
    }
    void on(const itch::OrderReplace& m) {
        if (!wanted(m.h)) {
            return;
        }
        begin(m.h);
        const book::ReplaceChange change = book::apply(books_, m);
        if (change.removed.applied && subscribed_[m.h.locate]) {
            exchange_.on_reduction(change.removed, false, false, now_,
                                   [this](const OrderUpdate& u) { emit_from_exchange(u); });
        }
        if (subscribed_[m.h.locate] && (change.removed.top_changed || change.added.top_changed)) {
            top_changed(m.h.locate);
        }
    }
    void on(const itch::Trade& m) {
        if (!subscribed_[m.h.locate]) {
            return;
        }
        begin(m.h);
        // A trade against a hidden order: the book does not change.
        if (can_trade(m.h.locate)) {
            exchange_.on_trade_price(m.h.locate, m.price, now_,
                                     [this](const OrderUpdate& u) { emit_from_exchange(u); });
        }
        Trade trade;
        trade.ts = now_;
        trade.price = m.price;
        trade.qty = m.qty;
        trade.locate = m.h.locate;
        trade.displayed = false;
        ++stats_.trades;
        strategy_.on_trade(trade, context_);
    }
    void on(const itch::CrossTrade& m) { begin(m.h); }
    void on(const itch::BrokenTrade& m) { begin(m.h); }
    void on_skipped(const itch::Header& h) { begin(h); }

private:
    /// With build_all_books off, book messages for untraded symbols are skipped entirely.
    [[nodiscard]] bool wanted(const itch::Header& h) const noexcept {
        return config_.build_all_books || subscribed_[h.locate];
    }

    /// Step 1 of every message: release what is due, then advance the clock.
    void begin(const itch::Header& h) {
        if (!queue_.empty() && queue_.top().release <= h.ts) {
            release_until(h.ts);
        }
        now_ = h.ts;
    }

    void release_until(Timestamp limit) {
        while (!queue_.empty() && queue_.top().release <= limit) {
            const Event event = queue_.pop();
            now_ = event.release;
            switch (event.kind) {
            case EventKind::NewOrder: {
                SimOrder* const order = exchange_.find(event.update.id);
                exchange_.on_new_order(*order, now_, books_, exchange_refusal(order->locate),
                                       [this](const OrderUpdate& u) { emit_from_exchange(u); });
                break;
            }
            case EventKind::Cancel: {
                SimOrder* const order = exchange_.find(event.update.id);
                exchange_.on_cancel(*order, now_,
                                    [this](const OrderUpdate& u) { emit_from_exchange(u); });
                break;
            }
            case EventKind::Update:
                apply_update_to_risk(event.update);
                strategy_.on_order_update(event.update, context_);
                break;
            case EventKind::Timer:
                ++stats_.timers_fired;
                strategy_.on_timer(now_, context_);
                break;
            }
        }
    }

    void after_change(const book::BookChange& change, const itch::Header& h) {
        if (change.applied && change.top_changed && subscribed_[h.locate]) {
            top_changed(h.locate);
        }
    }

    void after_withdrawal(const book::BookChange& change, const itch::Header& h) {
        if (!change.applied || !subscribed_[h.locate]) {
            return;
        }
        exchange_.on_reduction(change, false, false, now_,
                               [this](const OrderUpdate& u) { emit_from_exchange(u); });
        if (change.top_changed) {
            top_changed(h.locate);
        }
    }

    void after_execution(const book::BookChange& change, const itch::Header& h, Price price,
                         Qty qty, bool printable) {
        if (!change.applied || !subscribed_[h.locate]) {
            return;
        }
        // Executions that are part of a cross (non-printable) still move the queue, but
        // auctions are not simulated, so they never fill a simulated order.
        const bool fills_allowed = printable && can_trade(h.locate);
        const auto emit = [this](const OrderUpdate& u) { emit_from_exchange(u); };
        if (fills_allowed) {
            exchange_.on_trade_price(h.locate, price, now_, emit);
        }
        exchange_.on_reduction(change, true, fills_allowed, now_, emit);
        if (change.top_changed) {
            top_changed(h.locate);
        }
        Trade trade;
        trade.ts = now_;
        trade.price = price;
        trade.qty = qty;
        trade.locate = h.locate;
        trade.resting_side = change.side;
        trade.printable = printable;
        ++stats_.trades;
        strategy_.on_trade(trade, context_);
    }

    /// The top of a subscribed symbol's book changed: mark positions, record it, and tell the
    /// strategy.
    void top_changed(Locate locate) {
        const BookType& book = books_.book(locate);
        BookUpdate update;
        update.ts = now_;
        update.locate = locate;
        if (const auto bid = book.best_bid()) {
            update.has_bid = true;
            update.bid_price = bid->price;
            update.bid_qty = bid->qty;
        }
        if (const auto ask = book.best_ask()) {
            update.has_ask = true;
            update.ask_price = ask->price;
            update.ask_qty = ask->qty;
        }
        if (recorder_.enabled()) {
            record::BboRecord r;
            r.ts = now_;
            r.bid_qty = update.bid_qty;
            r.ask_qty = update.ask_qty;
            r.bid_price = update.bid_price;
            r.ask_price = update.ask_price;
            r.locate = locate;
            r.flags =
                static_cast<std::uint8_t>((update.has_bid ? 1U : 0U) | (update.has_ask ? 2U : 0U));
            recorder_.write(r);
        }
        // Positions are marked only while the symbol can trade. Outside the session the book
        // is thin and its mid means little: on the reference day one symbol ends the evening
        // quoted 200.00 x 414.95. The last in-session mark stands instead.
        if (can_trade(locate)) {
            risk::MarketView view;
            view.has_bid = update.has_bid;
            view.has_ask = update.has_ask;
            view.bid = update.bid_price;
            view.ask = update.ask_price;
            if (risk_.on_top_of_book(static_cast<std::size_t>(exchange_.symbol_index(locate)),
                                     view)) {
                trip_kill_switch();
            }
        }
        ++stats_.book_updates;
        strategy_.on_book_update(update, context_);
    }

    S& strategy_;
    Context context_;
};

} // namespace itchbt::sim
