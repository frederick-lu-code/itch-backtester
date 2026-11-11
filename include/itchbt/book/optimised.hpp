#pragma once

// Optimised containers for the order book. Each replaces one baseline container and must
// agree with it on every message (tests/property/book_property_test.cpp); each was kept only
// because it measured faster (BENCHMARKS.md, optimisation log).

#include "itchbt/book/types.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace itchbt::book {

/// Order reference to order, as an open-addressing hash table with linear probing.
///
/// Why it beats std::unordered_map here: one flat array instead of a node per order (no
/// allocation per add, no pointer chase per lookup), 32-byte slots so two fit a cache line,
/// and erase by backward shift, so there are no tombstones to slow later probes however long
/// the day runs. Sized up front from the expected number of live orders; it grows if that
/// guess was too small, and counts how often so a benchmark can prove it never did.
class FlatOrderMap {
public:
    explicit FlatOrderMap(std::size_t expected_orders = 0) {
        // Keep the load factor at or below 1/2 for the expected size: linear probing degrades
        // sharply past about 0.7.
        std::size_t capacity = kMinCapacity;
        while (capacity / 2 < expected_orders) {
            capacity *= 2;
        }
        allocate(capacity);
    }

    [[nodiscard]] Order* find(OrderRef ref) noexcept {
        const std::size_t i = locate(ref);
        return slots_[i].key == stored(ref) && stored(ref) != kEmpty ? &slots_[i].order : nullptr;
    }
    [[nodiscard]] const Order* find(OrderRef ref) const noexcept {
        const std::size_t i = locate(ref);
        return slots_[i].key == stored(ref) && stored(ref) != kEmpty ? &slots_[i].order : nullptr;
    }

    /// Returns false, leaving the map unchanged, if `ref` is already present (or is the one
    /// reference value the table cannot represent, 2^64 - 1).
    bool insert(OrderRef ref, const Order& order) {
        if (stored(ref) == kEmpty) {
            return false;
        }
        if ((size_ + 1) * 10 > capacity() * 7) {
            grow();
        }
        const std::size_t i = locate(ref);
        if (slots_[i].key != kEmpty) {
            return false;
        }
        slots_[i] = Slot{stored(ref), order};
        ++size_;
        return true;
    }

