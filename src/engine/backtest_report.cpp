#include "engine/backtest_report.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <sstream>

namespace hft {

BacktestReport compute_backtest_report(const std::vector<BacktestTrade>& trades,
                                       int total_ticks) {
    BacktestReport report;
    report.total_ticks = total_ticks;
    report.total_trades = static_cast<int>(trades.size());

    if (trades.empty()) return report;

    // Match open/close trades by instrument to compute round-trip PnL
    struct OpenPos {
        double avg_price = 0.0;
        int volume = 0;
        Direction direction = Direction::Buy;
    };
    std::map<std::string, OpenPos> positions;
    std::vector<double> round_trip_pnls;
    double gross_profit = 0.0;
    double gross_loss = 0.0;

    for (const auto& t : trades) {
        auto& pos = positions[t.instrument];

        if (t.offset == Offset::Open) {
            if (pos.volume == 0) {
                pos.avg_price = t.price;
                pos.volume = t.volume;
                pos.direction = t.direction;
            } else if (pos.direction == t.direction) {
                double total_cost = pos.avg_price * pos.volume + t.price * t.volume;
                pos.volume += t.volume;
                pos.avg_price = total_cost / pos.volume;
            }
        } else {
            if (pos.volume <= 0) continue;
            int close_vol = std::min(t.volume, pos.volume);
            double pnl = 0.0;
            if (pos.direction == Direction::Buy) {
                pnl = (t.price - pos.avg_price) * close_vol;
            } else {
                pnl = (pos.avg_price - t.price) * close_vol;
            }
            round_trip_pnls.push_back(pnl);
            if (pnl > 0) gross_profit += pnl;
            else gross_loss += std::abs(pnl);
            pos.volume -= close_vol;
        }
    }

    if (round_trip_pnls.empty()) {
        report.total_pnl = 0;
        return report;
    }

    for (double pnl : round_trip_pnls) {
        report.total_pnl += pnl;
        if (pnl > 0) report.winning_trades++;
        else if (pnl < 0) report.losing_trades++;
    }

    report.win_rate = static_cast<double>(report.winning_trades) /
                      static_cast<double>(round_trip_pnls.size());

    report.avg_trade_pnl = report.total_pnl / static_cast<double>(round_trip_pnls.size());

    report.profit_factor = (gross_loss > 0) ? (gross_profit / gross_loss) : 0.0;

    // Max drawdown from cumulative PnL curve
    double cumulative = 0.0;
    double peak = 0.0;
    report.max_pnl = -1e18;
    report.min_pnl = 1e18;
    std::vector<double> cum_pnls;
    cum_pnls.reserve(round_trip_pnls.size());

    for (double pnl : round_trip_pnls) {
        cumulative += pnl;
        cum_pnls.push_back(cumulative);
        if (cumulative > peak) peak = cumulative;
        double dd = peak - cumulative;
        if (dd > report.max_drawdown) report.max_drawdown = dd;
        if (cumulative > report.max_pnl) report.max_pnl = cumulative;
        if (cumulative < report.min_pnl) report.min_pnl = cumulative;
    }

    // Sharpe ratio (simplified: mean/stddev of per-trade returns)
    if (round_trip_pnls.size() > 1) {
        double mean = report.avg_trade_pnl;
        double sum_sq = 0.0;
        for (double pnl : round_trip_pnls) {
            double diff = pnl - mean;
            sum_sq += diff * diff;
        }
        double stddev = std::sqrt(sum_sq / static_cast<double>(round_trip_pnls.size() - 1));
        report.sharpe_ratio = (stddev > 1e-12) ? (mean / stddev) : 0.0;
    }

    return report;
}

std::string BacktestReport::to_string() const {
    std::ostringstream ss;
    ss << "====== Backtest Report ======\n";

    char buf[128];
    std::snprintf(buf, sizeof(buf), "  Total Ticks:     %d\n", total_ticks); ss << buf;
    std::snprintf(buf, sizeof(buf), "  Total Trades:    %d\n", total_trades); ss << buf;
    std::snprintf(buf, sizeof(buf), "  Round Trips:     %d (W:%d / L:%d)\n",
                  winning_trades + losing_trades, winning_trades, losing_trades); ss << buf;
    std::snprintf(buf, sizeof(buf), "  Total PnL:       %.2f\n", total_pnl); ss << buf;
    std::snprintf(buf, sizeof(buf), "  Max PnL:         %.2f\n", max_pnl); ss << buf;
    std::snprintf(buf, sizeof(buf), "  Min PnL:         %.2f\n", min_pnl); ss << buf;
    std::snprintf(buf, sizeof(buf), "  Max Drawdown:    %.2f\n", max_drawdown); ss << buf;
    std::snprintf(buf, sizeof(buf), "  Win Rate:        %.1f%%\n", win_rate * 100.0); ss << buf;
    std::snprintf(buf, sizeof(buf), "  Profit Factor:   %.2f\n", profit_factor); ss << buf;
    std::snprintf(buf, sizeof(buf), "  Avg Trade PnL:   %.2f\n", avg_trade_pnl); ss << buf;
    std::snprintf(buf, sizeof(buf), "  Sharpe Ratio:    %.3f\n", sharpe_ratio); ss << buf;

    ss << "=============================\n";
    return ss.str();
}

} // namespace hft
