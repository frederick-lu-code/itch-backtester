#pragma once

// The latency queue: everything that happens "later" in simulation time.
//
// A strategy's order takes effect at now + order latency, a cancel at now + cancel latency,
// and the exchange's answer reaches the strategy after a further ack latency. All of these,
// and timers, are events in one binary heap ordered by release time, with ties broken by the
// order in which they were queued. That total order is what makes a run reproducible.
//
// The heap lives in storage allocated once at start-up. Pushing onto a full queue fails
// instead of growing.

#include "itchbt/sim/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace itchbt::sim {

enum class EventKind : std::uint8_t {
    NewOrder, // an order reaches the exchange
    Cancel,   // a cancel request reaches the exchange
    Update,   // an order update reaches the strategy
    Timer,    // a timer the strategy set fires
};

struct Event {
    Timestamp release = 0;
    std::uint64_t seq = 0; // assigned by the queue
    OrderUpdate update;    // Update: the payload. NewOrder, Cancel: only update.id is used.
    EventKind kind = EventKind::Timer;
};

class EventQueue {
public:
    explicit EventQueue(std::size_t capacity) { heap_.reserve(capacity); }

    /// Queues an event. Returns false, changing nothing, if the queue is full.
    [[nodiscard]] bool push(Event event) {
        if (heap_.size() == heap_.capacity()) {
            return false;
        }
        event.seq = next_seq_++;
        heap_.push_back(event); // never reallocates: size < capacity
        std::ranges::push_heap(heap_, later);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return heap_.capacity(); }
    /// The next event to release. Undefined if empty.
    [[nodiscard]] const Event& top() const noexcept { return heap_.front(); }

    /// Removes and returns the next event. Undefined if empty.
    Event pop() {
        std::ranges::pop_heap(heap_, later);
        const Event event = heap_.back();
        heap_.pop_back();
        return event;
    }

private:
    // push_heap builds a max-heap, so "less" must mean "released later".
    static bool later(const Event& a, const Event& b) noexcept {
        return a.release != b.release ? a.release > b.release : a.seq > b.seq;
    }

    std::vector<Event> heap_;
    std::uint64_t next_seq_ = 0;
};

} // namespace itchbt::sim
