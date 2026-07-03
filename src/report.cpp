#include "backtester/report.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>

namespace bt {

static std::string fmt_ns(Timestamp ns) {
    // Format nanoseconds as HH:MM:SS.mmm (ignoring date for brevity)
    int64_t ms = ns / 1'000'000LL;
    int64_t s  = ms / 1000; ms %= 1000;
    int64_t m  = s / 60;    s  %= 60;
    int64_t h  = m / 60;    m  %= 60;
    std::ostringstream ss;
    ss << std::setfill('0')
       << std::setw(2) << h << ':'
       << std::setw(2) << m << ':'
       << std::setw(2) << s << '.'
       << std::setw(3) << ms;
    return ss.str();
}

static std::string fmt_price(double cents) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << (cents / 100.0);
    return "$" + ss.str();
}

std::string generate_report(const ReportData& rd) {
    std::ostringstream o;

    o << "# Backtest Report — " << rd.strategy_name << "\n\n";
    o << "| Field | Value |\n";
    o << "|---|---|\n";
    o << "| Strategy | " << rd.strategy_name << " |\n";
    o << "| Data source | " << rd.data_source << " |\n";
    o << "| Sim start | " << fmt_ns(rd.start_ns) << " |\n";
    o << "| Sim end | "   << fmt_ns(rd.end_ns)   << " |\n";
    o << "| Latency model | " << (rd.latency_ns / 1000) << " µs |\n";
    o << "\n## PnL Summary\n\n";
    o << "| Metric | Value |\n";
    o << "|---|---|\n";
    // final_pnl / realized_pnl are in dollars; fmt_price expects cents
    o << "| Final PnL | " << fmt_price(rd.final_pnl * 100.0) << " |\n";
    o << "| Realized PnL | " << fmt_price(rd.realized_pnl * 100.0) << " |\n";
    o << "\n## Execution Quality\n\n";
    o << "> Execution quality metrics reveal whether the strategy's performance is\n"
      << "> real alpha or an artefact of unrealistic fill assumptions.  A strategy\n"
      << "> with positive PnL but high slippage may be entirely explained by the\n"
      << "> slippage being unrealistically small in the sim.\n\n";
    o << "| Metric | Value |\n";
    o << "|---|---|\n";
    o << "| Orders submitted | " << rd.order_count << " |\n";
    o << "| Fills received | "   << rd.fill_count  << " |\n";
    o << std::fixed << std::setprecision(1);
    o << "| Fill rate | " << (rd.fill_rate * 100.0) << "% |\n";
    o << "| Avg slippage | " << fmt_price(rd.avg_slippage_cents) << " / share |\n";
    o << "| Total slippage cost | " << fmt_price(rd.total_slippage_cents) << " |\n";
    o << "| Latency cost | "
      << fmt_price(rd.avg_slippage_cents * static_cast<double>(rd.fill_count))
      << " (proxy) |\n";

    o << "\n## Equity Curve (sampled)\n\n";
    o << "| Time | PnL | Mark |\n";
    o << "|---|---|---|\n";

    // Sample at most 50 points
    const auto& ec = rd.equity_curve;
    std::size_t step = std::max<std::size_t>(1, ec.size() / 50);
    for (std::size_t i = 0; i < ec.size(); i += step) {
        o << "| " << fmt_ns(ec[i].ts)
          << " | " << fmt_price(ec[i].pnl * 100)
          << " | " << fmt_price(static_cast<double>(ec[i].mark))
          << " |\n";
    }

    return o.str();
}

ReportData make_report_data(const PnLTracker& tracker,
                             const std::string& strategy_name,
                             const std::string& data_source,
                             Timestamp start_ns,
                             Timestamp end_ns,
                             Timestamp latency_ns,
                             std::size_t order_count,
                             Price final_mark)
{
    ReportData rd;
    rd.strategy_name       = strategy_name;
    rd.data_source         = data_source;
    rd.start_ns            = start_ns;
    rd.end_ns              = end_ns;
    rd.latency_ns          = latency_ns;
    rd.final_pnl           = tracker.total_pnl(final_mark);
    rd.realized_pnl        = tracker.realized_pnl();
    rd.avg_slippage_cents  = tracker.avg_slippage_cents();
    rd.total_slippage_cents = tracker.total_slippage_cents();
    rd.fill_count          = tracker.fill_count();
    rd.order_count         = order_count;
    rd.fill_rate           = (order_count > 0)
                             ? static_cast<double>(tracker.fill_count()) / order_count
                             : 0.0;
    rd.equity_curve        = tracker.equity_curve();
    return rd;
}

} // namespace bt
