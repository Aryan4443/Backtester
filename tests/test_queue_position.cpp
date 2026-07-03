#include <gtest/gtest.h>
#include "backtester/execution_sim.hpp"
#include "backtester/sim_book.hpp"
#include "backtester/sim_clock.hpp"
#include "backtester/event_queue.hpp"

using namespace bt;

// ----------------------------------------------------------------
// Queue-position modeling tests.
//
// Assumption (documented in DESIGN_NOTES.md):
//   FIFO cancel — when another order at our price level is
//   cancelled, it does NOT advance our queue position.
//   We only advance when volume TRADES through.
// ----------------------------------------------------------------

static Event add_ev(OrderId id, Side side, Price p, Quantity q, Timestamp ts = 100) {
    Event e; e.ts = ts; e.kind = EventKind::MARKET_ADD;
    e.payload = MarketOrderPayload{id, side, p, q};
    return e;
}
// trade_ev used inline via TradePayload directly in tests below

// ----------------------------------------------------------------
// TEST 1: Limit order fills only after enough volume trades through.
// ----------------------------------------------------------------
TEST(QueuePosition, FillsAfterVolumeTradesThrough) {
    // Setup: 200 shares ahead of us at the ask (10010).
    // We post a limit sell for 50 at 10010.
    // We should NOT fill after 100 trades, but SHOULD fill after 200 trades.

    SimBook book;
    book.apply(add_ev(1, Side::SELL, 10010, 200));  // 200 ahead of us
    book.apply(add_ev(2, Side::BUY,  9990, 100));

    SimClock clock; clock.advance(150);
    ExecutionSimulator exec(0); // zero latency for simplicity
    DeterministicEventQueue queue;

    // Post our passive sell limit at 10010
    Order o;
    o.id  = exec.next_order_id();
    o.side = Side::SELL;
    o.kind = OrderKind::LIMIT;
    o.price = 10010;
    o.qty   = 50;
    o.submit_ns     = 150;
    o.mid_at_submit = 10000;

    // Manually post (simulate arriving at the exchange)
    exec.on_order_arrive(o, book, clock, queue);

    // Verify queued with 200 ahead
    ASSERT_EQ(exec.queue_positions().count(o.id), 1u);
    EXPECT_EQ(exec.queue_positions().at(o.id).size_ahead, 200);

    // Trade 100 — should NOT fill (still 100 ahead)
    clock.advance(200);
    auto fills1 = exec.on_market_trade(
        TradePayload{10010, 100, Side::BUY}, clock);
    EXPECT_TRUE(fills1.empty()) << "Should not fill — still 100 ahead";
    EXPECT_EQ(exec.queue_positions().at(o.id).size_ahead, 100);

    // Trade another 100 — queue exhausted, should fill
    clock.advance(300);
    auto fills2 = exec.on_market_trade(
        TradePayload{10010, 100, Side::BUY}, clock);
    ASSERT_EQ(fills2.size(), 1u) << "Should fill after queue exhausted";
    EXPECT_EQ(fills2[0].fill_qty,   -50) << "Sell fill is negative";
    EXPECT_EQ(fills2[0].fill_price, 10010);

    // Order should be removed from queue tracking
    EXPECT_EQ(exec.queue_positions().count(o.id), 0u);
}

// ----------------------------------------------------------------
// TEST 2: Trade at a different price does NOT affect our position.
// ----------------------------------------------------------------
TEST(QueuePosition, TradeAtDifferentPriceIgnored) {
    SimBook book;
    book.apply(add_ev(1, Side::SELL, 10010, 100));
    book.apply(add_ev(2, Side::BUY,  9990, 100));

    SimClock clock; clock.advance(150);
    ExecutionSimulator exec(0);
    DeterministicEventQueue queue;

    Order o;
    o.id  = exec.next_order_id();
    o.side = Side::SELL; o.kind = OrderKind::LIMIT;
    o.price = 10010; o.qty = 50;
    o.submit_ns = 150; o.mid_at_submit = 10000;
    exec.on_order_arrive(o, book, clock, queue);

    // Trade at a different price (10020) — should not affect our 10010 queue
    clock.advance(200);
    auto fills = exec.on_market_trade(
        TradePayload{10020, 100, Side::BUY}, clock);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(exec.queue_positions().at(o.id).size_ahead, 100);
}

// ----------------------------------------------------------------
// TEST 3: FIFO cancel assumption — cancellations don't advance us.
// ----------------------------------------------------------------
TEST(QueuePosition, FIFOCancelDoesNotAdvance) {
    SimBook book;
    // 200 ahead of us
    book.apply(add_ev(1, Side::SELL, 10010, 200));
    book.apply(add_ev(2, Side::BUY,  9990, 100));

    SimClock clock; clock.advance(150);
    ExecutionSimulator exec(0);
    DeterministicEventQueue queue;

    Order o;
    o.id  = exec.next_order_id();
    o.side = Side::SELL; o.kind = OrderKind::LIMIT;
    o.price = 10010; o.qty = 50;
    o.submit_ns = 150; o.mid_at_submit = 10000;
    exec.on_order_arrive(o, book, clock, queue);

    // Simulate cancellation of the 200-share order ahead
    Event del; del.ts = 160; del.kind = EventKind::MARKET_DELETE;
    del.payload = MarketOrderPayload{1, Side::SELL, 10010, 0};
    book.apply(del);

    // Our size_ahead should NOT change (FIFO cancel assumption)
    EXPECT_EQ(exec.queue_positions().at(o.id).size_ahead, 200)
        << "FIFO cancel: cancellations don't advance our position";
}

// ----------------------------------------------------------------
// TEST 4: Cancel our own resting order.
// ----------------------------------------------------------------
TEST(QueuePosition, CancelOwnOrder) {
    SimBook book;
    book.apply(add_ev(1, Side::BUY, 9990, 100));

    SimClock clock; clock.advance(100);
    ExecutionSimulator exec(0);
    DeterministicEventQueue queue;

    Order o;
    o.id  = exec.next_order_id();
    o.side = Side::SELL; o.kind = OrderKind::LIMIT;
    o.price = 10010; o.qty = 50;
    o.submit_ns = 100; o.mid_at_submit = 10000;
    exec.on_order_arrive(o, book, clock, queue);

    ASSERT_EQ(exec.queue_positions().count(o.id), 1u);

    bool cancelled = exec.cancel_order(o.id, book, clock, queue);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(exec.queue_positions().count(o.id), 0u);
}
