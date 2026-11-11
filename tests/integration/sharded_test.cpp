// Sharded replay must reproduce the single-threaded run exactly, symbol by symbol.

#include "imbalance_taker.hpp"
#include "noop.hpp"
#include "passive_maker.hpp"
#include "support/random_stream.hpp"
#include "support/temp_file.hpp"

#include "itchbt/record/recorder.hpp"
#include "itchbt/sim/sharded.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace itchbt::sim {
namespace {

using test::ItchWriter;

const std::vector<std::string> kSymbols = {"AAA", "BBB", "CCC", "DDD", "EEE"};
constexpr Locate kFirstLocate = 10;

ItchWriter market(std::uint64_t seed, std::size_t events = 60'000) {
    test::RandomStreamOptions options;
    options.seed = seed;
    options.events = events;
    options.symbols = kSymbols;
    options.symbols.emplace_back("FFF"); // in the file, traded by nobody
    options.first_locate = kFirstLocate;
    return test::random_stream(options);
}

/// A config with only per-symbol risk limits, which is what sharding supports.
Config shardable_config(const std::string& strategy) {
    Config c;
    c.strategy = strategy;
    c.symbols = kSymbols;
    c.expected_orders = 1 << 15;
    c.exchange.max_orders_per_symbol = 100'000;
    c.latency.order_ns = 2'500;
    c.latency.cancel_ns = 2'500;
    c.latency.ack_ns = 1'500;
    c.risk.max_position = 500;
    c.risk.max_open_orders = 4;
    c.risk.max_order_qty = 1'000;
    c.passive_maker.max_inventory = 300;
    c.imbalance_taker.cooldown_ns = 20'000;
    c.imbalance_taker.threshold_pct = 70;
    return c;
}

template <typename Record>
std::vector<Record> read(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>()};
    std::vector<Record> out;
    for (std::size_t pos = sizeof(record::FileHeader); pos + sizeof(Record) <= bytes.size();
         pos += sizeof(Record)) {
        Record r;
        std::memcpy(&r, bytes.data() + pos, sizeof(r));
        out.push_back(r);
    }
    return out;
}

/// One symbol's records from a set of files, concatenated in file order.
template <typename Record>
std::vector<Record> for_symbol(const std::vector<std::filesystem::path>& files, Locate locate) {
    std::vector<Record> out;
    for (const auto& file : files) {
        for (const Record& r : read<Record>(file)) {
            if (r.locate == locate) {
                out.push_back(r);
            }
        }
    }
    return out;
}

template <typename Record>
bool same_bytes(const std::vector<Record>& a, const std::vector<Record>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Record)) == 0);
}

template <typename Make>
RunStats run_single(const ItchWriter& stream, const Config& config,
                    const std::filesystem::path& dir, Make&& make) {
    record::Recorder recorder;
    std::string error;
    EXPECT_TRUE(recorder.open(dir, error)) << error;
    auto strategy = make(config);
    Backtester<decltype(strategy)> engine(config, strategy, recorder);
    EXPECT_TRUE(engine.prepare(stream.bytes(), error)) << error;
    EXPECT_TRUE(engine.run(stream.bytes()).ok());
    EXPECT_TRUE(recorder.close());
    return engine.stats();
}

template <typename Make>
void expect_shards_match_single(const std::string& strategy, std::size_t shards, std::uint64_t seed,
                                bool build_all_books, Make&& make) {
    SCOPED_TRACE(strategy + ", " + std::to_string(shards) + " shards, seed " +
                 std::to_string(seed));
    const ItchWriter stream = market(seed);
    Config config = shardable_config(strategy);
    config.build_all_books = build_all_books;
    const std::filesystem::path single_dir = test::TempFile::unique_path();
    const std::filesystem::path shard_dir = test::TempFile::unique_path();

    const RunStats single = run_single(stream, config, single_dir, make);
    const ShardedOutcome sharded = run_sharded(stream.bytes(), config, shards, shard_dir, true,
                                               record::RecorderMode::Async, make);
    ASSERT_TRUE(sharded.ok) << sharded.error;
    ASSERT_EQ(sharded.shards.size(), shards);
    EXPECT_EQ(sharded.parse.messages, single.messages);

    // The comparison is only worth something if the strategy actually traded.
    EXPECT_GT(single.orders_submitted, 100U);
    EXPECT_GT(single.fills, 30U);

    RunStats total;
    for (const ShardOutcome& shard : sharded.shards) {
        total.orders_submitted += shard.stats.orders_submitted;
        total.fills += shard.stats.fills;
        total.shares_filled += shard.stats.shares_filled;
        total.cancels_requested += shard.stats.cancels_requested;
        total.book_updates += shard.stats.book_updates;
        total.trades += shard.stats.trades;
        EXPECT_EQ(shard.stats.events_dropped, 0U);
    }
    EXPECT_EQ(total.orders_submitted, single.orders_submitted);
    EXPECT_EQ(total.fills, single.fills);
    EXPECT_EQ(total.shares_filled, single.shares_filled);
    EXPECT_EQ(total.cancels_requested, single.cancels_requested);
    EXPECT_EQ(total.book_updates, single.book_updates);
    EXPECT_EQ(total.trades, single.trades);

    const auto files = [&](const char* name) {
        std::vector<std::filesystem::path> out;
        for (std::size_t i = 0; i < shards; ++i) {
            out.push_back(shard_dir / ("shard-" + std::to_string(i)) / name);
        }
        return out;
    };
    for (std::size_t i = 0; i < kSymbols.size(); ++i) {
        const auto locate = static_cast<Locate>(kFirstLocate + i);
        SCOPED_TRACE(kSymbols[i]);
        const auto single_orders =
            for_symbol<record::OrderRecord>({single_dir / "orders.bin"}, locate);
        EXPECT_GT(single_orders.size(), 20U);
        EXPECT_TRUE(same_bytes(single_orders,
                               for_symbol<record::OrderRecord>(files("orders.bin"), locate)));
        EXPECT_TRUE(same_bytes(for_symbol<record::FillRecord>({single_dir / "fills.bin"}, locate),
                               for_symbol<record::FillRecord>(files("fills.bin"), locate)));
        EXPECT_TRUE(same_bytes(for_symbol<record::BboRecord>({single_dir / "bbo.bin"}, locate),
                               for_symbol<record::BboRecord>(files("bbo.bin"), locate)));
        EXPECT_TRUE(same_bytes(for_symbol<record::RiskRecord>({single_dir / "risk.bin"}, locate),
                               for_symbol<record::RiskRecord>(files("risk.bin"), locate)));
    }
    std::filesystem::remove_all(single_dir);
    std::filesystem::remove_all(shard_dir);
}

