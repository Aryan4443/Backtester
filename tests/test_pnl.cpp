#include <gtest/gtest.h>
#include "backtester/pnl_tracker.hpp"

using namespace bt;

static Fill make_fill(OrderId id, Price price, Quantity qty,
                      Timestamp ts = 1000, Price slippage = 0) {
    Fill f;
    f.order_id   = id;
    f.fill_price = price;
    f.fill_qty   = qty;   // positive = buy, negative = sell
    f.fill_ns    = ts;
    f.slippage   = slippage;
    return f;
}

// ----------------------------------------------------------------
// TEST 1: Buy then sell at the same price → zero PnL.
// ----------------------------------------------------------------
TEST(PnLTracker, BuySellZeroPnL) {
    PnLTracker pnl;
    pnl.on_fill(make_fill(1, 10000, 100));   // buy 100 at $100.00
    pnl.on_fill(make_fill(2, 10000, -100));  // sell 100 at $100.00

    EXPECT_EQ(pnl.position(), 0);
    EXPECT_NEAR(pnl.realized_pnl(), 0.0, 1e-9);
    EXPECT_NEAR(pnl.total_pnl(10000), 0.0, 1e-9);
}

// ----------------------------------------------------------------
// TEST 2: Buy low, sell high → positive PnL.
// ----------------------------------------------------------------
TEST(PnLTracker, BuyLowSellHigh) {
    PnLTracker pnl;
    pnl.on_fill(make_fill(1, 9900, 100));    // buy 100 @ $99.00
    pnl.on_fill(make_fill(2, 10100, -100));  // sell 100 @ $101.00

    EXPECT_EQ(pnl.position(), 0);
    // PnL = (101.00 - 99.00) * 100 = $200.00
    EXPECT_NEAR(pnl.realized_pnl(), 200.0, 1e-6);
}

// ----------------------------------------------------------------
// TEST 3: Unrealized PnL with open position.
// ----------------------------------------------------------------
TEST(PnLTracker, UnrealizedPnL) {
    PnLTracker pnl;
    pnl.on_fill(make_fill(1, 9900, 50));  // buy 50 @ $99.00

    // Mark at $100.00 → unrealized = (100.00 - 99.00) * 50 = $50.00
    EXPECT_NEAR(pnl.unrealized_pnl(10000), 50.0, 1e-6);
    EXPECT_NEAR(pnl.total_pnl(10000),      50.0, 1e-6);
    EXPECT_EQ(pnl.position(), 50);
}

// ----------------------------------------------------------------
// TEST 4: Slippage tracking.
// ----------------------------------------------------------------
TEST(PnLTracker, SlippageAccumulation) {
    PnLTracker pnl;
    pnl.on_fill(make_fill(1, 10010, 100,  1000, 10));  // 10 cents slippage
    pnl.on_fill(make_fill(2, 10020, -100, 2000, 20));  // 20 cents slippage

    EXPECT_EQ(pnl.fill_count(), 2u);
    EXPECT_NEAR(pnl.avg_slippage_cents(), 15.0, 1e-9);  // (10+20)/2
    EXPECT_NEAR(pnl.total_slippage_cents(), 30.0, 1e-9);
}

// ----------------------------------------------------------------
// TEST 5: Multiple partial fills on the same order.
// ----------------------------------------------------------------
TEST(PnLTracker, PartialFills) {
    PnLTracker pnl;
    pnl.on_fill(make_fill(1, 10000, 40));  // partial fill 1
    pnl.on_fill(make_fill(1, 10005, 60));  // partial fill 2

    EXPECT_EQ(pnl.position(), 100);
    EXPECT_EQ(pnl.fill_count(), 2u);
}

// ----------------------------------------------------------------
// TEST 6: Position tracking after several round trips.
// ----------------------------------------------------------------
TEST(PnLTracker, MultipleRoundTrips) {
    PnLTracker pnl;

    // Round trip 1: +$50 profit
    pnl.on_fill(make_fill(1, 10000, 10));
    pnl.on_fill(make_fill(2, 10500, -10));

    // Round trip 2: -$25 loss
    pnl.on_fill(make_fill(3, 10400, 10));
    pnl.on_fill(make_fill(4, 10150, -10));

    EXPECT_EQ(pnl.position(), 0);
    // PnL = (105.00-100.00)*10 + (101.50-104.00)*10 = 50 - 25 = 25
    EXPECT_NEAR(pnl.realized_pnl(), 25.0, 1e-4);
}
