#include "support/temp_file.hpp"

#include "itchbt/sim/config.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace itchbt::sim {
namespace {

std::optional<Config> load(std::string_view toml, std::string& error) {
    const test::TempFile file(std::as_bytes(std::span(toml.data(), toml.size())));
    return load_config(file.path(), error);
}

TEST(Config, EmptyFileGivesDefaultsWithEveryLimitOff) {
    std::string error;
    const auto c = load("", error);
    ASSERT_TRUE(c) << error;
    EXPECT_EQ(c->strategy, "noop");
    EXPECT_TRUE(c->symbols.empty());
    EXPECT_TRUE(c->build_all_books);
    EXPECT_EQ(c->latency.order_ns, 0U);
    EXPECT_EQ(c->risk.max_order_qty, std::numeric_limits<Qty>::max());
    EXPECT_EQ(c->risk.max_position, std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(c->risk.price_collar_bps, 0U);
    EXPECT_EQ(c->risk.loss_limit, 0);
    EXPECT_FALSE(c->exchange.extended_hours);
}

TEST(Config, ReadsEverySection) {
    std::string error;
    const auto c = load(R"(
strategy = "passive_maker"
symbols = ["AAPL", "MSFT"]
build_all_books = false
event_queue_capacity = 1024

[latency]
order_ns = 100
cancel_ns = 200
ack_ns = 300

[exchange]
extended_hours = true
max_orders_per_symbol = 50
max_open_orders_per_symbol = 5

[risk]
max_order_qty = 1000
max_order_notional_usd = 250000
price_collar_bps = 150
max_position = 700
max_gross_notional_usd = 1000000
max_open_orders = 3
rate_limit_per_second = 20
rate_limit_burst = 40
loss_limit_usd = 5000
reject_window_ns = 2000000000
reject_threshold = 9

[passive_maker]
lot = 200
max_inventory = 800
skew_per_lot = 50

[imbalance_taker]
lot = 300
max_inventory = 900
threshold_pct = 75
cooldown_ns = 123
)",
                        error);
    ASSERT_TRUE(c) << error;
    EXPECT_EQ(c->strategy, "passive_maker");
    EXPECT_EQ(c->symbols, (std::vector<std::string>{"AAPL", "MSFT"}));
    EXPECT_FALSE(c->build_all_books);
    EXPECT_EQ(c->event_queue_capacity, 1024U);
    EXPECT_EQ(c->latency.order_ns, 100U);
    EXPECT_EQ(c->latency.cancel_ns, 200U);
    EXPECT_EQ(c->latency.ack_ns, 300U);
    EXPECT_TRUE(c->exchange.extended_hours);
    EXPECT_EQ(c->exchange.max_orders_per_symbol, 50U);
    EXPECT_EQ(c->exchange.max_open_orders_per_symbol, 5U);
    EXPECT_EQ(c->risk.max_order_qty, 1000U);
    EXPECT_EQ(c->risk.max_order_notional, 2'500'000'000); // dollars become price units
    EXPECT_EQ(c->risk.price_collar_bps, 150U);
    EXPECT_EQ(c->risk.max_position, 700);
    EXPECT_EQ(c->risk.max_gross_notional, 10'000'000'000);
    EXPECT_EQ(c->risk.max_open_orders, 3U);
    EXPECT_EQ(c->risk.rate_limit_per_second, 20U);
    EXPECT_EQ(c->risk.rate_limit_burst, 40U);
    EXPECT_EQ(c->risk.loss_limit, 50'000'000);
    EXPECT_EQ(c->risk.reject_window_ns, 2'000'000'000U);
    EXPECT_EQ(c->risk.reject_threshold, 9U);
    EXPECT_EQ(c->passive_maker.lot, 200U);
    EXPECT_EQ(c->passive_maker.max_inventory, 800);
    EXPECT_EQ(c->passive_maker.skew_per_lot, 50U);
    EXPECT_EQ(c->imbalance_taker.lot, 300U);
    EXPECT_EQ(c->imbalance_taker.threshold_pct, 75U);
    EXPECT_EQ(c->imbalance_taker.cooldown_ns, 123U);
}

TEST(Config, UnknownKeysAreErrorsNotSilentDefaults) {
    std::string error;
    EXPECT_FALSE(load("[risk]\nmax_postion = 5\n", error)); // typo
    EXPECT_NE(error.find("risk.max_postion"), std::string::npos) << error;
    EXPECT_FALSE(load("stratgy = \"noop\"\n", error));
    EXPECT_NE(error.find("stratgy"), std::string::npos) << error;
    EXPECT_FALSE(load("[riks]\nmax_position = 5\n", error));
    EXPECT_NE(error.find("riks"), std::string::npos) << error;
}

TEST(Config, WrongTypesAndRangesAreRejected) {
    std::string error;
    EXPECT_FALSE(load("[risk]\nmax_position = \"lots\"\n", error));
    EXPECT_FALSE(load("[risk]\nmax_position = -5\n", error));
    EXPECT_FALSE(load("[risk]\nmax_order_qty = 99999999999\n", error)); // does not fit 32 bits
    EXPECT_NE(error.find("too large"), std::string::npos) << error;
    EXPECT_FALSE(load("symbols = \"AAPL\"\n", error));
    EXPECT_FALSE(load("symbols = [1, 2]\n", error));
    EXPECT_FALSE(load("build_all_books = 1\n", error));         // an integer is not a boolean
    EXPECT_FALSE(load("[risk]\nmax_position = true\n", error)); // nor the reverse
    EXPECT_FALSE(load("[risk]\nmax_position = 5.0\n", error));  // nor a float an integer
    EXPECT_FALSE(load("strategy = 5\n", error));
    EXPECT_FALSE(load("risk = 5\n", error));
    EXPECT_FALSE(load("strategy = \"martingale\"\n", error));
    EXPECT_FALSE(load("[imbalance_taker]\nthreshold_pct = 30\n", error));
    EXPECT_FALSE(load("[passive_maker]\nlot = 0\n", error));
    EXPECT_FALSE(load("event_queue_capacity = 2\n", error));
}

TEST(Config, MalformedTomlReportsTheFile) {
    std::string error;
    const test::TempFile file(std::as_bytes(std::span("strategy = ", 11)));
    EXPECT_FALSE(load_config(file.path(), error));
    EXPECT_NE(error.find(file.path().string()), std::string::npos) << error;
    EXPECT_FALSE(load_config("/nonexistent/itchbt.toml", error));
}

TEST(Config, RateLimitWithoutBurstDefaultsToOneSecondsWorth) {
    std::string error;
    const auto c = load("[risk]\nrate_limit_per_second = 25\n", error);
    ASSERT_TRUE(c) << error;
    EXPECT_EQ(c->risk.rate_limit_burst, 25U);
}

TEST(Config, ShippedConfigsLoad) {
    std::size_t loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(ITCHBT_CONFIG_DIR)) {
        if (entry.path().extension() == ".toml") {
            std::string error;
            EXPECT_TRUE(load_config(entry.path(), error)) << error;
            ++loaded;
        }
    }
    EXPECT_GE(loaded, 3U);
}

TEST(Config, JsonNamesTheStrategyAndSymbols) {
    Config c;
    c.strategy = "passive_maker";
    c.symbols = {"AAPL", "MSFT"};
    c.latency.order_ns = 77;
    const std::string json = to_json(c);
    EXPECT_NE(json.find("\"strategy\": \"passive_maker\""), std::string::npos);
    EXPECT_NE(json.find("\"symbols\": [\"AAPL\", \"MSFT\"]"), std::string::npos);
    EXPECT_NE(json.find("\"order_ns\": 77"), std::string::npos);
    EXPECT_EQ(json.front(), '{');
    EXPECT_EQ(json.back(), '}');
}

} // namespace
} // namespace itchbt::sim
