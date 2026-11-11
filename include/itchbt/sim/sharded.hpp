#pragma once

// Sharded replay: one engine per shard of symbols, each on its own thread, fed by a router.
//
//   file --> router thread --(SPSC queue of message pointers per shard)--> shard workers
//
// The router walks the file once, validates each record, reads its stock locate and pushes a
// pointer to the message onto the owning shard's queue. Session-wide messages (system events
// and anything else with locate 0) go to every shard. Each worker owns a complete engine:
// books, simulated exchange, risk gate, strategy instance and recorder.
//
// Why results match the single-threaded run exactly, per symbol: every message for a symbol
// goes to one shard, in file order, so that symbol's book, queue positions, fills and order
// ids evolve identically. What a shard does not see (other symbols' messages) could only have
// affected the *wall-clock moment* at which a queued event was released, never the simulation
// time it is released at or the state of this symbol's book at that time.
//
// What cannot be sharded: anything that couples symbols. A strategy that reads several symbols
// at once, and risk limits that span symbols (gross notional, total-PnL loss limit, the rate
// limit and the reject-rate switch), would behave differently per shard, so run_sharded
// refuses a config that enables them.

#include "itchbt/itch/parser.hpp"
#include "itchbt/record/recorder.hpp"
#include "itchbt/sim/config.hpp"
#include "itchbt/sim/engine.hpp"
#include "itchbt/util/spsc_ring.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace itchbt::sim {

/// What one shard reports when it finishes.
struct ShardOutcome {
    bool ok = false;
    std::string error;
    std::vector<std::string> symbols;
    std::vector<Locate> locates;
    std::vector<risk::SymbolRisk> symbol_risk;
    RunStats stats;
    record::RecordCounts records;
    record::RecorderStats recorder;
    risk::KillReason kill_reason = risk::KillReason::None;
    std::int64_t total_pnl2 = 0;
    std::uint64_t rejects = 0;
    std::uint64_t messages = 0; // messages this shard handled, including broadcasts
};

struct ShardedOutcome {
    bool ok = false;
    std::string error;
    itch::ParseResult parse;
    std::vector<ShardOutcome> shards;
    /// Times the router found a shard's queue full and had to wait.
    std::uint64_t router_waits = 0;
};

/// Names the settings that make a config unsuitable for sharding, or returns an empty string.
[[nodiscard]] inline std::string sharding_conflicts(const Config& config) {
    std::string conflicts;
    const auto add = [&conflicts](const char* name) {
        conflicts += (conflicts.empty() ? "" : ", ");
        conflicts += name;
    };
    if (config.risk.max_gross_notional < risk::RiskGate::kMaxNotional) {
        add("risk.max_gross_notional_usd");
    }
    if (config.risk.loss_limit != 0) {
        add("risk.loss_limit_usd");
    }
    if (config.risk.rate_limit_per_second != 0) {
        add("risk.rate_limit_per_second");
    }
    if (config.risk.reject_threshold != 0) {
        add("risk.reject_threshold");
    }
    return conflicts;
}

namespace detail {

using MessageQueue = SpscRing<const std::byte*>;

/// Decides which shard, if any, each locate's messages go to.
struct RoutingTable {
    static constexpr std::int16_t kDrop = -1;
    std::vector<std::int16_t> shard_of = std::vector<std::int16_t>(65536, kDrop);
};

/// The router's parser handler. parse() calls at() only for records that have passed
/// validation, so a malformed record can never reach a worker.
class Router : public itch::NullHandler {
public:
    using itch::NullHandler::on;

    Router(const std::byte* base, const RoutingTable& table,
           std::vector<std::unique_ptr<MessageQueue>>& queues)
        : base_(base), table_(table), queues_(queues) {}

    void at(std::uint64_t offset) noexcept {
        const std::byte* const message = base_ + offset + itch::kLengthPrefixSize;
        const auto locate = load_be<std::uint16_t>(message + 1);
        if (locate == 0) {
            // Session-wide (system events, market-wide circuit breakers): everyone needs it.
            for (auto& queue : queues_) {
                push(*queue, message);
            }
            return;
        }
        const std::int16_t shard = table_.shard_of[locate];
        if (shard != RoutingTable::kDrop) {
            push(*queues_[static_cast<std::size_t>(shard)], message);
        }
    }

    [[nodiscard]] std::uint64_t waits() const noexcept { return waits_; }

private:
    void push(MessageQueue& queue, const std::byte* message) noexcept {
        if (!queue.try_push(message)) {
            ++waits_;
            while (!queue.try_push(message)) {
                std::this_thread::yield();
            }
        }
    }

    const std::byte* base_;
    const RoutingTable& table_;
    std::vector<std::unique_ptr<MessageQueue>>& queues_;
    std::uint64_t waits_ = 0;
};

} // namespace detail

