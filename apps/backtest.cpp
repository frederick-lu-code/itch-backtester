// backtest: replays an ITCH file through the engine with a strategy and writes a run directory.
//
//   backtest --input data/<file> --config configs/passive_maker.toml --out runs/<name>
//            [--symbols AAPL,MSFT] [--max-messages N] [--shards N] [--skip-input-hash]
//            [--recorder async|sync|none]
//   backtest --print-schema
//
// The run directory holds orders.bin, fills.bin, bbo.bin, risk.bin and run.json. The four
// .bin files are a pure function of the input, the config and the binary. So is run.json,
// except for its final "timing" object (wall time and how long the engine waited on the
// recorder), which is the only thing that differs between otherwise identical runs.
//
// With --shards N the symbols are split across N engines on N threads and each writes its
// files under shard-<k>/. Per symbol, the records are identical to a single-threaded run.

#include "imbalance_taker.hpp"
#include "noop.hpp"
#include "passive_maker.hpp"

#include "itchbt/itch/reader.hpp"
#include "itchbt/record/recorder.hpp"
#include "itchbt/record/schema.hpp"
#include "itchbt/sim/config.hpp"
#include "itchbt/sim/engine.hpp"
#include "itchbt/sim/sharded.hpp"
#include "itchbt/util/cli.hpp"
#include "itchbt/util/platform.hpp"
#include "itchbt/util/sha256.hpp"

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifndef ITCHBT_GIT_COMMIT
#define ITCHBT_GIT_COMMIT "unknown"
#endif

