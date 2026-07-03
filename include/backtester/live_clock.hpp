#pragma once
#include "sim_clock.hpp"
#include <chrono>

// ============================================================
// LiveClock — extends SimClock with a wall-clock tick().
//
// The strategy still receives const SimClock& — it cannot tell
// it is running in live mode.  tick() is called by the live
// engine's watchdog / risk layer only; the event loop advances
// the clock via advance(e.ts) exactly as the backtester does.
//
// WARNING: do NOT call tick() on file-based (synthetic) feeds.
// File timestamps are not wall-clock times; mixing them will
// trigger the monotonicity guard in SimClock::advance().
// ============================================================

namespace bt {

class LiveClock : public SimClock {
public:
    // Advance to current wall-clock time (nanoseconds since epoch).
    // Safe to call repeatedly — monotonicity guard ignores no-ops.
    void tick() {
        auto ns = std::chrono::high_resolution_clock::now()
                      .time_since_epoch().count();
        advance(static_cast<Timestamp>(ns));
    }
};

} // namespace bt
