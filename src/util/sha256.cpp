#include "itchbt/util/sha256.hpp"

#include "itchbt/util/endian.hpp"

#include <bit>
#include <cstring>

namespace itchbt {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

} // namespace

void Sha256::compress(const std::byte* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = load_be<std::uint32_t>(block + 4 * i);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::array<std::uint32_t, 8> v = state_;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = std::rotr(v[4], 6) ^ std::rotr(v[4], 11) ^ std::rotr(v[4], 25);
        const std::uint32_t choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const std::uint32_t t1 = v[7] + s1 + choose + kRoundConstants[i] + w[i];
        const std::uint32_t s0 = std::rotr(v[0], 2) ^ std::rotr(v[0], 13) ^ std::rotr(v[0], 22);
        const std::uint32_t majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        const std::uint32_t t2 = s0 + majority;
        v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
    }
    for (std::size_t i = 0; i < 8; ++i) {
        state_[i] += v[i];
    }
}

void Sha256::update(std::span<const std::byte> data) noexcept {
    total_bytes_ += data.size();
    if (buffered_ != 0) {
        const std::size_t take = std::min(data.size(), buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, data.data(), take);
        buffered_ += take;
        data = data.subspan(take);
        if (buffered_ < buffer_.size()) {
            return;
        }
        compress(buffer_.data());
        buffered_ = 0;
    }
    while (data.size() >= buffer_.size()) {
        compress(data.data());
        data = data.subspan(buffer_.size());
    }
    if (!data.empty()) {
        std::memcpy(buffer_.data(), data.data(), data.size());
        buffered_ = data.size();
    }
}

std::string Sha256::hex_digest() {
    // Padding: a 1 bit, zeros, then the message length in bits as a 64-bit big-endian integer,
    // ending exactly on a block boundary.
    const std::uint64_t bit_length = total_bytes_ * 8;
    buffer_[buffered_++] = std::byte{0x80};
    if (buffered_ > 56) {
        std::memset(buffer_.data() + buffered_, 0, buffer_.size() - buffered_);
        compress(buffer_.data());
        buffered_ = 0;
    }
    std::memset(buffer_.data() + buffered_, 0, 56 - buffered_);
    for (std::size_t i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<std::byte>((bit_length >> (56 - 8 * i)) & 0xFFU);
    }
    compress(buffer_.data());

    static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string hex;
    hex.reserve(64);
    for (const std::uint32_t word : state_) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex.push_back(kHex[(word >> static_cast<unsigned>(shift)) & 0xFU]);
        }
    }
    return hex;
}

std::string sha256_hex(std::span<const std::byte> data) {
    Sha256 hash;
    hash.update(data);
    return hash.hex_digest();
}

} // namespace itchbt
