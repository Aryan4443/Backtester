#pragma once
#include "backtester/strategy.hpp"
#include <deque>
#include <numeric>
#include <cstdint>

// ============================================================
// MidCrossStrategy — trivial example strategy.
//
// Logic:
//   Track a rolling mean of mid-prices over the last N ticks.
//   - If current ask < rolling_mid * (1 - threshold): BUY signal
//     → submit a limit order to buy 1 lot at best_ask.
//   - If current bid > rolling_mid * (1 + threshold): SELL signal
//     → submit a limit order to sell 1 lot at best_bid.
//
// Constraints to avoid over-trading:
//   - At most one open order per side (tracked by flag).
//   - Flat position limit: won't buy if already long >= max_pos.
//
// This strategy is intentionally simple — its purpose is to
// exercise the execution simulator and parity harness, not to
// make money.
// ============================================================

namespace bt {

class MidCrossStrategy : public Strategy {
public:
    explicit MidCrossStrategy(
        int    window_size   = 20,    // rolling window for mid average
        double threshold     = 0.0005, // 5 bps
        Quantity max_position = 5,
        Quantity lot_size     = 1)
        : window_(window_size)
        , threshold_(threshold)
        , max_pos_(max_position)
        , lot_(lot_size)
    {}

    std::vector<Order> on_event(const MarketView& view,
                                const SimClock&   clock) override
    {
        if (!view.has_market()) return {};

        // Update rolling mid
        mid_history_.push_back(static_cast<double>(view.mid()));
        if (static_cast<int>(mid_history_.size()) > window_)
            mid_history_.pop_front();

        if (static_cast<int>(mid_history_.size()) < window_) return {};

        double rolling_mid = std::accumulate(
            mid_history_.begin(), mid_history_.end(), 0.0) / window_;

        auto bb = view.best_bid();
        auto ba = view.best_ask();
        if (!bb || !ba) return {};

        double ask_d = static_cast<double>(ba->price);
        double bid_d = static_cast<double>(bb->price);

        std::vector<Order> orders;

        // BUY signal: ask is cheap relative to rolling mid
        if (ask_d < rolling_mid * (1.0 - threshold_)
            && !has_open_buy_
            && position_ < max_pos_)
        {
            Order o;
            o.side  = Side::BUY;
            o.kind  = OrderKind::LIMIT;
            o.price = ba->price;  // join best ask (aggressive limit)
            o.qty   = lot_;
            o.submit_ns     = clock.now();
            o.mid_at_submit = view.mid();
            orders.push_back(o);
            has_open_buy_ = true;
        }

        // SELL signal: bid is rich relative to rolling mid
        if (bid_d > rolling_mid * (1.0 + threshold_)
            && !has_open_sell_
            && position_ > -max_pos_)
        {
            Order o;
            o.side  = Side::SELL;
            o.kind  = OrderKind::LIMIT;
            o.price = bb->price;  // join best bid (aggressive limit)
            o.qty   = lot_;
            o.submit_ns     = clock.now();
            o.mid_at_submit = view.mid();
            orders.push_back(o);
            has_open_sell_ = true;
        }

        return orders;
    }

    void on_fill(const Fill& fill) override {
        // Update position and clear the open-order flag.
        // We don't know the side from Fill alone, so track via sign convention:
        // positive fill_qty = buy fill.
        // (In a real system you'd look up the order in a map.)
        if (fill.fill_qty > 0) {
            position_     += fill.fill_qty;
            has_open_buy_  = false;
        } else {
            position_     += fill.fill_qty; // fill_qty already negative for sell
            has_open_sell_ = false;
        }
    }

    [[nodiscard]] Quantity position() const noexcept { return position_; }

private:
    int       window_;
    double    threshold_;
    Quantity  max_pos_;
    Quantity  lot_;

    std::deque<double> mid_history_;
    Quantity   position_     = 0;
    bool       has_open_buy_  = false;
    bool       has_open_sell_ = false;
};

} // namespace bt
