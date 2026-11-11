#pragma once

// Run configuration. Loaded from TOML at start-up (src/sim/config.cpp); plain data afterwards.

#include "itchbt/util/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace itchbt::sim {

/// One US dollar in price units (1/10000 USD).
inline constexpr std::int64_t kPriceUnitsPerDollar = 10'000;

struct LatencyConfig {
    Timestamp order_ns = 0;  // strategy -> exchange, new orders
    Timestamp cancel_ns = 0; // strategy -> exchange, cancels
    Timestamp ack_ns = 0;    // exchange -> strategy, every order update
};

struct ExchangeConfig {
    /// Allow orders and fills outside regular trading hours (while the symbol is trading).
    bool extended_hours = false;
    /// Orders a strategy may create per symbol over the whole run.
    std::uint32_t max_orders_per_symbol = 100'000;
    /// Orders that may rest on the simulated exchange per symbol at once.
    std::uint32_t max_open_orders_per_symbol = 64;
};

/// Limits in price units (1/10000 USD) and shares. The defaults disable every check.
struct RiskConfig {
    Qty max_order_qty = std::numeric_limits<Qty>::max();
    std::int64_t max_order_notional = std::numeric_limits<std::int64_t>::max();
    /// Reject if the limit price is more than this many basis points from the mid. 0 = off.
    std::uint32_t price_collar_bps = 0;
    std::int64_t max_position = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_gross_notional = std::numeric_limits<std::int64_t>::max();
    std::uint32_t max_open_orders = std::numeric_limits<std::uint32_t>::max();
    /// Token bucket on simulation time. 0 = off.
    std::uint32_t rate_limit_per_second = 0;
    std::uint32_t rate_limit_burst = 0;
    /// Kill switch: trip when total PnL falls below minus this. 0 = off.
    std::int64_t loss_limit = 0;
    /// Kill switch: trip on more than `reject_threshold` rejects within `reject_window_ns`.
    /// A threshold of 0 = off.
    Timestamp reject_window_ns = 1'000'000'000;
    std::uint32_t reject_threshold = 0;
};

struct PassiveMakerConfig {
    Qty lot = 100;
    std::int64_t max_inventory = 500;
    /// Price units to back the quote off per lot of inventory held on that side.
    Price skew_per_lot = 100;
};

struct ImbalanceTakerConfig {
    Qty lot = 100;
    std::int64_t max_inventory = 500;
    /// Fire when one side holds at least this percentage of top-of-book size.
    std::uint32_t threshold_pct = 80;
    Timestamp cooldown_ns = 1'000'000'000;
};

struct Config {
    std::string strategy = "noop";
    /// Symbols the strategy trades and receives callbacks for.
    std::vector<std::string> symbols;
    /// Build books for every symbol in the file, not just the ones above. Costs memory and
    /// time; has no effect on results.
    bool build_all_books = true;
    std::size_t event_queue_capacity = std::size_t{1} << 16;
    /// Expected peak live orders, to size the order store up front.
    std::size_t expected_orders = std::size_t{1} << 22;
    /// Records the writer thread may fall behind by before the engine blocks (M5).
    std::size_t recorder_ring_capacity = std::size_t{1} << 16;
    LatencyConfig latency;
    ExchangeConfig exchange;
    RiskConfig risk;
    PassiveMakerConfig passive_maker;
    ImbalanceTakerConfig imbalance_taker;
};

/// Loads a TOML config file. On failure returns nullopt and describes the problem in `error`.
/// Unknown keys are errors, so a typo cannot silently leave a limit at its default.
[[nodiscard]] std::optional<Config> load_config(const std::filesystem::path& path,
                                                std::string& error);

/// The config as a JSON object, for run.json.
[[nodiscard]] std::string to_json(const Config& config);

} // namespace itchbt::sim
