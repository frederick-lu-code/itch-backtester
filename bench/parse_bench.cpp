// Parse-only benchmark: a full ITCH file through the parser, timed over repeated runs.
//
//   parse_bench --input data/<file> [--reader mmap|read] [--handler decode|frame]
//               [--runs 10] [--warmup 1] [--cpu N] [--out result.json]
//
// --handler frame uses an empty handler, so only framing, validation and dispatch are measured
// (the compiler removes the unused decoders). --handler decode reads every field, which is what
// a real consumer pays. Both are reported; decode is the honest headline.

#include "bench_util.hpp"

#include "itchbt/itch/checksum.hpp"
#include "itchbt/itch/parser.hpp"
#include "itchbt/itch/reader.hpp"
#include "itchbt/util/cli.hpp"
#include "itchbt/util/platform.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace {

using namespace itchbt;

struct RunOutput {
    itch::ParseResult parse;
    std::uint64_t checksum = 0;
    bool io_ok = true;
};

template <typename Handler>
RunOutput run_once(const std::string& path, bool use_mmap) {
    RunOutput out;
    Handler handler;
    std::string error;
    if (use_mmap) {
        // Mapping is inside the timed region on purpose: page faults are part of this path.
        const auto file = itch::MappedFile::open(path, error);
        if (!file) {
            std::fprintf(stderr, "parse_bench: %s\n", error.c_str());
            out.io_ok = false;
            return out;
        }
        out.parse = itch::parse(file->bytes(), handler);
    } else {
        auto file = itch::BufferedFile::open(path, error);
        if (!file) {
            std::fprintf(stderr, "parse_bench: %s\n", error.c_str());
            out.io_ok = false;
            return out;
        }
        out.parse = itch::parse_stream(*file, handler);
        out.io_ok = !file->failed();
    }
    if constexpr (requires { handler.sum; }) {
        out.checksum = handler.sum;
    }
    return out;
}

int usage() {
    std::fputs("usage: parse_bench --input <file> [--reader mmap|read] [--handler decode|frame]\n"
               "                   [--runs N] [--warmup N] [--cpu N] [--out file.json]\n",
               stderr);
    return 2;
}

} // namespace

namespace {

int run(int argc, char** argv) {
    Args args(argc, argv);
    bool bad = false;
    const auto input = args.value("--input");
    const std::string reader(args.value("--reader").value_or("mmap"));
    const std::string handler(args.value("--handler").value_or("decode"));
    const std::uint64_t runs = args.number("--runs", bad).value_or(10);
    const std::uint64_t warmup = args.number("--warmup", bad).value_or(1);
    const auto cpu = args.number("--cpu", bad);
    const std::string out_path(args.value("--out").value_or("-"));
    if (!input || bad || !args.unused().empty() || (reader != "mmap" && reader != "read") ||
        (handler != "decode" && handler != "frame") || runs == 0) {
        return usage();
    }
    const std::string path(*input);
    const bool use_mmap = reader == "mmap";
    const std::string placement = place_benchmark_thread(
        cpu ? std::optional<unsigned>(static_cast<unsigned>(*cpu)) : std::nullopt);

    bench::Result result;
    result.benchmark = "parse_only";
    result.variant = reader + "/" + handler;
    result.notes.emplace_back("input", path);
    result.notes.emplace_back("thread_placement", placement);

    std::uint64_t reference_checksum = 0;
    for (std::uint64_t i = 0; i < warmup + runs; ++i) {
        const bench::Stopwatch watch;
        const RunOutput run = handler == "decode" ? run_once<itch::ChecksumHandler>(path, use_mmap)
                                                  : run_once<itch::NullHandler>(path, use_mmap);
        const double seconds = watch.seconds();
        if (!run.io_ok || !run.parse.ok()) {
            std::fprintf(stderr, "parse_bench: %s at offset %" PRIu64 "\n",
                         run.io_ok ? itch::to_string(run.parse.error) : "I/O error",
                         run.parse.offset);
            return 1;
        }
        // Every run must see the same data; a differing checksum means the benchmark is broken.
        if (i == 0) {
            reference_checksum = run.checksum;
        } else if (run.checksum != reference_checksum) {
            std::fputs("parse_bench: checksum changed between runs\n", stderr);
            return 1;
        }
        result.messages = run.parse.messages;
        result.bytes = run.parse.offset;
        const bool measured = i >= warmup;
        if (measured) {
            result.run_seconds.push_back(seconds);
        }
        std::fprintf(stderr, "%s run %" PRIu64 ": %.3f s, %.1f M msg/s, %.2f GB/s\n",
                     measured ? "measured" : "warm-up ", i, seconds,
                     static_cast<double>(run.parse.messages) / seconds / 1e6,
                     static_cast<double>(run.parse.offset) / seconds / 1e9);
    }
    result.counters.emplace_back("checksum_low32", reference_checksum & 0xFFFFFFFFU);
    result.counters.emplace_back("peak_rss_bytes", peak_rss_bytes());
    if (!result.write(out_path)) {
        std::fprintf(stderr, "parse_bench: cannot write %s\n", out_path.c_str());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return itchbt::run_tool("parse_bench", [&] { return run(argc, argv); });
}
