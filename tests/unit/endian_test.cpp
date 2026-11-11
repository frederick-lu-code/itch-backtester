#include "itchbt/util/endian.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace itchbt {
namespace {

template <typename... Ts>
std::array<std::byte, sizeof...(Ts)> bytes_of(Ts... values) {
    return {static_cast<std::byte>(values)...};
}

TEST(Endian, LoadsBigEndianIntegers) {
    const auto b = bytes_of(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);
    EXPECT_EQ(load_be<std::uint16_t>(b.data()), 0x0102U);
    EXPECT_EQ(load_be<std::uint32_t>(b.data()), 0x01020304U);
    EXPECT_EQ(load_be<std::uint64_t>(b.data()), 0x0102030405060708ULL);
}

TEST(Endian, LoadsFromUnalignedAddresses) {
    // Fields in ITCH messages sit at odd offsets; every offset must work.
    const auto b = bytes_of(0xAA, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xBB);
    EXPECT_EQ(load_be<std::uint16_t>(b.data() + 1), 0x0102U);
    EXPECT_EQ(load_be<std::uint32_t>(b.data() + 1), 0x01020304U);
    EXPECT_EQ(load_be<std::uint64_t>(b.data() + 1), 0x0102030405060708ULL);
}

TEST(Endian, LoadsSixByteTimestamp) {
    const auto open = bytes_of(0x1F, 0x1A, 0xCE, 0xD9, 0xF0, 0x00); // 09:30:00 in nanoseconds
    EXPECT_EQ(load_be48(open.data()), 34'200'000'000'000ULL);

    const auto max = bytes_of(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
    EXPECT_EQ(load_be48(max.data()), 0xFFFF'FFFF'FFFFULL);
}

TEST(Endian, SixByteLoadIgnoresNeighbouringBytes) {
    // The two bytes after a timestamp belong to the next field and must not leak in.
    const auto b = bytes_of(0xEE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xEE, 0xEE);
    EXPECT_EQ(load_be48(b.data() + 1), 1U);
}

TEST(Endian, HighBitSetValuesAreUnsigned) {
    const auto b = bytes_of(0xFF, 0xFE, 0xFD, 0xFC);
    EXPECT_EQ(load_be<std::uint16_t>(b.data()), 0xFFFEU);
    EXPECT_EQ(load_be<std::uint32_t>(b.data()), 0xFFFEFDFCU);
}

} // namespace
} // namespace itchbt