namespace {

using namespace itchbt;

struct SymbolReport {
    std::string name;
    Locate locate = 0;
    risk::SymbolRisk risk;
};

struct Outcome {
    bool ok = false;
    std::string error;
    itch::ParseResult parse;
    sim::RunStats stats;
    std::vector<SymbolReport> symbols; // in config order
    risk::KillReason kill_reason = risk::KillReason::None;
    std::int64_t total_pnl2 = 0;
    std::uint64_t rejects = 0;
    record::RecordCounts records;
    record::RecorderStats recorder;
    std::size_t shards = 1;
    std::uint64_t router_waits = 0;
};

struct Options {
    std::filesystem::path directory;
    record::RecorderMode mode = record::RecorderMode::Async;
    bool record = true;
    std::size_t shards = 1;
    std::uint64_t max_messages = UINT64_MAX;
};

const char* kill_reason_name(risk::KillReason reason) {
    switch (reason) {
    case risk::KillReason::None: return "none";
    case risk::KillReason::LossLimit: return "loss_limit";
    case risk::KillReason::RejectRate: return "reject_rate";
    }
    return "?";
}

/// Price units x shares x 2, as dollars. Reporting only: the engine never uses floating point.
double dollars_from_pnl2(std::int64_t pnl2) {
    return static_cast<double>(pnl2) / 2.0 / static_cast<double>(sim::kPriceUnitsPerDollar);
}

void add(sim::RunStats& total, const sim::RunStats& part) {
    total.orders_submitted += part.orders_submitted;
    total.orders_rejected_by_risk += part.orders_rejected_by_risk;
    total.orders_rejected_by_exchange += part.orders_rejected_by_exchange;
    total.fills += part.fills;
    total.shares_filled += part.shares_filled;
    total.cancels_requested += part.cancels_requested;
    total.timers_fired += part.timers_fired;
    total.book_updates += part.book_updates;
    total.trades += part.trades;
    total.submits_refused += part.submits_refused;
    total.events_dropped += part.events_dropped;
    total.peak_queue_depth = std::max(total.peak_queue_depth, part.peak_queue_depth);
}

template <typename MakeStrategy>
Outcome run_single(MakeStrategy&& make, const sim::Config& config, const Options& options,
                   std::span<const std::byte> data) {
    Outcome out;
    record::Recorder recorder;
    if (options.record &&
        !recorder.open(options.directory, out.error, options.mode, config.recorder_ring_capacity)) {
        return out;
    }
    auto strategy = make(config);
    sim::Backtester<decltype(strategy)> engine(config, strategy, recorder);
    if (!engine.prepare(data, out.error)) {
        return out;
    }
    out.parse = engine.run(data, options.max_messages);
    out.stats = engine.stats();
    out.records = recorder.counts();
    out.recorder = recorder.stats();
    if (!out.parse.ok()) {
        out.error = std::string(itch::to_string(out.parse.error)) + " at file offset " +
                    std::to_string(out.parse.offset);
        return out;
    }
    if (options.record && !recorder.close()) {
        out.error = "failed writing to " + options.directory.string();
        return out;
    }
    const risk::RiskGate& risk = engine.risk();
    for (std::size_t i = 0; i < engine.symbols().size(); ++i) {
        const Locate locate = engine.symbols()[i];
        out.symbols.push_back(
            {std::string(engine.directory().symbol(locate)), locate, risk.symbol(i)});
    }
    out.kill_reason = risk.kill_reason();
    out.total_pnl2 = risk.total_pnl2();
    out.rejects = risk.rejects();
    out.ok = true;
    return out;
}

template <typename MakeStrategy>
Outcome run_shards(MakeStrategy&& make, const sim::Config& config, const Options& options,
                   std::span<const std::byte> data) {
    Outcome out;
    out.shards = options.shards;
    sim::ShardedOutcome sharded =
        sim::run_sharded(data, config, options.shards, options.directory, options.record,
                         options.mode, make, options.max_messages);
    out.parse = sharded.parse;
    out.router_waits = sharded.router_waits;
    if (!sharded.ok) {
        out.error = sharded.error;
        return out;
    }
    out.stats.messages = sharded.parse.messages;
    for (const sim::ShardOutcome& shard : sharded.shards) {
        add(out.stats, shard.stats);
        out.records.orders += shard.records.orders;
        out.records.fills += shard.records.fills;
        out.records.bbo += shard.records.bbo;
        out.records.risk += shard.records.risk;
        out.recorder.blocked_records += shard.recorder.blocked_records;
        out.recorder.blocked_ns += shard.recorder.blocked_ns;
        out.total_pnl2 += shard.total_pnl2;
        out.rejects += shard.rejects;
    }
    // Symbols were dealt round-robin; read them back in config order.
    for (std::size_t i = 0; i < config.symbols.size(); ++i) {
        const sim::ShardOutcome& shard = sharded.shards[i % options.shards];
        const std::size_t slot = i / options.shards;
        out.symbols.push_back({shard.symbols[slot], shard.locates[slot], shard.symbol_risk[slot]});
    }
    out.ok = true;
    return out;
}

template <typename MakeStrategy>
Outcome execute(MakeStrategy&& make, const sim::Config& config, const Options& options,
                std::span<const std::byte> data) {
    return options.shards > 1 ? run_shards(make, config, options, data)
                              : run_single(make, config, options, data);
}

bool write_run_json(const std::filesystem::path& path, const std::string& input,
                    std::uint64_t input_size, const std::string& sha256, const sim::Config& config,
                    const Outcome& outcome, double wall_seconds) {
    std::FILE* out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
        return false;
    }
    const sim::RunStats& s = outcome.stats;
    std::fprintf(out, "{\n");
    std::fprintf(out, "  \"format_version\": %u,\n", record::kFormatVersion);
    std::fprintf(out, "  \"input\": {\"file\": \"%s\", \"size\": %" PRIu64 ", \"sha256\": %s},\n",
                 std::filesystem::path(input).filename().string().c_str(), input_size,
                 sha256.empty() ? "null" : ("\"" + sha256 + "\"").c_str());
    std::fprintf(out, "  \"git_commit\": \"%s\",\n", ITCHBT_GIT_COMMIT);
    std::fprintf(out, "  \"compiler\": \"%s\",\n", __VERSION__);
    std::fprintf(out, "  \"book\": \"%s\",\n", book::kDefaultBookName);
    std::fprintf(out, "  \"shards\": %zu,\n", outcome.shards);
    std::fprintf(out, "  \"config\": %s,\n", sim::to_json(config).c_str());
    std::fputs("  \"symbols\": [", out);
    for (std::size_t i = 0; i < outcome.symbols.size(); ++i) {
        std::fprintf(out, "%s{\"symbol\": \"%s\", \"locate\": %u}", i == 0 ? "" : ", ",
                     outcome.symbols[i].name.c_str(), unsigned{outcome.symbols[i].locate});
    }
    std::fputs("],\n", out);
    std::fprintf(
        out,
        "  \"stats\": {\"messages\": %" PRIu64 ", \"orders_submitted\": %" PRIu64
        ", \"orders_rejected_by_risk\": %" PRIu64 ", \"orders_rejected_by_exchange\": %" PRIu64
        ", \"fills\": %" PRIu64 ", \"shares_filled\": %" PRIu64 ", \"cancels_requested\": %" PRIu64
        ", \"timers_fired\": %" PRIu64 ", \"book_updates\": %" PRIu64 ", \"trades\": %" PRIu64
        ", \"submits_refused\": %" PRIu64 ", \"events_dropped\": %" PRIu64
        ", \"peak_queue_depth\": %zu},\n",
        s.messages, s.orders_submitted, s.orders_rejected_by_risk, s.orders_rejected_by_exchange,
        s.fills, s.shares_filled, s.cancels_requested, s.timers_fired, s.book_updates, s.trades,
        s.submits_refused, s.events_dropped, s.peak_queue_depth);
    std::fprintf(out,
                 "  \"records\": {\"orders\": %" PRIu64 ", \"fills\": %" PRIu64
                 ", \"bbo\": %" PRIu64 ", \"risk\": %" PRIu64 "},\n",
                 outcome.records.orders, outcome.records.fills, outcome.records.bbo,
                 outcome.records.risk);
    std::fprintf(out,
                 "  \"risk\": {\"kill_switch\": \"%s\", \"total_pnl_x2\": %" PRId64
                 ", \"rejects\": %" PRIu64 ", \"symbols\": [",
                 kill_reason_name(outcome.kill_reason), outcome.total_pnl2, outcome.rejects);
    for (std::size_t i = 0; i < outcome.symbols.size(); ++i) {
        const SymbolReport& r = outcome.symbols[i];
        std::fprintf(out,
                     "%s{\"symbol\": \"%s\", \"position\": %" PRId64 ", \"realised\": %" PRId64
                     ", \"unrealised_x2\": %" PRId64 "}",
                     i == 0 ? "" : ", ", r.name.c_str(), r.risk.position, r.risk.realised(),
                     r.risk.unrealised2());
    }
    std::fputs("]},\n", out);
    // Kept last and on its own line: these are the only values that vary between otherwise
    // identical runs. Everything above this line is a pure function of input, config and binary.
    std::fprintf(out,
                 "  \"timing\": {\"wall_time_s\": %.3f, \"recorder_blocked_records\": %" PRIu64
                 ", \"recorder_blocked_ns\": %" PRIu64 ", \"router_waits\": %" PRIu64 "}\n}\n",
                 wall_seconds, outcome.recorder.blocked_records, outcome.recorder.blocked_ns,
                 outcome.router_waits);
    return std::fclose(out) == 0;
}