/// Replays `data` across `shards` worker threads. `make_strategy(config)` builds one strategy
/// instance per shard. Each shard records into `<out_dir>/shard-<k>/` unless `record` is
/// false. Traded symbols are dealt to shards round-robin in config order.
template <typename MakeStrategy>
[[nodiscard]] ShardedOutcome
run_sharded(std::span<const std::byte> data, const Config& config, std::size_t shards,
            const std::filesystem::path& out_dir, bool record, record::RecorderMode mode,
            MakeStrategy&& make_strategy,
            std::uint64_t max_messages = std::numeric_limits<std::uint64_t>::max()) {
    ShardedOutcome outcome;
    if (shards < 2 || shards > 256) {
        outcome.error = "shard count must be between 2 and 256";
        return outcome;
    }
    if (const std::string conflicts = sharding_conflicts(config); !conflicts.empty()) {
        outcome.error =
            "these settings span symbols and cannot be used with --shards: " + conflicts;
        return outcome;
    }

    // Resolve symbols once, on this thread, to build the routing table.
    itch::SymbolDirectory directory;
    {
        struct Scan : itch::NullHandler {
            using itch::NullHandler::on;
            explicit Scan(itch::SymbolDirectory& d) : directory(d) {}
            void on(const itch::StockDirectory& m) { directory.on(m); }
            void on(const itch::AddOrder& /*m*/) noexcept { finished = true; }
            [[nodiscard]] bool done() const noexcept { return finished; }
            itch::SymbolDirectory& directory;
            bool finished = false;
        } scan(directory);
        const itch::ParseResult scanned = itch::parse(data, scan);
        if (!scanned.ok()) {
            outcome.parse = scanned;
            outcome.error = std::string(itch::to_string(scanned.error)) + " at file offset " +
                            std::to_string(scanned.offset);
            return outcome;
        }
    }
    detail::RoutingTable table;
    std::vector<Config> shard_configs(shards, config);
    for (Config& c : shard_configs) {
        c.symbols.clear();
        // Each shard holds a share of the day's orders.
        c.expected_orders = config.expected_orders / shards + 1;
    }
    for (std::size_t i = 0; i < config.symbols.size(); ++i) {
        const auto locate = directory.find(config.symbols[i]);
        if (!locate) {
            outcome.error =
                "symbol '" + config.symbols[i] + "' is not in the file's stock directory";
            return outcome;
        }
        table.shard_of[*locate] = static_cast<std::int16_t>(i % shards);
        shard_configs[i % shards].symbols.push_back(config.symbols[i]);
    }
    if (config.build_all_books) {
        // Untraded symbols' books are built too; spread them evenly.
        for (std::size_t locate = 1; locate < table.shard_of.size(); ++locate) {
            if (table.shard_of[locate] == detail::RoutingTable::kDrop) {
                table.shard_of[locate] = static_cast<std::int16_t>(locate % shards);
            }
        }
    }

    std::vector<std::unique_ptr<detail::MessageQueue>> queues;
    for (std::size_t i = 0; i < shards; ++i) {
        queues.push_back(std::make_unique<detail::MessageQueue>(std::size_t{1} << 16));
    }
    outcome.shards.resize(shards);
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> failed{false};

    const auto worker = [&](std::size_t index) {
        ShardOutcome& out = outcome.shards[index];
        const Config& shard_config = shard_configs[index];
        record::Recorder recorder;
        auto strategy = make_strategy(shard_config);
        Backtester<decltype(strategy)> engine(shard_config, strategy, recorder);
        bool prepared = engine.prepare(data, out.error);
        if (prepared && record) {
            prepared = recorder.open(out_dir / ("shard-" + std::to_string(index)), out.error, mode,
                                     shard_config.recorder_ring_capacity);
        }
        if (!prepared) {
            failed.store(true, std::memory_order_release);
        }
        ready.fetch_add(1, std::memory_order_release);

        if (prepared) {
            engine.start();
        }
        // Drain the queue to its end marker even after a failure, so the router never blocks.
        detail::MessageQueue& queue = *queues[index];
        std::uint64_t handled = 0;
        for (;;) {
            const std::byte* message = nullptr;
            if (!queue.try_pop(message)) {
                std::this_thread::yield();
                continue;
            }
            if (message == nullptr) {
                break; // end of data
            }
            if (prepared) {
                engine.handle(message);
                ++handled;
            }
        }
        if (!prepared) {
            return;
        }
        engine.finish(handled);
        out.messages = handled;
        out.stats = engine.stats();
        out.records = recorder.counts();
        out.recorder = recorder.stats();
        out.symbols = shard_config.symbols;
        out.locates.assign(engine.symbols().begin(), engine.symbols().end());
        for (std::size_t i = 0; i < out.locates.size(); ++i) {
            out.symbol_risk.push_back(engine.risk().symbol(i));
        }
        out.kill_reason = engine.risk().kill_reason();
        out.total_pnl2 = engine.risk().total_pnl2();
        out.rejects = engine.risk().rejects();
        out.ok = !record || recorder.close();
        if (!out.ok) {
            out.error = "failed writing shard " + std::to_string(index) + " output";
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(shards);
    for (std::size_t i = 0; i < shards; ++i) {
        threads.emplace_back(worker, i);
    }
    // Workers allocate their engines first; start routing once they are all ready.
    while (ready.load(std::memory_order_acquire) < shards) {
        std::this_thread::yield();
    }

    detail::Router router(data.data(), table, queues);
    if (!failed.load(std::memory_order_acquire)) {
        outcome.parse = itch::parse(data, router, max_messages);
    }
    for (auto& queue : queues) {
        while (!queue->try_push(nullptr)) {
            std::this_thread::yield();
        }
    }
    for (std::thread& t : threads) {
        t.join();
    }
    outcome.router_waits = router.waits();

    for (const ShardOutcome& shard : outcome.shards) {
        if (!shard.ok && outcome.error.empty()) {
            outcome.error = shard.error.empty() ? "a shard failed" : shard.error;
        }
    }
    if (outcome.error.empty() && !outcome.parse.ok()) {
        outcome.error = std::string(itch::to_string(outcome.parse.error)) + " at file offset " +
                        std::to_string(outcome.parse.offset);
    }
    outcome.ok = outcome.error.empty();
    return outcome;
}

} // namespace itchbt::sim
