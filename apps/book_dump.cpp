// book-dump: rebuilds the book for one or more symbols and prints it as of a time of day or a
// message count. The csv format with --checkpoint-every is what the differential test against
// the Python reference book consumes.
//
//   book-dump --input data/<file> --symbol AAPL --at 10:30:00 --depth 10
//   book-dump --input data/<file> --symbols AAPL,MSFT --max-messages 5000000 --depth 0
//             --format csv --checkpoint-every 1000000

#include "itchbt/book/default.hpp"
#include "itchbt/itch/directory.hpp"
#include "itchbt/itch/parser.hpp"
#include "itchbt/itch/reader.hpp"
#include "itchbt/util/cli.hpp"
#include "itchbt/util/time.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace itchbt;

struct Options {
    std::vector<std::string> symbols;
    Timestamp at = kNanosPerDay; // apply every message stamped at or before this
    std::uint64_t max_messages = UINT64_MAX;
    std::uint64_t checkpoint_every = 0;
    std::size_t depth = 10; // 0 means every level
    bool csv = false;
};

class DumpHandler {
public:
    explicit DumpHandler(const Options& options) : options_(options), wanted_(65536, false) {}

    void on(const itch::StockDirectory& m) {
        if (begin(m.h)) {
            directory_.on(m);
            for (const std::string& symbol : options_.symbols) {
                if (m.symbol.view() == symbol) {
                    wanted_[m.h.locate] = true;
                    locates_.push_back(m.h.locate);
                }
            }
            end();
        }
    }
    // Only the requested symbols are tracked, so memory stays small on a full-day file.
    void on(const itch::AddOrder& m) { apply_if_wanted(m); }
    void on(const itch::OrderExecuted& m) { apply_if_wanted(m); }
    void on(const itch::OrderExecutedWithPrice& m) { apply_if_wanted(m); }
    void on(const itch::OrderCancel& m) { apply_if_wanted(m); }
    void on(const itch::OrderDelete& m) { apply_if_wanted(m); }
    void on(const itch::OrderReplace& m) { apply_if_wanted(m); }
    template <typename Message>
    void on(const Message& m) {
        skip(m.h);
    }
    void on_skipped(const itch::Header& h) { skip(h); }

    [[nodiscard]] bool done() const noexcept { return done_; }
    [[nodiscard]] std::uint64_t applied() const noexcept { return applied_; }
    [[nodiscard]] Timestamp last_ts() const noexcept { return last_ts_; }
    [[nodiscard]] const book::Anomalies& anomalies() const noexcept { return books_.anomalies(); }
    [[nodiscard]] std::size_t resolved() const noexcept { return locates_.size(); }

    void dump() const {
        for (const Locate locate : locates_) {
            if (options_.csv) {
                dump_csv(locate);
            } else {
                dump_text(locate);
            }
        }
    }

private:
    // A message stamped after the cut-off ends the run without being applied.
    bool begin(const itch::Header& h) noexcept {
        if (h.ts > options_.at) {
            done_ = true;
            return false;
        }
        last_ts_ = h.ts;
        return true;
    }
    void end() {
        ++applied_;
        if (options_.checkpoint_every != 0 && applied_ % options_.checkpoint_every == 0) {
            dump();
        }
    }
    void skip(const itch::Header& h) {
        if (begin(h)) {
            end();
        }
    }
    template <typename Message>
    void apply_if_wanted(const Message& m) {
        if (begin(m.h)) {
            if (wanted_[m.h.locate]) {
                book::apply(books_, m);
            }
            end();
        }
    }

    void dump_csv(Locate locate) const {
        const std::string symbol(directory_.symbol(locate));
        const auto& book = books_.book(locate);
        for (const Side side : {Side::Buy, Side::Sell}) {
            std::size_t n = 0;
            book.side(side).for_each([&](const book::LevelView& v) {
                std::printf("%" PRIu64 ",%s,%c,%u,%" PRIu64 ",%u\n", applied_, symbol.c_str(),
                            side == Side::Buy ? 'B' : 'S', v.price, v.qty, v.orders);
                return options_.depth == 0 || ++n < options_.depth;
            });
        }
    }

