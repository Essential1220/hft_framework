#pragma once

#include "common/types.h"

#include <string>
#include <vector>

namespace hft {

struct BacktestTrade {
    std::string instrument;
    Direction direction;
    Offset offset;
    double price;
    int volume;
    std::string time;
};

struct BacktestReport {
    int total_ticks = 0;
    int total_trades = 0;
    int winning_trades = 0;
    int losing_trades = 0;
    double total_pnl = 0.0;
    double max_drawdown = 0.0;
    double sharpe_ratio = 0.0;
    double win_rate = 0.0;
    double profit_factor = 0.0;
    double max_pnl = 0.0;
    double min_pnl = 0.0;
    double avg_trade_pnl = 0.0;

    std::string to_string() const;
};

BacktestReport compute_backtest_report(const std::vector<BacktestTrade>& trades,
                                       int total_ticks);

} // namespace hft
