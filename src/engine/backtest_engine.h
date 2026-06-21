#pragma once

#include "engine/backtest_report.h"
#include "engine/i_trading_context.h"
#include "common/types.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hft {

class StrategyBase;

struct BacktestConfig {
    std::vector<std::string> tick_files;
    std::string strategy_type;
    std::string script_path;
    std::string instrument;
    int order_size = 1;
};

class BacktestContext : public ITradingContext {
public:
    void send_order(const OrderRequest& req) override;
    std::string send_order_with_ref(const OrderRequest& req) override;
    void cancel_order(const std::string& order_ref) override;
    bool cancel_order(const std::string& order_ref, const std::string& account_id) override;
    uint32_t add_conditional_order(const ConditionalOrder& order) override;
    void cancel_conditional_order(uint32_t id) override;
    uint32_t allocate_cond_group_id() override;
    PositionInfo get_position(const char* instrument, Direction dir, const std::string& account_id) const override;
    int get_net_position(const char* instrument, const std::string& account_id) const override;
    WindowedOrderBook get_order_book(const char* instrument) const override;
    AccountInfo get_account_info(const std::string& account_id) const override;
    void strategy_log(const std::string& strategy_id, int level, const std::string& message) override;
    void save_strategy_state(const std::string& strategy_id,
                             const std::map<std::string, std::string>& state) override;
    std::map<std::string, std::string> load_strategy_state(const std::string& strategy_id) override;
    int register_timer(const std::string& strategy_id, int interval_ms) override;
    void unregister_timer(int timer_id) override;
    std::vector<KlineBar> query_klines(const std::string& instrument,
                                        const std::string& period,
                                        size_t count) const override;

    void set_current_tick(const TickData& tick) { current_tick_ = tick; }
    const std::vector<BacktestTrade>& trades() const { return trades_; }
    void set_strategy(StrategyBase* s) { strategy_ = s; }

private:
    TickData current_tick_{};
    std::vector<BacktestTrade> trades_;
    std::map<std::string, int> net_positions_;
    uint32_t order_seq_ = 0;
    uint32_t group_seq_ = 0;
    StrategyBase* strategy_ = nullptr;
    std::map<std::string, std::map<std::string, std::string>> saved_states_;
};

class BacktestEngine {
public:
    bool run(const BacktestConfig& config);
    const BacktestReport& report() const { return report_; }

private:
    BacktestReport report_;
};

} // namespace hft
