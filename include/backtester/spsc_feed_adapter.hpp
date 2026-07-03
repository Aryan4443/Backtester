#pragma once
#include "backtester/feed_handler.hpp"
#include <feedhandler/market_event.hpp>
#include <feedhandler/pipeline.hpp>     // fh::PipelineMsg, fh::kRingCapacity
#include <feedhandler/sequencer.hpp>    // fh::Sequencer, fh::Disposition
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <array>
#include <string>

// ============================================================
// SpscFeedAdapter<Channel>
//
// Bridges the feed handler project's SPSC channel into bt::FeedHandler.
//
// Channel must satisfy:
//   bool receive(fh::PipelineMsg& out)  — returns false when empty.
//
// Typical instantiation:
//   using Chan = fh::SpscChannel<fh::PipelineMsg, fh::kRingCapacity>;
//   Chan channel(fh::Backpressure::Block);
//   SpscFeedAdapter<Chan> adapter(channel, "AAPL");
//
// ---- Thread model ----
//   Thread A (ingest): FeedSource → parse → channel.send()
//   Thread B (engine): adapter.poll() → bt::LiveEngine::run()  ← you are here
//
// ---- What the adapter does on Thread B ----
//   1. channel.receive() → fh::PipelineMsg
//   2. fh::Sequencer::offer() — gap detection + delta replay buffering
//   3. Drain sequencer.pop_ready() for any now-contiguous buffered events
//   4. Convert each in-order fh::MarketEvent → ≥1 bt::Event
//
// ---- Symbol filtering ----
//   Pass the ticker as a string (e.g. "AAPL"). The adapter upper-cases it
//   and pads to 8 chars to match the MarketEvent.symbol field (ITCH
//   right-space-pads tickers). Empty string = accept all symbols.
//
// ---- Price conversion ----
//   fh: 4-decimal fixed-point  ($100.00 = 1,000,000 ticks, /10000 → $)
//   bt: cents                  ($100.00 = 10,000 cents,    /100   → $)
//   Formula: bt_price_cents = fh_price_ticks / 100
//
// ---- Timestamps ----
//   fh::MarketEvent.timestamp_ns = ns since midnight (ITCH convention).
//   bt::SimClock treats timestamps as opaque ordered integers.
//   Monotonicity holds within a trading session — correct.
//
// ---- Order cache ----
//   ITCH Replace does not carry the old order's price/side. Partial Cancel
//   carries the cancelled qty, not the new remaining qty. The adapter tracks
//   { price_ticks, remaining_size, side } per order_ref to reconstruct both.
//
// ---- Gap / recovery ----
//   Delta replay gaps: sequencer buffers future events; adapter drains them
//   once the missing seq arrives — no action needed from the caller.
//   Overflow gaps (too large to buffer): adapter calls resync_to_snapshot()
//   to advance past the hole. The bt::SimBook state may be temporarily
//   inconsistent; the strategy should check view.has_market() before trading.
//   Pass a non-null fh::Sequencer* (external) to inspect recovery state.
// ============================================================

namespace bt {

template<typename Channel>
class SpscFeedAdapter : public FeedHandler {
public:
    explicit SpscFeedAdapter(Channel&           channel,
                             const std::string& symbol_filter = "")
        : channel_(channel)
        , sym_filter_(make_sym_filter(symbol_filter))
        , has_sym_filter_(!symbol_filter.empty())
    {}

    [[nodiscard]] std::vector<Event> poll() override {
        std::vector<Event> out;
        fh::PipelineMsg msg;

        while (channel_.receive(msg)) {
            const fh::MarketEvent& me = msg.event;

            // Symbol filter (compare against right-space-padded char[8])
            if (has_sym_filter_ && std::memcmp(me.symbol, sym_filter_.data(), 8) != 0)
                continue;

            // Sequencer: gap detection + delta replay
            auto disp = seq_.offer(msg.sequence, me);

            switch (disp) {
                case fh::Disposition::Apply:
                    translate(me, out);
                    // Drain any buffered events that are now contiguous
                    {
                        fh::MarketEvent buffered;
                        while (seq_.pop_ready(buffered))
                            translate(buffered, out);
                    }
                    break;

                case fh::Disposition::Buffered:
                    // Held in sequencer; will appear via pop_ready() later
                    break;

                case fh::Disposition::Drop:
                    // Duplicate / already-seen sequence — ignore
                    break;

                case fh::Disposition::Overflow:
                    // Gap too large to buffer. Accept data loss and advance.
                    // The sequencer sets needs_snapshot_; we synthesise a resync
                    // at the current sequence so the stream stays usable.
                    seq_.resync_to_snapshot(msg.sequence);
                    translate(me, out);
                    break;
            }
        }

        return out;
    }

    [[nodiscard]] bool healthy() const noexcept override { return healthy_; }

    // Call from the ingest thread once the FeedSource is exhausted.
    void shutdown() noexcept { healthy_ = false; }

    // Expose sequencer state (gap count, stale flag, etc.) for logging/monitoring.
    [[nodiscard]] const fh::Sequencer& sequencer() const noexcept { return seq_; }

private:
    Channel&           channel_;
    bool               healthy_        = true;
    uint64_t           seq_counter_    = 0;   // bt::Event.seq monotone counter

    std::array<char,8> sym_filter_;
    bool               has_sym_filter_;

    fh::Sequencer      seq_;                  // gap detection, lives on Thread B

    // Per-order cache: needed for Replace (old price unknown from ITCH message)
    // and partial Cancel (must compute new remaining qty).
    struct OrderInfo {
        int64_t  price_ticks = 0;
        uint32_t remaining   = 0;
        fh::Side side        = fh::Side::None;
    };
    std::unordered_map<uint64_t, OrderInfo> orders_;

    // ---- helpers ----

