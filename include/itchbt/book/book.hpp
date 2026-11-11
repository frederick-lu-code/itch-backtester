#pragma once

// The limit order book: one Book per stock locate, plus one order store shared by all symbols
// (ITCH order references are unique across symbols within a day).
//
// BookSet is a template over the two containers that dominate its cost, the order store and
// the per-side level container. The baseline instantiation uses the standard library; the
// optimised one (M6) swaps both. They must agree on every message.

#include "itchbt/book/baseline.hpp"
#include "itchbt/book/types.hpp"
#include "itchbt/itch/messages.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace itchbt::book {

/// One side (bids or asks) of one symbol's book.
template <typename Levels>
class SideBook {
public:
    explicit SideBook(Side side) noexcept : side_(side) {}

    struct Delta {
        LevelQty before = 0;
        LevelQty after = 0;
        bool top_changed = false;
    };

    /// A new order of `qty` shares joins the level at `price`.
    Delta add(Price price, Qty qty) {
        const std::uint32_t key = priority_key(side_, price);
        const auto best = levels_.best();
        Level& level = levels_.find_or_insert(key);
        const Delta delta{level.qty, level.qty + qty, !best || key >= best->first};
        level.qty += qty;
        ++level.orders;
        return delta;
    }

    /// `qty` shares leave the level at `price`. If `order_gone`, an order left with them, and
    /// a level with no orders left is removed.
    Delta reduce(Price price, Qty qty, bool order_gone) {
        const std::uint32_t key = priority_key(side_, price);
        Level* const level = levels_.find(key);
        assert(level != nullptr && "reduce on a level that does not exist");
        assert(level->qty >= qty && "level quantity would go negative");
        const auto best = levels_.best();
        const Delta delta{level->qty, level->qty - qty, best && key == best->first};
        level->qty -= qty;
        if (order_gone) {
            assert(level->orders > 0);
            if (--level->orders == 0) {
                assert(level->qty == 0 && "empty level still holds shares");
                levels_.erase(key);
            }
        }
        return delta;
    }

    [[nodiscard]] std::optional<LevelView> best() const {
        const auto best = levels_.best();
        if (!best) {
            return std::nullopt;
        }
        return LevelView{price_of_key(side_, best->first), best->second.qty, best->second.orders};
    }

    /// The level at `price`; zero quantity and orders if there is none.
    [[nodiscard]] LevelView level(Price price) const {
        const Level* const level = levels_.find(priority_key(side_, price));
        return level == nullptr ? LevelView{price, 0, 0}
                                : LevelView{price, level->qty, level->orders};
    }

    /// Fills `out` with levels from best to worst and returns how many were written.
    [[nodiscard]] std::size_t depth(std::span<LevelView> out) const {
        std::size_t n = 0;
        levels_.for_each_best_first([&](std::uint32_t key, const Level& level) {
            if (n == out.size()) {
                return false;
            }
            out[n++] = LevelView{price_of_key(side_, key), level.qty, level.orders};
            return true;
        });
        return n;
    }

    /// Visits every level from best to worst until `f` returns false.
    template <typename F>
    void for_each(F&& f) const {
        levels_.for_each_best_first([&](std::uint32_t key, const Level& level) {
            return f(LevelView{price_of_key(side_, key), level.qty, level.orders});
        });
    }

    [[nodiscard]] std::size_t level_count() const noexcept { return levels_.size(); }
    [[nodiscard]] Side side() const noexcept { return side_; }

private:
    Side side_;
    Levels levels_;
};

/// Both sides of one symbol.
template <typename Levels>
struct Book {
    SideBook<Levels> bids{Side::Buy};
    SideBook<Levels> asks{Side::Sell};

    [[nodiscard]] SideBook<Levels>& side(Side s) noexcept { return s == Side::Buy ? bids : asks; }
    [[nodiscard]] const SideBook<Levels>& side(Side s) const noexcept {
        return s == Side::Buy ? bids : asks;
    }
    [[nodiscard]] std::optional<LevelView> best_bid() const { return bids.best(); }
    [[nodiscard]] std::optional<LevelView> best_ask() const { return asks.best(); }

    /// True if the best bid is at or above the best ask.
    [[nodiscard]] bool crossed_or_locked() const {
        const auto bid = bids.best();
        const auto ask = asks.best();
        return bid && ask && bid->price >= ask->price;
    }
};

/// All books for a day, with the shared order store.
template <typename OrderMap, typename Levels>
class BookSet {
public:
    using BookType = Book<Levels>;

    /// Locates are 16 bits on the wire, so one book per possible locate is allocated up front
    /// and indexing never needs a bounds check or a resize.
    static constexpr std::size_t kLocateCount = 65536;

    explicit BookSet(std::size_t expected_orders = 0)
        : orders_(expected_orders), books_(kLocateCount) {}

    /// A, F: a new order joins the back of its price level.
    BookChange add(OrderRef ref, Locate locate, Side side, Qty qty, Price price) {
        const Sequence seq = ++sequence_;
        if (!orders_.insert(ref, Order{seq, price, qty, locate, side})) {
            ++anomalies_.duplicate_refs;
            return {};
        }
        peak_orders_ = orders_.size() > peak_orders_ ? orders_.size() : peak_orders_;
        const auto delta = books_[locate].side(side).add(price, qty);
        return {true, delta.top_changed, false,       side, locate, price,
                qty,  delta.before,      delta.after, seq};
    }

