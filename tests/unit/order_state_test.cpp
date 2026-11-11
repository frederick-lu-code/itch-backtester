#include "itchbt/sim/order_state.hpp"

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <utility>

namespace itchbt::sim {
namespace {

using S = OrderState;

constexpr std::array<S, kOrderStateCount> kAll = {S::PendingNew,    S::Live,   S::PartiallyFilled,
                                                  S::PendingCancel, S::Filled, S::Cancelled,
                                                  S::Rejected};

// The specification's table, typed out independently of the implementation.
const std::set<std::pair<S, S>> kLegal = {
    {S::PendingNew, S::Live},         {S::PendingNew, S::Rejected},
    {S::PendingNew, S::Filled},    // IOC filled in full on arrival
    {S::PendingNew, S::Cancelled}, // IOC remainder
    {S::Live, S::PartiallyFilled},    {S::Live, S::Filled},
    {S::Live, S::PendingCancel},      {S::PartiallyFilled, S::PartiallyFilled},
    {S::PartiallyFilled, S::Filled},  {S::PartiallyFilled, S::PendingCancel},
    {S::PendingCancel, S::Cancelled}, {S::PendingCancel, S::PartiallyFilled},
    {S::PendingCancel, S::Filled},
};

TEST(OrderState, EveryPairAgreesWithTheSpecifiedTable) {
    int legal = 0;
    for (const S from : kAll) {
        for (const S to : kAll) {
            const bool expected = kLegal.contains({from, to});
            EXPECT_EQ(can_transition(from, to), expected)
                << to_string(from) << " -> " << to_string(to);
            legal += expected ? 1 : 0;
        }
    }
    EXPECT_EQ(legal, 13); // 13 legal transitions, so 36 of the 49 pairs are rejected
}

TEST(OrderState, TerminalStatesAllowNothing) {
    for (const S terminal : {S::Filled, S::Cancelled, S::Rejected}) {
        EXPECT_TRUE(is_terminal(terminal));
        for (const S to : kAll) {
            EXPECT_FALSE(can_transition(terminal, to)) << to_string(terminal);
        }
    }
    for (const S live : {S::PendingNew, S::Live, S::PartiallyFilled, S::PendingCancel}) {
        EXPECT_FALSE(is_terminal(live));
    }
}

TEST(OrderState, TransitionMovesTheState) {
    S state = S::PendingNew;
    transition(state, S::Live);
    transition(state, S::PartiallyFilled);
    transition(state, S::PartiallyFilled);
    transition(state, S::PendingCancel);
    transition(state, S::Cancelled);
    EXPECT_EQ(state, S::Cancelled);
}

#ifndef NDEBUG
TEST(OrderStateDeathTest, IllegalTransitionAbortsInDebugBuilds) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            S state = S::Filled;
            transition(state, S::Live);
        },
        "illegal order state transition");
    EXPECT_DEATH(
        {
            S state = S::Live;
            transition(state, S::Cancelled); // must pass through PendingCancel
        },
        "illegal order state transition");
}
#endif

} // namespace
} // namespace itchbt::sim
