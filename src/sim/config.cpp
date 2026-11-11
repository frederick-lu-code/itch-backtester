#include "itchbt/sim/config.hpp"

#include <toml++/toml.hpp>

#include <cstdio>
#include <limits>
#include <string_view>

namespace itchbt::sim {

namespace {

/// Reads values out of one TOML table and remembers which keys were asked for, so that
/// anything left over can be reported as a mistake.
///
/// Values are read with value_exact<T>(), not value<T>(): the latter converts between
/// booleans, integers and floats, so `build_all_books = 1` or `max_position = true` would be
/// accepted silently.
class Section {
public:
    Section(const toml::table* table, std::string name, std::string& error)
        : table_(table), name_(std::move(name)), error_(error) {}

    template <typename T>
    void integer(std::string_view key, T& out, std::int64_t scale = 1) {
        seen_.emplace_back(key);
        if (table_ == nullptr || !table_->contains(key)) {
            return;
        }
        const auto value = (*table_)[key].value_exact<std::int64_t>();
        if (!value || *value < 0) {
            fail(key, "must be a non-negative integer");
            return;
        }
        // Compared as unsigned: T may be as wide as 64 bits.
        const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<T>::max()) /
                                    static_cast<std::uint64_t>(scale);
        if (static_cast<std::uint64_t>(*value) > limit) {
            fail(key, "is too large");
            return;
        }
        out = static_cast<T>(*value * scale);
    }

    void boolean(std::string_view key, bool& out) {
        seen_.emplace_back(key);
        if (table_ == nullptr || !table_->contains(key)) {
            return;
        }
        const auto value = (*table_)[key].value_exact<bool>();
        if (!value) {
            fail(key, "must be true or false");
            return;
        }
        out = *value;
    }

    void string(std::string_view key, std::string& out) {
        seen_.emplace_back(key);
        if (table_ == nullptr || !table_->contains(key)) {
            return;
        }
        const auto value = (*table_)[key].value_exact<std::string>();
        if (!value) {
            fail(key, "must be a string");
            return;
        }
        out = *value;
    }

    void strings(std::string_view key, std::vector<std::string>& out) {
        seen_.emplace_back(key);
        if (table_ == nullptr || !table_->contains(key)) {
            return;
        }
        const toml::array* const array = (*table_)[key].as_array();
        if (array == nullptr) {
            fail(key, "must be an array of strings");
            return;
        }
        out.clear();
        for (const toml::node& node : *array) {
            const auto value = node.value_exact<std::string>();
            if (!value) {
                fail(key, "must be an array of strings");
                return;
            }
            out.push_back(*value);
        }
    }

    /// A nested table. The key counts as known.
    [[nodiscard]] const toml::table* table(std::string_view key) {
        seen_.emplace_back(key);
        if (table_ == nullptr || !table_->contains(key)) {
            return nullptr;
        }
        const toml::table* const nested = (*table_)[key].as_table();
        if (nested == nullptr) {
            fail(key, "must be a table");
        }
        return nested;
    }

    /// Reports any key in the table that no accessor asked for.
    void finish() {
        if (table_ == nullptr) {
            return;
        }
        for (const auto& [key, node] : *table_) {
            bool known = false;
            for (const std::string& seen : seen_) {
                known = known || seen == key.str();
            }
            if (!known) {
                fail(key.str(), "is not a recognised setting");
            }
        }
    }

private:
    void fail(std::string_view key, const char* what) {
        if (error_.empty()) {
            error_ = (name_.empty() ? std::string() : name_ + ".") + std::string(key) + " " + what;
        }
    }

    const toml::table* table_;
    std::string name_;
    std::string& error_;
    std::vector<std::string> seen_;
};

void append(std::string& json, const char* key, std::uint64_t value, bool last = false) {
    json += "\"";
    json += key;
    json += "\": " + std::to_string(value) + (last ? "" : ", ");
}

} // namespace