void print_summary(const sim::Config& config, const Outcome& outcome, const Options& options,
                   double wall_seconds) {
    const sim::RunStats& s = outcome.stats;
    std::printf("strategy %s on %zu symbols%s: %" PRIu64 " messages in %.2f s (%.2f M msg/s)\n",
                config.strategy.c_str(), config.symbols.size(),
                outcome.shards > 1 ? (", " + std::to_string(outcome.shards) + " shards").c_str()
                                   : "",
                s.messages, wall_seconds, static_cast<double>(s.messages) / wall_seconds / 1e6);
    std::printf("  orders %" PRIu64 " (risk rejects %" PRIu64 ", exchange rejects %" PRIu64
                "), fills %" PRIu64 " (%" PRIu64 " shares), cancels %" PRIu64 "\n",
                s.orders_submitted, s.orders_rejected_by_risk, s.orders_rejected_by_exchange,
                s.fills, s.shares_filled, s.cancels_requested);
    for (const SymbolReport& r : outcome.symbols) {
        std::printf("  %-8s position %8" PRId64 "  realised %12.2f  unrealised %12.2f\n",
                    r.name.c_str(), r.risk.position,
                    static_cast<double>(r.risk.realised()) /
                        static_cast<double>(sim::kPriceUnitsPerDollar),
                    dollars_from_pnl2(r.risk.unrealised2()));
    }
    std::printf("  total PnL %.2f USD, kill switch: %s\n", dollars_from_pnl2(outcome.total_pnl2),
                kill_reason_name(outcome.kill_reason));
    if (s.events_dropped != 0 || s.submits_refused != 0) {
        std::printf("  WARNING: %" PRIu64 " events dropped, %" PRIu64
                    " submits refused; raise event_queue_capacity or max_orders_per_symbol\n",
                    s.events_dropped, s.submits_refused);
    }
    if (outcome.recorder.blocked_records != 0) {
        std::printf("  recorder: engine waited on a full ring for %" PRIu64 " records, %.3f ms\n",
                    outcome.recorder.blocked_records,
                    static_cast<double>(outcome.recorder.blocked_ns) / 1e6);
    }
    std::printf("  peak RSS %.0f MiB, run directory %s\n",
                static_cast<double>(peak_rss_bytes()) / (1024.0 * 1024.0),
                options.directory.string().c_str());
}

