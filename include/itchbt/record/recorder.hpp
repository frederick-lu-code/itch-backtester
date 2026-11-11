#pragma once

// Writes a run directory: orders.bin, fills.bin, bbo.bin, risk.bin.
//
// This is the synchronous recorder: each record is appended to a stdio buffer on the engine
// thread. It is the simplest thing that is correct and deterministic. M5 moves the I/O to a
// writer thread behind a ring buffer; this implementation stays as the baseline it is
// measured against.

#include "itchbt/record/records.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace itchbt::record {

struct RecordCounts {
    std::uint64_t orders = 0;
    std::uint64_t fills = 0;
    std::uint64_t bbo = 0;
    std::uint64_t risk = 0;
};

class Recorder {
public:
    /// A recorder that discards everything, for benchmarks and tests that need no output.
    Recorder() = default;
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    ~Recorder();

    /// Creates `directory` and the four files in it. Returns false and sets `error` on failure.
    [[nodiscard]] bool open(const std::filesystem::path& directory, std::string& error);
    /// Flushes and closes. Returns false if any write failed at any point.
    [[nodiscard]] bool close();

    void write(const OrderRecord& r) noexcept { put(kOrders, &r, sizeof(r), counts_.orders); }
    void write(const FillRecord& r) noexcept { put(kFills, &r, sizeof(r), counts_.fills); }
    void write(const BboRecord& r) noexcept { put(kBbo, &r, sizeof(r), counts_.bbo); }
    void write(const RiskRecord& r) noexcept { put(kRisk, &r, sizeof(r), counts_.risk); }

    [[nodiscard]] const RecordCounts& counts() const noexcept { return counts_; }
    [[nodiscard]] bool enabled() const noexcept { return files_[0] != nullptr; }

private:
    enum : std::size_t { kOrders, kFills, kBbo, kRisk, kFileCount };

    void put(std::size_t file, const void* record, std::size_t size,
             std::uint64_t& count) noexcept {
        ++count;
        if (files_[file] != nullptr && std::fwrite(record, size, 1, files_[file]) != 1) {
            failed_ = true;
        }
    }

    std::array<std::FILE*, kFileCount> files_{};
    RecordCounts counts_;
    bool failed_ = false;
};

} // namespace itchbt::record