    static std::array<char,8> make_sym_filter(const std::string& sym) {
        std::array<char,8> out;
        out.fill(' ');
        for (size_t i = 0; i < std::min(sym.size(), size_t(8)); ++i)
            out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(sym[i])));
        return out;
    }

    // fh 4-decimal ticks → bt cents.  $100.00 = 1,000,000 ticks = 10,000 cents.
    static Price to_cents(int64_t ticks) noexcept {
        return static_cast<Price>(ticks / 100);
    }

    static bt::Side to_side(fh::Side s) noexcept {
        return s == fh::Side::Buy ? bt::Side::BUY : bt::Side::SELL;
    }

    Event make_add(uint64_t oid, fh::Side side, int64_t price_ticks,
                   uint32_t qty, Timestamp ts) {
        Event e; e.ts = ts; e.seq = seq_counter_++;
        e.kind    = EventKind::MARKET_ADD;
        e.payload = MarketOrderPayload{
            oid, to_side(side), to_cents(price_ticks), static_cast<Quantity>(qty)};
        return e;
    }

    Event make_modify(uint64_t oid, fh::Side side, int64_t price_ticks,
                      uint32_t new_qty, Timestamp ts) {
        Event e; e.ts = ts; e.seq = seq_counter_++;
        e.kind    = EventKind::MARKET_MODIFY;
        e.payload = MarketOrderPayload{
            oid, to_side(side), to_cents(price_ticks), static_cast<Quantity>(new_qty)};
        return e;
    }

    Event make_delete(uint64_t oid, fh::Side side, int64_t price_ticks,
                      Timestamp ts) {
        Event e; e.ts = ts; e.seq = seq_counter_++;
        e.kind    = EventKind::MARKET_DELETE;
        e.payload = MarketOrderPayload{oid, to_side(side), to_cents(price_ticks), 0};
        return e;
    }

    Event make_trade(int64_t price_ticks, uint32_t qty,
                     bt::Side aggressor, Timestamp ts) {
        Event e; e.ts = ts; e.seq = seq_counter_++;
        e.kind    = EventKind::MARKET_TRADE;
        e.payload = TradePayload{
            to_cents(price_ticks), static_cast<Quantity>(qty), aggressor};
        return e;
    }

    // Shrink or remove an order from the cache; emit MODIFY or DELETE into out.
    void reduce_or_delete(uint64_t oid, uint32_t by,
                          int64_t price_ticks_override,
                          Timestamp ts, std::vector<Event>& out) {
        auto it = orders_.find(oid);
        if (it == orders_.end()) return;
        OrderInfo& info = it->second;
        uint32_t remaining = (info.remaining > by) ? (info.remaining - by) : 0;
        info.remaining = remaining;
        if (remaining == 0) {
            out.push_back(make_delete(oid, info.side, info.price_ticks, ts));
            orders_.erase(it);
        } else {
            out.push_back(make_modify(oid, info.side, info.price_ticks, remaining, ts));
        }
        (void)price_ticks_override;
    }

    // ---- main translation ----

    void translate(const fh::MarketEvent& me, std::vector<Event>& out) {
        auto ts = static_cast<Timestamp>(me.timestamp_ns);
        using ET = fh::EventType;

        switch (me.type) {

            case ET::Add: {
                orders_[me.order_ref] = {me.price_ticks, me.size, me.side};
                out.push_back(make_add(me.order_ref, me.side,
                                       me.price_ticks, me.size, ts));
                break;
            }

            case ET::Cancel: {
                // Partial cancel: size = shares cancelled (not new remaining).
                // Emit MODIFY with new remaining, or DELETE if fully cancelled.
                reduce_or_delete(me.order_ref, me.size, me.price_ticks, ts, out);
                break;
            }

            case ET::Delete: {
                orders_.erase(me.order_ref);
                out.push_back(make_delete(me.order_ref, me.side,
                                          me.price_ticks, ts));
                break;
            }

            case ET::Execute:
            case ET::ExecuteWithPrice: {
                // Resting order hit: emit TRADE first so queue positions advance,
                // then shrink/remove the resting order.
                bt::Side passive   = to_side(me.side);
                bt::Side aggressor = (passive == Side::BUY) ? Side::SELL : Side::BUY;
                out.push_back(make_trade(me.price_ticks, me.size, aggressor, ts));
                reduce_or_delete(me.order_ref, me.size, me.price_ticks, ts, out);
                break;
            }

            case ET::Replace: {
                // Atomic: DELETE old order_ref + ADD new_order_ref at new price/size.
                // ITCH Replace does not carry old price — must look it up from cache.
                auto it = orders_.find(me.order_ref);
                if (it != orders_.end()) {
                    out.push_back(make_delete(me.order_ref, it->second.side,
                                              it->second.price_ticks, ts));
                    orders_.erase(it);
                }
                orders_[me.new_order_ref] = {me.price_ticks, me.size, me.side};
                out.push_back(make_add(me.new_order_ref, me.side,
                                       me.price_ticks, me.size, ts));
                break;
            }

            case ET::Trade: {
                // Non-displayable (cross/hidden) trade: tape only, no book mutation.
                // Aggressor side unknown in ITCH 'P'; use BUY as placeholder so
                // queue positions on the ask side have a chance to advance.
                out.push_back(make_trade(me.price_ticks, me.size, Side::BUY, ts));
                break;
            }

            // L2SetLevel: bt::SimBook is L3-centric (per-order tracking).
            // Binance diff-depth updates carry no order IDs. Skip.
            // TODO: add SimBook::set_level() if Binance backtesting is needed.
            case ET::L2SetLevel:
            case ET::L2EventHeader:
            case ET::SystemEvent:
            case ET::Unknown:
            default:
                break;
        }
    }
};

} // namespace bt
