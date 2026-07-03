#pragma once
#include "types.hpp"
#include "sim_book.hpp"
#include "live_clock.hpp"
#include "event_queue.hpp"
#include "market_view.hpp"
#include "pnl_tracker.hpp"
#include "strategy.hpp"
#include "feed_handler.hpp"
#include "order_router.hpp"
#include "risk.hpp"
#include <atomic>
#include <cstddef>

// ============================================================
// LiveEngine — production event loop.
//
// Replaces the manual event loop in main.cpp with a reusable
// component that wires FeedHandler → SimBook → Strategy →
// OrderRouter → RiskCheck in the correct order.
//
// Event processing order per iteration:
//   1. feed_.poll()              — get new market events
//   2. queue_.pop()              — next event in timestamp order
//   3. clock_.advance(e.ts)      — sim clock tracks event time
//   4. book_.apply(e)            — book updated first
//   5. router_.on_market_trade() — passive limit fills from queue pos
//   6. router_.on_order_arrive() — aggressive/market fills on arrival
//   7. router_.poll()            — real exchange fills (non-paper path)
//   8. strategy_.on_event()      — strategy sees current book snapshot
//   9. risk_.allow() + send()    — orders pass risk gate before routing
//  10. risk_.check_kill()        — hard floor check; cancel-all on trip
//
// Thread safety:
//   run() blocks the calling thread.  Call stop() from another thread
//   (e.g. a signal handler) to trigger a clean shutdown.
// ============================================================

namespace bt {

struct LiveEngineConfig {
    bool        verbose         = false;
    std::size_t equity_sample_n = 1000;   // record equity every N events
};

class LiveEngine {
public:
    LiveEngine(FeedHandler&     feed,
               OrderRouter&     router,
               Strategy&        strategy,
               RiskCheck&       risk,
               LiveEngineConfig cfg = {});

    // Blocks until feed exhausted, stop() called, or risk kill fires.
    void run();

    // Signal the loop to stop cleanly (safe to call from any thread).
    void stop() noexcept { running_ = false; }

    // ---- Accessors (valid after run() returns) ----
    [[nodiscard]] const PnLTracker& pnl()         const noexcept { return pnl_; }
    [[nodiscard]] const SimBook&    book()         const noexcept { return book_; }
    [[nodiscard]] const LiveClock&  clock()        const noexcept { return clock_; }
    [[nodiscard]] std::size_t       order_count()  const noexcept { return order_count_; }
    [[nodiscard]] std::size_t       event_count()  const noexcept { return event_count_; }

private:
    FeedHandler&     feed_;
    OrderRouter&     router_;
    Strategy&        strategy_;
    RiskCheck&       risk_;
    LiveEngineConfig cfg_;

    SimBook   book_;
    LiveClock clock_;
    DeterministicEventQueue queue_;
    PnLTracker pnl_;

    std::size_t       order_count_ = 0;
    std::size_t       event_count_ = 0;
    std::atomic<bool> running_{false};

    void process_event(const Event& e);
    void dispatch_fills(const std::vector<Fill>& fills);
    void call_strategy(const Event& e);
    void cancel_all();
};

} // namespace bt
