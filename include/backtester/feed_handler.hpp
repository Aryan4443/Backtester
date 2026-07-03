#pragma once
#include "types.hpp"
#include "event_queue.hpp"
#include "market_replay.hpp"
#include <vector>
#include <string>

// ============================================================
// FeedHandler — pure interface your existing feed project
// implements to push events into the live engine.
//
// Implement poll() to translate your feed's native messages
// into bt::Event structs and return them.  The live engine
// calls poll() in a tight loop and pushes events into its
// DeterministicEventQueue.
//
// Sequence numbers:
//   Set e.seq to the feed's own sequence number.  The engine
//   uses it as the secondary sort key — so events with the
//   same nanosecond timestamp are ordered by the feed's own
//   ordering, which is the exchange's ground truth.
//
// Gap detection:
//   If your feed drops packets, call request_gap_fill() to
//   trigger TCP retransmit / snapshot recovery.  The default
//   implementation is a no-op; override it.
// ============================================================

namespace bt {

class FeedHandler {
public:
    virtual ~FeedHandler() = default;

    // Non-blocking poll.  Return all events decoded since last call.
    // Empty vector = no new events right now; the engine loops again.
    [[nodiscard]] virtual std::vector<Event> poll() = 0;

    // Request gap fill for missed sequence range [from, to].
    // Default no-op; override for exchanges with TCP retransmit channels.
    virtual void request_gap_fill(uint64_t /*from*/, uint64_t /*to*/) {}

    // True while the feed has more events to deliver.
    // Return false only on clean shutdown; the engine stops when this is false
    // AND the internal queue is empty.
    [[nodiscard]] virtual bool healthy() const noexcept = 0;
};

// ----------------------------------------------------------------
// FileFeedHandler — streams events from a recorded CSV file.
// Useful for paper-trading against historical data and for tests.
// Delivers one event per poll() call (mimics an incremental feed).
// ----------------------------------------------------------------
class FileFeedHandler : public FeedHandler {
public:
    explicit FileFeedHandler(const std::string& path) {
        DeterministicEventQueue tmp;
        MarketReplay::load_csv(path, tmp);
        events_.reserve(tmp.size());
        while (!tmp.empty()) events_.push_back(tmp.pop());
    }

    [[nodiscard]] std::vector<Event> poll() override {
        if (cursor_ >= events_.size()) return {};
        return {events_[cursor_++]};
    }

    [[nodiscard]] bool healthy() const noexcept override {
        return cursor_ < events_.size();
    }

private:
    std::vector<Event> events_;
    std::size_t        cursor_ = 0;
};

} // namespace bt
