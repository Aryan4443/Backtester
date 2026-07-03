#include <gtest/gtest.h>
#include "backtester/execution_sim.hpp"
#include "backtester/sim_book.hpp"
#include "backtester/sim_clock.hpp"
#include "backtester/event_queue.hpp"

using namespace bt;

// Helpers
static Event add_event(OrderId id, Side side, Price p, Quantity q, Timestamp ts = 1000) {
    Event e; e.ts = ts; e.kind = EventKind::MARKET_ADD;
    e.payload = MarketOrderPayload{id, side, p, q};
    return e;
}
static Order make_market_order(Side side, Quantity qty, Timestamp submit_ns = 1000) {
    Order o; o.side = side; o.kind = OrderKind::MARKET;
    o.qty = qty; o.submit_ns = submit_ns; o.mid_at_submit = 10000;
    return o;
}
static Order make_limit_order(Side side, Price price, Quantity qty,
                               Timestamp submit_ns = 1000, Price mid = 10000) {
    Order o; o.side = side; o.kind = OrderKind::LIMIT;
    o.price = price; o.qty = qty; o.submit_ns = submit_ns; o.mid_at_submit = mid;
    return o;
}

// ----------------------------------------------------------------
// TEST 1: Market order fills completely at the best level.
// ----------------------------------------------------------------
TEST(ExecutionSim, MarketOrderFillsBestLevel) {
    SimBook book;
    book.apply(add_event(1, Side::SELL, 10010, 200));
    book.apply(add_event(2, Side::BUY,  9990, 200));

    SimClock clock; clock.advance(1050); // after latency
    ExecutionSimulator exec(50);
    DeterministicEventQueue queue;

    Order o = make_market_order(Side::BUY, 100, 1000);
    o.id = exec.next_order_id();

    auto fills = exec.on_order_arrive(o, book, clock, queue);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].fill_qty,   100);
    EXPECT_EQ(fills[0].fill_price, 10010);
    EXPECT_GT(fills[0].slippage,   0) << "Buyer should pay above mid";
}

// ----------------------------------------------------------------
// TEST 2: Market buy order walks TWO levels.
// ----------------------------------------------------------------
TEST(ExecutionSim, MarketOrderWalksTwoLevels) {
    SimBook book;
    // Level 1: 50 shares at 10010
    book.apply(add_event(1, Side::SELL, 10010, 50));
    // Level 2: 100 shares at 10020
    book.apply(add_event(2, Side::SELL, 10020, 100));
    book.apply(add_event(3, Side::BUY,  9990, 200));

    SimClock clock; clock.advance(2000);
    ExecutionSimulator exec(50);
    DeterministicEventQueue queue;

    Order o = make_market_order(Side::BUY, 120, 1950);
    o.id = exec.next_order_id();

    auto fills = exec.on_order_arrive(o, book, clock, queue);

    // Should get two partial fills across two levels
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].fill_price, 10010);
    EXPECT_EQ(fills[0].fill_qty,   50);
    EXPECT_EQ(fills[1].fill_price, 10020);
    EXPECT_EQ(fills[1].fill_qty,   70);  // 120 - 50 = 70 from level 2

    // Slippage should be positive (we paid above mid) for both fills
    EXPECT_GE(fills[0].slippage, 0);
    EXPECT_GE(fills[1].slippage, 0);
    EXPECT_GT(fills[1].slippage, fills[0].slippage) << "Deeper fill = more slippage";
}

// ----------------------------------------------------------------
// TEST 3: Limit order arrives after the price moves away — no fill.
// The latency model means market events in [T, T+L) fire first,
// so an adverse move before T+L can eliminate our fill opportunity.
// ----------------------------------------------------------------
TEST(ExecutionSim, DelayedOrderMissesPrice) {
    SimBook book;
    book.apply(add_event(1, Side::SELL, 10010, 100));
    book.apply(add_event(2, Side::BUY,  9990, 100));

    SimClock clock; clock.advance(2000); // ORDER_ARRIVE fires here
    ExecutionSimulator exec(50);
    DeterministicEventQueue queue;

    // Limit buy at 10010 — but by arrive time, ask has moved to 10020
    // Simulate this by deleting the 10010 ask before calling on_order_arrive
    Event del; del.ts = 1500; del.kind = EventKind::MARKET_DELETE;
    del.payload = MarketOrderPayload{1, Side::SELL, 10010, 0};
    book.apply(del);
    Event add2; add2.ts = 1600; add2.kind = EventKind::MARKET_ADD;
    add2.payload = MarketOrderPayload{3, Side::SELL, 10020, 100};
    book.apply(add2);

    // Now order arrives at 2000 — ask is at 10020, our limit is 10010 → rest passively
    Order o = make_limit_order(Side::BUY, 10010, 50, 1950, 10000);
    o.id = exec.next_order_id();

    auto fills = exec.on_order_arrive(o, book, clock, queue);
    EXPECT_TRUE(fills.empty()) << "Order should rest — price moved away";

    // Verify it is in queue tracking
    EXPECT_EQ(exec.queue_positions().count(o.id), 1u);
}

// ----------------------------------------------------------------
// TEST 4: ORDER_ARRIVE event is scheduled at submit_time + latency.
// ----------------------------------------------------------------
TEST(ExecutionSim, LatencySchedulesArrival) {
    SimBook book;
    book.apply(add_event(1, Side::SELL, 10010, 100));
    book.apply(add_event(2, Side::BUY,  9990, 100));

    SimClock clock; clock.advance(5000);
    ExecutionSimulator exec(50'000); // 50µs latency
    DeterministicEventQueue queue;

    Order o = make_market_order(Side::BUY, 10, 5000);
    Event arrive = exec.on_order_submit(o, book, clock, queue);

    EXPECT_EQ(arrive.ts, 5000 + 50'000) << "Arrive event must be at T+L";
    EXPECT_EQ(arrive.kind, EventKind::ORDER_ARRIVE);
    EXPECT_EQ(queue.size(), 1u);
}

// ----------------------------------------------------------------
// TEST 5: Sell market order fills bids correctly (negative fill_qty).
// ----------------------------------------------------------------
TEST(ExecutionSim, SellMarketOrder) {
    SimBook book;
    book.apply(add_event(1, Side::BUY,  9990, 100));
    book.apply(add_event(2, Side::SELL, 10010, 100));

    SimClock clock; clock.advance(2000);
    ExecutionSimulator exec(50);
    DeterministicEventQueue queue;

    Order o = make_market_order(Side::SELL, 50, 1950);
    o.id = exec.next_order_id();

    auto fills = exec.on_order_arrive(o, book, clock, queue);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].fill_qty, -50) << "Sell fills should be negative";
    EXPECT_EQ(fills[0].fill_price, 9990);
}
