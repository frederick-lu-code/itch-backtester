#pragma once

// SHA-256 (FIPS 180-4), for recording the input file's digest in run.json.
//
// Written here rather than pulled in as a dependency: it is 80 lines, it runs once per run
// outside the hot path, and its test checks it against the standard's published vectors.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace itchbt {

class Sha256 {
public:
    void update(std::span<const std::byte> data) noexcept;
    /// Finishes the hash and returns it as 64 lowercase hex characters. Call once.
    [[nodiscard]] std::string hex_digest();

private:
    void compress(const std::byte* block) noexcept;

    std::array<std::uint32_t, 8> state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::byte, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

[[nodiscard]] std::string sha256_hex(std::span<const std::byte> data);

} // namespace itchbt
