#pragma once

// ITCH 5.0 framing and decoding.
//
// A file is a sequence of records: a 2-byte big-endian length, then that many bytes of message.
// parse() walks the records, validates each against the length the specification gives for its
// type, decodes it and hands it to a handler bound at compile time so the calls inline.
//
// No allocation, no exceptions, no copying of the input: errors are returned, not thrown.

#include "itchbt/itch/messages.hpp"
#include "itchbt/util/endian.hpp"
#include "itchbt/util/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

namespace itchbt::itch {

inline constexpr std::size_t kLengthPrefixSize = 2;
inline constexpr std::size_t kHeaderSize = 11;

namespace detail {

constexpr std::array<std::uint8_t, 256> make_length_table() {
    std::array<std::uint8_t, 256> table{};
    const auto set = [&table](char type, std::uint8_t length) {
        table[static_cast<unsigned char>(type)] = length;
    };
    set('S', 12); // System event
    set('R', 39); // Stock directory
    set('H', 25); // Stock trading action
    set('Y', 20); // Reg SHO restriction
    set('L', 26); // Market participant position
    set('V', 35); // MWCB decline level
    set('W', 12); // MWCB status
    set('K', 28); // IPO quoting period update
    set('J', 35); // LULD auction collar
    set('h', 21); // Operational halt
    set('A', 36); // Add order
    set('F', 40); // Add order with MPID
    set('E', 31); // Order executed
    set('C', 36); // Order executed with price
    set('X', 23); // Order cancel
    set('D', 19); // Order delete
    set('U', 35); // Order replace
    set('P', 44); // Trade (non-cross)
    set('Q', 40); // Cross trade
    set('B', 19); // Broken trade
    set('I', 50); // NOII
    set('N', 20); // RPII
    set('O', 48); // Direct listing with capital raise
    return table;
}

inline constexpr std::array<std::uint8_t, 256> kLengthTable = make_length_table();

[[nodiscard]] inline char char_at(const std::byte* m, std::size_t offset) noexcept {
    return std::to_integer<char>(m[offset]);
}

[[nodiscard]] inline Symbol symbol_at(const std::byte* m, std::size_t offset) noexcept {
    Symbol s;
    std::memcpy(s.chars.data(), m + offset, s.chars.size());
    return s;
}

[[nodiscard]] inline Side side_at(const std::byte* m, std::size_t offset) noexcept {
    // The specification allows only 'B' and 'S'. Anything else decodes as Sell rather than
    // failing: real files never contain it, and the fuzzer must not find a crash here.
    return char_at(m, offset) == 'B' ? Side::Buy : Side::Sell;
}

} // namespace detail

/// Length in bytes of a message of the given type, excluding the 2-byte length prefix.
/// Zero for a type ITCH 5.0 does not define.
[[nodiscard]] constexpr std::size_t message_length(char type) noexcept {
    return detail::kLengthTable[static_cast<unsigned char>(type)];
}

// Decoders. Each takes a pointer to the first byte of a message (the type byte) that is known
// to be at least message_length(type) bytes long. Offsets are from the official specification.

[[nodiscard]] inline Header decode_header(const std::byte* m) noexcept {
    Header h;
    h.type = detail::char_at(m, 0);
    h.locate = load_be<std::uint16_t>(m + 1);
    h.tracking = load_be<std::uint16_t>(m + 3);
    h.ts = load_be48(m + 5);
    return h;
}

[[nodiscard]] inline SystemEvent decode_system_event(const std::byte* m) noexcept {
    return {decode_header(m), detail::char_at(m, 11)};
}

[[nodiscard]] inline StockDirectory decode_stock_directory(const std::byte* m) noexcept {
    StockDirectory d;
    d.h = decode_header(m);
    d.symbol = detail::symbol_at(m, 11);
    d.market_category = detail::char_at(m, 19);
    d.financial_status = detail::char_at(m, 20);
    d.round_lot_size = load_be<std::uint32_t>(m + 21);
    d.round_lots_only = detail::char_at(m, 25);
    d.issue_classification = detail::char_at(m, 26);
    d.issue_sub_type = {detail::char_at(m, 27), detail::char_at(m, 28)};
    d.authenticity = detail::char_at(m, 29);
    d.short_sale_threshold = detail::char_at(m, 30);
    d.ipo_flag = detail::char_at(m, 31);
    d.luld_tier = detail::char_at(m, 32);
    d.etp_flag = detail::char_at(m, 33);
    d.etp_leverage_factor = load_be<std::uint32_t>(m + 34);
    d.inverse_indicator = detail::char_at(m, 38);
    return d;
}

[[nodiscard]] inline TradingAction decode_trading_action(const std::byte* m) noexcept {
    TradingAction a;
    a.h = decode_header(m);
    a.symbol = detail::symbol_at(m, 11);
    a.state = detail::char_at(m, 19);
    // Offset 20 is reserved.
    a.reason = {detail::char_at(m, 21), detail::char_at(m, 22), detail::char_at(m, 23),
                detail::char_at(m, 24)};
    return a;
}

/// Decodes both 'A' and 'F'; they share a layout up to the trailing MPID of 'F'.
[[nodiscard]] inline AddOrder decode_add_order(const std::byte* m) noexcept {
    AddOrder a;
    a.h = decode_header(m);
    a.ref = load_be<std::uint64_t>(m + 11);
    a.side = detail::side_at(m, 19);
    a.qty = load_be<std::uint32_t>(m + 20);
    a.symbol = detail::symbol_at(m, 24);
    a.price = load_be<std::uint32_t>(m + 32);
    return a;
}

[[nodiscard]] inline OrderExecuted decode_order_executed(const std::byte* m) noexcept {
    OrderExecuted e;
    e.h = decode_header(m);
    e.ref = load_be<std::uint64_t>(m + 11);
    e.qty = load_be<std::uint32_t>(m + 19);
    e.match = load_be<std::uint64_t>(m + 23);
    return e;
}

[[nodiscard]] inline OrderExecutedWithPrice
decode_order_executed_with_price(const std::byte* m) noexcept {
    OrderExecutedWithPrice e;
    e.h = decode_header(m);
    e.ref = load_be<std::uint64_t>(m + 11);
    e.qty = load_be<std::uint32_t>(m + 19);
    e.match = load_be<std::uint64_t>(m + 23);
    e.printable = detail::char_at(m, 31) == 'Y';
    e.price = load_be<std::uint32_t>(m + 32);
    return e;
}

[[nodiscard]] inline OrderCancel decode_order_cancel(const std::byte* m) noexcept {
    OrderCancel c;
    c.h = decode_header(m);
    c.ref = load_be<std::uint64_t>(m + 11);
    c.qty = load_be<std::uint32_t>(m + 19);
    return c;
}

[[nodiscard]] inline OrderDelete decode_order_delete(const std::byte* m) noexcept {
    return {decode_header(m), load_be<std::uint64_t>(m + 11)};
}

[[nodiscard]] inline OrderReplace decode_order_replace(const std::byte* m) noexcept {
    OrderReplace u;
    u.h = decode_header(m);
    u.old_ref = load_be<std::uint64_t>(m + 11);
    u.new_ref = load_be<std::uint64_t>(m + 19);
    u.qty = load_be<std::uint32_t>(m + 27);
    u.price = load_be<std::uint32_t>(m + 31);
    return u;
}

[[nodiscard]] inline Trade decode_trade(const std::byte* m) noexcept {
    Trade t;
    t.h = decode_header(m);
    // Offset 11 is the order reference, always zero for this message.
    t.side = detail::side_at(m, 19);
    t.qty = load_be<std::uint32_t>(m + 20);
    t.symbol = detail::symbol_at(m, 24);
    t.price = load_be<std::uint32_t>(m + 32);
    t.match = load_be<std::uint64_t>(m + 36);
    return t;
}

[[nodiscard]] inline CrossTrade decode_cross_trade(const std::byte* m) noexcept {
    CrossTrade q;
    q.h = decode_header(m);
    q.shares = load_be<std::uint64_t>(m + 11); // eight bytes here, unlike other share fields
    q.symbol = detail::symbol_at(m, 19);
    q.price = load_be<std::uint32_t>(m + 27);
    q.match = load_be<std::uint64_t>(m + 31);
    q.cross_type = detail::char_at(m, 39);
    return q;
}

[[nodiscard]] inline BrokenTrade decode_broken_trade(const std::byte* m) noexcept {
    return {decode_header(m), load_be<std::uint64_t>(m + 11)};
}

/// Decodes one validated message and passes it to the matching handler overload.
///
/// A handler provides on(const T&) for each struct in messages.hpp and
/// on_skipped(const Header&) for the types the engine recognises but does not use.
template <typename Handler>
inline void dispatch(const std::byte* m, Handler& handler) {
    switch (detail::char_at(m, 0)) {
    case 'A':
    case 'F': handler.on(decode_add_order(m)); break;
    case 'E': handler.on(decode_order_executed(m)); break;
    case 'C': handler.on(decode_order_executed_with_price(m)); break;
    case 'X': handler.on(decode_order_cancel(m)); break;
    case 'D': handler.on(decode_order_delete(m)); break;
    case 'U': handler.on(decode_order_replace(m)); break;
    case 'P': handler.on(decode_trade(m)); break;
    case 'Q': handler.on(decode_cross_trade(m)); break;
    case 'B': handler.on(decode_broken_trade(m)); break;
    case 'S': handler.on(decode_system_event(m)); break;
    case 'R': handler.on(decode_stock_directory(m)); break;
    case 'H': handler.on(decode_trading_action(m)); break;
    default: handler.on_skipped(decode_header(m)); break;
    }
}

enum class ParseError : std::uint8_t {
    None,
    Truncated,      // the data ends inside a length prefix or a message
    UnknownType,    // the type byte is not an ITCH 5.0 message type
    LengthMismatch, // the record length differs from the specified length for its type
};

[[nodiscard]] constexpr const char* to_string(ParseError e) noexcept {
    switch (e) {
    case ParseError::None: return "none";
    case ParseError::Truncated: return "truncated record";
    case ParseError::UnknownType: return "unknown message type";
    case ParseError::LengthMismatch: return "length mismatch";
    }
    return "?";
}

struct ParseResult {
    ParseError error = ParseError::None;
    /// On success, the number of bytes consumed. On error, the offset of the offending record's
    /// length prefix, relative to the start of the data passed in.
    std::uint64_t offset = 0;
    /// Messages decoded and delivered before stopping.
    std::uint64_t messages = 0;
    /// For UnknownType and LengthMismatch: the offending record's type byte and stated length.
    char type = '\0';
    std::uint16_t length = 0;

