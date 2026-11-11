#pragma once

// Conversions between ITCH timestamps (nanoseconds since midnight) and text.

#include "itchbt/util/types.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace itchbt {

inline constexpr Timestamp kNanosPerSecond = 1'000'000'000;
inline constexpr Timestamp kNanosPerDay = 86'400 * kNanosPerSecond;

/// Formats as HH:MM:SS.nnnnnnnnn.
[[nodiscard]] inline std::string format_time(Timestamp ts) {
    const Timestamp seconds = ts / kNanosPerSecond;
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%02llu:%02llu:%02llu.%09llu",
                  static_cast<unsigned long long>(seconds / 3600),
                  static_cast<unsigned long long>(seconds / 60 % 60),
                  static_cast<unsigned long long>(seconds % 60),
                  static_cast<unsigned long long>(ts % kNanosPerSecond));
    return text.data();
}

/// Parses HH:MM[:SS[.fraction]] into nanoseconds since midnight.
[[nodiscard]] inline std::optional<Timestamp> parse_time(std::string_view text) {
    std::array<Timestamp, 3> parts{0, 0, 0};
    std::size_t part = 0;
    std::size_t digits = 0;
    Timestamp fraction = 0;
    Timestamp fraction_scale = kNanosPerSecond;
    bool in_fraction = false;

    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            const auto digit = static_cast<Timestamp>(c - '0');
            if (in_fraction) {
                if (fraction_scale == 1) {
                    return std::nullopt; // more than nine fractional digits
                }
                fraction_scale /= 10;
                fraction += digit * fraction_scale;
            } else {
                if (++digits > 2) {
                    return std::nullopt;
                }
                parts[part] = parts[part] * 10 + digit;
            }
        } else if (c == ':' && !in_fraction && part < 2 && digits > 0) {
            ++part;
            digits = 0;
        } else if (c == '.' && !in_fraction && part == 2 && digits > 0) {
            in_fraction = true;
        } else {
            return std::nullopt;
        }
    }
    if (in_fraction && fraction_scale == kNanosPerSecond) {
        return std::nullopt; // a decimal point with no digits after it
    }
    if (part == 0 || digits == 0 || parts[0] > 23 || parts[1] > 59 || parts[2] > 59) {
        return std::nullopt;
    }
    return (parts[0] * 3600 + parts[1] * 60 + parts[2]) * kNanosPerSecond + fraction;
}

} // namespace itchbt
