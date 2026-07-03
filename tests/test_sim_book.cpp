#include <gtest/gtest.h>
#include "backtester/sim_book.hpp"

using namespace bt;

static Event add_event(OrderId id, Side side, Price p, Quantity q) {
    Event e;
    e.ts = 1000;
    e.kind = EventKind::MARKET_ADD;
    e.payload = MarketOrderPayload{id, side, p, q};
    return e;
}
static Event del_event(OrderId id, Side side, Price p) {
    Event e;
    e.ts = 2000;
    e.kind = EventKind::MARKET_DELETE;
    e.payload = MarketOrderPayload{id, side, p, 0};
    return e;
}
static Event trade_event(Price p, Quantity q, Side aggressor) {
    Event e;
    e.ts = 3000;
    e.kind = EventKind::MARKET_TRADE;
    e.payload = TradePayload{p, q, aggressor};
    return e;
}

// ----------------------------------------------------------------
// TEST: Best bid and ask after adding orders.
// ----------------------------------------------------------------
TEST(SimBook, BestBidAsk) {
    SimBook book;
    book.apply(add_event(1, Side::BUY,  9950, 100));
    book.apply(add_event(2, Side::BUY,  9940, 200));
    book.apply(add_event(3, Side::SELL, 10000, 100));
    book.apply(add_event(4, Side::SELL, 10010, 50));

    auto bb = book.best_bid();
    auto ba = book.best_ask();

    ASSERT_TRUE(bb.has_value());
    ASSERT_TRUE(ba.has_value());
    EXPECT_EQ(bb->first,  9950);
    EXPECT_EQ(bb->second, 100);
    EXPECT_EQ(ba->first,  10000);
    EXPECT_EQ(ba->second, 100);
}

// ----------------------------------------------------------------
// TEST: Mid price.
// ----------------------------------------------------------------
TEST(SimBook, MidPrice) {
    SimBook book;
    book.apply(add_event(1, Side::BUY,  9950, 100));
    book.apply(add_event(2, Side::SELL, 10050, 100));
    EXPECT_EQ(book.mid_price(), 10000);  // (9950 + 10050) / 2
}

// ----------------------------------------------------------------
// TEST: Trade consumes ask levels (buyer is aggressor).
// ----------------------------------------------------------------
TEST(SimBook, TradeConsumesAsk) {
    SimBook book;
    book.apply(add_event(1, Side::SELL, 10000, 50));
    book.apply(add_event(2, Side::BUY,  9950, 100));

    // Buyer takes 50 shares from the ask at 10000
    book.apply(trade_event(10000, 50, Side::BUY));

    auto ba = book.best_ask();
    EXPECT_FALSE(ba.has_value()) << "Ask should be fully consumed";
}

// ----------------------------------------------------------------
// TEST: Partial trade leaves remaining qty.
// ----------------------------------------------------------------
TEST(SimBook, PartialTrade) {
    SimBook book;
    book.apply(add_event(1, Side::SELL, 10000, 100));
    book.apply(add_event(2, Side::BUY,  9950, 100));

    book.apply(trade_event(10000, 60, Side::BUY));

    auto ba = book.best_ask();
    ASSERT_TRUE(ba.has_value());
    EXPECT_EQ(ba->first,  10000);
    EXPECT_EQ(ba->second, 40);   // 100 - 60 = 40 remaining
}

// ----------------------------------------------------------------
// TEST: Delete removes order.
// ----------------------------------------------------------------
TEST(SimBook, DeleteOrder) {
    SimBook book;
    book.apply(add_event(1, Side::BUY, 9950, 100));
    book.apply(add_event(2, Side::BUY, 9950, 200));

    book.apply(del_event(1, Side::BUY, 9950));

    auto bb = book.best_bid();
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->second, 200);   // only order #2 remains
}

// ----------------------------------------------------------------
// TEST: Multiple levels sorted correctly.
// ----------------------------------------------------------------
TEST(SimBook, MultipleLevels) {
    SimBook book;
    book.apply(add_event(1, Side::BUY, 9940, 100));
    book.apply(add_event(2, Side::BUY, 9950, 200));
    book.apply(add_event(3, Side::BUY, 9930, 300));

    auto levels = book.bid_levels(3);
    ASSERT_EQ(levels.size(), 3u);
    EXPECT_EQ(levels[0].price, 9950);  // highest first
    EXPECT_EQ(levels[1].price, 9940);
    EXPECT_EQ(levels[2].price, 9930);
}

// ----------------------------------------------------------------
// TEST: Replay known slice matches expected checkpoints.
// ----------------------------------------------------------------
TEST(SimBook, ReplayCheckpoints) {
    // Scenario: add two bids and one ask, trade 50, delete one bid.
    SimBook book;
    book.apply(add_event(1, Side::BUY,  9950, 100));
    book.apply(add_event(2, Side::BUY,  9940, 200));
    book.apply(add_event(3, Side::SELL, 10000, 80));

    // Checkpoint 1: two-sided market
    EXPECT_EQ(book.best_bid()->first, 9950);
    EXPECT_EQ(book.best_ask()->first, 10000);
    EXPECT_EQ(book.mid_price(), 9975);

    // Trade 50 from the ask
    book.apply(trade_event(10000, 50, Side::BUY));

    // Checkpoint 2: ask partially consumed
    EXPECT_EQ(book.best_ask()->second, 30);

    // Delete bid #1
    book.apply(del_event(1, Side::BUY, 9950));

    // Checkpoint 3: best bid is now 9940
    EXPECT_EQ(book.best_bid()->first, 9940);
}
