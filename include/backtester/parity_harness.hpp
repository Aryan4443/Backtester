#pragma once
#include "types.hpp"
#include "strategy.hpp"
#include "pnl_tracker.hpp"
#include <vector>
#include <string>
#include <functional>
#include <memory>

// ============================================================
// ParityHarness
//
// Runs the same strategy through two execution paths on
// identical input data and asserts the decision/fill streams
// are bit-for-bit identical.
//
// BACKTEST path
//   Events are loaded into a DeterministicEventQueue and
//   processed in a tight loop.  This is the normal backtester.
//
// LIVE-REPLAY path
//   Events are delivered one-by-one via a callback handler,
//   mimicking how a production system receives incremental
//   feed updates.  The strategy is called identically; the
//   event loop still uses a DeterministicEventQueue internally.
//
// Why does this matter?
//   Common bugs caught by this harness that a normal backtest
//   misses:
//   1. Look-ahead bias: a strategy that reads from a future
//      snapshot will produce different decisions in the live-
//      replay path (which doesn't have that snapshot yet).
//   2. Wall-clock leakage: any code that reads real time will
//      diverge because the two paths execute at different wall-
//      clock instants.
//   3. Hash-map ordering bugs: std::unordered_map iteration
//      order can vary between runs; if it appears consistent
//      within a single run but varies between runs, only the
//      parity harness (which runs twice back-to-back) catches it.
//   4. State mutation: if the strategy object carries state that
//      is not reset between runs, the second path sees polluted
//      state.
//
// Hash function:
//   We compute a running FNV-1a 64-bit hash over the sequence
//   of (order_id, fill_price, fill_qty, fill_ns) tuples for
//   every fill.  Two identical fill streams produce identical
//   hashes.
// ============================================================

namespace bt {

struct RunResult {
    std::vector<Fill> fills;
    PnLTracker        pnl;
    uint64_t          stream_hash = 0;  // FNV-1a over fill tuples
};

class ParityHarness {
public:
    // Raw event list (pre-parsed).
    using EventList = std::vector<Event>;

    // A factory that returns a fresh Strategy with reset state.
    // Required because each path must start from identical strategy state.
    using StrategyFactory = std::function<std::unique_ptr<Strategy>()>;

    // Run both paths.  Returns true if hashes match.
    bool run(const EventList& events,
             StrategyFactory  factory,
             Timestamp        latency_ns = 50'000,
             bool             verbose    = false);

    // After run(), inspect individual results.
    [[nodiscard]] const RunResult& backtest_result()   const noexcept { return bt_result_; }
    [[nodiscard]] const RunResult& live_replay_result() const noexcept { return lr_result_; }

    [[nodiscard]] bool last_parity_ok() const noexcept { return last_ok_; }

    // Utility: compute FNV-1a hash of a fill stream.
    static uint64_t hash_fills(const std::vector<Fill>& fills);

private:
    RunResult run_backtest(const EventList& events,
                           Strategy& strategy,
                           Timestamp latency_ns);

    RunResult run_live_replay(const EventList& events,
                              Strategy& strategy,
                              Timestamp latency_ns);

    RunResult bt_result_;
    RunResult lr_result_;
    bool      last_ok_ = false;
};

} // namespace bt
