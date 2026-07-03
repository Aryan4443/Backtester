#pragma once
#include "types.hpp"
#include "sim_book.hpp"
#include "sim_clock.hpp"
#include "event_queue.hpp"
#include <map>
#include <vector>

// ============================================================
// ExecutionSimulator
//
// Models realistic fill mechanics:
//
// 1. Latency
//    An order submitted at sim-time T becomes active at T + L.
//    We implement this by enqueuing an ORDER_ARRIVE event at T+L.
//    All market events in [T, T+L) will fire first (they have
//    earlier timestamps), naturally applying adverse selection
//    before the order rests or fills.
//    Assumption: L is configurable; default 50 microseconds.
//
// 2. Market orders
//    Walk the book from best level, filling greedily across
//    price levels until qty is satisfied or the book is exhausted.
//    Slippage = (avg fill price - mid at submit time) * side_sign.
//
// 3. Aggressive limit orders (cross the spread)
//    Treated as market orders capped at the limit price.
//
// 4. Passive limit orders (rest in the book)
//    Queue position: on arrival, record size_ahead = current
//    total qty at that level.  Decrement size_ahead on each
//    MARKET_TRADE at that price.  Fill when size_ahead == 0.
//    Cancellation assumption: FIFO cancel — cancellations of
//    other orders at the same level do NOT advance our position.
//    This is conservative (realistic for most exchanges).
//
// 5. Partial fills
//    Any unfilled remainder stays resting and is tracked.
// ============================================================

namespace bt {

struct QueuePosition {
    Order    order;
    Quantity size_ahead = 0;    // cumulative qty ahead of us at the price level
    Quantity filled_qty = 0;    // how much has been filled so far
};

class ExecutionSimulator {
public:
    explicit ExecutionSimulator(Timestamp latency_ns = 50'000)
        : latency_ns_(latency_ns)
    {}

    // Called by the event loop when the strategy submits an order.
    // Returns an ORDER_ARRIVE event scheduled at clock.now() + latency.
    [[nodiscard]] Event on_order_submit(Order order,
                                        const SimBook& book,
                                        const SimClock& clock,
                                        DeterministicEventQueue& queue);

    // Called by the event loop when an ORDER_ARRIVE event fires.
    // Processes fills, emits FILL events back into the queue.
    // Returns fills generated (may be empty for a resting limit).
    std::vector<Fill> on_order_arrive(const Order& order,
                                      SimBook& book,
                                      const SimClock& clock,
                                      DeterministicEventQueue& queue);

    // Called for every MARKET_TRADE event — updates queue positions.
    // Returns any fills triggered by the trade consuming our queue position.
    std::vector<Fill> on_market_trade(const TradePayload& trade,
                                      const SimClock& clock);

    // Cancel a resting limit order.
    // Returns true if found; emits ORDER_CANCEL event into queue.
    bool cancel_order(OrderId id,
                      SimBook& book,
                      const SimClock& clock,
                      DeterministicEventQueue& queue);

    [[nodiscard]] Timestamp latency() const noexcept { return latency_ns_; }

    // Assign order IDs deterministically.
    [[nodiscard]] OrderId next_order_id() noexcept { return next_order_id_++; }

    // Access pending queue positions (for testing).
    [[nodiscard]] const std::map<OrderId, QueuePosition>& queue_positions() const noexcept {
        return queue_positions_;
    }

private:
    Timestamp latency_ns_;
    OrderId   next_order_id_ = 1;

    // Resting limit orders awaiting queue-position fills.
    // std::map for deterministic iteration.
    std::map<OrderId, QueuePosition> queue_positions_;

    // Walk the book and fill a marketable order.
    std::vector<Fill> fill_market_order(Order order,
                                        SimBook& book,
                                        const SimClock& clock);

    // Attempt to fill an aggressive limit (crosses spread).
    std::vector<Fill> fill_aggressive_limit(Order order,
                                            SimBook& book,
                                            const SimClock& clock);

    // Post a passive limit into the queue tracking structure.
    void post_passive_limit(const Order& order, const SimBook& book);
};

} // namespace bt
