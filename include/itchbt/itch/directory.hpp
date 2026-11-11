#pragma once

// Per-day reference data built from the feed itself: the locate-to-symbol map from stock
// directory messages, and session and halt state from system events and trading actions.
// Locate codes are valid for one day only, so nothing here is ever hard-coded.

#include "itchbt/itch/messages.hpp"
#include "itchbt/util/types.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace itchbt::itch {

class SymbolDirectory {
public:
    SymbolDirectory() : symbols_(kLocateCount), known_(kLocateCount, false) {}

    void on(const StockDirectory& m) {
        symbols_[m.h.locate] = m.symbol;
        if (!known_[m.h.locate]) {
            known_[m.h.locate] = true;
            ++count_;
        }
        by_name_[std::string(m.symbol.view())] = m.h.locate;
    }

    [[nodiscard]] std::optional<Locate> find(std::string_view symbol) const {
        const auto it = by_name_.find(std::string(symbol));
        return it == by_name_.end() ? std::nullopt : std::optional<Locate>(it->second);
    }
    [[nodiscard]] bool known(Locate locate) const noexcept { return known_[locate]; }
    /// The symbol for `locate`; empty if no directory message named it.
    [[nodiscard]] std::string_view symbol(Locate locate) const noexcept {
        return known_[locate] ? symbols_[locate].view() : std::string_view{};
    }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    static constexpr std::size_t kLocateCount = 65536;

    std::vector<Symbol> symbols_;
    std::vector<bool> known_;
    std::unordered_map<std::string, Locate> by_name_;
    std::size_t count_ = 0;
};

/// Session phase and per-symbol trading state.
class MarketState {
public:
    MarketState() : state_(kLocateCount, ' ') {}

    void on(const SystemEvent& m) noexcept {
        if (m.code == 'Q') {
            regular_hours_ = true;
        } else if (m.code == 'M') {
            regular_hours_ = false;
        }
        last_event_ = m.code;
    }
    void on(const TradingAction& m) noexcept { state_[m.h.locate] = m.state; }

    /// Between "start of market hours" and "end of market hours".
    [[nodiscard]] bool regular_hours() const noexcept { return regular_hours_; }
    /// The symbol's last trading action was "T". A symbol absent from the pre-open spin has
    /// no trading action and counts as halted, as the specification says to assume.
    [[nodiscard]] bool trading(Locate locate) const noexcept { return state_[locate] == 'T'; }
    [[nodiscard]] char trading_state(Locate locate) const noexcept { return state_[locate]; }
    [[nodiscard]] char last_system_event() const noexcept { return last_event_; }

private:
    static constexpr std::size_t kLocateCount = 65536;

    std::vector<char> state_;
    bool regular_hours_ = false;
    char last_event_ = '\0';
};

} // namespace itchbt::itch
