// backtester/main.cpp — CLI runner
//
// Usage:
//   backtester --data <file.csv> [--latency <ns>] [--strategy <name>]
//              [--parity] [--output <report.md>]
//
// Defaults:
//   latency  = 50000 ns (50µs)
//   strategy = mid_cross
//   parity   = false (run parity harness if --parity set)

#include "backtester/types.hpp"
#include "backtester/event_queue.hpp"
#include "backtester/sim_book.hpp"
#include "backtester/sim_clock.hpp"
#include "backtester/market_replay.hpp"
#include "backtester/market_view.hpp"
#include "backtester/execution_sim.hpp"
#include "backtester/pnl_tracker.hpp"
#include "backtester/report.hpp"
#include "backtester/parity_harness.hpp"
#include "strategies/mid_cross_strategy.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <memory>
#include <cstring>

using namespace bt;

static void usage() {
    std::cerr
        << "Usage: backtester --data <file.csv> [--latency <ns>]\n"
        << "                  [--strategy mid_cross] [--parity] [--output <report.md>]\n";
}

int main(int argc, char** argv) {
    std::string data_file;
    Timestamp   latency_ns  = 50'000;
    std::string strat_name  = "mid_cross";
    bool        run_parity  = false;
    std::string output_file;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data") == 0 && i+1 < argc)
            data_file = argv[++i];
        else if (std::strcmp(argv[i], "--latency") == 0 && i+1 < argc)
            latency_ns = std::stoll(argv[++i]);
        else if (std::strcmp(argv[i], "--strategy") == 0 && i+1 < argc)
            strat_name = argv[++i];
        else if (std::strcmp(argv[i], "--parity") == 0)
            run_parity = true;
        else if (std::strcmp(argv[i], "--output") == 0 && i+1 < argc)
            output_file = argv[++i];
        else {
            usage(); return 1;
        }
    }

    if (data_file.empty()) { usage(); return 1; }

    // ---- Load events ----
    DeterministicEventQueue queue;
    std::size_t n = MarketReplay::load_csv(data_file, queue);
    std::cout << "Loaded " << n << " market events from " << data_file << "\n";

    // Collect raw events for parity harness (before processing)
    std::vector<Event> raw_events;
    {
        DeterministicEventQueue tmp_q;
        MarketReplay::load_csv(data_file, tmp_q);
        while (!tmp_q.empty()) raw_events.push_back(tmp_q.pop());
    }

    // ---- Run parity harness if requested ----
    if (run_parity) {
        std::cout << "Running parity harness...\n";
        ParityHarness harness;
        bool ok = harness.run(
            raw_events,
            [&]() -> std::unique_ptr<Strategy> {
                return std::make_unique<MidCrossStrategy>(20, 0.0005);
            },
            latency_ns,
            /*verbose=*/true
        );
        if (!ok) {
            std::cerr << "PARITY FAILURE — sim/live divergence detected!\n";
            return 2;
        }
        std::cout << "Parity OK.\n";
    }

    // ---- Full backtest ----
    SimBook   book;
    SimClock  clock;
    ExecutionSimulator exec_sim(latency_ns);
    PnLTracker pnl;
    MidCrossStrategy strategy(20, 0.0005);
    std::size_t order_count = 0;

    Timestamp start_ns = 0, end_ns = 0;
    bool first = true;

    while (!queue.empty()) {
        Event e = queue.pop();
        clock.advance(e.ts);
        if (first) { start_ns = e.ts; first = false; }
        end_ns = e.ts;

        // Apply to book
        if (e.kind == EventKind::MARKET_ADD    ||
            e.kind == EventKind::MARKET_MODIFY  ||
            e.kind == EventKind::MARKET_DELETE  ||
            e.kind == EventKind::MARKET_TRADE)
        {
            book.apply(e);
            if (e.kind == EventKind::MARKET_TRADE) {
                auto fills = exec_sim.on_market_trade(
                    std::get<TradePayload>(e.payload), clock);
                for (auto& f : fills) {
                    pnl.on_fill(f);
                    strategy.on_fill(f);
                }
            }
        }

        if (e.kind == EventKind::ORDER_ARRIVE) {
            auto fills = exec_sim.on_order_arrive(
                std::get<Order>(e.payload), book, clock, queue);
            for (auto& f : fills) {
                pnl.on_fill(f);
                strategy.on_fill(f);
            }
        }

        // Call strategy on market events
        if (e.kind == EventKind::MARKET_ADD    ||
            e.kind == EventKind::MARKET_MODIFY  ||
            e.kind == EventKind::MARKET_DELETE  ||
            e.kind == EventKind::MARKET_TRADE)
        {
            MarketView view(book, clock.now(), e.kind);
            auto orders = strategy.on_event(view, clock);
            for (auto& o : orders) {
                (void)exec_sim.on_order_submit(o, book, clock, queue);
                ++order_count;
            }
        }

        // Sample equity every 200 events
        if ((order_count % 200) == 0 && book.mid_price() > 0)
            pnl.record_equity(clock.now(), book.mid_price());
    }
    strategy.on_done();

    // ---- Report ----
    Price final_mark = book.mid_price();
    auto rd = make_report_data(pnl, strat_name, data_file,
                               start_ns, end_ns, latency_ns,
                               order_count, final_mark);
    std::string report = generate_report(rd);

    if (output_file.empty()) {
        std::cout << "\n" << report;
    } else {
        std::ofstream f(output_file);
        f << report;
        std::cout << "Report written to " << output_file << "\n";
    }

    return 0;
}