int usage() {
    std::fputs("usage: backtest --input <file> --config <file.toml> --out <directory>\n"
               "                [--symbols A,B,...] [--max-messages N] [--shards N]\n"
               "                [--skip-input-hash] [--recorder async|sync|none]\n"
               "       backtest --print-schema\n",
               stderr);
    return 2;
}

int run(int argc, char** argv) {
    Args args(argc, argv);
    if (args.flag("--print-schema")) {
        std::fputs(record::schema_json().c_str(), stdout);
        return 0;
    }
    bool bad = false;
    Options options;
    const auto input = args.value("--input");
    const auto config_path = args.value("--config");
    const auto out_dir = args.value("--out");
    const auto symbols = args.value("--symbols");
    options.max_messages = args.number("--max-messages", bad).value_or(UINT64_MAX);
    options.shards = static_cast<std::size_t>(args.number("--shards", bad).value_or(1));
    const bool skip_hash = args.flag("--skip-input-hash");
    const std::string recorder_mode(args.value("--recorder").value_or("async"));
    if (!input || !config_path || !out_dir || bad || !args.unused().empty() ||
        options.shards == 0 ||
        (recorder_mode != "async" && recorder_mode != "sync" && recorder_mode != "none")) {
        return usage();
    }
    options.directory = std::string(*out_dir);
    options.record = recorder_mode != "none";
    options.mode =
        recorder_mode == "sync" ? record::RecorderMode::Sync : record::RecorderMode::Async;

    std::string error;
    auto config = sim::load_config(std::string(*config_path), error);
    if (!config) {
        std::fprintf(stderr, "backtest: %s\n", error.c_str());
        return 1;
    }
    if (symbols) {
        config->symbols = split_commas(*symbols);
    }
    const std::string input_path(*input);
    const auto file = itch::MappedFile::open(input_path, error);
    if (!file) {
        std::fprintf(stderr, "backtest: %s\n", error.c_str());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(options.directory, ec);
    if (ec) {
        std::fprintf(stderr, "backtest: cannot create %s: %s\n", options.directory.string().c_str(),
                     ec.message().c_str());
        return 1;
    }

    // Wall time is measured here, in the tool. The engine itself never looks at a clock.
    const auto started = std::chrono::steady_clock::now();
    Outcome outcome;
    if (config->strategy == "passive_maker") {
        outcome =
            execute([](const sim::Config& c) { return strategies::PassiveMaker(c.passive_maker); },
                    *config, options, file->bytes());
    } else if (config->strategy == "imbalance_taker") {
        outcome = execute(
            [](const sim::Config& c) { return strategies::ImbalanceTaker(c.imbalance_taker); },
            *config, options, file->bytes());
    } else {
        outcome = execute([](const sim::Config& /*c*/) { return strategies::NoOp(); }, *config,
                          options, file->bytes());
    }
    const double wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!outcome.ok) {
        std::fprintf(stderr, "backtest: %s\n", outcome.error.c_str());
        return 1;
    }

    const std::string sha256 = skip_hash ? std::string() : sha256_hex(file->bytes());
    if (!write_run_json(options.directory / "run.json", input_path, file->bytes().size(), sha256,
                        *config, outcome, wall_seconds)) {
        std::fprintf(stderr, "backtest: cannot write run.json\n");
        return 1;
    }
    print_summary(*config, outcome, options, wall_seconds);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return itchbt::run_tool("backtest", [&] { return run(argc, argv); });
}
