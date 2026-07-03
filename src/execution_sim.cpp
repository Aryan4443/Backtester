#include "backtester/execution_sim.hpp"
#include <stdexcept>
#include <algorithm>

namespace bt {

// ----------------------------------------------------------------
// on_order_submit
// Enqueue an ORDER_ARRIVE event at T + latency.
// Market events in [T, T+L) will fire first due to the event
// queue's timestamp ordering — this is where adverse selection
// comes from structurally.
// ----------------------------------------------------------------
Event ExecutionSimulator::on_order_submit(Order order,
                                           const SimBook& book,
                                           const SimClock& clock,
                                           DeterministicEventQueue& queue)
{
    order.id        = next_order_id_++;
    order.submit_ns = clock.now();
    if (order.mid_at_submit == 0)
        order.mid_at_submit = book.mid_price();

    Event arrive;
    arrive.ts      = clock.future(latency_ns_);
    arrive.seq     = 0;   // det_id assigned by queue.push()
    arrive.kind    = EventKind::ORDER_ARRIVE;
    arrive.payload = order;

    queue.push(arrive);
    return arrive;  // caller may log this
}

// ----------------------------------------------------------------
// on_order_arrive
// ----------------------------------------------------------------
std::vector<Fill> ExecutionSimulator::on_order_arrive(const Order& order,
                                                       SimBook& book,
                                                       const SimClock& clock,
                                                       DeterministicEventQueue& queue)
{
    (void)queue;  // future: could enqueue fill events; caller handles them

    if (order.kind == OrderKind::MARKET) {
        return fill_market_order(order, book, clock);
    }

    // LIMIT order
    auto bb = book.best_bid();
    auto ba = book.best_ask();

    if (order.side == Side::BUY) {
        // Aggressive if our limit >= best ask
        if (ba && order.price >= ba->first)
            return fill_aggressive_limit(order, book, clock);
        else {
            post_passive_limit(order, book);
            return {};
        }
    } else {
        // Aggressive if our limit <= best bid
        if (bb && order.price <= bb->first)
            return fill_aggressive_limit(order, book, clock);
        else {
            post_passive_limit(order, book);
            return {};
        }
    }
}

// ----------------------------------------------------------------
// on_market_trade
// For every resting limit we track, decrement size_ahead by the
// trade quantity at the matching price+side.
// Fill when size_ahead reaches 0.
// ----------------------------------------------------------------
std::vector<Fill> ExecutionSimulator::on_market_trade(const TradePayload& trade,
                                                       const SimClock& clock)
{
    std::vector<Fill> fills;

    for (auto& [id, qp] : queue_positions_) {
        // A trade at price P on the passive side consumes our queue position
        // if our resting order is on the passive side at the same price.
        // passive side = opposite of aggressor
        Side passive = opposite(trade.aggressor);
        if (qp.order.side != passive) continue;
        if (qp.order.price != trade.price) continue;
        if (qp.size_ahead <= 0) continue;

        // Decrement size_ahead by the trade qty
        Quantity consumed = std::min(qp.size_ahead, trade.qty);
        qp.size_ahead -= consumed;

        // If size_ahead is now 0 (or below), we fill.
        if (qp.size_ahead <= 0) {
            Quantity fill_qty = qp.order.qty - qp.filled_qty;
            if (fill_qty <= 0) continue;

            Fill f;
            f.order_id   = qp.order.id;
            f.fill_price = qp.order.price;
            f.fill_qty   = (qp.order.side == Side::BUY) ? fill_qty : -fill_qty;
            f.fill_ns    = clock.now();
            f.slippage   = (qp.order.side == Side::BUY)
                           ? (f.fill_price - qp.order.mid_at_submit)
                           : (qp.order.mid_at_submit - f.fill_price);

            qp.filled_qty += fill_qty;
            fills.push_back(f);
        }
    }

    // Remove fully filled queue positions
    for (auto it = queue_positions_.begin(); it != queue_positions_.end(); ) {
        if (it->second.filled_qty >= it->second.order.qty)
            it = queue_positions_.erase(it);
        else
            ++it;
    }

    return fills;
}

// ----------------------------------------------------------------
// cancel_order
// ----------------------------------------------------------------
bool ExecutionSimulator::cancel_order(OrderId id,
                                       SimBook& /*book*/,
                                       const SimClock& clock,
                                       DeterministicEventQueue& queue)
{
    auto it = queue_positions_.find(id);
    if (it == queue_positions_.end()) return false;
    queue_positions_.erase(it);

    Event e;
    e.ts      = clock.now();
    e.seq     = 0;
    e.kind    = EventKind::ORDER_CANCEL;
    e.payload = CancelConf{id};
    queue.push(e);
    return true;
}

// ----------------------------------------------------------------
// fill_market_order
// Walk the book from best available level.
// ----------------------------------------------------------------
std::vector<Fill> ExecutionSimulator::fill_market_order(Order order,
                                                         SimBook& book,
                                                         const SimClock& clock)
{
    std::vector<Fill> fills;
    Quantity remaining = order.qty;

    if (order.side == Side::BUY) {
        // Walk asks (lowest first)
        auto& asks = const_cast<std::map<Price, Level>&>(book.asks());
        for (auto it = asks.begin(); it != asks.end() && remaining > 0; ) {
            auto& level = it->second;
            for (auto oit = level.orders.begin();
                 oit != level.orders.end() && remaining > 0; ) {
                Quantity take = std::min(oit->second, remaining);
                Fill f;
                f.order_id   = order.id;
                f.fill_price = level.price;
                f.fill_qty   = take;
                f.fill_ns    = clock.now();
                f.slippage   = level.price - order.mid_at_submit;
                fills.push_back(f);

                remaining        -= take;
                oit->second      -= take;
                level.total_qty  -= take;
                if (oit->second == 0) oit = level.orders.erase(oit);
                else                  ++oit;
            }
            if (level.empty()) it = asks.erase(it);
            else               ++it;
        }
    } else {
        // Walk bids (highest first)
        auto& bids = const_cast<std::map<Price, Level, std::greater<Price>>&>(book.bids());
        for (auto it = bids.begin(); it != bids.end() && remaining > 0; ) {
            auto& level = it->second;
            for (auto oit = level.orders.begin();
                 oit != level.orders.end() && remaining > 0; ) {
                Quantity take = std::min(oit->second, remaining);
                Fill f;
                f.order_id   = order.id;
                f.fill_price = level.price;
                f.fill_qty   = -take;  // negative = sell
                f.fill_ns    = clock.now();
                f.slippage   = order.mid_at_submit - level.price;
                fills.push_back(f);

                remaining        -= take;
                oit->second      -= take;
                level.total_qty  -= take;
                if (oit->second == 0) oit = level.orders.erase(oit);
                else                  ++oit;
            }
            if (level.empty()) it = bids.erase(it);
            else               ++it;
        }
    }
    return fills;
}

// ----------------------------------------------------------------
// fill_aggressive_limit
// Same as market order but capped at the limit price.
// ----------------------------------------------------------------
std::vector<Fill> ExecutionSimulator::fill_aggressive_limit(Order order,
                                                             SimBook& book,
                                                             const SimClock& clock)
{
    std::vector<Fill> fills;
    Quantity remaining = order.qty;

    if (order.side == Side::BUY) {
        auto& asks = const_cast<std::map<Price, Level>&>(book.asks());
        for (auto it = asks.begin();
             it != asks.end() && remaining > 0 && it->first <= order.price; ) {
            auto& level = it->second;
            for (auto oit = level.orders.begin();
                 oit != level.orders.end() && remaining > 0; ) {
                Quantity take = std::min(oit->second, remaining);
                Fill f;
                f.order_id   = order.id;
                f.fill_price = level.price;
                f.fill_qty   = take;
                f.fill_ns    = clock.now();
                f.slippage   = level.price - order.mid_at_submit;
                fills.push_back(f);

                remaining       -= take;
                oit->second     -= take;
                level.total_qty -= take;
                if (oit->second == 0) oit = level.orders.erase(oit);
                else                  ++oit;
            }
            if (level.empty()) it = asks.erase(it);
            else               ++it;
        }
    } else {
        auto& bids = const_cast<std::map<Price, Level, std::greater<Price>>&>(book.bids());
        for (auto it = bids.begin();
             it != bids.end() && remaining > 0 && it->first >= order.price; ) {
            auto& level = it->second;
            for (auto oit = level.orders.begin();
                 oit != level.orders.end() && remaining > 0; ) {
                Quantity take = std::min(oit->second, remaining);
                Fill f;
                f.order_id   = order.id;
                f.fill_price = level.price;
                f.fill_qty   = -take;
                f.fill_ns    = clock.now();
                f.slippage   = order.mid_at_submit - level.price;
                fills.push_back(f);

                remaining       -= take;
                oit->second     -= take;
                level.total_qty -= take;
                if (oit->second == 0) oit = level.orders.erase(oit);
                else                  ++oit;
            }
            if (level.empty()) it = bids.erase(it);
            else               ++it;
        }
    }

    // Post any unfilled remainder as a passive limit
    if (remaining > 0) {
        Order remainder = order;
        remainder.qty   = remaining;
        post_passive_limit(remainder, book);
    }

    return fills;
}

// ----------------------------------------------------------------
// post_passive_limit
// Record size_ahead = current total qty at the price level.
// FIFO cancel assumption: cancellations by others don't move us
// forward — this is the conservative, documented choice.
// ----------------------------------------------------------------
void ExecutionSimulator::post_passive_limit(const Order& order,
                                             const SimBook& book)
{
    QueuePosition qp;
    qp.order = order;
    qp.size_ahead = (order.side == Side::BUY)
                    ? book.qty_at_bid_level(order.price)
                    : book.qty_at_ask_level(order.price);
    qp.filled_qty = 0;
    queue_positions_[order.id] = qp;
}

} // namespace bt