    /// E, C, X: `qty` shares leave an order. An order reduced to zero is removed.
    BookChange reduce(OrderRef ref, Qty qty) {
        Order* const order = orders_.find(ref);
        if (order == nullptr) {
            ++anomalies_.unknown_refs;
            return {};
        }
        if (qty > order->qty) {
            // Never seen in real data; clamp so the level total cannot underflow.
            ++anomalies_.over_reductions;
            qty = order->qty;
        }
        const Order before = *order;
        const bool gone = qty == before.qty;
        const auto delta = books_[before.locate].side(before.side).reduce(before.price, qty, gone);
        if (gone) {
            orders_.erase(ref);
        } else {
            order->qty -= qty;
        }
        return {true, delta.top_changed, gone,        before.side, before.locate, before.price,
                qty,  delta.before,      delta.after, before.seq};
    }

    /// D: the whole order leaves the book.
    BookChange remove(OrderRef ref) {
        const Order* const order = orders_.find(ref);
        if (order == nullptr) {
            ++anomalies_.unknown_refs;
            return {};
        }
        const Order before = *order;
        const auto delta =
            books_[before.locate].side(before.side).reduce(before.price, before.qty, true);
        orders_.erase(ref);
        return {true,         delta.top_changed, true,         before.side, before.locate,
                before.price, before.qty,        delta.before, delta.after, before.seq};
    }

    /// U: the old order is removed and a new one added with the old one's side and symbol,
    /// at a new sequence, so it goes to the back of the queue even at an unchanged price.
    ReplaceChange replace(OrderRef old_ref, OrderRef new_ref, Qty qty, Price price) {
        const Order* const old_order = orders_.find(old_ref);
        if (old_order == nullptr) {
            ++anomalies_.unknown_refs;
            return {};
        }
        const Locate locate = old_order->locate;
        const Side side = old_order->side;
        ReplaceChange change;
        change.removed = remove(old_ref);
        change.added = add(new_ref, locate, side, qty, price);
        return change;
    }

    [[nodiscard]] const BookType& book(Locate locate) const noexcept { return books_[locate]; }
    [[nodiscard]] const Order* find_order(OrderRef ref) const { return orders_.find(ref); }
    [[nodiscard]] std::size_t live_orders() const noexcept { return orders_.size(); }
    [[nodiscard]] std::size_t peak_live_orders() const noexcept { return peak_orders_; }
    /// The sequence most recently assigned; every live order has a sequence <= this.
    [[nodiscard]] Sequence last_sequence() const noexcept { return sequence_; }
    [[nodiscard]] const Anomalies& anomalies() const noexcept { return anomalies_; }

    /// Full recount: rebuilds every level from the order store and compares it with the level
    /// containers. O(orders); for periodic checks and tests, not the hot path. On failure
    /// returns false and describes the first difference in `error`.
    [[nodiscard]] bool verify(std::string& error) const {
        std::map<std::tuple<Locate, Side, Price>, Level> expected;
        orders_.for_each([&](OrderRef, const Order& o) {
            Level& level = expected[{o.locate, o.side, o.price}];
            level.qty += o.qty;
            ++level.orders;
        });
        std::size_t actual_levels = 0;
        for (std::size_t locate = 0; locate < books_.size(); ++locate) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                bool ok = true;
                books_[locate].side(side).for_each([&](const LevelView& view) {
                    ++actual_levels;
                    const auto it = expected.find({static_cast<Locate>(locate), side, view.price});
                    if (it == expected.end() || it->second.qty != view.qty ||
                        it->second.orders != view.orders) {
                        error = "locate " + std::to_string(locate) +
                                (side == Side::Buy ? " bid " : " ask ") +
                                std::to_string(view.price) + ": level holds " +
                                std::to_string(view.qty) + " shares in " +
                                std::to_string(view.orders) + " orders, order store says " +
                                (it == expected.end() ? std::string("no such level")
                                                      : std::to_string(it->second.qty) + " in " +
                                                            std::to_string(it->second.orders));
                        ok = false;
                    }
                    return ok;
                });
                if (!ok) {
                    return false;
                }
            }
        }
        if (actual_levels != expected.size()) {
            error = "book holds " + std::to_string(actual_levels) +
                    " levels, order store implies " + std::to_string(expected.size());
            return false;
        }
        return true;
    }

private:
    OrderMap orders_;
    std::vector<BookType> books_;
    Sequence sequence_ = 0;
    std::size_t peak_orders_ = 0;
    Anomalies anomalies_;
};

// Applying decoded ITCH messages. Kept as free functions so the engine, the tools and the
// tests all drive a book the same way.

template <typename Books>
BookChange apply(Books& books, const itch::AddOrder& m) {
    return books.add(m.ref, m.h.locate, m.side, m.qty, m.price);
}
template <typename Books>
BookChange apply(Books& books, const itch::OrderExecuted& m) {
    return books.reduce(m.ref, m.qty);
}
template <typename Books>
BookChange apply(Books& books, const itch::OrderExecutedWithPrice& m) {
    return books.reduce(m.ref, m.qty);
}
template <typename Books>
BookChange apply(Books& books, const itch::OrderCancel& m) {
    return books.reduce(m.ref, m.qty);
}
template <typename Books>
BookChange apply(Books& books, const itch::OrderDelete& m) {
    return books.remove(m.ref);
}
template <typename Books>
ReplaceChange apply(Books& books, const itch::OrderReplace& m) {
    return books.replace(m.old_ref, m.new_ref, m.qty, m.price);
}

/// The reference implementation: standard-library containers throughout.
using BaselineBooks = BookSet<StdOrderMap, MapLevels>;

} // namespace itchbt::book
