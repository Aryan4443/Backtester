// live_main.cpp — wires the feed handler project into the backtester LiveEngine.
//
// Thread layout:
//   Thread A (ingest): ItchFileSource → fh::itch::parse → channel.send()
//   Thread B (engine): SpscFeedAdapter.poll() → bt::LiveEngine::run()
//
// Swap ItchFileSource for your live CryptoWsSource / multicast adapter when ready.
//
// Usage:
//   live_trader --symbol AAPL --file data/sample.itch
//               [--latency <ns>]         default 50000
//               [--max-pos <N>]          default 100
//               [--hard-floor <dollars>] default -1000
//               [--output <report.md>]
//               [--strategy mid_cross|warmup_take]  default warmup_take
//               [--verbose]

#include "backtester/spsc_feed_adapter.hpp"
#include "backtester/paper_router.hpp"
#include "backtester/risk.hpp"
#include "backtester/live_engine.hpp"
#include "backtester/report.hpp"
#include "strategies/mid_cross_strategy.hpp"
#include "strategies/warmup_take_strategy.hpp"

// ---- feed handler project ----
#include <feedhandler/pipeline.hpp>          // fh::PipelineMsg, fh::kRingCapacity
#include <feedhandler/spsc_ring_buffer.hpp>  // fh::SpscChannel, fh::Backpressure
#include <feedhandler/instrumentation.hpp>   // fh::now_ns()
#include <feedhandler/feed_source.hpp>       // fh::RawFrame
#include <feedhandler/itch_file_source.hpp>  // fh::ItchFileSource
#include <feedhandler/itch_parser.hpp>       // fh::itch::parse, fh::itch::ParseStatus

#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <cstring>

using namespace bt;

// ---- channel type: carries PipelineMsg (MarketEvent + recv_ns + sequence) ----
using Chan = fh::SpscChannel<fh::PipelineMsg, fh::kRingCapacity>;

static void usage() {
    std::cerr
        << "Usage: live_trader --symbol <SYM> --file <path.itch>\n"
        << "         [--latency <ns>]         default 50000\n"
        << "         [--max-pos <N>]          default 100\n"
        << "         [--hard-floor <dollars>] default -1000\n"
        << "         [--output <report.md>]\n"
        << "         [--strategy mid_cross|warmup_take]  default warmup_take\n"
        << "         [--verbose]\n";
}

int main(int argc, char** argv) {
    std::string  symbol;
    std::string  itch_file;
    Timestamp    latency_ns  = 50'000;
    std::string  output_file;
    std::string  strategy_name = "warmup_take";
    bool         verbose     = false;
    RiskLimits   risk_limits;

    for (int i = 1; i < argc; ++i) {
        if      (std::strcmp(argv[i], "--symbol")     == 0 && i+1 < argc) symbol                       = argv[++i];
        else if (std::strcmp(argv[i], "--file")       == 0 && i+1 < argc) itch_file                    = argv[++i];
        else if (std::strcmp(argv[i], "--latency")    == 0 && i+1 < argc) latency_ns                   = std::stoll(argv[++i]);
        else if (std::strcmp(argv[i], "--max-pos")    == 0 && i+1 < argc) risk_limits.max_position     = std::stoll(argv[++i]);
        else if (std::strcmp(argv[i], "--hard-floor") == 0 && i+1 < argc) risk_limits.hard_floor       = std::stod(argv[++i]);
        else if (std::strcmp(argv[i], "--output")     == 0 && i+1 < argc) output_file                  = argv[++i];
        else if (std::strcmp(argv[i], "--strategy")   == 0 && i+1 < argc) strategy_name                = argv[++i];
        else if (std::strcmp(argv[i], "--verbose")    == 0)                verbose                      = true;
        else { usage(); return 1; }
    }
    if (symbol.empty() || itch_file.empty()) { usage(); return 1; }

    // ----------------------------------------------------------------
    // 1. Shared channel (Block policy: Thread A spins if Thread B falls behind).
    // ----------------------------------------------------------------
    Chan channel(fh::Backpressure::Block);

    // ----------------------------------------------------------------
    // 2. Backtester components (Thread B owns these).
    // ----------------------------------------------------------------
    SpscFeedAdapter<Chan> adapter(channel, symbol);
    PaperOrderRouter      router(latency_ns);
    RiskCheck             risk(risk_limits);

    // mid_cross needs several bps of mid move; itch_gen tape is flat ~$100,
    // so default to warmup_take for a visible end-to-end smoke test.
    MidCrossStrategy   mid_cross(20, 0.0005);
    WarmupTakeStrategy warmup_take(50, 1);
    Strategy* strategy = nullptr;
    if (strategy_name == "mid_cross")       strategy = &mid_cross;
    else if (strategy_name == "warmup_take") strategy = &warmup_take;
    else { std::cerr << "unknown --strategy " << strategy_name << "\n"; usage(); return 1; }

    LiveEngineConfig cfg;
    cfg.verbose         = verbose;
    cfg.equity_sample_n = 500;

    LiveEngine engine(adapter, router, *strategy, risk, cfg);

    // ----------------------------------------------------------------
    // 3. Thread A: ingest pipeline.
    //    ItchFileSource → fh::itch::parse → channel.send()
    //    Swap ItchFileSource for your live feed source here.
    // ----------------------------------------------------------------
    std::thread ingest_thread([&]() {
        fh::ItchFileSource src(itch_file);
        if (!src.ok()) {
            std::cerr << "[ingest] cannot open " << itch_file << "\n";
            adapter.shutdown();
            return;
        }

        fh::RawFrame     frame;
        fh::MarketEvent  me;

        while (src.next_frame(frame)) {
            auto status = fh::itch::parse(frame.data, frame.len, me);
            if (status != fh::itch::ParseStatus::Ok) continue;

            fh::PipelineMsg msg{me, fh::now_ns(), frame.sequence};

            // channel.send() spins until space (Backpressure::Block).
            channel.send(msg);
        }

        if (verbose)
            std::cout << "[ingest] done — " << src.frames_read() << " frames\n";

        adapter.shutdown(); // signals LiveEngine to stop after draining channel
    });

    // ----------------------------------------------------------------
    // 4. Thread B: live engine (blocks until channel drained + shutdown).
    // ----------------------------------------------------------------
    std::cout << "Live trading: symbol=" << symbol
              << " file=" << itch_file
              << " latency=" << latency_ns << "ns"
              << " max_pos=" << risk_limits.max_position
              << " hard_floor=$" << risk_limits.hard_floor
              << "\n";

    engine.run();
    ingest_thread.join();

    // Log sequencer stats (gaps, duplicates, recoveries)
    auto& seq_stats = adapter.sequencer().stats();
    std::cout << "[sequencer] accepted=" << seq_stats.accepted
              << " gaps="      << seq_stats.gap_episodes
              << " missing="   << seq_stats.missing_total
              << " buffered="  << seq_stats.buffered_released
              << " recoveries="<< seq_stats.snapshot_recoveries
              << " drops="     << channel.dropped()
              << "\n";

    if (risk.killed())
        std::cerr << "[WARN] Risk kill triggered.\n";

    // ----------------------------------------------------------------
    // 5. Report.
    // ----------------------------------------------------------------
    auto rd = make_report_data(
        engine.pnl(), strategy_name + "(" + symbol + ")", itch_file,
        /*start_ns=*/0, engine.clock().now(),
        latency_ns, engine.order_count(), engine.book().mid_price());

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
