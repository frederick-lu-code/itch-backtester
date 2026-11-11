// backtest: replays an ITCH file through the engine with a strategy and writes a run directory.
//
//   backtest --input data/<file> --config configs/passive_maker.toml --out runs/<name>
//            [--symbols AAPL,MSFT] [--max-messages N] [--skip-input-hash]
//            [--recorder async|sync]
//
// The run directory holds orders.bin, fills.bin, bbo.bin, risk.bin and run.json. The four
// .bin files are a pure function of the input, the config and the binary. So is run.json,
// except for its final "timing" object (wall time and how long the engine waited on the
// recorder), which is the only thing that differs between otherwise identical runs.

#include "imbalance_taker.hpp"
#include "noop.hpp"
#include "passive_maker.hpp"

#include "itchbt/itch/reader.hpp"
#include "itchbt/record/recorder.hpp"
#include "itchbt/sim/config.hpp"
#include "itchbt/sim/engine.hpp"
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

#ifndef ITCHBT_GIT_COMMIT
#define ITCHBT_GIT_COMMIT "unknown"
#endif

namespace {

using namespace itchbt;

struct Outcome {
    bool ok = false;
    std::string error;
    itch::ParseResult parse;
    sim::RunStats stats;
    std::string symbols_json;
    std::string risk_json;
    std::string risk_summary;
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

template <typename S>
Outcome run_strategy(S& strategy, const sim::Config& config, record::Recorder& recorder,
                     std::span<const std::byte> data, std::uint64_t max_messages) {
    Outcome out;
    sim::Backtester<S> engine(config, strategy, recorder);
    if (!engine.prepare(data, out.error)) {
        return out;
    }
    out.parse = engine.run(data, max_messages);
    out.stats = engine.stats();
    if (!out.parse.ok()) {
        out.error = std::string(itch::to_string(out.parse.error)) + " at file offset " +
                    std::to_string(out.parse.offset);
        return out;
    }
    out.ok = true;

    const risk::RiskGate& risk = engine.risk();
    out.risk_json = "{\"kill_switch\": \"" + std::string(kill_reason_name(risk.kill_reason())) +
                    "\", \"total_pnl_x2\": " + std::to_string(risk.total_pnl2()) +
                    ", \"rejects\": " + std::to_string(risk.rejects()) + ", \"symbols\": [";
    std::array<char, 256> line{};
    for (std::size_t i = 0; i < engine.symbols().size(); ++i) {
        const Locate locate = engine.symbols()[i];
        const risk::SymbolRisk& s = risk.symbol(i);
        const std::string name(engine.directory().symbol(locate));
        out.symbols_json += std::string(i == 0 ? "" : ", ") + "{\"symbol\": \"" + name +
                            "\", \"locate\": " + std::to_string(locate) + "}";
        out.risk_json += std::string(i == 0 ? "" : ", ") + "{\"symbol\": \"" + name +
                         "\", \"position\": " + std::to_string(s.position) +
                         ", \"realised\": " + std::to_string(s.realised()) +
                         ", \"unrealised_x2\": " + std::to_string(s.unrealised2()) + "}";
        std::snprintf(line.data(), line.size(),
                      "  %-8s position %8" PRId64 "  realised %12.2f  unrealised %12.2f\n",
                      name.c_str(), s.position,
                      static_cast<double>(s.realised()) /
                          static_cast<double>(sim::kPriceUnitsPerDollar),
                      dollars_from_pnl2(s.unrealised2()));
        out.risk_summary += line.data();
    }
    out.risk_json += "]}";
    std::snprintf(line.data(), line.size(), "  total PnL %.2f USD, kill switch: %s\n",
                  dollars_from_pnl2(risk.total_pnl2()), kill_reason_name(risk.kill_reason()));
    out.risk_summary += line.data();
    return out;
}

bool write_run_json(const std::filesystem::path& path, const std::string& input,
                    std::uint64_t input_size, const std::string& sha256, const sim::Config& config,
                    const Outcome& outcome, const record::RecordCounts& counts,
                    const record::RecorderStats& recorder, double wall_seconds) {
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
    std::fprintf(out, "  \"config\": %s,\n", sim::to_json(config).c_str());
    std::fprintf(out, "  \"symbols\": [%s],\n", outcome.symbols_json.c_str());
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
                 counts.orders, counts.fills, counts.bbo, counts.risk);
    std::fprintf(out, "  \"risk\": %s,\n", outcome.risk_json.c_str());
    // Kept last and on its own line: these are the only values that vary between otherwise
    // identical runs. Everything above this line is a pure function of input, config and binary.
    std::fprintf(out,
                 "  \"timing\": {\"wall_time_s\": %.3f, \"recorder_blocked_records\": %" PRIu64
                 ", \"recorder_blocked_ns\": %" PRIu64 "}\n}\n",
                 wall_seconds, recorder.blocked_records, recorder.blocked_ns);
    return std::fclose(out) == 0;
}

int usage() {
    std::fputs("usage: backtest --input <file> --config <file.toml> --out <directory>\n"
               "                [--symbols A,B,...] [--max-messages N] [--skip-input-hash]\n",
               stderr);
    return 2;
}

int run(int argc, char** argv) {
    Args args(argc, argv);
    bool bad = false;
    const auto input = args.value("--input");
    const auto config_path = args.value("--config");
    const auto out_dir = args.value("--out");
    const auto symbols = args.value("--symbols");
    const std::uint64_t max_messages = args.number("--max-messages", bad).value_or(UINT64_MAX);
    const bool skip_hash = args.flag("--skip-input-hash");
    const std::string recorder_mode(args.value("--recorder").value_or("async"));
    if (!input || !config_path || !out_dir || bad || !args.unused().empty() ||
        (recorder_mode != "async" && recorder_mode != "sync")) {
        return usage();
    }

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
    record::Recorder recorder;
    const std::filesystem::path directory{std::string(*out_dir)};
    if (!recorder.open(directory, error,
                       recorder_mode == "sync" ? record::RecorderMode::Sync
                                               : record::RecorderMode::Async,
                       config->recorder_ring_capacity)) {
        std::fprintf(stderr, "backtest: %s\n", error.c_str());
        return 1;
    }

    // Wall time is measured here, in the tool. The engine itself never looks at a clock.
    const auto started = std::chrono::steady_clock::now();
    Outcome outcome;
    if (config->strategy == "passive_maker") {
        strategies::PassiveMaker strategy(config->passive_maker);
        outcome = run_strategy(strategy, *config, recorder, file->bytes(), max_messages);
    } else if (config->strategy == "imbalance_taker") {
        strategies::ImbalanceTaker strategy(config->imbalance_taker);
        outcome = run_strategy(strategy, *config, recorder, file->bytes(), max_messages);
    } else {
        strategies::NoOp strategy;
        outcome = run_strategy(strategy, *config, recorder, file->bytes(), max_messages);
    }
    const double wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!outcome.ok) {
        std::fprintf(stderr, "backtest: %s\n", outcome.error.c_str());
        return 1;
    }
    const record::RecordCounts counts = recorder.counts();
    const record::RecorderStats recorder_stats = recorder.stats();
    if (!recorder.close()) {
        std::fprintf(stderr, "backtest: failed writing to %s\n", directory.string().c_str());
        return 1;
    }

