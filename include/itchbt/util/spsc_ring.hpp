#pragma once

// A bounded single-producer single-consumer ring buffer.
//
// Exactly one thread may call try_push and exactly one (other) thread may call try_pop. Under
// that contract it is lock-free and wait-free: each operation is a few loads and one store.
//
// Each side keeps a cached copy of the other side's index and re-reads the shared atomic only
// when the cache says the ring is full (producer) or empty (consumer). In steady state that
// keeps the two threads off each other's cache lines.

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace itchbt {

/// Bytes kept between data written by different threads. 128 covers Apple Silicon, whose
/// cache lines are twice the usual 64 bytes.
inline constexpr std::size_t kCacheLineSize = 128;

template <typename T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "slots are copied without constructors");

public:
    /// Capacity is rounded up to a power of two so that indices wrap with a mask.
    explicit SpscRing(std::size_t capacity)
        : capacity_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)), mask_(capacity_ - 1),
          slots_(capacity_) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    /// Producer only. Returns false, changing nothing, if the ring is full.
    [[nodiscard]] bool try_push(const T& value) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == capacity_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == capacity_) {
                return false;
            }
        }
        slots_[tail & mask_] = value;
        // Release: the consumer must see the slot's contents before it sees the new tail.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Consumer only. Returns false, leaving `out` untouched, if the ring is empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) {
                return false;
            }
        }
        out = slots_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    /// Items currently queued. Exact only when both threads are idle.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return static_cast<std::size_t>(tail_.load(std::memory_order_acquire) -
                                        head_.load(std::memory_order_acquire));
    }

private:
    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<T> slots_; // sized once in the constructor, never resized
    // Consumer's line: its index, and its cache of the producer's.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> head_{0};
    std::uint64_t tail_cache_ = 0;
    // Producer's line.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> tail_{0};
    std::uint64_t head_cache_ = 0;
    // Keeps whatever follows this object off the producer's line.
    alignas(kCacheLineSize) char padding_ = 0;
};

} // namespace itchbt
