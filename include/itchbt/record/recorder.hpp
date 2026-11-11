#pragma once

// Writes a run directory: orders.bin, fills.bin, bbo.bin, risk.bin.
//
// Two modes produce byte-identical files:
//
//   Async (default): the engine copies each record into a bounded single-producer
//     single-consumer ring and a writer thread does the file I/O. The engine's hot path makes
//     no system call. If the ring is full the engine waits; it never drops a record. How
//     often and for how long it waited is counted.
//   Sync: each record is appended to a stdio buffer on the engine thread. Kept as the
//     baseline the async mode is measured against.

#include "itchbt/record/records.hpp"
#include "itchbt/util/spsc_ring.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace itchbt::record {

enum class RecorderMode : std::uint8_t { Sync, Async };

struct RecordCounts {
    std::uint64_t orders = 0;
    std::uint64_t fills = 0;
    std::uint64_t bbo = 0;
    std::uint64_t risk = 0;
};

struct RecorderStats {
    /// Records that found the ring full and had to wait.
    std::uint64_t blocked_records = 0;
    /// Total wall time the engine spent waiting for the writer, in nanoseconds.
    std::uint64_t blocked_ns = 0;
};

class Recorder {
public:
    /// A recorder that discards everything, for benchmarks and tests that need no output.
    Recorder() = default;
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    ~Recorder();

    /// Creates `directory` and the four files in it, and in Async mode starts the writer
    /// thread. Returns false and sets `error` on failure.
    [[nodiscard]] bool open(const std::filesystem::path& directory, std::string& error,
                            RecorderMode mode = RecorderMode::Async,
                            std::size_t ring_capacity = std::size_t{1} << 16);
    /// Drains the ring, stops the writer, flushes and closes. Returns false if any write
    /// failed at any point.
    [[nodiscard]] bool close();

    void write(const OrderRecord& r) noexcept { put(kOrders, &r, sizeof(r), counts_.orders); }
    void write(const FillRecord& r) noexcept { put(kFills, &r, sizeof(r), counts_.fills); }
    void write(const BboRecord& r) noexcept { put(kBbo, &r, sizeof(r), counts_.bbo); }
    void write(const RiskRecord& r) noexcept { put(kRisk, &r, sizeof(r), counts_.risk); }

    [[nodiscard]] const RecordCounts& counts() const noexcept { return counts_; }
    [[nodiscard]] const RecorderStats& stats() const noexcept { return stats_; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] RecorderMode mode() const noexcept { return mode_; }

private:
    enum : std::size_t { kOrders, kFills, kBbo, kRisk, kFileCount };
    static constexpr std::size_t kMaxRecordSize = 48;
    static_assert(sizeof(OrderRecord) <= kMaxRecordSize && sizeof(FillRecord) <= kMaxRecordSize &&
                  sizeof(BboRecord) <= kMaxRecordSize && sizeof(RiskRecord) <= kMaxRecordSize);

    /// One record on its way to the writer: which file, how long, and the bytes.
    struct Slot {
        std::uint32_t file = 0;
        std::uint32_t size = 0;
        std::array<std::byte, kMaxRecordSize> bytes{};
    };

    void put(std::size_t file, const void* record, std::size_t size,
             std::uint64_t& count) noexcept {
        ++count;
        if (!enabled_) {
            return;
        }
        if (mode_ == RecorderMode::Async) {
            Slot slot;
            slot.file = static_cast<std::uint32_t>(file);
            slot.size = static_cast<std::uint32_t>(size);
            std::memcpy(slot.bytes.data(), record, size);
            if (!ring_->try_push(slot)) [[unlikely]] {
                wait_and_push(slot);
            }
        } else if (std::fwrite(record, size, 1, files_[file]) != 1) {
            failed_.store(true, std::memory_order_relaxed);
        }
    }

    /// The ring is full: wait for the writer to make room. Out of line, off the hot path.
    void wait_and_push(const Slot& slot) noexcept;
    void writer_loop() noexcept;

    std::array<std::FILE*, kFileCount> files_{};
    std::unique_ptr<SpscRing<Slot>> ring_;
    std::thread writer_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    RecordCounts counts_;
    RecorderStats stats_;
    RecorderMode mode_ = RecorderMode::Sync;
    bool enabled_ = false;
};

} // namespace itchbt::record