    const std::string sha256 = skip_hash ? std::string() : sha256_hex(file->bytes());
    if (!write_run_json(directory / "run.json", input_path, file->bytes().size(), sha256, *config,
                        outcome, counts, recorder_stats, wall_seconds)) {
        std::fprintf(stderr, "backtest: cannot write run.json\n");
        return 1;
    }

    const sim::RunStats& s = outcome.stats;
    std::printf("strategy %s on %zu symbols: %" PRIu64 " messages in %.2f s (%.2f M msg/s)\n",
                config->strategy.c_str(), config->symbols.size(), s.messages, wall_seconds,
                static_cast<double>(s.messages) / wall_seconds / 1e6);
    std::printf("  orders %" PRIu64 " (risk rejects %" PRIu64 ", exchange rejects %" PRIu64
                "), fills %" PRIu64 " (%" PRIu64 " shares), cancels %" PRIu64 "\n",
                s.orders_submitted, s.orders_rejected_by_risk, s.orders_rejected_by_exchange,
                s.fills, s.shares_filled, s.cancels_requested);
    std::fputs(outcome.risk_summary.c_str(), stdout);
    if (s.events_dropped != 0 || s.submits_refused != 0) {
        std::printf("  WARNING: %" PRIu64 " events dropped, %" PRIu64
                    " submits refused; raise event_queue_capacity or max_orders_per_symbol\n",
                    s.events_dropped, s.submits_refused);
    }
    if (recorder_stats.blocked_records != 0) {
        std::printf("  recorder: engine waited on a full ring for %" PRIu64 " records, %.3f ms\n",
                    recorder_stats.blocked_records,
                    static_cast<double>(recorder_stats.blocked_ns) / 1e6);
    }
    std::printf("  peak RSS %.0f MiB, run directory %s\n",
                static_cast<double>(peak_rss_bytes()) / (1024.0 * 1024.0),
                directory.string().c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return itchbt::run_tool("backtest", [&] { return run(argc, argv); });
}
