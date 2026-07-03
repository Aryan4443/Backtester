#pragma once
#include "types.hpp"
#include "pnl_tracker.hpp"
#include <vector>
#include <chrono>
#include <cmath>

// ============================================================
// RiskCheck — pre-send gate and hard kill switch.
//
// allow() is called for every order before it hits the router.
// check_kill() is called once per event loop iteration.
//
// Limits enforced:
//   max_position     — abs(position + new_qty) must not exceed this
//   max_drawdown     — soft floor: blocks new orders when breached
//   hard_floor       — hard kill: cancels everything, stops engine
//   max_orders_per_s — order rate limit (rolling 1-second window)
//
// Uses std::chrono::steady_clock intentionally — risk checks must
// use real wall-clock time so rate limits and kill decisions are
// not tied to (potentially stale) sim-time.
// ============================================================

namespace bt {

struct RiskLimits {
    Quantity max_position     = 100;
    double   max_drawdown     = -500.0;   // dollars; soft floor
    double   hard_floor       = -1000.0;  // dollars; hard kill
    int      max_orders_per_s = 100;
};

class RiskCheck {
public:
    explicit RiskCheck(RiskLimits limits = {}) : limits_(limits) {}

    // Returns true if the order is allowed to proceed.
    // mark: current mid price (cents) for PnL valuation.
    [[nodiscard]] bool allow(const Order& o, const PnLTracker& pnl, Price mark) {
        if (killed_) return false;

        // Position limit
        Quantity delta = (o.side == Side::BUY) ? o.qty : -o.qty;
        if (std::abs(pnl.position() + delta) > limits_.max_position)
            return false;

        // Soft drawdown floor
        if (pnl.total_pnl(mark) < limits_.max_drawdown)
            return false;

        // Order rate limit (rolling 1-second window)
        auto now = std::chrono::steady_clock::now();
        prune_window(now);
        if (static_cast<int>(order_times_.size()) >= limits_.max_orders_per_s)
            return false;

        order_times_.push_back(now);
        return true;
    }

    // Hard kill check — call once per event loop iteration.
    // Returns true if kill was newly or previously triggered.
    bool check_kill(const PnLTracker& pnl, Price mark) {
        if (!killed_ && pnl.total_pnl(mark) < limits_.hard_floor)
            killed_ = true;
        return killed_;
    }

    void kill() noexcept { killed_ = true; }

    [[nodiscard]] bool        killed() const noexcept { return killed_; }
    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }

private:
    RiskLimits limits_;
    bool       killed_ = false;

    using TP = std::chrono::steady_clock::time_point;
    std::vector<TP> order_times_;

    void prune_window(TP now) {
        auto cutoff = now - std::chrono::seconds(1);
        auto it = order_times_.begin();
        while (it != order_times_.end() && *it < cutoff) ++it;
        order_times_.erase(order_times_.begin(), it);
    }
};

} // namespace bt
