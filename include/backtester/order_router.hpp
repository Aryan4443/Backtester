#pragma once
#include "types.hpp"
#include "sim_book.hpp"
#include "sim_clock.hpp"
#include "event_queue.hpp"
#include <vector>

// ============================================================
// OrderRouter — pure interface for exchange connectivity.
//
// Implement this to wrap your broker/exchange API (FIX, native,
// etc.).  The live engine calls send() for new orders and
// cancel() for cancellations, then calls poll() each iteration
// to collect any fills or cancel confirmations.
//
// For paper trading, use PaperOrderRouter (paper_router.hpp)
// which internally runs the ExecutionSimulator.
//
// Thread safety: the live engine calls all methods from a single
// thread.  If your router receives fills on a separate thread,
// buffer them in poll() behind a mutex.
// ============================================================

namespace bt {

struct ExecReport {
    std::vector<Fill>       fills;
    std::vector<CancelConf> cancels;
};

class OrderRouter {
public:
    virtual ~OrderRouter() = default;

    // Submit an order.  The router assigns o.id before sending.
    // Returns assigned OrderId (0 = rejected before send).
    virtual OrderId send(Order& o) = 0;

    // Cancel a resting order.  Returns false if not found.
    virtual bool cancel(OrderId id) = 0;

    // Non-blocking poll for fills/cancels received since last call.
    [[nodiscard]] virtual ExecReport poll() = 0;

    [[nodiscard]] virtual bool healthy() const noexcept = 0;

    // ---- Hooks for paper / shadow simulation ----
    // Real routers return empty here; fills come via poll().
    // PaperOrderRouter overrides both to drive its sim.

    // Called by the engine when an ORDER_ARRIVE event fires.
    virtual std::vector<Fill> on_order_arrive(const Order& /*o*/) { return {}; }

    // Called by the engine on every MARKET_TRADE event.
    virtual std::vector<Fill> on_market_trade(const TradePayload& /*t*/) { return {}; }

    // IDs of all currently resting orders (for cancel-all on kill).
    [[nodiscard]] virtual std::vector<OrderId> resting_ids() const { return {}; }

    // Called once by the engine so the router can hold references to
    // internal engine state (book, clock, queue) it needs.
    // Real routers ignore this; PaperOrderRouter stores the pointers.
    virtual void wire(SimBook* /*book*/,
                      SimClock* /*clock*/,
                      DeterministicEventQueue* /*queue*/) {}
};

} // namespace bt
