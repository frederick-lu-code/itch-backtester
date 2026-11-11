#pragma once

// Baseline containers: the obviously correct implementation built from the standard library.
// Everything faster is measured against these and must agree with them message for message.

#include "itchbt/book/types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace itchbt::book {

/// Order reference to order, on std::unordered_map.
class StdOrderMap {
public:
    explicit StdOrderMap(std::size_t expected_orders = 0) { map_.reserve(expected_orders); }

    [[nodiscard]] Order* find(OrderRef ref) {
        const auto it = map_.find(ref);
        return it == map_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const Order* find(OrderRef ref) const {
        const auto it = map_.find(ref);
        return it == map_.end() ? nullptr : &it->second;
    }
    /// Returns false, leaving the map unchanged, if `ref` is already present.
    bool insert(OrderRef ref, const Order& order) { return map_.emplace(ref, order).second; }
    void erase(OrderRef ref) { map_.erase(ref); }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

    template <typename F>
    void for_each(F&& f) const {
        for (const auto& [ref, order] : map_) {
            f(ref, order);
        }
    }

private:
    std::unordered_map<OrderRef, Order> map_;
};

/// One side's price levels on std::map, keyed by priority key (largest is best).
class MapLevels {
public:
    [[nodiscard]] Level* find(std::uint32_t key) {
        const auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const Level* find(std::uint32_t key) const {
        const auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] Level& find_or_insert(std::uint32_t key) { return map_[key]; }
    void erase(std::uint32_t key) { map_.erase(key); }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

    /// The best level's key and state, if any.
    [[nodiscard]] std::optional<std::pair<std::uint32_t, Level>> best() const {
        if (map_.empty()) {
            return std::nullopt;
        }
        return *map_.rbegin();
    }

    /// Visits levels from best to worst until `f` returns false.
    template <typename F>
    void for_each_best_first(F&& f) const {
        for (const auto& [key, level] : std::views::reverse(map_)) {
            if (!f(key, level)) {
                return;
            }
        }
    }

private:
    std::map<std::uint32_t, Level> map_;
};

} // namespace itchbt::book
