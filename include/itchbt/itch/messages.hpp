#pragma once

// Decoded ITCH 5.0 messages: small trivially copyable structs in host byte order.

#include "itchbt/util/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace itchbt::itch {

/// An 8-character stock symbol, left-justified and space-padded as on the wire.
struct Symbol {
    std::array<char, 8> chars{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

    Symbol() = default;
    /// Builds a padded symbol from text; characters beyond the eighth are dropped.
    explicit constexpr Symbol(std::string_view text) noexcept {
        for (std::size_t i = 0; i < chars.size() && i < text.size(); ++i) {
            chars[i] = text[i];
        }
    }

    /// The symbol without its trailing padding.
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        std::size_t length = chars.size();
        while (length > 0 && chars[length - 1] == ' ') {
            --length;
        }
        return {chars.data(), length};
    }

    friend bool operator==(const Symbol&, const Symbol&) = default;
};

/// The 11-byte header every message starts with.
struct Header {
    Timestamp ts = 0;
    Locate locate = 0;
    std::uint16_t tracking = 0;
    char type = '\0';
};

struct SystemEvent { // 'S'
    Header h;
    char code = '\0';
};

struct StockDirectory { // 'R'
    Header h;
    Symbol symbol;
    char market_category = ' ';
    char financial_status = ' ';
    std::uint32_t round_lot_size = 0;
    char round_lots_only = ' ';
    char issue_classification = ' ';
    std::array<char, 2> issue_sub_type{' ', ' '};
    char authenticity = ' ';
    char short_sale_threshold = ' ';
    char ipo_flag = ' ';
    char luld_tier = ' ';
    char etp_flag = ' ';
    std::uint32_t etp_leverage_factor = 0;
    char inverse_indicator = ' ';
};

struct TradingAction { // 'H'
    Header h;
    Symbol symbol;
    char state = ' ';
    std::array<char, 4> reason{' ', ' ', ' ', ' '};
};

/// 'A' and 'F'. The MPID attribution of 'F' is not kept: nothing in the engine uses it.
struct AddOrder {
    Header h;
    OrderRef ref = 0;
    Side side = Side::Buy;
    Qty qty = 0;
    Symbol symbol;
    Price price = 0;
};

struct OrderExecuted { // 'E': executes at the resting order's price
    Header h;
    OrderRef ref = 0;
    Qty qty = 0;
    MatchNo match = 0;
};

struct OrderExecutedWithPrice { // 'C': executes at its own price
    Header h;
    OrderRef ref = 0;
    Qty qty = 0;
    MatchNo match = 0;
    bool printable = false;
    Price price = 0;
};

struct OrderCancel { // 'X': partial cancel
    Header h;
    OrderRef ref = 0;
    Qty qty = 0;
};

struct OrderDelete { // 'D'
    Header h;
    OrderRef ref = 0;
};

/// 'U'. Carries no side or symbol: the new order inherits both from the one it replaces.
struct OrderReplace {
    Header h;
    OrderRef old_ref = 0;
    OrderRef new_ref = 0;
    Qty qty = 0;
    Price price = 0;
};

/// 'P': execution against a non-displayed order. Does not change the book.
struct Trade {
    Header h;
    Side side = Side::Buy; // always Buy on the wire since 2014; carries no information
    Qty qty = 0;
    Symbol symbol;
    Price price = 0;
    MatchNo match = 0;
};

struct CrossTrade { // 'Q'
    Header h;
    std::uint64_t shares = 0;
    Symbol symbol;
    Price price = 0;
    MatchNo match = 0;
    char cross_type = ' ';
};

struct BrokenTrade { // 'B'
    Header h;
    MatchNo match = 0;
};

} // namespace itchbt::itch
