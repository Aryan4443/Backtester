#include "backtester/sim_book.hpp"
#include <stdexcept>

namespace bt {

// ----------------------------------------------------------------
// apply
// ----------------------------------------------------------------
void SimBook::apply(const Event& e) {
    switch (e.kind) {
        case EventKind::MARKET_ADD:
            apply_add(std::get<MarketOrderPayload>(e.payload));
            break;
        case EventKind::MARKET_MODIFY:
            apply_modify(std::get<MarketOrderPayload>(e.payload));
            break;
        case EventKind::MARKET_DELETE:
            apply_delete(std::get<MarketOrderPayload>(e.payload));
            break;
        case EventKind::MARKET_TRADE:
            apply_trade(std::get<TradePayload>(e.payload));
            break;
        default:
            break; // non-market events are silently ignored
    }
}

// ----------------------------------------------------------------
// apply_add: insert a new resting order.
// ----------------------------------------------------------------
void SimBook::apply_add(const MarketOrderPayload& p) {
    if (p.side == Side::BUY) {
        bids_[p.price].price = p.price;
        bids_[p.price].add(p.order_id, p.qty);
    } else {
        asks_[p.price].price = p.price;
        asks_[p.price].add(p.order_id, p.qty);
    }
}

// ----------------------------------------------------------------
// apply_modify: replace size on an existing order.
// ----------------------------------------------------------------
void SimBook::apply_modify(const MarketOrderPayload& p) {
    auto modify_in = [&](auto& side_map) {
        auto it = side_map.find(p.price);
        if (it == side_map.end()) return;
        auto& level = it->second;
        auto oit = level.orders.find(p.order_id);
        if (oit == level.orders.end()) return;
        level.total_qty -= oit->second;
        oit->second      = p.qty;
        level.total_qty += p.qty;
        if (level.empty()) side_map.erase(it);
    };
    if (p.side == Side::BUY) modify_in(bids_);
    else                     modify_in(asks_);
}

// ----------------------------------------------------------------
// apply_delete: remove a resting order entirely.
// ----------------------------------------------------------------
void SimBook::apply_delete(const MarketOrderPayload& p) {
    auto del_from = [&](auto& side_map) {
        auto it = side_map.find(p.price);
        if (it == side_map.end()) return;
        it->second.remove(p.order_id, it->second.orders.count(p.order_id)
                          ? it->second.orders.at(p.order_id) : 0);
        if (it->second.empty()) side_map.erase(it);
    };
    if (p.side == Side::BUY) del_from(bids_);
    else                     del_from(asks_);
}

// ----------------------------------------------------------------
// apply_trade: consume qty from the passive side of the book.
//
// The aggressor is the side that took liquidity, so the passive
// (resting) side is opposite(aggressor).
// We remove from the best available levels until qty is consumed.
// ----------------------------------------------------------------
void SimBook::apply_trade(const TradePayload& p) {
    Quantity remaining = p.qty;

    if (p.aggressor == Side::BUY) {
        // Buyer is aggressive → removes from asks (passive sellers)
        for (auto it = asks_.begin(); it != asks_.end() && remaining > 0; ) {
            if (it->first > p.price) break; // only consume at or below trade price
            auto& level = it->second;
            // Remove from orders in deterministic (map key) order
            for (auto oit = level.orders.begin();
                 oit != level.orders.end() && remaining > 0; ) {
                Quantity take = std::min(oit->second, remaining);
                oit->second  -= take;
                level.total_qty -= take;
                remaining       -= take;
                if (oit->second == 0) oit = level.orders.erase(oit);
                else                  ++oit;
            }
            if (level.empty()) it = asks_.erase(it);
            else               ++it;
        }
    } else {
        // Seller is aggressive → removes from bids (passive buyers)
        for (auto it = bids_.begin(); it != bids_.end() && remaining > 0; ) {
            if (it->first < p.price) break; // only consume at or above trade price
            auto& level = it->second;
            for (auto oit = level.orders.begin();
                 oit != level.orders.end() && remaining > 0; ) {
                Quantity take = std::min(oit->second, remaining);
                oit->second  -= take;
                level.total_qty -= take;
                remaining       -= take;
                if (oit->second == 0) oit = level.orders.erase(oit);
                else                  ++oit;
            }
            if (level.empty()) it = bids_.erase(it);
            else               ++it;
        }
    }
}

// ----------------------------------------------------------------
// Queries
// ----------------------------------------------------------------
std::optional<std::pair<Price, Quantity>> SimBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    auto& l = bids_.begin()->second;
    return std::make_pair(l.price, l.total_qty);
}

std::optional<std::pair<Price, Quantity>> SimBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    auto& l = asks_.begin()->second;
    return std::make_pair(l.price, l.total_qty);
}

Price SimBook::mid_price() const noexcept {
    auto bb = best_bid();
    auto ba = best_ask();
    if (!bb || !ba) return 0;
    return (bb->first + ba->first) / 2;
}

Price SimBook::spread() const noexcept {
    auto bb = best_bid();
    auto ba = best_ask();
    if (!bb || !ba) return 0;
    return ba->first - bb->first;
}

std::vector<Level> SimBook::bid_levels(int n) const {
    std::vector<Level> out;
    out.reserve(static_cast<std::size_t>(n));
    for (auto& [p, l] : bids_) {
        if (static_cast<int>(out.size()) >= n) break;
        // Snapshot price/qty only — do not copy per-order maps (can be huge).
        out.push_back(Level{l.price, l.total_qty, {}});
    }
    return out;
}

std::vector<Level> SimBook::ask_levels(int n) const {
    std::vector<Level> out;
    out.reserve(static_cast<std::size_t>(n));
    for (auto& [p, l] : asks_) {
        if (static_cast<int>(out.size()) >= n) break;
        out.push_back(Level{l.price, l.total_qty, {}});
    }
    return out;
}

Quantity SimBook::qty_at_bid(Price p) const noexcept {
    auto it = bids_.find(p);
    return it != bids_.end() ? it->second.total_qty : 0;
}

Quantity SimBook::qty_at_ask(Price p) const noexcept {
    auto it = asks_.find(p);
    return it != asks_.end() ? it->second.total_qty : 0;
}

Quantity SimBook::qty_at_bid_level(Price p) const noexcept { return qty_at_bid(p); }
Quantity SimBook::qty_at_ask_level(Price p) const noexcept { return qty_at_ask(p); }

Quantity SimBook::available_liquidity(Side side, Price limit_price) const noexcept {
    Quantity total = 0;
    if (side == Side::BUY) {
        // buying: consume asks at or below limit_price
        for (auto& [p, l] : asks_) {
            if (p > limit_price) break;
            total += l.total_qty;
        }
    } else {
        // selling: consume bids at or above limit_price
        for (auto& [p, l] : bids_) {
            if (p < limit_price) break;
            total += l.total_qty;
        }
    }
    return total;
}

} // namespace bt
