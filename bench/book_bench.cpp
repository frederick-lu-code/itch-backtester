// Book-build benchmark: a full ITCH file through the parser into the order book for every
// symbol, timed over repeated runs.
//
//   book_bench --input data/<file> [--book baseline] [--reserve N] [--runs 10] [--warmup 1]
//              [--cpu N] [--max-messages N] [--out result.json]
//
// A fingerprint of every level of every book is computed after each run, outside the timed
// region. It must be identical across runs and across book implementations: a faster book
// that builds a different book is not faster, it is wrong.

#include "bench_util.hpp"

#include "itchbt/book/book.hpp"
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

template <typename Books>
struct BuildHandler : itch::NullHandler {
    using itch::NullHandler::on;

    explicit BuildHandler(std::size_t reserve) : books(reserve) {}

    void on(const itch::AddOrder& m) { book::apply(books, m); }
    void on(const itch::OrderExecuted& m) { book::apply(books, m); }
    void on(const itch::OrderExecutedWithPrice& m) { book::apply(books, m); }
    void on(const itch::OrderCancel& m) { book::apply(books, m); }
    void on(const itch::OrderDelete& m) { book::apply(books, m); }
    void on(const itch::OrderReplace& m) { book::apply(books, m); }

    Books books;
};

struct RunOutput {
    bool ok = false;
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t peak_orders = 0;
    std::uint64_t orders_added = 0;
    std::uint64_t anomalies = 0;
    double seconds = 0;
};

template <typename Books>
RunOutput run_once(const std::string& path, std::size_t reserve, std::uint64_t max_messages) {
    RunOutput out;
    std::string error;
    const auto file = itch::MappedFile::open(path, error);
    if (!file) {
        std::fprintf(stderr, "book_bench: %s\n", error.c_str());
        return out;
    }
    // Construction (and any reserve) is inside the timed region: it is part of the cost.
    const bench::Stopwatch watch;
    BuildHandler<Books> handler(reserve);
    const itch::ParseResult result = itch::parse(file->bytes(), handler, max_messages);
    out.seconds = watch.seconds();
    if (!result.ok()) {
        std::fprintf(stderr, "book_bench: %s at offset %" PRIu64 "\n",
                     itch::to_string(result.error), result.offset);
        return out;
    }
    out.ok = true;
    out.messages = result.messages;
    out.bytes = result.offset;
    out.peak_orders = handler.books.peak_live_orders();
    out.orders_added = handler.books.last_sequence();
    out.anomalies = handler.books.anomalies().total();
    for (std::size_t locate = 0; locate < Books::kLocateCount; ++locate) {
        for (const Side side : {Side::Buy, Side::Sell}) {
            handler.books.book(static_cast<Locate>(locate))
                .side(side)
                .for_each([&](const book::LevelView& v) {
                    const auto mix = [&out](std::uint64_t x) {
                        out.fingerprint = (out.fingerprint ^ x) * 0x9E3779B97F4A7C15ULL;
                    };
                    mix(locate);
                    mix(static_cast<std::uint64_t>(side));
                    mix(v.price);
                    mix(v.qty);
                    mix(v.orders);
                    return true;
                });
        }
    }
    return out;
}

int usage() {
    std::fputs("usage: book_bench --input <file> [--book baseline] [--reserve N] [--runs N]\n"
               "                  [--warmup N] [--cpu N] [--max-messages N] [--out file.json]\n",
               stderr);
    return 2;
}

int run(int argc, char** argv) {
    Args args(argc, argv);
    bool bad = false;
    const auto input = args.value("--input");
    const std::string book_name(args.value("--book").value_or("baseline"));
    const auto reserve = static_cast<std::size_t>(args.number("--reserve", bad).value_or(0));
    const std::uint64_t runs = args.number("--runs", bad).value_or(10);
    const std::uint64_t warmup = args.number("--warmup", bad).value_or(1);
    const std::uint64_t max_messages = args.number("--max-messages", bad).value_or(UINT64_MAX);
    const auto cpu = args.number("--cpu", bad);
    const std::string out_path(args.value("--out").value_or("-"));
    if (!input || bad || !args.unused().empty() || runs == 0 || book_name != "baseline") {
        return usage();
    }
    const std::string path(*input);
    const std::string placement = place_benchmark_thread(
        cpu ? std::optional<unsigned>(static_cast<unsigned>(*cpu)) : std::nullopt);

    bench::Result result;
    result.benchmark = "book_build";
    result.variant = book_name + (reserve != 0 ? "/reserve" : "");
    result.notes.emplace_back("input", path);
    result.notes.emplace_back("thread_placement", placement);

    RunOutput first;
    for (std::uint64_t i = 0; i < warmup + runs; ++i) {
        const RunOutput run = run_once<book::BaselineBooks>(path, reserve, max_messages);
        if (!run.ok) {
            return 1;
        }
        if (i == 0) {
            first = run;
        } else if (run.fingerprint != first.fingerprint) {
            std::fputs("book_bench: book fingerprint changed between runs\n", stderr);
            return 1;
        }
        const bool measured = i >= warmup;
        if (measured) {
            result.run_seconds.push_back(run.seconds);
        }
        std::fprintf(stderr, "%s run %" PRIu64 ": %.3f s, %.2f M msg/s\n",
                     measured ? "measured" : "warm-up ", i, run.seconds,
                     static_cast<double>(run.messages) / run.seconds / 1e6);
    }
    result.messages = first.messages;
    result.bytes = first.bytes;
    result.counters.emplace_back("book_fingerprint_low32", first.fingerprint & 0xFFFFFFFFU);
    result.counters.emplace_back("orders_added", first.orders_added);
    result.counters.emplace_back("peak_live_orders", first.peak_orders);
    result.counters.emplace_back("anomalies", first.anomalies);
    result.counters.emplace_back("reserve", reserve);
    result.counters.emplace_back("peak_rss_bytes", peak_rss_bytes());
    if (!result.write(out_path)) {
        std::fprintf(stderr, "book_bench: cannot write %s\n", out_path.c_str());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return itchbt::run_tool("book_bench", [&] { return run(argc, argv); });
}
