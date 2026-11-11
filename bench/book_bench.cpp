// Book-build benchmark: a full ITCH file through the parser into the order book for every
// symbol, timed over repeated runs.
//
//   book_bench --input data/<file> [--book baseline|flat_orders|binary_levels|optimised]
//              [--reserve N] [--runs 10] [--warmup 1] [--cpu N] [--max-messages N]
//              [--out result.json]
//
// A fingerprint of every level of every book is folded in at regular checkpoints during the
// run and once at the end. It must be identical across runs and across book implementations:
// a faster book that builds a different book is not faster, it is wrong. The checkpoints are
// inside the timed region and cost every variant the same few milliseconds.
//
// Built with -DITCHBT_TIMING=ON (the `timing` preset) it also times every message with the
// CPU's tick counter and reports latency percentiles by message type. Throughput is always
// quoted from the untimed build; the timed build's own throughput shows what timing costs.

#include "bench_util.hpp"

#include "itchbt/book/book.hpp"
#include "itchbt/itch/parser.hpp"
#include "itchbt/itch/reader.hpp"
#include "itchbt/util/cli.hpp"
#include "itchbt/util/histogram.hpp"
#include "itchbt/util/platform.hpp"
#include "itchbt/util/ticks.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace itchbt;

#ifdef ITCHBT_TIMING
constexpr bool kTiming = true;
#else
constexpr bool kTiming = false;
#endif

/// Latency samples in ticks, one histogram per message type.
struct Latency {
    Latency() : by_type(256) {}
    std::vector<Histogram> by_type;
};

template <typename Books>
struct BuildHandler {
    BuildHandler(std::size_t reserve, Latency* latency_out)
        : books(reserve), latency(latency_out) {}

#ifdef ITCHBT_BENCH_PREFETCH
    // Experiment: start loading the order-store slot the next message will need while this
    // one is still being handled. See the optimisation log for the verdict.
    void lookahead(const std::byte* next) noexcept {
        switch (std::to_integer<char>(next[0])) {
        case 'A':
        case 'F':
        case 'E':
        case 'C':
        case 'X':
        case 'D':
        case 'U': books.prefetch_order(load_be<std::uint64_t>(next + 11)); break;
        default: break;
        }
    }
#endif

    void on(const itch::AddOrder& m) { apply(m); }
    void on(const itch::OrderExecuted& m) { apply(m); }
    void on(const itch::OrderExecutedWithPrice& m) { apply(m); }
    void on(const itch::OrderCancel& m) { apply(m); }
    void on(const itch::OrderDelete& m) { apply(m); }
    void on(const itch::OrderReplace& m) { apply(m); }
    template <typename Message>
    void on(const Message& m) noexcept {
        type = m.h.type;
    }
    void on_skipped(const itch::Header& h) noexcept { type = h.type; }

#ifdef ITCHBT_TIMING
    // Entry and exit of each message, around decode, dispatch and the book update.
    void at(std::uint64_t /*offset*/) noexcept { started = ticks_begin(); }
    void after() noexcept {
        latency->by_type[static_cast<unsigned char>(type)].record(ticks_end() - started);
    }
#endif

    template <typename Message>
    void apply(const Message& m) {
        type = m.h.type;
        book::apply(books, m);
        // Every 2^24 book operations, fold the whole state of every book into the fingerprint.
        // At the end of a full day the books are empty, so a fingerprint taken only then
        // would be the same for any implementation, right or wrong.
        if ((++operations & ((std::uint64_t{1} << 24) - 1)) == 0) {
            fold_books();
        }
    }

    void fold_books() {
        const auto mix = [this](std::uint64_t x) {
            fingerprint = (fingerprint ^ x) * 0x9E3779B97F4A7C15ULL;
        };
        for (std::size_t locate = 0; locate < Books::kLocateCount; ++locate) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                books.book(static_cast<Locate>(locate))
                    .side(side)
                    .for_each([&](const book::LevelView& v) {
                        mix(locate);
                        mix(static_cast<std::uint64_t>(side));
                        mix(v.price);
                        mix(v.qty);
                        mix(v.orders);
                        return true;
                    });
            }
        }
        mix(books.live_orders());
        ++checkpoints;
    }

    Books books;
    Latency* latency;
    std::uint64_t operations = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t checkpoints = 0;
    std::uint64_t started = 0;
    char type = '\0';
};

struct RunOutput {
    bool ok = false;
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t checkpoints = 0;
    std::uint64_t peak_orders = 0;
    std::uint64_t orders_added = 0;
    std::uint64_t anomalies = 0;
    std::uint64_t rehashes = 0;
    std::uint64_t minor_faults = 0;
    std::uint64_t major_faults = 0;
    double seconds = 0;
};

template <typename Books>
RunOutput run_once(const std::string& path, std::size_t reserve, std::uint64_t max_messages,
                   Latency* latency) {
    RunOutput out;
    std::string error;
    const auto file = itch::MappedFile::open(path, error);
    if (!file) {
        std::fprintf(stderr, "book_bench: %s\n", error.c_str());
        return out;
    }
    // Construction (and any reserve) is inside the timed region: it is part of the cost.
    const PageFaults faults_before = page_faults();
    const bench::Stopwatch watch;
    BuildHandler<Books> handler(reserve, latency);
    const itch::ParseResult result = itch::parse(file->bytes(), handler, max_messages);
    out.seconds = watch.seconds();
    const PageFaults faults_after = page_faults();
    out.minor_faults = faults_after.minor - faults_before.minor;
    out.major_faults = faults_after.major - faults_before.major;
    out.rehashes = handler.books.order_store_rehashes();
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
    handler.fold_books(); // and once more at the end
    out.fingerprint = handler.fingerprint;
    out.checkpoints = handler.checkpoints;
    return out;
}

