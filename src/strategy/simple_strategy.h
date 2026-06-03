#pragma once
// ============================================
// simple_strategy.h - Simple momentum strategy (简单动量策略)
//
// A C++ native strategy that trades on consecutive price movements
// with configurable cooldown to prevent overtrading.
// 基于连续价格变动交易的 C++ 原生策略，带可配置冷却期防止过度交易。
// ============================================

#include "strategy/strategy_base.h"

#include <chrono>
#include <cstring>
#include <deque>

namespace hft {

class SimpleStrategy : public StrategyBase {
public:
    // Constructor: initialize strategy parameters (构造函数，初始化策略参数)
    // instrument: instrument code to trade (交易合约代码)
    // order_size: lots per order (每次下单的手数)
    // momentum_ticks: number of consecutive ticks for momentum (计算动量的 Tick 数量)
    // cooldown_seconds: cooldown after signal before next trade (下单后的冷却时间/秒)
    SimpleStrategy(const char* instrument, int order_size,
                   int momentum_ticks, int cooldown_seconds);

    // ---- Implement StrategyBase interface (实现 StrategyBase 接口) ----
    void on_init() override;
    void on_tick(const TickData& tick) override;
    void on_order(const OrderInfo& order) override;
    void on_trade(const TradeInfo& trade) override;
    void on_reconnect() override;

private:
    // Sync current position from the engine (从引擎同步当前持仓 / 同步持仓)
    void sync_position_from_engine();
    // Check if the strategy is in cooldown (检查是否处于冷却期 / 冷却检查)
    bool is_cooling_down() const;
    // Check if the tick's bid and ask prices are valid (检查 Tick 数据中买卖价是否有效 / 盘口有效性)
    bool is_valid_bid_ask(const TickData& tick) const;

    // ---- Strategy parameters (策略参数) ----
    char instrument_[16]{};       // Trading instrument (交易合约)
    int order_size_ = 1;          // Order size in lots (下单手数)
    int momentum_ticks_ = 3;      // Momentum period in ticks (动量周期/Tick数)
    int cooldown_seconds_ = 5;    // Cooldown duration in seconds (冷却时间/秒)

    // ---- Strategy state (策略状态) ----
    std::deque<double> price_history_; // Historical price queue for momentum calculation (历史价格队列，用于计算动量)
    int position_ = 0;                 // Current net position (当前净持仓)
    std::chrono::steady_clock::time_point last_signal_time_; // Time of last signal (上次产生信号的时间)
    bool in_cooldown_ = false;         // Whether currently in cooldown (是否处于冷却状态)
};

} // namespace hft
