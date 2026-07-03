#include "backtester/live_engine.hpp"
#include <iostream>

namespace bt {

LiveEngine::LiveEngine(FeedHandler&     feed,
                       OrderRouter&     router,
                       Strategy&        strategy,
                       RiskCheck&       risk,
                       LiveEngineConfig cfg)
    : feed_(feed), router_(router), strategy_(strategy), risk_(risk), cfg_(cfg)
{
    // Give the router references to our internal state so PaperOrderRouter
    // (and any shadow-sim router) can push ORDER_ARRIVE events into our queue.
    router_.wire(&book_, &clock_, &queue_);
}

// ----------------------------------------------------------------
// run
// ----------------------------------------------------------------
void LiveEngine::run() {
    running_ = true;

    while (running_) {
        // 1. Pump feed into the queue.
        for (auto& e : feed_.poll())
            queue_.push(e);

        // 2. Stop if queue is empty and feed is done.
        if (queue_.empty()) {
            if (!feed_.healthy()) break;
            continue; // spin until next poll delivers events
        }

        // 3. Process one event.
        Event e = queue_.pop();
        clock_.advance(e.ts);
        process_event(e);
        ++event_count_;

        // 4. Collect fills from real exchange path (no-op for paper).
        auto report = router_.poll();
        dispatch_fills(report.fills);
        for (auto& conf : report.cancels)
            strategy_.on_cancel(conf);

        // 5. Equity sampling.
        if (event_count_ % cfg_.equity_sample_n == 0 && book_.mid_price() > 0)
            pnl_.record_equity(clock_.now(), book_.mid_price());

        // 6. Hard kill check.
        if (risk_.check_kill(pnl_, book_.mid_price())) {
            if (cfg_.verbose)
                std::cout << "[LiveEngine] RISK KILL — P&L "
                          << pnl_.total_pnl(book_.mid_price())
                          << " < hard floor " << risk_.limits().hard_floor
                          << " — cancelling all orders\n";
            cancel_all();
            break;
        }
    }

    strategy_.on_done();
}

// ----------------------------------------------------------------
// process_event
// ----------------------------------------------------------------
void LiveEngine::process_event(const Event& e) {
    switch (e.kind) {
        case EventKind::MARKET_ADD:
        case EventKind::MARKET_MODIFY:
        case EventKind::MARKET_DELETE:
            book_.apply(e);
            call_strategy(e);
            break;

        case EventKind::MARKET_TRADE: {
            book_.apply(e);
            // Passive limit fills driven by queue-position tracking.
            dispatch_fills(
                router_.on_market_trade(std::get<TradePayload>(e.payload)));
            call_strategy(e);
            break;
        }

        case EventKind::ORDER_ARRIVE: {
            // Fires for paper router (ORDER_ARRIVE is in our queue).
            // Real routers push nothing here; their fills come from poll().
            dispatch_fills(
                router_.on_order_arrive(std::get<Order>(e.payload)));
            break;
        }

        case EventKind::ORDER_CANCEL:
            strategy_.on_cancel(std::get<CancelConf>(e.payload));
            break;

        default: break;
    }
}

// ----------------------------------------------------------------
// dispatch_fills — fan out a fill vector to pnl + strategy.
// ----------------------------------------------------------------
void LiveEngine::dispatch_fills(const std::vector<Fill>& fills) {
    for (auto& f : fills) {
        pnl_.on_fill(f);
        strategy_.on_fill(f);
    }
}

// ----------------------------------------------------------------
// call_strategy — build MarketView snapshot, call on_event(),
// gate each returned order through risk, then route it.
// ----------------------------------------------------------------
void LiveEngine::call_strategy(const Event& e) {
    MarketView view(book_, clock_.now(), e.kind);
    auto orders = strategy_.on_event(view, clock_);
    Price mark  = book_.mid_price();

    for (auto& o : orders) {
        if (!risk_.allow(o, pnl_, mark)) {
            if (cfg_.verbose)
                std::cout << "[LiveEngine] order rejected by risk (pos="
                          << pnl_.position() << ")\n";
            continue;
        }
        router_.send(o);
        ++order_count_;
    }
}

// ----------------------------------------------------------------
// cancel_all — cancel every resting order (called on kill).
// ----------------------------------------------------------------
void LiveEngine::cancel_all() {
    for (OrderId id : router_.resting_ids())
        router_.cancel(id);
}

} // namespace bt