RunOutput run_named(const std::string& book, const std::string& path, std::size_t reserve,
                    std::uint64_t max_messages, Latency* latency) {
    if (book == "baseline") {
        return run_once<book::BaselineBooks>(path, reserve, max_messages, latency);
    }
    if (book == "flat_orders") {
        return run_once<book::FlatOrderBooks>(path, reserve, max_messages, latency);
    }
    if (book == "binary_levels") {
        return run_once<book::BinaryLevelBooks>(path, reserve, max_messages, latency);
    }
    return run_once<book::OptimisedBooks>(path, reserve, max_messages, latency);
}

std::string latency_row(const char* label, const Histogram& h, double ns_per_tick) {
    const auto ns = [ns_per_tick](std::uint64_t ticks) {
        return static_cast<double>(ticks) * ns_per_tick;
    };
    char row[320];
    std::snprintf(row, sizeof(row),
                  "{\"type\": \"%s\", \"count\": %" PRIu64
                  ", \"p50_ns\": %.1f, \"p90_ns\": %.1f, \"p99_ns\": %.1f, \"p999_ns\": %.1f, "
                  "\"max_ns\": %.1f, \"mean_ns\": %.1f}",
                  label, h.count(), ns(h.percentile(0.50)), ns(h.percentile(0.90)),
                  ns(h.percentile(0.99)), ns(h.percentile(0.999)), ns(h.max()),
                  h.mean() * ns_per_tick);
    return row;
}

int usage() {
    std::fputs("usage: book_bench --input <file> [--book baseline|flat_orders|binary_levels|"
               "optimised]\n"
               "                  [--reserve N] [--runs N] [--warmup N] [--cpu N]\n"
               "                  [--max-messages N] [--out file.json]\n",
               stderr);
    return 2;
}

int run(int argc, char** argv) {
    Args args(argc, argv);
    bool bad = false;
    const auto input = args.value("--input");
    const std::string book_name(args.value("--book").value_or("optimised"));
    const auto reserve = static_cast<std::size_t>(args.number("--reserve", bad).value_or(0));
    const std::uint64_t runs = args.number("--runs", bad).value_or(10);
    const std::uint64_t warmup = args.number("--warmup", bad).value_or(1);
    const std::uint64_t max_messages = args.number("--max-messages", bad).value_or(UINT64_MAX);
    const auto cpu = args.number("--cpu", bad);
    const std::string out_path(args.value("--out").value_or("-"));
    if (!input || bad || !args.unused().empty() || runs == 0 ||
        (book_name != "baseline" && book_name != "flat_orders" && book_name != "binary_levels" &&
         book_name != "optimised")) {
        return usage();
    }
    const std::string path(*input);
    const std::string placement = place_benchmark_thread(
        cpu ? std::optional<unsigned>(static_cast<unsigned>(*cpu)) : std::nullopt);
    const double ns_per_tick = kTiming ? calibrate_ns_per_tick() : 0.0;

    bench::Result result;
    result.benchmark = kTiming ? "book_latency" : "book_build";
    result.variant = book_name + (reserve != 0 ? "/reserve" : "");
#ifdef ITCHBT_BENCH_PREFETCH
    result.variant += "/prefetch";
#endif
    result.notes.emplace_back("input", path);
    result.notes.emplace_back("thread_placement", placement);

    const auto latency = std::make_unique<Latency>();
    RunOutput first;
    for (std::uint64_t i = 0; i < warmup + runs; ++i) {
        const bool measured = i >= warmup;
        // Latency is accumulated over the measured runs only.
        const auto scratch = measured ? nullptr : std::make_unique<Latency>();
        const RunOutput run = run_named(book_name, path, reserve, max_messages,
                                        measured ? latency.get() : scratch.get());
        if (!run.ok) {
            return 1;
        }
        if (i == 0) {
            first = run;
        } else if (run.fingerprint != first.fingerprint) {
            std::fputs("book_bench: book fingerprint changed between runs\n", stderr);
            return 1;
        }
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
    result.counters.emplace_back("fingerprint_checkpoints", first.checkpoints);
    result.counters.emplace_back("orders_added", first.orders_added);
    result.counters.emplace_back("peak_live_orders", first.peak_orders);
    result.counters.emplace_back("anomalies", first.anomalies);
    result.counters.emplace_back("reserve", reserve);
    result.counters.emplace_back("peak_rss_bytes", peak_rss_bytes());
    result.counters.emplace_back("order_store_rehashes", first.rehashes);
    result.counters.emplace_back("minor_page_faults_per_run", first.minor_faults);
    result.counters.emplace_back("major_page_faults_per_run", first.major_faults);

    if (kTiming) {
        const auto all = std::make_unique<Histogram>();
        std::string rows;
        for (std::size_t type = 0; type < latency->by_type.size(); ++type) {
            const Histogram& h = latency->by_type[type];
            if (h.count() == 0) {
                continue;
            }
            all->merge(h);
            const char label[2] = {static_cast<char>(type), '\0'};
            rows += ", " + latency_row(label, h, ns_per_tick);
        }
        char resolution[64];
        std::snprintf(resolution, sizeof(resolution), "\"tick_ns\": %.3f", ns_per_tick);
        result.raw_members.emplace_back(resolution);
        result.raw_members.push_back("\"latency\": [" + latency_row("all", *all, ns_per_tick) +
                                     rows + "]");
    }
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