    [[nodiscard]] constexpr bool ok() const noexcept { return error == ParseError::None; }
};

/// Parses whole records from `data` until it is exhausted, `max_messages` have been delivered,
/// or a record is malformed. Every read is bounds-checked against the end of `data`.
template <typename Handler>
[[nodiscard]] inline ParseResult
parse(std::span<const std::byte> data, Handler& handler,
      std::uint64_t max_messages = std::numeric_limits<std::uint64_t>::max()) {
    const std::byte* const begin = data.data();
    const std::size_t size = data.size();
    std::size_t pos = 0;
    ParseResult result;

    while (pos < size && result.messages < max_messages) {
        if (size - pos < kLengthPrefixSize) {
            result.error = ParseError::Truncated;
            break;
        }
        const std::uint16_t length = load_be<std::uint16_t>(begin + pos);
        if (size - pos - kLengthPrefixSize < length) {
            result.error = ParseError::Truncated;
            result.length = length;
            break;
        }
        const std::byte* const message = begin + pos + kLengthPrefixSize;
        // A zero-length record has no type byte to read.
        const char type = length == 0 ? '\0' : detail::char_at(message, 0);
        const std::size_t expected = message_length(type);
        if (expected != length || length == 0) [[unlikely]] {
            result.error =
                expected == 0 && length != 0 ? ParseError::UnknownType : ParseError::LengthMismatch;
            result.type = type;
            result.length = length;
            break;
        }
        dispatch(message, handler);
        ++result.messages;
        pos += kLengthPrefixSize + length;
    }
    result.offset = pos;
    return result;
}

/// A handler that ignores everything. Derive from it and add `using NullHandler::on;` to
/// override only the messages of interest.
struct NullHandler {
    void on(const SystemEvent&) noexcept {}
    void on(const StockDirectory&) noexcept {}
    void on(const TradingAction&) noexcept {}
    void on(const AddOrder&) noexcept {}
    void on(const OrderExecuted&) noexcept {}
    void on(const OrderExecutedWithPrice&) noexcept {}
    void on(const OrderCancel&) noexcept {}
    void on(const OrderDelete&) noexcept {}
    void on(const OrderReplace&) noexcept {}
    void on(const Trade&) noexcept {}
    void on(const CrossTrade&) noexcept {}
    void on(const BrokenTrade&) noexcept {}
    void on_skipped(const Header&) noexcept {}
};

} // namespace itchbt::itch