    void erase(OrderRef ref) noexcept {
        std::size_t hole = locate(ref);
        // The second test matters: reference 2^64 - 1 is stored as 0, the empty marker, so
        // without it an empty slot would "match" and the size would be decremented.
        if (slots_[hole].key != stored(ref) || stored(ref) == kEmpty) {
            return;
        }
        // Backward-shift deletion: walk the run that follows the hole and move back any entry
        // whose home position does not lie strictly between the hole and itself. The table is
        // left exactly as if the erased key had never been inserted.
        std::size_t next = (hole + 1) & mask_;
        while (slots_[next].key != kEmpty) {
            const std::size_t home = home_of(slots_[next].key);
            if (((next - home) & mask_) >= ((next - hole) & mask_)) {
                slots_[hole] = slots_[next];
                hole = next;
            }
            next = (next + 1) & mask_;
        }
        slots_[hole].key = kEmpty;
        --size_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    /// Times the table had to grow after construction. Zero when it was sized correctly.
    [[nodiscard]] std::uint64_t rehashes() const noexcept { return rehashes_; }

    /// Hints the CPU to load the slot `ref` hashes to. For callers that know the next
    /// reference before they need it.
    void prefetch(OrderRef ref) const noexcept {
        __builtin_prefetch(&slots_[home_of(stored(ref))]);
    }

    template <typename F>
    void for_each(F&& f) const {
        for (const Slot& slot : slots_) {
            if (slot.key != kEmpty) {
                f(slot.key - 1, slot.order);
            }
        }
    }

private:
    struct Slot {
        std::uint64_t key = 0; // reference + 1, so that zero can mean "empty"
        Order order;
    };
    static_assert(sizeof(Slot) == 32);

    static constexpr std::uint64_t kEmpty = 0;
    static constexpr std::size_t kMinCapacity = 16;

    [[nodiscard]] static constexpr std::uint64_t stored(OrderRef ref) noexcept { return ref + 1; }

    /// Fibonacci hashing: multiply by 2^64 / phi and keep the top bits. ITCH references are
    /// close to sequential, and this spreads neighbours across the table instead of leaving
    /// them in one long run.
    [[nodiscard]] std::size_t home_of(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    /// The slot holding `ref`, or the empty slot where it would go.
    [[nodiscard]] std::size_t locate(OrderRef ref) const noexcept {
        const std::uint64_t key = stored(ref);
        std::size_t i = home_of(key);
        while (slots_[i].key != kEmpty && slots_[i].key != key) {
            i = (i + 1) & mask_;
        }
        return i;
    }

    void allocate(std::size_t capacity) {
        slots_.assign(capacity, Slot{});
        mask_ = capacity - 1;
        shift_ = static_cast<unsigned>(64 - std::countr_zero(capacity));
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        allocate(old.size() * 2);
        for (const Slot& slot : old) {
            if (slot.key != kEmpty) {
                std::size_t i = home_of(slot.key);
                while (slots_[i].key != kEmpty) {
                    i = (i + 1) & mask_;
                }
                slots_[i] = slot;
            }
        }
        ++rehashes_;
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
    std::uint64_t rehashes_ = 0;
    unsigned shift_ = 0;
};

/// How VectorLevels finds a level.
enum class LevelSearch : std::uint8_t {
    Binary,         // std::lower_bound over the whole side
    LinearFromBest, // walk back from the best level
};

/// One side's price levels in a contiguous vector sorted by priority key, best level last.
///
/// Why it beats std::map here: activity concentrates at the top of the book, so the levels
/// that matter sit together at the end of one array, and adding or removing a level near the
/// best moves only the few entries beyond it. A deep insert costs a memmove of 16-byte
/// entries, which is still cheap next to a tree's pointer chasing.
template <LevelSearch Search>
class VectorLevelsT {
public:
    [[nodiscard]] Level* find(std::uint32_t key) noexcept {
        const std::size_t i = lower_bound(key);
        return i < levels_.size() && levels_[i].key == key ? &levels_[i].level : nullptr;
    }
    [[nodiscard]] const Level* find(std::uint32_t key) const noexcept {
        const std::size_t i = lower_bound(key);
        return i < levels_.size() && levels_[i].key == key ? &levels_[i].level : nullptr;
    }

    [[nodiscard]] Level& find_or_insert(std::uint32_t key) {
        const std::size_t i = lower_bound(key);
        if (i == levels_.size() || levels_[i].key != key) {
            levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), Entry{key, Level{}});
        }
        return levels_[i].level;
    }

    void erase(std::uint32_t key) noexcept {
        const std::size_t i = lower_bound(key);
        if (i < levels_.size() && levels_[i].key == key) {
            levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }
    void reserve(std::size_t levels) { levels_.reserve(levels); }

    [[nodiscard]] std::optional<std::pair<std::uint32_t, Level>> best() const noexcept {
        if (levels_.empty()) {
            return std::nullopt;
        }
        return std::pair{levels_.back().key, levels_.back().level};
    }

    template <typename F>
    void for_each_best_first(F&& f) const {
        for (std::size_t i = levels_.size(); i-- > 0;) {
            if (!f(levels_[i].key, levels_[i].level)) {
                return;
            }
        }
    }

private:
    struct Entry {
        std::uint32_t key = 0;
        Level level;
    };

    /// Index of the first entry whose key is not less than `key`.
    [[nodiscard]] std::size_t lower_bound(std::uint32_t key) const noexcept {
        if constexpr (Search == LevelSearch::Binary) {
            const auto it =
                std::lower_bound(levels_.begin(), levels_.end(), key,
                                 [](const Entry& entry, std::uint32_t k) { return entry.key < k; });
            return static_cast<std::size_t>(it - levels_.begin());
        } else {
            std::size_t i = levels_.size();
            while (i > 0 && levels_[i - 1].key >= key) {
                --i;
            }
            return i;
        }
    }

    std::vector<Entry> levels_;
};

using VectorLevels = VectorLevelsT<LevelSearch::LinearFromBest>;
using BinaryVectorLevels = VectorLevelsT<LevelSearch::Binary>;

} // namespace itchbt::book
