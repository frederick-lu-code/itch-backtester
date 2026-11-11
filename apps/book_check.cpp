// book-check: replays a whole file through the order book and checks the invariants the
// project relies on. Exits non-zero if any fails.
//
//   book-check --input data/<file> [--max-messages N] [--recount-every N]
//              [--crosses-out closes.csv] [--max-reports 20] [--reopen-tolerance-ms 100]
//
// Checked:
//   - every E, C, X, D and U refers to a live order; no reference is re-used while live;
//     no order is reduced by more than it holds (so no quantity can go negative)
//   - each level's total equals the sum of its orders (full recount, at the end and every
//     --recount-every messages)
//   - no symbol's book is crossed or locked during regular hours while it is trading, except
//     for the moment after a halt or IPO release while the reopening cross is still being
//     published (see "reopening transients" below)
//
// Reopening transients: when a halted symbol is released, Nasdaq sends the trading action "T"
// and the cross trade, and only then finishes streaming the non-printable executions that
// make up the cross. For a few hundred microseconds the feed's book is crossed although the
// market is not. Such a state is tolerated only if the book has been continuously crossed
// since the release and the release was less than --reopen-tolerance-ms ago.
// --crosses-out writes every cross trade (opening, closing, halt) for reconciliation against
// published prices.

#include "itchbt/book/book.hpp"
#include "itchbt/itch/directory.hpp"
#include "itchbt/itch/parser.hpp"
#include "itchbt/itch/reader.hpp"
#include "itchbt/util/cli.hpp"
#include "itchbt/util/platform.hpp"
#include "itchbt/util/time.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using namespace itchbt;

class CheckHandler {
public:
    CheckHandler(std::uint64_t recount_every, std::uint64_t max_reports, Timestamp tolerance,
                 std::FILE* crosses)
        : recount_every_(recount_every), max_reports_(max_reports), tolerance_(tolerance),
          crosses_(crosses), reopening_(65536, false), reopened_at_(65536, 0) {}

    void at(std::uint64_t offset) noexcept { offset_ = offset; }

    void on(const itch::SystemEvent& m) {
        market_.on(m);
        tick();
    }
    void on(const itch::StockDirectory& m) {
        directory_.on(m);
        tick();
    }
    void on(const itch::TradingAction& m) {
        market_.on(m);
        if (m.state == 'T') {
            reopening_[m.h.locate] = true;
            reopened_at_[m.h.locate] = m.h.ts;
        }
        tick();
    }
    void on(const itch::AddOrder& m) { check(book::apply(books_, m), m.h); }
    void on(const itch::OrderExecuted& m) { check(book::apply(books_, m), m.h); }
    void on(const itch::OrderExecutedWithPrice& m) { check(book::apply(books_, m), m.h); }
    void on(const itch::OrderCancel& m) { check(book::apply(books_, m), m.h); }
    void on(const itch::OrderDelete& m) { check(book::apply(books_, m), m.h); }
    void on(const itch::OrderReplace& m) {
        const book::ReplaceChange change = book::apply(books_, m);
        // Judge the pair as one event: the book is legitimately odd between the two halves.
        check(change.added.applied ? change.added : change.removed, m.h);
    }
    void on(const itch::CrossTrade& m) {
        if (crosses_ != nullptr) {
            std::fprintf(crosses_, "%s,%c,%u,%" PRIu64 ",%s\n",
                         std::string(m.symbol.view()).c_str(), m.cross_type, m.price, m.shares,
                         format_time(m.h.ts).c_str());
        }
        tick();
    }
    template <typename Message>
    void on(const Message& /*m*/) {
        tick();
    }
    void on_skipped(const itch::Header& /*h*/) { tick(); }

    [[nodiscard]] bool final_recount() {
        std::string error;
        if (!books_.verify(error)) {
            std::printf("RECOUNT FAILED at end of file: %s\n", error.c_str());
            ++recount_failures_;
        }
        ++recounts_;
        return recount_failures_ == 0;
    }

    void report() const {
        const book::Anomalies& a = books_.anomalies();
        std::printf("messages:                  %" PRIu64 "\n", messages_);
        std::printf("symbols:                   %zu\n", directory_.size());
        std::printf("orders added:              %" PRIu64 "\n", books_.last_sequence());
        std::printf("peak live orders:          %zu\n", books_.peak_live_orders());
        std::printf("live orders at end:        %zu\n", books_.live_orders());
        std::printf("unknown order references:  %" PRIu64 "\n", a.unknown_refs);
        std::printf("duplicate order references:%" PRIu64 "\n", a.duplicate_refs);
        std::printf("over-reductions:           %" PRIu64 "\n", a.over_reductions);
        std::printf("full recounts:             %" PRIu64 " (%" PRIu64 " failed)\n", recounts_,
                    recount_failures_);
        std::printf("crossed/locked checks:     %" PRIu64 "\n", cross_checks_);
        std::printf("reopening transients:      %" PRIu64 " (longest %.3f ms after release)\n",
                    reopen_transients_, static_cast<double>(longest_transient_) / 1e6);
        std::printf("crossed/locked violations: %" PRIu64 "\n", cross_violations_);
        std::printf("peak RSS:                  %.1f MiB\n",
                    static_cast<double>(peak_rss_bytes()) / (1024.0 * 1024.0));
    }

