#pragma once

// A parser handler that keeps every message it is given, for assertions in tests.

#include "itchbt/itch/messages.hpp"

#include <vector>

namespace itchbt::test {

struct Collector {
    std::vector<itch::SystemEvent> system_events;
    std::vector<itch::StockDirectory> directories;
    std::vector<itch::TradingAction> trading_actions;
    std::vector<itch::AddOrder> adds;
    std::vector<itch::OrderExecuted> executions;
    std::vector<itch::OrderExecutedWithPrice> priced_executions;
    std::vector<itch::OrderCancel> cancels;
    std::vector<itch::OrderDelete> deletes;
    std::vector<itch::OrderReplace> replaces;
    std::vector<itch::Trade> trades;
    std::vector<itch::CrossTrade> crosses;
    std::vector<itch::BrokenTrade> broken_trades;
    std::vector<itch::Header> skipped;
    /// Type byte of every message, in arrival order.
    std::vector<char> order;

    void on(const itch::SystemEvent& m) { keep(system_events, m); }
    void on(const itch::StockDirectory& m) { keep(directories, m); }
    void on(const itch::TradingAction& m) { keep(trading_actions, m); }
    void on(const itch::AddOrder& m) { keep(adds, m); }
    void on(const itch::OrderExecuted& m) { keep(executions, m); }
    void on(const itch::OrderExecutedWithPrice& m) { keep(priced_executions, m); }
    void on(const itch::OrderCancel& m) { keep(cancels, m); }
    void on(const itch::OrderDelete& m) { keep(deletes, m); }
    void on(const itch::OrderReplace& m) { keep(replaces, m); }
    void on(const itch::Trade& m) { keep(trades, m); }
    void on(const itch::CrossTrade& m) { keep(crosses, m); }
    void on(const itch::BrokenTrade& m) { keep(broken_trades, m); }
    void on_skipped(const itch::Header& h) {
        skipped.push_back(h);
        order.push_back(h.type);
    }

private:
    template <typename Message>
    void keep(std::vector<Message>& into, const Message& m) {
        into.push_back(m);
        order.push_back(m.h.type);
    }
};

} // namespace itchbt::test
