#pragma once
#include "types.hpp"
#include <queue>
#include <vector>
#include <functional>
#include <stdexcept>

// ============================================================
// DeterministicEventQueue
//
// A min-priority queue ordered by (ts, seq, det_id).
//
// Why these three keys?
//   ts      — the primary business ordering (earlier events first)
//   seq     — feed sequence from the market; two events at the
//             same nanosecond should honour the feed's ordering
//   det_id  — a monotone counter we assign at push() time.
//             Guarantees a *total* order even when ts==seq
//             (e.g. multiple strategy orders submitted in the
//             same tick, or ORDER_ARRIVE events we inject).
//
// No std::unordered_* anywhere: std::priority_queue uses a
// comparison functor, iteration order is irrelevant here.
// ============================================================

namespace bt {

class DeterministicEventQueue {
public:
    DeterministicEventQueue() = default;

    // Push an event. Assigns det_id automatically.
    void push(Event e) {
        e.det_id = next_det_id_++;
        pq_.push(std::move(e));
    }

    // Pop the smallest event (lowest ts, then seq, then det_id).
    [[nodiscard]] Event pop() {
        if (pq_.empty()) throw std::runtime_error("EventQueue: pop on empty queue");
        Event e = pq_.top();
        pq_.pop();
        return e;
    }

    [[nodiscard]] const Event& peek() const {
        if (pq_.empty()) throw std::runtime_error("EventQueue: peek on empty queue");
        return pq_.top();
    }

    [[nodiscard]] bool empty() const noexcept { return pq_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return pq_.size(); }

    // Reset det_id counter (for deterministic test replay from scratch).
    void reset_counter() noexcept { next_det_id_ = 0; }

private:
    // Greater<Event> means the queue returns the *smallest* element first
    // (min-heap) — we want earliest timestamp at the top.
    struct Compare {
        bool operator()(const Event& a, const Event& b) const noexcept {
            // priority_queue pops the element for which Compare returns false
            // i.e. the "greatest" element is popped first.
            // We want the *smallest* (earliest) event first, so invert:
            return a > b;   // a > b means b should come first
        }
    };

    std::priority_queue<Event, std::vector<Event>, Compare> pq_;
    uint64_t next_det_id_ = 0;
};

} // namespace bt
