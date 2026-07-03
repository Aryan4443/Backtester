#pragma once
#include "pnl_tracker.hpp"
#include <string>
#include <cstddef>

namespace bt {

struct ReportData {
    std::string strategy_name;
    std::string data_source;
    Timestamp   start_ns        = 0;
    Timestamp   end_ns          = 0;
    Timestamp   latency_ns      = 0;

    // From PnLTracker
    double   final_pnl           = 0.0;
    double   realized_pnl        = 0.0;
    double   avg_slippage_cents  = 0.0;
    double   total_slippage_cents = 0.0;
    std::size_t fill_count       = 0;
    std::size_t order_count      = 0;
    double   fill_rate           = 0.0;   // fills / orders

    // Equity curve (sampled)
    std::vector<PnLTracker::EquityPoint> equity_curve;
};

// Generate a markdown report string.
std::string generate_report(const ReportData& rd);

// Helper: populate ReportData from a PnLTracker.
ReportData make_report_data(const PnLTracker& tracker,
                             const std::string& strategy_name,
                             const std::string& data_source,
                             Timestamp start_ns,
                             Timestamp end_ns,
                             Timestamp latency_ns,
                             std::size_t order_count,
                             Price final_mark);

} // namespace bt
