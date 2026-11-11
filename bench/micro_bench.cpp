// Micro-benchmarks of the data structures and of strategy dispatch, on Google Benchmark.
//
//   micro_bench [--benchmark_filter=...] [--benchmark_out=file.json --benchmark_out_format=json]
//
// The order-map benchmarks replay a real trace of order references (adds, lookups for
// executions and cancels, erases) taken from the start of the ITCH file named by the
// ITCHBT_DATA environment variable, because hash-table behaviour depends heavily on the key
// distribution and on how long keys live. Without that variable they fall back to a synthetic
// trace and say so in the benchmark label.

#include "support/random_stream.hpp"
#include "virtual_adapter.hpp"

#include "itchbt/book/book.hpp"
#include "itchbt/itch/parser.hpp"
#include "itchbt/itch/reader.hpp"
#include "itchbt/record/recorder.hpp"
#include "itchbt/sim/engine.hpp"
#include "itchbt/util/spsc_ring.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace itchbt;

// --- Order map: replay of a real reference trace ------------------------------------------------

enum class OpKind : std::uint8_t { Insert, Lookup, Erase };
struct Op {
    OrderRef ref;
    OpKind kind;
};

struct Trace {
    std::vector<Op> ops;
    std::string source;
};

/// Turns book messages into the order-store operations they cause, tracking each order's
/// remaining quantity so an execution or cancel to zero becomes a lookup plus an erase.
struct TraceBuilder : itch::NullHandler {
    using itch::NullHandler::on;
    std::vector<Op> ops;
    book::FlatOrderMap live{1 << 21};

    void add(OrderRef ref, Qty qty) {
        book::Order order;
        order.qty = qty;
        if (live.insert(ref, order)) {
            ops.push_back({ref, OpKind::Insert});
        }
    }
    void reduce(OrderRef ref, Qty qty) {
        book::Order* const order = live.find(ref);
        if (order == nullptr) {
            return;
        }
        ops.push_back({ref, OpKind::Lookup});
        if (qty >= order->qty) {
            live.erase(ref);
            ops.push_back({ref, OpKind::Erase});
        } else {
            order->qty -= qty;
        }
    }
    void remove(OrderRef ref) {
        if (live.find(ref) != nullptr) {
            ops.push_back({ref, OpKind::Lookup});
            live.erase(ref);
            ops.push_back({ref, OpKind::Erase});
        }
    }
    void on(const itch::AddOrder& m) { add(m.ref, m.qty); }
    void on(const itch::OrderExecuted& m) { reduce(m.ref, m.qty); }
    void on(const itch::OrderExecutedWithPrice& m) { reduce(m.ref, m.qty); }
    void on(const itch::OrderCancel& m) { reduce(m.ref, m.qty); }
    void on(const itch::OrderDelete& m) { remove(m.ref); }
    void on(const itch::OrderReplace& m) {
        remove(m.old_ref);
        add(m.new_ref, m.qty);
    }
};

const Trace& trace() {
    static const Trace loaded = [] {
        Trace t;
        // Messages from mid-morning, when the book is full, rather than the quiet pre-open.
        constexpr std::uint64_t kMessages = 80'000'000;
        constexpr std::size_t kKeep = 20'000'000;
        if (const char* path = std::getenv("ITCHBT_DATA")) { // NOLINT(concurrency-mt-unsafe)
            std::string error;
            if (const auto file = itch::MappedFile::open(path, error)) {
                TraceBuilder builder;
                if (itch::parse(file->bytes(), builder, kMessages).ok() && !builder.ops.empty()) {
                    const std::size_t skip =
                        builder.ops.size() > kKeep ? builder.ops.size() - kKeep : 0;
                    // The kept window must be self-contained: drop lookups and erases of
                    // orders inserted before it.
                    book::FlatOrderMap seen{1 << 21};
                    for (std::size_t i = skip; i < builder.ops.size(); ++i) {
                        const Op op = builder.ops[i];
                        if (op.kind == OpKind::Insert) {
                            seen.insert(op.ref, book::Order{});
                            t.ops.push_back(op);
                        } else if (seen.find(op.ref) != nullptr) {
                            t.ops.push_back(op);
                            if (op.kind == OpKind::Erase) {
                                seen.erase(op.ref);
                            }
                        }
                    }
                    t.source = "real trace";
                    return t;
                }
            }
        }
        // Synthetic stand-in: sequential references with random lifetimes.
        std::mt19937_64 rng(1);
        std::vector<OrderRef> live;
        OrderRef next = 1;
        for (std::size_t i = 0; i < 4'000'000; ++i) {
            if (live.empty() || rng() % 100 < 45) {
                t.ops.push_back({next, OpKind::Insert});
                live.push_back(next++);
            } else {
                const std::size_t index = rng() % live.size();
                t.ops.push_back({live[index], OpKind::Lookup});
                if (rng() % 100 < 80) {
                    t.ops.push_back({live[index], OpKind::Erase});
                    live[index] = live.back();
                    live.pop_back();
                }
            }
        }
        t.source = "synthetic trace (set ITCHBT_DATA for the real one)";
        return t;
    }();
    return loaded;
}

