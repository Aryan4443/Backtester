#pragma once
#include "types.hpp"
#include "sim_book.hpp"
#include "sim_clock.hpp"
#include <optional>
#include <vector>

// ============================================================
// MarketView — an immutable snapshot of market state passed to
// the strategy's on_event() callback.
//
// Look-ahead bias is structurally impossible:
//   - MarketView is constructed AFTER the current event is
//     applied to SimBook (so the strategy sees that event's
//     effect), but BEFORE any future event is processed.
//   - The strategy receives a const MarketView& and a
//     const SimClock& — there is no reference to the live
//     SimBook or to future events anywhere in the interface.
//   - The snapshot is a value copy of bid/ask levels, not a
//     pointer or reference into the live book.
//
// This means a strategy that somehow tries to read the book
// object directly simply has no handle to it.
// ============================================================

namespace bt {

// One price level visible to the strategy.
struct BookLevel {
    Price    price     = 0;
    Quantity total_qty = 0;
};

class MarketView {
public:
    // Constructed by the event loop after applying each event.
    explicit MarketView(const SimBook& book, Timestamp as_of, EventKind trigger)
        : as_of_(as_of), trigger_(trigger)
    {
        // Copy top-5 levels (value semantics → no future data leaks).
        for (auto& l : book.bid_levels(5))
            bids_.push_back({l.price, l.total_qty});
        for (auto& l : book.ask_levels(5))
            asks_.push_back({l.price, l.total_qty});

        mid_   = book.mid_price();
        spread_= book.spread();
    }

    // ---- Accessors ----

    [[nodiscard]] Timestamp as_of()    const noexcept { return as_of_; }
    [[nodiscard]] EventKind trigger()  const noexcept { return trigger_; }
    [[nodiscard]] Price     mid()      const noexcept { return mid_; }
    [[nodiscard]] Price     spread()   const noexcept { return spread_; }

    [[nodiscard]] std::optional<BookLevel> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return bids_.front();
    }
    [[nodiscard]] std::optional<BookLevel> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return asks_.front();
    }

    [[nodiscard]] const std::vector<BookLevel>& bids() const noexcept { return bids_; }
    [[nodiscard]] const std::vector<BookLevel>& asks() const noexcept { return asks_; }

    // Returns true if there is a two-sided market.
    [[nodiscard]] bool has_market() const noexcept {
        return !bids_.empty() && !asks_.empty();
    }

private:
    Timestamp             as_of_   = 0;
    EventKind             trigger_ = EventKind::MARKET_ADD;
    Price                 mid_     = 0;
    Price                 spread_  = 0;
    std::vector<BookLevel> bids_;
    std::vector<BookLevel> asks_;

    // No pointer to SimBook, no reference to future data.
    // This is the structural guarantee.
};

} // namespace bt
