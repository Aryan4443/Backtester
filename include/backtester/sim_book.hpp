#pragma once
#include "types.hpp"
#include <map>
#include <optional>
#include <vector>
#include <stdexcept>

// ============================================================
// SimBook — simulated L2 order book.
//
// Design:
//   - std::map (ordered, deterministic iteration) for both
//     bid and ask sides.  Never use unordered_map here.
//   - Bids: std::map<Price, Level, std::greater<Price>>
//     highest bid price first.
//   - Asks: std::map<Price, Level>
//     lowest ask price first.
//   - Each Level stores individual order IDs + quantities to
//     support queue-position tracking in the execution sim.
//
// Applying events:
//   apply(Event) dispatches on EventKind:
//     MARKET_ADD    → insert order at price level
//     MARKET_MODIFY → update order quantity
//     MARKET_DELETE → remove order
//     MARKET_TRADE  → remove qty from the passive side (bids if
//                     aggressor is SELL; asks if aggressor is BUY)
// ============================================================

namespace bt {

struct Level {
    Price    price     = 0;
    Quantity total_qty = 0;
    // Orders at this level in arrival order (std::map for determinism).
    // key = order_id, value = remaining quantity.
    std::map<OrderId, Quantity> orders;

    void add(OrderId id, Quantity q) {
        orders[id] += q;
        total_qty   += q;
    }

    // Returns quantity actually removed (≤ requested).
    Quantity remove(OrderId id, Quantity q) {
        auto it = orders.find(id);
        if (it == orders.end()) return 0;
        Quantity removed = std::min(it->second, q);
        it->second -= removed;
        total_qty  -= removed;
        if (it->second == 0) orders.erase(it);
        return removed;
    }

    bool empty() const noexcept { return orders.empty(); }
};

class SimBook {
public:
    SimBook() = default;

    // Apply one market event (ADD / MODIFY / DELETE / TRADE).
    // Returns the trade if this was a MARKET_TRADE event (for queue tracking).
    void apply(const Event& e);

    // ---- Queries (const, safe for strategy use) ----

    std::optional<std::pair<Price, Quantity>> best_bid() const noexcept;
    std::optional<std::pair<Price, Quantity>> best_ask() const noexcept;

    // Midpoint price (returns 0 if one side is empty).
    [[nodiscard]] Price mid_price() const noexcept;

    // Spread in cents.
    [[nodiscard]] Price spread() const noexcept;

    // Top N levels (up to available depth).
    std::vector<Level> bid_levels(int n = 5) const;
    std::vector<Level> ask_levels(int n = 5) const;

    // Total resting quantity at a price level.
    [[nodiscard]] Quantity qty_at_bid(Price p) const noexcept;
    [[nodiscard]] Quantity qty_at_ask(Price p) const noexcept;

    // Cumulative bid/ask qty above/below a threshold (for queue position).
    // qty_ahead_of_bid(p): total qty resting at price p ahead of any new order
    //   (i.e. the current total_qty at level p).
    [[nodiscard]] Quantity qty_at_bid_level(Price p) const noexcept;
    [[nodiscard]] Quantity qty_at_ask_level(Price p) const noexcept;

    // For walking-the-book simulations: how much volume is available
    // at or better than `price` on the given side.
    [[nodiscard]] Quantity available_liquidity(Side side, Price price) const noexcept;

    // Access raw book (for execution sim to walk levels).
    const std::map<Price, Level, std::greater<Price>>& bids() const noexcept { return bids_; }
    const std::map<Price, Level>&                      asks() const noexcept { return asks_; }

private:
    // Bids: highest price first
    std::map<Price, Level, std::greater<Price>> bids_;
    // Asks: lowest price first
    std::map<Price, Level>                      asks_;

    void apply_add(const MarketOrderPayload& p);
    void apply_modify(const MarketOrderPayload& p);
    void apply_delete(const MarketOrderPayload& p);
    void apply_trade(const TradePayload& p);
};

} // namespace bt
