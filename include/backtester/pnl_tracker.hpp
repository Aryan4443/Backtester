#pragma once
#include "types.hpp"
#include <vector>
#include <cstdint>

// ============================================================
// PnLTracker
//
// Tracks: position, cash (in cents), realized PnL, and execution
// quality metrics (slippage, fill count, fill rate).
//
// Why execution quality > raw PnL for strategy evaluation:
//   Raw PnL can look good if the strategy happens to be in the
//   market during a favourable period; slippage and fill rate
//   reveal whether the execution model is realistic, whether
//   the strategy is latency-sensitive, and whether the sim
//   overstates what live trading would achieve.
// ============================================================

namespace bt {

struct FillRecord {
    OrderId   order_id   = 0;
    Price     fill_price = 0;
    Quantity  fill_qty   = 0;   // positive = buy, negative = sell
    Timestamp fill_ns    = 0;
    Price     slippage   = 0;   // cents per share; positive = paid more than mid
};

class PnLTracker {
public:
    PnLTracker() = default;

    // Call once per fill.
    void on_fill(const Fill& f);

    // ---- Snapshot queries ----

    [[nodiscard]] Quantity position()    const noexcept { return position_; }
    [[nodiscard]] double   cash()        const noexcept { return cash_; }
    [[nodiscard]] double   realized_pnl() const noexcept { return realized_pnl_; }

    // Mark-to-market using current mid price (in cents).
    // unrealized = position * (mark_price - avg_cost_price) / 100
    [[nodiscard]] double unrealized_pnl(Price mark) const noexcept {
        return static_cast<double>(position_)
               * (static_cast<double>(mark) - cost_basis_cents_)
               / 100.0;
    }
    [[nodiscard]] double total_pnl(Price mark) const noexcept {
        return realized_pnl_ + unrealized_pnl(mark);
    }

    // ---- Execution quality ----

    [[nodiscard]] double avg_slippage_cents() const noexcept {
        if (fill_count_ == 0) return 0.0;
        return static_cast<double>(total_slippage_cents_) / fill_count_;
    }
    [[nodiscard]] std::size_t fill_count()  const noexcept { return fill_count_; }
    [[nodiscard]] double total_slippage_cents() const noexcept {
        return static_cast<double>(total_slippage_cents_);
    }

    // ---- Equity curve ----

    struct EquityPoint {
        Timestamp ts  = 0;
        double    pnl = 0.0;
        Price     mark = 0;
    };

    void record_equity(Timestamp ts, Price mark) {
        equity_curve_.push_back({ts, total_pnl(mark), mark});
    }
    [[nodiscard]] const std::vector<EquityPoint>& equity_curve() const noexcept {
        return equity_curve_;
    }

private:
    Quantity position_           = 0;
    double   cash_               = 0.0;
    double   realized_pnl_       = 0.0;
    double   cost_basis_cents_   = 0.0; // average cost * position
    int64_t  total_slippage_cents_ = 0;
    std::size_t fill_count_      = 0;

    std::vector<EquityPoint> equity_curve_;
};

} // namespace bt
