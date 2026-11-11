#pragma once

// A valid, pseudo-random ITCH order flow for property, determinism and benchmark tests.
// Uses raw mt19937_64 output only (no <random> distributions), so the stream for a given seed
// is the same on every platform and standard library.

#include "support/itch_writer.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace itchbt::test {

struct RandomStreamOptions {
    std::uint64_t seed = 1;
    std::size_t events = 10'000;
    std::vector<std::string> symbols = {"AAA", "BBB", "CCC"};
    Locate first_locate = 10;
    Timestamp open_ts = 1'000;
    Timestamp step_ns = 1'000; // time between events
    /// Half-width of the price range, in ticks of 100. Narrow, so levels fill and empty.
    std::uint32_t price_range_ticks = 12;
    Price mid = 1'000'000;
    /// Emit the session preamble (directory, trading actions, market open) and a close.
    bool with_session = true;
};

/// Builds the stream. Orders never cross the spread: buys sit at or below mid, sells above,
/// so the book stays uncrossed and executions are plausible.
inline ItchWriter random_stream(const RandomStreamOptions& options) {
    struct Live {
        OrderRef ref;
        Locate locate;
        Qty qty;
        Side side;
    };
    std::mt19937_64 rng(options.seed);
    const auto pick = [&rng](std::uint64_t n) { return rng() % n; };

    ItchWriter w;
    Timestamp ts = 1;
    if (options.with_session) {
        w.system_event(ts++, 'O');
        for (std::size_t i = 0; i < options.symbols.size(); ++i) {
            const auto locate = static_cast<Locate>(options.first_locate + i);
            w.stock_directory(ts++, locate, options.symbols[i]);
            w.trading_action(ts++, locate, options.symbols[i], 'T');
        }
        w.system_event(options.open_ts, 'Q');
    }
    ts = options.open_ts + options.step_ns;

    std::vector<Live> live;
    OrderRef next_ref = 1;
    MatchNo next_match = 1;
    const auto random_price = [&](Side side) {
        const auto offset = static_cast<Price>(pick(options.price_range_ticks) * 100);
        return side == Side::Buy ? options.mid - offset : options.mid + 100 + offset;
    };

    for (std::size_t i = 0; i < options.events; ++i, ts += options.step_ns) {
        const std::uint64_t action = pick(100);
        if (live.empty() || action < 45) {
            const std::size_t s = pick(options.symbols.size());
            const auto locate = static_cast<Locate>(options.first_locate + s);
            const Side side = pick(2) == 0 ? Side::Buy : Side::Sell;
            const auto qty = static_cast<Qty>((pick(10) + 1) * 100);
            if (pick(10) == 0) {
                w.add_mpid(ts, locate, next_ref, side, qty, options.symbols[s], random_price(side),
                           "GSCO");
            } else {
                w.add(ts, locate, next_ref, side, qty, options.symbols[s], random_price(side));
            }
            live.push_back({next_ref++, locate, qty, side});
            continue;
        }
        const std::size_t index = pick(live.size());
        Live& order = live[index];
        bool gone = false;
        if (action < 58) {
            w.del(ts, order.locate, order.ref);
            gone = true;
        } else if (action < 76) {
            const auto take = static_cast<Qty>(pick(order.qty) + 1);
            w.execute(ts, order.locate, order.ref, take, next_match++);
            order.qty -= take;
            gone = order.qty == 0;
        } else if (action < 80) {
            const auto take = static_cast<Qty>(pick(order.qty) + 1);
            w.execute_with_price(ts, order.locate, order.ref, take, next_match++, pick(4) != 0,
                                 options.mid);
            order.qty -= take;
            gone = order.qty == 0;
        } else if (action < 90) {
            const auto take = static_cast<Qty>(pick(order.qty) + 1);
            w.cancel(ts, order.locate, order.ref, take);
            order.qty -= take;
            gone = order.qty == 0;
        } else if (action < 97) {
            // A replace carries no side: the new order inherits it, so price it on that side.
            const auto qty = static_cast<Qty>((pick(10) + 1) * 100);
            w.replace(ts, order.locate, order.ref, next_ref, qty, random_price(order.side));
            order = {next_ref++, order.locate, qty, order.side};
        } else {
            const std::size_t s = pick(options.symbols.size());
            w.trade(ts, static_cast<Locate>(options.first_locate + s), Side::Buy,
                    static_cast<Qty>((pick(5) + 1) * 100), options.symbols[s], options.mid + 50,
                    next_match++);
        }
        if (gone) {
            live[index] = live.back();
            live.pop_back();
        }
    }
    if (options.with_session) {
        w.system_event(ts, 'M').system_event(ts + 1, 'C');
    }
    return w;
}

} // namespace itchbt::test