std::optional<Config> load_config(const std::filesystem::path& path, std::string& error) {
    toml::table root_table;
    try {
        root_table = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        error = path.string() + ": " + std::string(e.description());
        return std::nullopt;
    }

    Config config;
    error.clear();
    Section root(&root_table, "", error);
    root.string("strategy", config.strategy);
    root.strings("symbols", config.symbols);
    root.boolean("build_all_books", config.build_all_books);
    root.integer("event_queue_capacity", config.event_queue_capacity);
    root.integer("expected_orders", config.expected_orders);
    root.integer("level_reserve", config.level_reserve);
    root.integer("recorder_ring_capacity", config.recorder_ring_capacity);

    Section latency(root.table("latency"), "latency", error);
    latency.integer("order_ns", config.latency.order_ns);
    latency.integer("cancel_ns", config.latency.cancel_ns);
    latency.integer("ack_ns", config.latency.ack_ns);
    latency.finish();

    Section exchange(root.table("exchange"), "exchange", error);
    exchange.boolean("extended_hours", config.exchange.extended_hours);
    exchange.integer("max_orders_per_symbol", config.exchange.max_orders_per_symbol);
    exchange.integer("max_open_orders_per_symbol", config.exchange.max_open_orders_per_symbol);
    exchange.finish();

    // Money limits are written in whole dollars and held in price units.
    Section risk(root.table("risk"), "risk", error);
    risk.integer("max_order_qty", config.risk.max_order_qty);
    risk.integer("max_order_notional_usd", config.risk.max_order_notional, kPriceUnitsPerDollar);
    risk.integer("price_collar_bps", config.risk.price_collar_bps);
    risk.integer("max_position", config.risk.max_position);
    risk.integer("max_gross_notional_usd", config.risk.max_gross_notional, kPriceUnitsPerDollar);
    risk.integer("max_open_orders", config.risk.max_open_orders);
    risk.integer("rate_limit_per_second", config.risk.rate_limit_per_second);
    risk.integer("rate_limit_burst", config.risk.rate_limit_burst);
    risk.integer("loss_limit_usd", config.risk.loss_limit, kPriceUnitsPerDollar);
    risk.integer("reject_window_ns", config.risk.reject_window_ns);
    risk.integer("reject_threshold", config.risk.reject_threshold);
    risk.finish();

    Section maker(root.table("passive_maker"), "passive_maker", error);
    maker.integer("lot", config.passive_maker.lot);
    maker.integer("max_inventory", config.passive_maker.max_inventory);
    maker.integer("skew_per_lot", config.passive_maker.skew_per_lot);
    maker.finish();

    Section taker(root.table("imbalance_taker"), "imbalance_taker", error);
    taker.integer("lot", config.imbalance_taker.lot);
    taker.integer("max_inventory", config.imbalance_taker.max_inventory);
    taker.integer("threshold_pct", config.imbalance_taker.threshold_pct);
    taker.integer("cooldown_ns", config.imbalance_taker.cooldown_ns);
    taker.finish();
    root.finish();

    if (error.empty()) {
        if (config.strategy != "noop" && config.strategy != "passive_maker" &&
            config.strategy != "imbalance_taker") {
            error = "strategy must be noop, passive_maker or imbalance_taker";
        } else if (config.event_queue_capacity < 16) {
            error = "event_queue_capacity must be at least 16";
        } else if (config.passive_maker.lot == 0 || config.imbalance_taker.lot == 0) {
            error = "lot must be at least 1";
        } else if (config.imbalance_taker.threshold_pct < 50 ||
                   config.imbalance_taker.threshold_pct > 100) {
            error = "imbalance_taker.threshold_pct must be between 50 and 100";
        } else if (config.exchange.max_orders_per_symbol == 0 ||
                   config.exchange.max_open_orders_per_symbol == 0) {
            error = "exchange order limits must be at least 1";
        }
    }
    if (!error.empty()) {
        error = path.string() + ": " + error;
        return std::nullopt;
    }
    // A rate limit with no burst size would reject everything; default the burst to one
    // second's worth.
    if (config.risk.rate_limit_per_second != 0 && config.risk.rate_limit_burst == 0) {
        config.risk.rate_limit_burst = config.risk.rate_limit_per_second;
    }
    return config;
}

std::string to_json(const Config& config) {
    std::string json = "{\"strategy\": \"" + config.strategy + "\", \"symbols\": [";
    for (std::size_t i = 0; i < config.symbols.size(); ++i) {
        json += (i == 0 ? "\"" : ", \"") + config.symbols[i] + "\"";
    }
    json += "], \"build_all_books\": ";
    json += config.build_all_books ? "true, " : "false, ";
    append(json, "event_queue_capacity", config.event_queue_capacity);
    append(json, "expected_orders", config.expected_orders);
    append(json, "level_reserve", config.level_reserve);
    append(json, "recorder_ring_capacity", config.recorder_ring_capacity);

    json += "\"latency\": {";
    append(json, "order_ns", config.latency.order_ns);
    append(json, "cancel_ns", config.latency.cancel_ns);
    append(json, "ack_ns", config.latency.ack_ns, true);

    json += "}, \"exchange\": {\"extended_hours\": ";
    json += config.exchange.extended_hours ? "true, " : "false, ";
    append(json, "max_orders_per_symbol", config.exchange.max_orders_per_symbol);
    append(json, "max_open_orders_per_symbol", config.exchange.max_open_orders_per_symbol, true);

    // Limits are reported in price units (1/10000 USD), exactly as the engine holds them.
    const auto u = [](std::int64_t v) { return static_cast<std::uint64_t>(v); };
    json += "}, \"risk\": {";
    append(json, "max_order_qty", config.risk.max_order_qty);
    append(json, "max_order_notional", u(config.risk.max_order_notional));
    append(json, "price_collar_bps", config.risk.price_collar_bps);
    append(json, "max_position", u(config.risk.max_position));
    append(json, "max_gross_notional", u(config.risk.max_gross_notional));
    append(json, "max_open_orders", config.risk.max_open_orders);
    append(json, "rate_limit_per_second", config.risk.rate_limit_per_second);
    append(json, "rate_limit_burst", config.risk.rate_limit_burst);
    append(json, "loss_limit", u(config.risk.loss_limit));
    append(json, "reject_window_ns", config.risk.reject_window_ns);
    append(json, "reject_threshold", config.risk.reject_threshold, true);

    json += "}, \"passive_maker\": {";
    append(json, "lot", config.passive_maker.lot);
    append(json, "max_inventory", u(config.passive_maker.max_inventory));
    append(json, "skew_per_lot", config.passive_maker.skew_per_lot, true);

    json += "}, \"imbalance_taker\": {";
    append(json, "lot", config.imbalance_taker.lot);
    append(json, "max_inventory", u(config.imbalance_taker.max_inventory));
    append(json, "threshold_pct", config.imbalance_taker.threshold_pct);
    append(json, "cooldown_ns", config.imbalance_taker.cooldown_ns, true);
    json += "}}";
    return json;
}

} // namespace itchbt::sim
