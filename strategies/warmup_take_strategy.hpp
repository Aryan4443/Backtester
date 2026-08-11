#pragma once
#include "backtester/strategy.hpp"

// ============================================================
// WarmupTakeStrategy — demo strategy for flat synthetic tapes.
//
// mid_cross needs mid to move by several bps. itch_gen keeps mid
// pinned near $100, so mid_cross never fires. This strategy:
//   - waits until a two-sided book exists for `warmup` events
//   - then crosses the spread once (buy 1 @ best ask)
// so live_trader / paper routing can be smoke-tested end-to-end.
// ============================================================

namespace bt {

class WarmupTakeStrategy : public Strategy {
public:
    explicit WarmupTakeStrategy(int warmup = 50, Quantity lot = 1)
        : warmup_(warmup), lot_(lot) {}

    std::vector<Order> on_event(const MarketView& view,
                                const SimClock&   clock) override
    {
        if (done_ || !view.has_market()) return {};
        if (++seen_ < warmup_) return {};

        auto ba = view.best_ask();
        if (!ba) return {};

        Order o;
        o.side          = Side::BUY;
        o.kind          = OrderKind::LIMIT;
        o.price         = ba->price;  // marketable: take the ask
        o.qty           = lot_;
        o.submit_ns     = clock.now();
        o.mid_at_submit = view.mid();
        done_ = true;
        return {o};
    }

    void on_fill(const Fill& fill) override {
        position_ += fill.fill_qty;
    }

    [[nodiscard]] Quantity position() const noexcept { return position_; }

private:
    int       warmup_;
    Quantity  lot_;
    int       seen_     = 0;
    bool      done_     = false;
    Quantity  position_ = 0;
};

} // namespace bt
