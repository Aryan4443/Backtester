#include "backtester/pnl_tracker.hpp"

namespace bt {

void PnLTracker::on_fill(const Fill& f) {
    ++fill_count_;
    total_slippage_cents_ += f.slippage;

    // fill_qty > 0 → buy;  fill_qty < 0 → sell
    Quantity qty = f.fill_qty;  // signed

    if (qty > 0) {
        // Buying: pay cash, increase position
        double cost = static_cast<double>(f.fill_price) * static_cast<double>(qty) / 100.0;
        cash_ -= cost;

        // Update average cost basis
        if (position_ >= 0) {
            // Adding to a long: weighted average
            cost_basis_cents_ = (cost_basis_cents_ * position_ + f.fill_price * qty)
                                 / static_cast<double>(position_ + qty);
        } else {
            // Closing a short (or flipping)
            Quantity closed = std::min(qty, -position_);
            double close_pnl = static_cast<double>(cost_basis_cents_ - f.fill_price)
                               * closed / 100.0;
            realized_pnl_ += close_pnl;
            Quantity open = qty - closed;
            if (open > 0) cost_basis_cents_ = f.fill_price;
        }
        position_ += qty;

    } else {
        // Selling (qty < 0)
        Quantity abs_qty = -qty;
        double proceeds = static_cast<double>(f.fill_price) * abs_qty / 100.0;
        cash_ += proceeds;

        if (position_ > 0) {
            // Closing a long
            Quantity closed = std::min(abs_qty, position_);
            double close_pnl = static_cast<double>(f.fill_price - cost_basis_cents_)
                               * closed / 100.0;
            realized_pnl_ += close_pnl;
            Quantity open = abs_qty - closed;
            if (open > 0) cost_basis_cents_ = f.fill_price;
        } else {
            // Adding to a short
            cost_basis_cents_ = (position_ == 0)
                ? f.fill_price
                : (cost_basis_cents_ * (-position_) + f.fill_price * abs_qty)
                  / static_cast<double>(-position_ + abs_qty);
        }
        position_ -= abs_qty;
    }
}

} // namespace bt
