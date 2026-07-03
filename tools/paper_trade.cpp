// paper_trade — run a strategy through the LiveEngine against a CSV file.
//
// This is the "paper trading" entry point: real LiveEngine code path,
// real risk checks, real order routing logic — but filled by
// PaperOrderRouter (ExecutionSimulator) instead of a live exchange.
//
// Usage:
//   paper_trade --data <file.csv> [--latency <ns>] [--strategy mid_cross]
//               [--max-pos <N>] [--drawdown <$>] [--hard-floor <$>]
//               [--max-ops <N/s>] [--output <report.md>] [--verbose]
//
// Swap in your own FeedHandler and OrderRouter implementations here
// when you are ready to go live.

#include "backtester/types.hpp"
#include "backtester/feed_handler.hpp"
#include "backtester/order_router.hpp"
#include "backtester/paper_router.hpp"
#include "backtester/risk.hpp"
#include "backtester/live_engine.hpp"
#include "backtester/report.hpp"
#include "strategies/mid_cross_strategy.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <memory>
#include <cstring>

using namespace bt;

static void usage() {
    std::cerr
        << "Usage: paper_trade --data <file.csv>\n"
        << "         [--latency <ns>]        default 50000\n"
        << "         [--strategy mid_cross]  default mid_cross\n"
        << "         [--max-pos <N>]         default 100\n"
        << "         [--drawdown <dollars>]  default -500\n"
        << "         [--hard-floor <dollars>] default -1000\n"
        << "         [--max-ops <N/s>]       default 100\n"
        << "         [--output <report.md>]\n"
        << "         [--verbose]\n";
}

int main(int argc, char** argv) {
    std::string data_file;
    Timestamp   latency_ns  = 50'000;
    std::string strat_name  = "mid_cross";
    std::string output_file;
    bool        verbose     = false;

    RiskLimits risk_limits;

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--data")        == 0 && i+1 < argc) data_file        = argv[++i];
        else if (std::strcmp(argv[i], "--latency")     == 0 && i+1 < argc) latency_ns       = std::stoll(argv[++i]);
        else if (std::strcmp(argv[i], "--strategy")    == 0 && i+1 < argc) strat_name       = argv[++i];
        else if (std::strcmp(argv[i], "--max-pos")     == 0 && i+1 < argc) risk_limits.max_position     = std::stoll(argv[++i]);
        else if (std::strcmp(argv[i], "--drawdown")    == 0 && i+1 < argc) risk_limits.max_drawdown     = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--hard-floor")  == 0 && i+1 < argc) risk_limits.hard_floor       = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--max-ops")     == 0 && i+1 < argc) risk_limits.max_orders_per_s = std::stoi(argv[++i]);
        else if (std::strcmp(argv[i], "--output")      == 0 && i+1 < argc) output_file      = argv[++i];
        else if (std::strcmp(argv[i], "--verbose")     == 0)                verbose          = true;
        else { usage(); return 1; }
    }

    if (data_file.empty()) { usage(); return 1; }

    // ---- Components ----
    FileFeedHandler  feed(data_file);
    PaperOrderRouter router(latency_ns);
    MidCrossStrategy strategy(20, 0.0005);
    RiskCheck        risk(risk_limits);

    LiveEngineConfig cfg;
    cfg.verbose        = verbose;
    cfg.equity_sample_n = 200;

    LiveEngine engine(feed, router, strategy, risk, cfg);

    std::cout << "Paper trading: " << strat_name
              << " | latency=" << latency_ns << "ns"
              << " | max_pos=" << risk_limits.max_position
              << " | hard_floor=$" << risk_limits.hard_floor
              << "\n";

    engine.run();

    if (risk.killed())
        std::cout << "[WARN] Risk kill triggered.\n";

    // ---- Report ----
    auto& pnl    = engine.pnl();
    auto& book   = engine.book();
    Price mark   = book.mid_price();
    auto rd = make_report_data(
        pnl, strat_name, data_file,
        /*start_ns=*/0, engine.clock().now(),
        latency_ns, engine.order_count(), mark);

    std::string report = generate_report(rd);

    if (output_file.empty()) {
        std::cout << "\n" << report;
    } else {
        std::ofstream f(output_file);
        f << report;
        std::cout << "Report written to " << output_file << "\n";
    }

    return risk.killed() ? 2 : 0;
}