template <typename Map>
void BM_OrderMapReplay(benchmark::State& state) {
    const Trace& t = trace();
    std::uint64_t found = 0;
    for (auto _ : state) {
        Map map(static_cast<std::size_t>(state.range(0)));
        for (const Op& op : t.ops) {
            switch (op.kind) {
            case OpKind::Insert: map.insert(op.ref, book::Order{}); break;
            case OpKind::Lookup: found += map.find(op.ref) != nullptr ? 1U : 0U; break;
            case OpKind::Erase: map.erase(op.ref); break;
            }
        }
    }
    benchmark::DoNotOptimize(found);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(t.ops.size()));
    state.SetLabel(t.source + (state.range(0) == 0 ? ", not pre-sized" : ", pre-sized"));
}
BENCHMARK_TEMPLATE(BM_OrderMapReplay, book::StdOrderMap)
    ->Arg(0)
    ->Arg(1 << 21)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_OrderMapReplay, book::FlatOrderMap)
    ->Arg(0)
    ->Arg(1 << 21)
    ->Unit(benchmark::kMillisecond);

// --- Price levels -----------------------------------------------------------------------------

/// Adds and removes shares at prices that random-walk near the top of a book `depth` levels
/// deep, which is where real activity concentrates.
template <typename Levels>
void BM_LevelUpdate(benchmark::State& state) {
    const auto depth = static_cast<std::uint32_t>(state.range(0));
    book::SideBook<Levels> side(Side::Buy);
    for (std::uint32_t i = 0; i < depth; ++i) {
        side.add(1'000'000 - i * 100, 100);
    }
    std::mt19937_64 rng(5);
    std::vector<Price> prices(4096);
    for (Price& p : prices) {
        // Geometric-ish: most updates within a few levels of the best.
        const auto level = static_cast<std::uint32_t>(std::countr_zero(rng() | (1ULL << 20)));
        p = 1'000'000 - (level % depth) * 100;
    }
    std::size_t i = 0;
    for (auto _ : state) {
        const Price price = prices[i++ & 4095];
        side.add(price, 100);
        side.reduce(price, 100, true);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK_TEMPLATE(BM_LevelUpdate, book::MapLevels)->Arg(16)->Arg(256)->Arg(4096);
BENCHMARK_TEMPLATE(BM_LevelUpdate, book::BinaryVectorLevels)->Arg(16)->Arg(256)->Arg(4096);
BENCHMARK_TEMPLATE(BM_LevelUpdate, book::VectorLevels)->Arg(16)->Arg(256)->Arg(4096);

/// The other half of a level container's job: creating and destroying levels at the touch.
template <typename Levels>
void BM_LevelCreateDestroyAtTop(benchmark::State& state) {
    const auto depth = static_cast<std::uint32_t>(state.range(0));
    book::SideBook<Levels> side(Side::Buy);
    for (std::uint32_t i = 0; i < depth; ++i) {
        side.add(1'000'000 - i * 100, 100);
    }
    for (auto _ : state) {
        side.add(1'000'100, 100); // new best level
        side.reduce(1'000'100, 100, true);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK_TEMPLATE(BM_LevelCreateDestroyAtTop, book::MapLevels)->Arg(256)->Arg(4096);
BENCHMARK_TEMPLATE(BM_LevelCreateDestroyAtTop, book::VectorLevels)->Arg(256)->Arg(4096);

// --- SPSC ring ---------------------------------------------------------------------------------

struct Slot56 {
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, 6> payload{};
};

/// Items per second through the ring with a producer and a consumer on separate threads, the
/// way the recorder uses it. The consumer checks the sequence, so a lost or reordered item
/// would fail the benchmark rather than flatter it.
void BM_SpscTransfer(benchmark::State& state) {
    SpscRing<Slot56> ring(static_cast<std::size_t>(state.range(0)));
    std::atomic<bool> stop{false};
    std::atomic<bool> broken{false};
    std::thread consumer([&] {
        Slot56 item;
        std::uint64_t expected = 0;
        for (;;) {
            if (ring.try_pop(item)) {
                if (item.sequence != expected++) {
                    broken.store(true);
                }
            } else if (stop.load(std::memory_order_acquire)) {
                if (!ring.try_pop(item)) {
                    return;
                }
                ++expected;
            }
        }
    });
    Slot56 item;
    for (auto _ : state) {
        while (!ring.try_push(item)) {
        }
        ++item.sequence;
    }
    stop.store(true, std::memory_order_release);
    consumer.join();
    if (broken.load()) {
        state.SkipWithError("ring lost or reordered an item");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscTransfer)->Arg(1 << 10)->Arg(1 << 16)->UseRealTime();

// --- Strategy dispatch ---------------------------------------------------------------------------

/// A strategy that does a token amount of work in each market-data callback, so that the cost
/// of reaching the callback is visible next to it.
struct Counting {
    std::uint64_t sum = 0;
    void on_start(sim::Context& /*ctx*/) noexcept {}
    void on_book_update(const sim::BookUpdate& u, sim::Context& /*ctx*/) noexcept {
        sum += u.bid_qty + u.ask_qty;
    }
    void on_trade(const sim::Trade& t, sim::Context& /*ctx*/) noexcept { sum += t.qty; }
    void on_order_update(const sim::OrderUpdate& /*u*/, sim::Context& /*ctx*/) noexcept {}
    void on_timer(Timestamp /*ts*/, sim::Context& /*ctx*/) noexcept {}
    void on_end(sim::Context& /*ctx*/) noexcept {}
};

const test::ItchWriter& dispatch_stream() {
    static const test::ItchWriter stream = [] {
        test::RandomStreamOptions options;
        options.seed = 3;
        options.events = 400'000;
        return test::random_stream(options);
    }();
    return stream;
}

/// Replays a synthetic stream through the whole engine. Comparing the two variants isolates
/// what virtual dispatch costs in context, where the compiler can or cannot inline the
/// callback into the event loop. A stand-alone loop around one call does not show that: it
/// measures the benchmark's own dependency chain instead.
template <typename S>
void run_engine(benchmark::State& state, S& strategy) {
    sim::Config config;
    config.symbols = {"AAA", "BBB", "CCC"};
    config.expected_orders = 1 << 16;
    const test::ItchWriter& stream = dispatch_stream();
    std::uint64_t callbacks = 0;
    std::uint64_t messages = 0;
    for (auto _ : state) {
        record::Recorder discard;
        sim::Backtester<S> engine(config, strategy, discard);
        std::string error;
        if (!engine.prepare(stream.bytes(), error) || !engine.run(stream.bytes()).ok()) {
            state.SkipWithError("replay failed");
            break;
        }
        callbacks += engine.stats().book_updates + engine.stats().trades;
        messages += engine.stats().messages;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(messages));
    state.counters["callbacks_per_run"] =
        static_cast<double>(callbacks) / static_cast<double>(state.iterations());
    state.counters["messages_per_run"] =
        static_cast<double>(messages) / static_cast<double>(state.iterations());
}

void BM_EngineDispatchConcept(benchmark::State& state) {
    Counting strategy;
    run_engine(state, strategy);
    benchmark::DoNotOptimize(strategy.sum);
}
BENCHMARK(BM_EngineDispatchConcept)->Unit(benchmark::kMillisecond);

void BM_EngineDispatchVirtual(benchmark::State& state) {
    strategies::Polymorphic<Counting> strategy;
    strategies::IStrategy* erased = &strategy;
    benchmark::DoNotOptimize(erased); // the adapter must not see through to the concrete type
    strategies::VirtualAdapter adapter(*erased);
    run_engine(state, adapter);
    benchmark::DoNotOptimize(strategy.strategy().sum);
}
BENCHMARK(BM_EngineDispatchVirtual)->Unit(benchmark::kMillisecond);

} // namespace

BENCHMARK_MAIN();
