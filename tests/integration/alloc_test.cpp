// The hot path must not allocate after start-up.
//
// This executable replaces global operator new with one that counts calls made while a flag is
// set on the calling thread. The engine is prepared (all its storage is allocated then), the
// flag is raised, a stream is replayed with a strategy that trades, and the count must be zero.
//
// Kept in its own executable: replacing operator new affects the whole program.

#include "imbalance_taker.hpp"
#include "noop.hpp"
#include "passive_maker.hpp"
#include "support/random_stream.hpp"
#include "virtual_adapter.hpp"

#include "itchbt/record/recorder.hpp"
#include "itchbt/sim/engine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>

namespace {

thread_local bool g_counting = false;
std::atomic<std::size_t> g_allocations{0};

void* counted_alloc(std::size_t size) {
    if (g_counting) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* const p = std::malloc(size == 0 ? 1 : size); // NOLINT(*-no-malloc, *-owning-memory)
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

} // namespace

// NOLINTBEGIN(*-owning-memory, *-no-malloc, misc-new-delete-overloads)
void* operator new(std::size_t size) {
    return counted_alloc(size);
}
void* operator new[](std::size_t size) {
    return counted_alloc(size);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t /*size*/) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t /*size*/) noexcept {
    std::free(p);
}
// NOLINTEND(*-owning-memory, *-no-malloc, misc-new-delete-overloads)

namespace itchbt::sim {
namespace {

struct Counted {
    std::size_t allocations = 0;
    RunStats stats;
    std::uint64_t rehashes = 0;
};

/// Replays `stream` and counts allocations made by the engine thread during run().
template <typename S>
Counted count_run(const test::ItchWriter& stream, const Config& config, S& strategy) {
    record::Recorder discard;
    Backtester<S> engine(config, strategy, discard);
    std::string error;
    EXPECT_TRUE(engine.prepare(stream.bytes(), error)) << error;
    g_allocations.store(0);
    g_counting = true;
    const itch::ParseResult result = engine.run(stream.bytes());
    g_counting = false;
    EXPECT_TRUE(result.ok());
    return {g_allocations.load(), engine.stats(), engine.books().order_store_rehashes()};
}

Config trading_config() {
    Config c;
    c.symbols = {"AAA", "BBB", "CCC"};
    c.build_all_books = false;
    c.expected_orders = 1 << 16;
    c.level_reserve = 1024;
    c.exchange.max_orders_per_symbol = 200'000;
    c.latency.order_ns = 2'000;
    c.latency.cancel_ns = 2'000;
    c.latency.ack_ns = 1'000;
    c.risk.max_position = 600;
    c.risk.rate_limit_per_second = 100'000;
    c.risk.rate_limit_burst = 10;
    c.risk.reject_threshold = 1'000'000;
    return c;
}

test::ItchWriter stream(std::size_t events) {
    test::RandomStreamOptions options;
    options.seed = 21;
    options.events = events;
    return test::random_stream(options);
}

TEST(HotPathAllocations, CounterSeesAllocations) {
    // The test below proves nothing unless the counter works.
    g_allocations.store(0);
    g_counting = true;
    // Published through a volatile pointer so the optimiser cannot remove the allocation.
    static int* volatile escaped = nullptr;
    escaped = new int(7); // NOLINT(*-owning-memory)
    g_counting = false;
    delete escaped; // NOLINT(*-owning-memory)
    EXPECT_EQ(g_allocations.load(), 1U);
}

TEST(HotPathAllocations, ReplayWithNoStrategyAllocatesNothing) {
    strategies::NoOp noop;
    const Counted c = count_run(stream(100'000), trading_config(), noop);
    EXPECT_EQ(c.allocations, 0U);
    EXPECT_EQ(c.rehashes, 0U);
    EXPECT_GT(c.stats.book_updates, 5'000U);
}

TEST(HotPathAllocations, PassiveMakerAllocatesNothing) {
    const Config config = trading_config();
    strategies::PassiveMaker maker(config.passive_maker);
    const Counted c = count_run(stream(100'000), config, maker);
    EXPECT_EQ(c.allocations, 0U);
    // The run must have exercised orders, fills, cancels and rejects to mean anything.
    EXPECT_GT(c.stats.orders_submitted, 1'000U);
    EXPECT_GT(c.stats.fills, 100U);
    EXPECT_GT(c.stats.cancels_requested, 1'000U);
    EXPECT_GT(c.stats.orders_rejected_by_risk, 10U);
    EXPECT_EQ(c.stats.events_dropped, 0U);
    EXPECT_EQ(c.stats.submits_refused, 0U);
}

TEST(HotPathAllocations, ImbalanceTakerAllocatesNothing) {
    Config config = trading_config();
    config.imbalance_taker.cooldown_ns = 10'000;
    config.imbalance_taker.threshold_pct = 70;
    strategies::ImbalanceTaker taker(config.imbalance_taker);
    const Counted c = count_run(stream(100'000), config, taker);
    EXPECT_EQ(c.allocations, 0U);
    EXPECT_GT(c.stats.fills, 20U);
}

TEST(HotPathAllocations, VirtualDispatchAllocatesNothing) {
    const Config config = trading_config();
    strategies::Polymorphic<strategies::PassiveMaker> maker(config.passive_maker);
    strategies::VirtualAdapter adapter(maker);
    const Counted c = count_run(stream(50'000), config, adapter);
    EXPECT_EQ(c.allocations, 0U);
    EXPECT_GT(c.stats.fills, 50U);
}

TEST(HotPathAllocations, UndersizedStoresAllocateAndSayso) {
    // The guarantee depends on sizing: with the order store and level arrays too small, the
    // engine still runs correctly, but it allocates, and the rehash counter shows why.
    Config config = trading_config();
    config.expected_orders = 16;
    config.level_reserve = 1;
    strategies::NoOp noop;
    const Counted c = count_run(stream(100'000), config, noop);
    EXPECT_GT(c.allocations, 0U);
    EXPECT_GT(c.rehashes, 0U);
}

} // namespace
} // namespace itchbt::sim
