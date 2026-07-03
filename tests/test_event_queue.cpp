#include <gtest/gtest.h>
#include "backtester/event_queue.hpp"
#include "backtester/sim_clock.hpp"
#include <vector>
#include <algorithm>
#include <random>

using namespace bt;

// Helper: make a minimal market event at timestamp ts.
static Event make_event(Timestamp ts, uint64_t seq = 0) {
    Event e;
    e.ts    = ts;
    e.seq   = seq;
    e.kind  = EventKind::MARKET_ADD;
    e.payload = MarketOrderPayload{1, Side::BUY, 10000, 100};
    return e;
}

// ----------------------------------------------------------------
// TEST 1: Events pop in strict timestamp order regardless of push order.
// ----------------------------------------------------------------
TEST(EventQueue, PopOrderByTimestamp) {
    DeterministicEventQueue q;
    q.push(make_event(300, 0));
    q.push(make_event(100, 0));
    q.push(make_event(200, 0));
    q.push(make_event(150, 0));

    EXPECT_EQ(q.pop().ts, 100);
    EXPECT_EQ(q.pop().ts, 150);
    EXPECT_EQ(q.pop().ts, 200);
    EXPECT_EQ(q.pop().ts, 300);
    EXPECT_TRUE(q.empty());
}

// ----------------------------------------------------------------
// TEST 2: Tie-breaking: same timestamp, different seq → pop by seq.
// ----------------------------------------------------------------
TEST(EventQueue, TieBreakBySeq) {
    DeterministicEventQueue q;
    q.push(make_event(100, 3));
    q.push(make_event(100, 1));
    q.push(make_event(100, 2));

    EXPECT_EQ(q.pop().seq, 1);
    EXPECT_EQ(q.pop().seq, 2);
    EXPECT_EQ(q.pop().seq, 3);
}

// ----------------------------------------------------------------
// TEST 3: Same timestamp AND same seq → tie-break by det_id
//         (det_id is assigned in push order, so first-pushed pops first).
// ----------------------------------------------------------------
TEST(EventQueue, TieBreakByDetId) {
    DeterministicEventQueue q;
    // Push three events with identical (ts, seq); only det_id differs.
    auto e1 = make_event(100, 0);
    auto e2 = make_event(100, 0);
    auto e3 = make_event(100, 0);
    q.push(e1);
    q.push(e2);
    q.push(e3);

    // Pop order must be deterministic (e1 first, then e2, then e3).
    auto p1 = q.pop();
    auto p2 = q.pop();
    auto p3 = q.pop();
    EXPECT_LT(p1.det_id, p2.det_id);
    EXPECT_LT(p2.det_id, p3.det_id);
}

// ----------------------------------------------------------------
// TEST 4: Two runs over the same shuffled input produce identical
//         pop order.  This is the core determinism guarantee.
// ----------------------------------------------------------------
TEST(EventQueue, DeterministicAcrossRuns) {
    // Build a shuffled list of events.
    std::vector<Event> events;
    for (int i = 0; i < 100; ++i)
        events.push_back(make_event(i * 1000LL, static_cast<uint64_t>(i % 7)));

    std::mt19937 rng(999);
    std::shuffle(events.begin(), events.end(), rng);

    auto run = [&]() {
        DeterministicEventQueue q;
        q.reset_counter();
        for (auto& e : events) q.push(e);
        std::vector<std::pair<Timestamp, uint64_t>> order;
        while (!q.empty()) {
            auto e = q.pop();
            order.emplace_back(e.ts, e.seq);
        }
        return order;
    };

    auto run1 = run();
    auto run2 = run();
    EXPECT_EQ(run1, run2) << "Pop order must be identical across runs";
}

// ----------------------------------------------------------------
// TEST 5: Pop on empty queue throws.
// ----------------------------------------------------------------
TEST(EventQueue, PopEmptyThrows) {
    DeterministicEventQueue q;
    EXPECT_THROW({ (void)q.pop(); }, std::runtime_error);
}

// ----------------------------------------------------------------
// TEST 6: Determinism explanation
// ----------------------------------------------------------------
TEST(EventQueue, SimClockNeverGoesBackward) {
    // SimClock should throw if time goes backward.
    SimClock c;
    c.advance(100);
    c.advance(200);
    EXPECT_EQ(c.now(), 200);
    EXPECT_THROW(c.advance(50), std::logic_error);
}
