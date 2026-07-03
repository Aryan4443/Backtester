#pragma once
#include "types.hpp"
#include "market_view.hpp"
#include "sim_clock.hpp"
#include <vector>

// ============================================================
// Strategy — the interface every trading strategy must implement.
//
// on_event() is called by the event loop once per market event,
// after the sim book has been updated and a fresh MarketView
// snapshot built.
//
// Look-ahead guarantee:
//   - MarketView contains only data up to clock.now().
//   - The strategy has no reference to the live SimBook.
//   - The strategy has no reference to future events.
//   - clock is const — the strategy cannot advance it.
//
// The strategy can NOT distinguish between backtest mode and
// live-replay mode.  This is by design: the same object runs
// in both paths, and the parity harness asserts the outputs
// are bit-for-bit identical.
//
// Order IDs:
//   The strategy must not assign order IDs.  The engine assigns
//   them (monotone counter) so IDs are reproducible across runs.
// ============================================================

namespace bt {

class Strategy {
public:
    virtual ~Strategy() = default;

    // Called for every market event (ADD / MODIFY / DELETE / TRADE).
    // Returns zero or more orders to submit.
    // Orders must have id == 0; the engine will assign the real ID.
    [[nodiscard]] virtual std::vector<Order>
    on_event(const MarketView& view, const SimClock& clock) = 0;

    // Called when a fill is confirmed.
    // Default: no-op.  Override to update internal state.
    virtual void on_fill(const Fill& /*fill*/) {}

    // Called when a cancel is confirmed.
    virtual void on_cancel(const CancelConf& /*conf*/) {}

    // Optional: called at end of simulation for cleanup.
    virtual void on_done() {}
};

} // namespace bt
