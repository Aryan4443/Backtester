#pragma once
#include "order_router.hpp"
#include "execution_sim.hpp"

// ============================================================
// PaperOrderRouter
//
// Wraps ExecutionSimulator to give you a fully functional
// OrderRouter without a real exchange connection.
//
// How it works:
//   send()             → exec_.on_order_submit() → pushes ORDER_ARRIVE
//                        into the engine's queue at T + latency.
//   on_order_arrive()  → exec_.on_order_arrive() → may return fills
//                        (aggressive limits / market orders).
//   on_market_trade()  → exec_.on_market_trade() → returns fills for
//                        any passive limits whose queue position clears.
//   poll()             → always empty (fills surface via the two hooks).
//   cancel()           → exec_.cancel_order() → pushes ORDER_CANCEL.
//
// Shadow sim usage:
//   Keep a PaperOrderRouter alongside a real OrderRouter.  Run both
//   in parallel; compare fill streams to calibrate latency / fill
//   rate assumptions without risking real capital.
// ============================================================

namespace bt {

class PaperOrderRouter : public OrderRouter {
public:
    explicit PaperOrderRouter(Timestamp latency_ns = 50'000)
        : exec_(latency_ns) {}

    // ---- OrderRouter interface ----

    OrderId send(Order& o) override {
        (void)exec_.on_order_submit(o, *book_, *clock_, *queue_);
        return o.id;
    }

    bool cancel(OrderId id) override {
        return exec_.cancel_order(id, *book_, *clock_, *queue_);
    }

    // Fills come via on_order_arrive / on_market_trade, not poll().
    [[nodiscard]] ExecReport poll() override { return {}; }

    [[nodiscard]] bool healthy() const noexcept override { return book_ != nullptr; }

    std::vector<Fill> on_order_arrive(const Order& o) override {
        return exec_.on_order_arrive(o, *book_, *clock_, *queue_);
    }

    std::vector<Fill> on_market_trade(const TradePayload& t) override {
        return exec_.on_market_trade(t, *clock_);
    }

    [[nodiscard]] std::vector<OrderId> resting_ids() const override {
        std::vector<OrderId> ids;
        ids.reserve(exec_.queue_positions().size());
        for (auto& [id, _] : exec_.queue_positions())
            ids.push_back(id);
        return ids;
    }

    void wire(SimBook* book, SimClock* clock, DeterministicEventQueue* queue) override {
        book_  = book;
        clock_ = clock;
        queue_ = queue;
    }

    // ---- Direct access for testing ----
    [[nodiscard]] ExecutionSimulator& exec() noexcept { return exec_; }

private:
    ExecutionSimulator       exec_;
    SimBook*                 book_  = nullptr;
    SimClock*                clock_ = nullptr;
    DeterministicEventQueue* queue_ = nullptr;
};

} // namespace bt
