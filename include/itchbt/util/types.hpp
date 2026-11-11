#pragma once

#include <cstdint>

namespace itchbt {

/// Nanoseconds since midnight. Six bytes on the wire, so the top 16 bits are always zero.
using Timestamp = std::uint64_t;

/// Stock locate code. Assigned per day by Nasdaq; never hard-code one.
using Locate = std::uint16_t;

/// Order reference number, unique within a day across all symbols.
using OrderRef = std::uint64_t;

/// Match number of an execution, unique within a day.
using MatchNo = std::uint64_t;

/// Whole shares.
using Qty = std::uint32_t;

/// Price in units of 1/10000 USD (ITCH "Price (4)").
using Price = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

/// Largest timestamp that fits the 6-byte wire field.
inline constexpr Timestamp kMaxWireTimestamp = (Timestamp{1} << 48) - 1;

} // namespace itchbt
