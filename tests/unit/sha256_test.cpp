#include "itchbt/util/sha256.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace itchbt {
namespace {

std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

// Test vectors from FIPS 180-4 / the NIST examples.
TEST(Sha256, MatchesPublishedVectors) {
    EXPECT_EQ(sha256_hex(bytes("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex(bytes("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, MillionAs) {
    const std::string text(1'000'000, 'a');
    EXPECT_EQ(sha256_hex(bytes(text)),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, ChunkedUpdatesMatchOneShot) {
    std::string text;
    for (int i = 0; i < 1000; ++i) {
        text += static_cast<char>('a' + i % 26);
    }
    const std::string expected = sha256_hex(bytes(text));
    // Every split point around the 64-byte block boundary and the 56-byte padding boundary.
    for (const std::size_t chunk :
         {std::size_t{1}, std::size_t{55}, std::size_t{56}, std::size_t{63}, std::size_t{64},
          std::size_t{65}, std::size_t{127}, std::size_t{999}}) {
        Sha256 hash;
        for (std::size_t pos = 0; pos < text.size(); pos += chunk) {
            hash.update(bytes(std::string_view(text).substr(pos, chunk)));
        }
        EXPECT_EQ(hash.hex_digest(), expected) << "chunk size " << chunk;
    }
}

TEST(Sha256, LengthsAroundThePaddingBoundary) {
    // 55 bytes fits length in one block, 56 needs a second; both must differ from each other.
    const std::string a(55, 'x');
    const std::string b(56, 'x');
    const std::string c(64, 'x');
    EXPECT_NE(sha256_hex(bytes(a)), sha256_hex(bytes(b)));
    EXPECT_NE(sha256_hex(bytes(b)), sha256_hex(bytes(c)));
    EXPECT_EQ(sha256_hex(bytes(a)).size(), 64U);
}

} // namespace
} // namespace itchbt
