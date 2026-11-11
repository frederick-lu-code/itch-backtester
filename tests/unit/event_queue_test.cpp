#include "itchbt/sim/event_queue.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace itchbt::sim {
namespace {

Event event(Timestamp release, OrderId id, EventKind kind = EventKind::NewOrder) {
    Event e;
    e.release = release;
    e.kind = kind;
    e.update.id = id;
    return e;
}

TEST(EventQueue, ReleasesInTimeOrder) {
    EventQueue q(16);
    ASSERT_TRUE(q.push(event(300, 1)));
    ASSERT_TRUE(q.push(event(100, 2)));
    ASSERT_TRUE(q.push(event(200, 3)));
    EXPECT_EQ(q.top().update.id, 2U);
    std::vector<OrderId> order;
    while (!q.empty()) {
        order.push_back(q.pop().update.id);
    }
    EXPECT_EQ(order, (std::vector<OrderId>{2, 3, 1}));
}

TEST(EventQueue, TiesBreakBySubmissionOrder) {
    EventQueue q(64);
    for (OrderId id = 1; id <= 20; ++id) {
        ASSERT_TRUE(q.push(event(500, id)));
    }
    ASSERT_TRUE(q.push(event(499, 99)));
    EXPECT_EQ(q.pop().update.id, 99U);
    for (OrderId id = 1; id <= 20; ++id) {
        EXPECT_EQ(q.pop().update.id, id);
    }
}

TEST(EventQueue, InterleavedPushAndPopKeepsOrder) {
    EventQueue q(8);
    ASSERT_TRUE(q.push(event(10, 1)));
    ASSERT_TRUE(q.push(event(30, 3)));
    EXPECT_EQ(q.pop().update.id, 1U);
    ASSERT_TRUE(q.push(event(20, 2)));
    ASSERT_TRUE(q.push(event(30, 4))); // same time as id 3, queued later
    EXPECT_EQ(q.pop().update.id, 2U);
    EXPECT_EQ(q.pop().update.id, 3U);
    EXPECT_EQ(q.pop().update.id, 4U);
    EXPECT_TRUE(q.empty());
}

TEST(EventQueue, RefusesToGrowPastItsCapacity) {
    EventQueue q(3);
    EXPECT_EQ(q.capacity(), 3U);
    EXPECT_TRUE(q.push(event(1, 1)));
    EXPECT_TRUE(q.push(event(2, 2)));
    EXPECT_TRUE(q.push(event(3, 3)));
    EXPECT_FALSE(q.push(event(0, 4)));
    EXPECT_EQ(q.size(), 3U);
    EXPECT_EQ(q.top().update.id, 1U); // the refused event left no trace
    q.pop();
    EXPECT_TRUE(q.push(event(0, 5)));
    EXPECT_EQ(q.top().update.id, 5U);
}

TEST(EventQueue, CarriesKindAndPayload) {
    EventQueue q(4);
    Event e = event(7, 42, EventKind::Update);
    e.update.kind = UpdateKind::Fill;
    e.update.fill_qty = 100;
    ASSERT_TRUE(q.push(e));
    const Event out = q.pop();
    EXPECT_EQ(out.kind, EventKind::Update);
    EXPECT_EQ(out.update.kind, UpdateKind::Fill);
    EXPECT_EQ(out.update.fill_qty, 100U);
    EXPECT_EQ(out.release, 7U);
}

} // namespace
} // namespace itchbt::sim