    void dump_text(Locate locate) const {
        const auto& book = books_.book(locate);
        std::vector<book::LevelView> bids;
        std::vector<book::LevelView> asks;
        const auto collect = [this](std::vector<book::LevelView>& into) {
            return [this, &into](const book::LevelView& v) {
                into.push_back(v);
                return options_.depth == 0 || into.size() < options_.depth;
            };
        };
        book.bids.for_each(collect(bids));
        book.asks.for_each(collect(asks));

        std::printf("%s (locate %u) after %" PRIu64 " messages, last at %s\n",
                    std::string(directory_.symbol(locate)).c_str(), unsigned{locate}, applied_,
                    format_time(last_ts_).c_str());
        std::printf("  %6s %10s %12s | %-12s %-10s %-6s\n", "orders", "shares", "bid", "ask",
                    "shares", "orders");
        const std::size_t rows = bids.size() > asks.size() ? bids.size() : asks.size();
        for (std::size_t i = 0; i < rows; ++i) {
            if (i < bids.size()) {
                std::printf("  %6u %10" PRIu64 " %12.4f |", bids[i].orders, bids[i].qty,
                            static_cast<double>(bids[i].price) / 10000.0);
            } else {
                std::printf("  %6s %10s %12s |", "", "", "");
            }
            if (i < asks.size()) {
                std::printf(" %-12.4f %-10" PRIu64 " %-6u",
                            static_cast<double>(asks[i].price) / 10000.0, asks[i].qty,
                            asks[i].orders);
            }
            std::putchar('\n');
        }
        std::printf("  levels: %zu bid, %zu ask\n\n", book.bids.level_count(),
                    book.asks.level_count());
    }

    const Options& options_;
    book::DefaultBooks books_;
    itch::SymbolDirectory directory_;
    std::vector<bool> wanted_;
    std::vector<Locate> locates_;
    std::uint64_t applied_ = 0;
    Timestamp last_ts_ = 0;
    bool done_ = false;
};

int usage() {
    std::fputs("usage: book-dump --input <file> (--symbol S | --symbols S1,S2,...)\n"
               "                 [--at HH:MM:SS[.frac]] [--max-messages N] [--depth N]\n"
               "                 [--format text|csv] [--checkpoint-every N]\n"
               "  --depth 0 prints every level. csv columns: messages,symbol,side,price,\n"
               "  shares,orders with price in units of 1/10000 USD.\n",
               stderr);
    return 2;
}

int run(int argc, char** argv) {
    Args args(argc, argv);
    Options options;
    bool bad = false;
    const auto input = args.value("--input");
    if (const auto s = args.value("--symbol")) {
        options.symbols.emplace_back(*s);
    }
    if (const auto s = args.value("--symbols")) {
        for (std::string& symbol : split_commas(*s)) {
            options.symbols.push_back(std::move(symbol));
        }
    }
    if (const auto at = args.value("--at")) {
        const auto parsed = parse_time(*at);
        if (!parsed) {
            std::fprintf(stderr, "book-dump: cannot parse time '%s'\n", std::string(*at).c_str());
            return 2;
        }
        options.at = *parsed;
    }
    options.max_messages = args.number("--max-messages", bad).value_or(UINT64_MAX);
    options.checkpoint_every = args.number("--checkpoint-every", bad).value_or(0);
    options.depth = static_cast<std::size_t>(args.number("--depth", bad).value_or(10));
    const std::string format(args.value("--format").value_or("text"));
    options.csv = format == "csv";
    if (!input || options.symbols.empty() || bad || !args.unused().empty() ||
        (format != "text" && format != "csv")) {
        return usage();
    }

    std::string error;
    const auto file = itch::MappedFile::open(std::string(*input), error);
    if (!file) {
        std::fprintf(stderr, "book-dump: %s\n", error.c_str());
        return 1;
    }
    DumpHandler handler(options);
    const itch::ParseResult result = itch::parse(file->bytes(), handler, options.max_messages);
    if (!result.ok()) {
        std::fprintf(stderr, "book-dump: %s at file offset %" PRIu64 "\n",
                     itch::to_string(result.error), result.offset);
        return 1;
    }
    if (handler.resolved() != options.symbols.size()) {
        std::fputs("book-dump: one or more symbols were not found in the stock directory\n",
                   stderr);
        return 1;
    }
    // With checkpoints on, the final state is printed only if it was not just printed.
    if (options.checkpoint_every == 0 || handler.applied() % options.checkpoint_every != 0) {
        handler.dump();
    }
    if (handler.anomalies().total() != 0) {
        std::fprintf(stderr, "book-dump: %" PRIu64 " book anomalies; run book-check\n",
                     handler.anomalies().total());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return itchbt::run_tool("book-dump", [&] { return run(argc, argv); });
}
