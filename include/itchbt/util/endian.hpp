#pragma once

// Big-endian field decoding for ITCH.
//
// Every load goes through std::memcpy into a correctly typed object and is then byte swapped.
// Nothing here casts a byte pointer to a wider type, so there is no alignment or strict-aliasing
// undefined behaviour, and compilers still reduce each load to a single mov plus bswap.

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itchbt {

// std::byteswap is C++23; the project builds as C++20, so use the compiler builtins.
[[nodiscard]] constexpr std::uint16_t byteswap(std::uint16_t v) noexcept {
    return __builtin_bswap16(v);
}
[[nodiscard]] constexpr std::uint32_t byteswap(std::uint32_t v) noexcept {
    return __builtin_bswap32(v);
}
[[nodiscard]] constexpr std::uint64_t byteswap(std::uint64_t v) noexcept {
    return __builtin_bswap64(v);
}

/// Reads a big-endian unsigned integer of sizeof(T) bytes starting at `p`.
template <typename T>
    requires std::same_as<T, std::uint16_t> || std::same_as<T, std::uint32_t> ||
             std::same_as<T, std::uint64_t>
[[nodiscard]] inline T load_be(const std::byte* p) noexcept {
    T value;
    std::memcpy(&value, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        value = byteswap(value);
    }
    return value;
}

/// Reads a 6-byte big-endian unsigned integer (the ITCH timestamp) starting at `p`.
/// Touches exactly six bytes, so it is safe on the last message of a mapping.
[[nodiscard]] inline std::uint64_t load_be48(const std::byte* p) noexcept {
    return (std::uint64_t{load_be<std::uint16_t>(p)} << 32) | load_be<std::uint32_t>(p + 2);
}

} // namespace itchbt
