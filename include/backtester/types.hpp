#pragma once
#include <cstdint>
#include <variant>
#include <string>

// ============================================================
// Core types for the deterministic backtester.
//
// Design notes:
//  - All prices are int64_t in cents (e.g. $100.50 = 10050).
//    Fixed-point avoids float non-determinism across platforms.
//  - Timestamps are int64_t nanoseconds since epoch.
//  - Quantities are int64_t shares/lots.
// ============================================================

namespace bt {

using Timestamp = int64_t;   // nanoseconds
using Price     = int64_t;   // cents (fixed-point: $1.00 = 100)
using Quantity  = int64_t;   // shares / lots
using OrderId   = uint64_t;

// -----------------------------------------------------------
// Side / OrderKind
// -----------------------------------------------------------
enum class Side : uint8_t { BUY = 0, SELL = 1 };

inline Side opposite(Side s) {
    return s == Side::BUY ? Side::SELL : Side::BUY;
}

enum class OrderKind : uint8_t { MARKET = 0, LIMIT = 1 };

// -----------------------------------------------------------
// EventKind
// -----------------------------------------------------------
enum class EventKind : uint8_t {
    MARKET_ADD    = 0,   // new resting order in the book
    MARKET_MODIFY = 1,   // size change on an existing resting order
    MARKET_DELETE = 2,   // remove resting order (cancel/full fill from feed)
    MARKET_TRADE  = 3,   // reported trade (price + aggressor side)
    ORDER_ARRIVE  = 4,   // strategy order arrives at exchange (after latency)
    ORDER_FILL    = 5,   // fill/partial fill returned to strategy
    ORDER_CANCEL  = 6,   // cancel confirmation
};

// -----------------------------------------------------------
// Payloads (one per EventKind; use std::variant for type safety)
// -----------------------------------------------------------

// MARKET_ADD / MARKET_MODIFY / MARKET_DELETE
struct MarketOrderPayload {
    OrderId  order_id;
    Side     side;
    Price    price;
    Quantity qty;      // 0 for DELETE
};

// MARKET_TRADE
struct TradePayload {
    Price    price;
    Quantity qty;
    Side     aggressor;   // side that was aggressive (took liquidity)
};

// Order submitted by strategy; also used for ORDER_ARRIVE
struct Order {
    OrderId   id       = 0;
    Side      side     = Side::BUY;
    OrderKind kind     = OrderKind::LIMIT;
    Price     price    = 0;     // 0 for market orders
    Quantity  qty      = 0;
    Timestamp submit_ns = 0;    // sim-time of strategy submission
    Price     mid_at_submit = 0; // for slippage calc
};

// Fill confirmation
struct Fill {
    OrderId   order_id  = 0;
    Price     fill_price = 0;
    Quantity  fill_qty   = 0;
    Timestamp fill_ns    = 0;
    Price     slippage   = 0;   // fill_price - mid_at_submit (+ = bad for buyer)
};

// Cancel confirmation
struct CancelConf {
    OrderId order_id = 0;
};

using EventPayload = std::variant<
    MarketOrderPayload,
    TradePayload,
    Order,
    Fill,
    CancelConf,
    std::monostate  // for timer / sentinel events
>;

// -----------------------------------------------------------
// Event — the fundamental unit in the simulation.
//
// Total ordering: (ts, seq, det_id)
//   ts      — nanosecond timestamp (primary sort key)
//   seq     — feed sequence number; breaks ties within same ts
//   det_id  — a monotone counter assigned at enqueue time;
//             guarantees a total order even if ts==seq match
//             (e.g. strategy orders submitted at the same tick).
// -----------------------------------------------------------
struct Event {
    Timestamp    ts      = 0;
    uint64_t     seq     = 0;
    uint64_t     det_id  = 0;   // assigned by DeterministicEventQueue
    EventKind    kind    = EventKind::MARKET_ADD;
    EventPayload payload = std::monostate{};

    // Total strict weak ordering — no ambiguity.
    bool operator>(const Event& o) const noexcept {
        if (ts     != o.ts)     return ts     > o.ts;
        if (seq    != o.seq)    return seq    > o.seq;
        return det_id > o.det_id;
    }
    bool operator<(const Event& o) const noexcept { return o > *this; }
    bool operator==(const Event& o) const noexcept {
        return ts == o.ts && seq == o.seq && det_id == o.det_id;
    }
};

} // namespace bt
