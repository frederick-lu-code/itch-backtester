#pragma once

// Value types shared by every order book implementation.

#include "itchbt/util/types.hpp"

#include <cstdint>

namespace itchbt::book {

/// Global arrival counter. Assigned to an order when it is added and again when it is
/// replaced, so within a price level a lower sequence means earlier in the queue.
using Sequence = std::uint64_t;

/// Total shares at a price level. Wider than Qty: many orders can sit at one price.
using LevelQty = std::uint64_t;

/// A resting order, as kept in the order store.
struct Order {
    Sequence seq = 0;
    Price price = 0;
    Qty qty = 0;
    Locate locate = 0;
    Side side = Side::Buy;
};

/// Aggregate state of one price level.
struct Level {
    LevelQty qty = 0;
    std::uint32_t orders = 0;
};

/// A price level as seen from outside the book.
struct LevelView {
    Price price = 0;
    LevelQty qty = 0;
    std::uint32_t orders = 0;

    friend bool operator==(const LevelView&, const LevelView&) = default;
};

/// What an operation did to the book: enough for the simulated exchange to track queue
/// position and for the strategy to see top-of-book changes, without re-reading the book.
struct BookChange {
    /// False if the operation referred to an unknown order (or re-used a live reference) and
    /// was ignored. The other fields are then meaningless.
    bool applied = false;
    /// The best price on this side, or the size at it, changed.
    bool top_changed = false;
    /// The order left the book (delete, or an execution or cancel that took it to zero).
    bool order_removed = false;
    Side side = Side::Buy;
    Locate locate = 0;
    /// Price of the level that changed.
    Price price = 0;
    /// Shares added to or removed from the order.
    Qty delta = 0;
    LevelQty level_before = 0;
    LevelQty level_after = 0;
    /// Sequence of the order that changed.
    Sequence seq = 0;
};

/// A replace is a removal of the old order and an add of the new one, at a new sequence.
struct ReplaceChange {
    BookChange removed;
    BookChange added;
};

/// Counts of things that should never happen in a well-formed feed. All are also the
/// invariants checked on the real file.
struct Anomalies {
    std::uint64_t unknown_refs = 0;    // E, C, X, D or U for an order that is not live
    std::uint64_t duplicate_refs = 0;  // A, F or U introducing a reference that is already live
    std::uint64_t over_reductions = 0; // E, C or X for more shares than the order has left

    [[nodiscard]] std::uint64_t total() const noexcept {
        return unknown_refs + duplicate_refs + over_reductions;
    }
};

/// Maps a price to a key in which a larger key is always a better price, so both sides of the
/// book share one container implementation with the best level last.
[[nodiscard]] constexpr std::uint32_t priority_key(Side side, Price price) noexcept {
    return side == Side::Buy ? price : ~price;
}
[[nodiscard]] constexpr Price price_of_key(Side side, std::uint32_t key) noexcept {
    return side == Side::Buy ? key : ~key;
}

} // namespace itchbt::book