    [[nodiscard]] bool passed() const noexcept {
        return books_.anomalies().total() == 0 && recount_failures_ == 0 && cross_violations_ == 0;
    }

private:
    void tick() {
        ++messages_;
        if (recount_every_ != 0 && messages_ % recount_every_ == 0) {
            std::string error;
            ++recounts_;
            if (!books_.verify(error)) {
                ++recount_failures_;
                std::printf("RECOUNT FAILED after %" PRIu64 " messages: %s\n", messages_,
                            error.c_str());
            }
        }
    }

    void check(const book::BookChange& change, const itch::Header& h) {
        if (!change.applied) {
            if (reports_++ < max_reports_) {
                std::printf("ANOMALY %c at offset %" PRIu64 " (%s, message %" PRIu64
                            "): order reference not applicable\n",
                            h.type, offset_, format_time(h.ts).c_str(), messages_);
            }
        } else if (market_.regular_hours() && market_.trading(change.locate)) {
            ++cross_checks_;
            const auto& book = books_.book(change.locate);
            const auto bid = book.best_bid();
            const auto ask = book.best_ask();
            const bool crossed = bid && ask && bid->price >= ask->price;
            if (!crossed) {
                reopening_[change.locate] = false;
            } else if (reopening_[change.locate] &&
                       h.ts - reopened_at_[change.locate] <= tolerance_) {
                ++reopen_transients_;
                const Timestamp age = h.ts - reopened_at_[change.locate];
                longest_transient_ = age > longest_transient_ ? age : longest_transient_;
            } else {
                ++cross_violations_;
                if (reports_++ < max_reports_) {
                    std::printf("CROSSED %s at offset %" PRIu64 " (%s, message %" PRIu64
                                ", type %c): bid %u x ask %u\n",
                                std::string(directory_.symbol(change.locate)).c_str(), offset_,
                                format_time(h.ts).c_str(), messages_, h.type, bid->price,
                                ask->price);
                }
            }
        }
        tick();
    }

    book::BaselineBooks books_;
    itch::SymbolDirectory directory_;
    itch::MarketState market_;
    std::uint64_t recount_every_;
    std::uint64_t max_reports_;
    Timestamp tolerance_;
    std::FILE* crosses_;
    std::vector<bool> reopening_;        // released and continuously crossed since
    std::vector<Timestamp> reopened_at_; // time of the last "T" trading action
    std::uint64_t reopen_transients_ = 0;
    Timestamp longest_transient_ = 0;
    std::uint64_t offset_ = 0;
    std::uint64_t messages_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t recounts_ = 0;
    std::uint64_t recount_failures_ = 0;
    std::uint64_t cross_checks_ = 0;
    std::uint64_t cross_violations_ = 0;
};

int usage() {
    std::fputs("usage: book-check --input <file> [--max-messages N] [--recount-every N]\n"
               "                  [--crosses-out file.csv] [--max-reports N]\n"
               "                  [--reopen-tolerance-ms N]\n",
               stderr);
    return 2;
}

int run(int argc, char** argv) {
    Args args(argc, argv);
    bool bad = false;
    const auto input = args.value("--input");
    const std::uint64_t max_messages = args.number("--max-messages", bad).value_or(UINT64_MAX);
    const std::uint64_t recount_every = args.number("--recount-every", bad).value_or(0);
    const std::uint64_t max_reports = args.number("--max-reports", bad).value_or(20);
    const Timestamp tolerance = args.number("--reopen-tolerance-ms", bad).value_or(100) * 1'000'000;
    const auto crosses_path = args.value("--crosses-out");
    if (!input || bad || !args.unused().empty()) {
        return usage();
    }

    std::string error;
    const auto file = itch::MappedFile::open(std::string(*input), error);
    if (!file) {
        std::fprintf(stderr, "book-check: %s\n", error.c_str());
        return 1;
    }
    std::FILE* crosses = nullptr;
    if (crosses_path) {
        crosses = std::fopen(std::string(*crosses_path).c_str(), "w");
        if (crosses == nullptr) {
            std::fprintf(stderr, "book-check: cannot write %s\n",
                         std::string(*crosses_path).c_str());
            return 1;
        }
        std::fputs("symbol,cross_type,price,shares,time\n", crosses);
    }

    CheckHandler handler(recount_every, max_reports, tolerance, crosses);
    const itch::ParseResult result = itch::parse(file->bytes(), handler, max_messages);
    if (crosses != nullptr) {
        std::fclose(crosses);
    }
    if (!result.ok()) {
        std::fprintf(stderr, "book-check: %s at file offset %" PRIu64 "\n",
                     itch::to_string(result.error), result.offset);
        return 1;
    }
    const bool recount_ok = handler.final_recount();
    handler.report();
    const bool passed = recount_ok && handler.passed();
    std::printf("\n%s\n", passed ? "PASS: all invariants hold" : "FAIL: see above");
    return passed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    return itchbt::run_tool("book-check", [&] { return run(argc, argv); });
}