const auto kMaker = [](const Config& c) { return strategies::PassiveMaker(c.passive_maker); };
const auto kTaker = [](const Config& c) { return strategies::ImbalanceTaker(c.imbalance_taker); };

TEST(Sharded, PassiveMakerMatchesSingleThreadedPerSymbol) {
    for (const std::size_t shards : {std::size_t{2}, std::size_t{3}, std::size_t{5}}) {
        expect_shards_match_single("passive_maker", shards, 31, true, kMaker);
    }
}

TEST(Sharded, ImbalanceTakerMatchesSingleThreadedPerSymbol) {
    expect_shards_match_single("imbalance_taker", 2, 32, true, kTaker);
    expect_shards_match_single("imbalance_taker", 4, 33, false, kTaker);
}

TEST(Sharded, MoreShardsThanSymbolsStillWorks) {
    expect_shards_match_single("passive_maker", 8, 34, false, kMaker);
}

TEST(Sharded, EveryMessageReachesExactlyOneShardOrAll) {
    const ItchWriter stream = market(35, 20'000);
    Config config = shardable_config("noop");
    config.build_all_books = true;
    const auto make = [](const Config& /*c*/) { return strategies::NoOp(); };
    const ShardedOutcome out =
        run_sharded(stream.bytes(), config, 3, {}, false, record::RecorderMode::Async, make);
    ASSERT_TRUE(out.ok) << out.error;
    std::uint64_t handled = 0;
    for (const ShardOutcome& shard : out.shards) {
        handled += shard.messages;
    }
    // Three session-wide system events (open of messages, market open, market close... and the
    // final close) are delivered to every shard; everything else to exactly one.
    const std::uint64_t broadcasts = 4;
    EXPECT_EQ(handled, out.parse.messages + broadcasts * 2);
}

TEST(Sharded, RefusesLimitsThatSpanSymbols) {
    const ItchWriter stream = market(36, 1'000);
    const auto make = [](const Config& /*c*/) { return strategies::NoOp(); };
    const auto error_for = [&](Config config) {
        return run_sharded(stream.bytes(), config, 2, {}, false, record::RecorderMode::Async, make)
            .error;
    };
    Config loss = shardable_config("noop");
    loss.risk.loss_limit = 1;
    EXPECT_NE(error_for(loss).find("risk.loss_limit_usd"), std::string::npos);
    Config gross = shardable_config("noop");
    gross.risk.max_gross_notional = 1'000'000;
    EXPECT_NE(error_for(gross).find("risk.max_gross_notional_usd"), std::string::npos);
    Config rate = shardable_config("noop");
    rate.risk.rate_limit_per_second = 10;
    rate.risk.reject_threshold = 5;
    const std::string both = error_for(rate);
    EXPECT_NE(both.find("risk.rate_limit_per_second"), std::string::npos);
    EXPECT_NE(both.find("risk.reject_threshold"), std::string::npos);
    EXPECT_TRUE(sharding_conflicts(shardable_config("noop")).empty());
}

TEST(Sharded, ReportsBadInputsWithoutHanging) {
    const ItchWriter stream = market(37, 1'000);
    const auto make = [](const Config& /*c*/) { return strategies::NoOp(); };
    Config unknown = shardable_config("noop");
    unknown.symbols.emplace_back("NOPE");
    const ShardedOutcome a =
        run_sharded(stream.bytes(), unknown, 2, {}, false, record::RecorderMode::Async, make);
    EXPECT_FALSE(a.ok);
    EXPECT_NE(a.error.find("NOPE"), std::string::npos);

    const Config config = shardable_config("noop");
    EXPECT_FALSE(
        run_sharded(stream.bytes(), config, 1, {}, false, record::RecorderMode::Async, make).ok);
    EXPECT_FALSE(
        run_sharded(stream.bytes(), config, 1000, {}, false, record::RecorderMode::Async, make).ok);

    // A file that goes bad part-way: the workers must still be shut down cleanly.
    ItchWriter broken = market(38, 5'000);
    const std::vector<std::byte> junk(5, std::byte{'z'});
    broken.record(junk);
    const ShardedOutcome b =
        run_sharded(broken.bytes(), config, 3, {}, false, record::RecorderMode::Async, make);
    EXPECT_FALSE(b.ok);
    EXPECT_EQ(b.parse.error, itch::ParseError::UnknownType);
    EXPECT_NE(b.error.find("unknown message type"), std::string::npos);
}

} // namespace
} // namespace itchbt::sim
