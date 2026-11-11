#pragma once

// The order state machine as an explicit transition table.
//
//   PendingNew       -> Live, Rejected, Filled (IOC), Cancelled (IOC remainder)
//   Live             -> PartiallyFilled, Filled, PendingCancel
//   PartiallyFilled  -> PartiallyFilled, Filled, PendingCancel
//   PendingCancel    -> Cancelled, PartiallyFilled, Filled
//   Filled, Cancelled, Rejected are terminal.
//
// Every state change in the simulator goes through transition(). An illegal one is a bug in
// the simulator, not a condition to handle, so it aborts in debug builds.

#include "itchbt/sim/types.hpp"

#include <array>
#include <cassert>
#include <cstdint>

namespace itchbt::sim {

namespace detail {

constexpr std::array<std::uint8_t, kOrderStateCount> make_transition_table() {
    const auto bit = [](OrderState s) {
        return static_cast<std::uint8_t>(1U << static_cast<unsigned>(s));
    };
    std::array<std::uint8_t, kOrderStateCount> table{};
    table[static_cast<std::size_t>(OrderState::PendingNew)] =
        static_cast<std::uint8_t>(bit(OrderState::Live) | bit(OrderState::Rejected) |
                                  bit(OrderState::Filled) | bit(OrderState::Cancelled));
    table[static_cast<std::size_t>(OrderState::Live)] =
        static_cast<std::uint8_t>(bit(OrderState::PartiallyFilled) | bit(OrderState::Filled) |
                                  bit(OrderState::PendingCancel));
    table[static_cast<std::size_t>(OrderState::PartiallyFilled)] =
        static_cast<std::uint8_t>(bit(OrderState::PartiallyFilled) | bit(OrderState::Filled) |
                                  bit(OrderState::PendingCancel));
    table[static_cast<std::size_t>(OrderState::PendingCancel)] = static_cast<std::uint8_t>(
        bit(OrderState::Cancelled) | bit(OrderState::PartiallyFilled) | bit(OrderState::Filled));
    // Filled, Cancelled and Rejected keep their zero rows: nothing follows them.
    return table;
}

inline constexpr std::array<std::uint8_t, kOrderStateCount> kTransitions = make_transition_table();

} // namespace detail

[[nodiscard]] constexpr bool can_transition(OrderState from, OrderState to) noexcept {
    return ((detail::kTransitions[static_cast<std::size_t>(from)] >> static_cast<unsigned>(to)) &
            1U) != 0;
}

[[nodiscard]] constexpr bool is_terminal(OrderState s) noexcept {
    return s == OrderState::Filled || s == OrderState::Cancelled || s == OrderState::Rejected;
}

[[nodiscard]] constexpr const char* to_string(OrderState s) noexcept {
    switch (s) {
    case OrderState::PendingNew: return "pending_new";
    case OrderState::Live: return "live";
    case OrderState::PartiallyFilled: return "partially_filled";
    case OrderState::PendingCancel: return "pending_cancel";
    case OrderState::Filled: return "filled";
    case OrderState::Cancelled: return "cancelled";
    case OrderState::Rejected: return "rejected";
    }
    return "?";
}

/// Moves `state` to `to`. Aborts in debug builds if the table does not allow it.
constexpr void transition(OrderState& state, OrderState to) noexcept {
    assert(can_transition(state, to) && "illegal order state transition");
    state = to;
}

} // namespace itchbt::sim
