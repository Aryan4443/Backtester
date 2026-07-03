#pragma once
#include "types.hpp"
#include <stdexcept>

// ============================================================
// SimClock — the ONLY clock in the simulation.
//
// Design: advances monotonically as events are popped from the
// queue; it NEVER reads wall-clock time (no std::chrono::now(),
// no gettimeofday, no CLOCK_REALTIME).  Enforcing this at the
// type level makes look-ahead impossible: the strategy only
// receives a const SimClock& and can only observe now(), which
// equals the currently-processing event's timestamp.
// ============================================================

namespace bt {

class SimClock {
public:
    SimClock() = default;

    // Current simulation time (nanoseconds since epoch).
    [[nodiscard]] Timestamp now() const noexcept { return now_; }

    // Called by the event loop when an event is popped.
    // Monotonicity is an invariant: the clock never goes backward.
    void advance(Timestamp t) {
        if (t < now_) {
            throw std::logic_error(
                "SimClock: time went backward — event ordering broken");
        }
        now_ = t;
    }

    // Produce a timestamp offset by delta_ns from now.
    // Used by the execution simulator to schedule ORDER_ARRIVE events.
    [[nodiscard]] Timestamp future(Timestamp delta_ns) const noexcept {
        return now_ + delta_ns;
    }

private:
    Timestamp now_ = 0;
};

} // namespace bt
