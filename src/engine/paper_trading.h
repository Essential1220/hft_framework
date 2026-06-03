#pragma once
// ============================================
// paper_trading.h - Paper trading / simulation engine (模拟交易引擎)
// Simulates order execution for backtesting and dry-run scenarios.
// (为回测与演练场景模拟订单执行)
// ============================================

#include "common/types.h"
#include <atomic>
#include <mutex>
#include <string>
#include <cstdint>

namespace hft {

class TradingEngine;

class PaperTradingEngine {
public:
    void init(TradingEngine* engine);
    bool start(const std::string& account_id = "");
    void stop();
    bool is_active() const;
    std::string active_account_id() const;

    SendOrderResult simulate_order(const OrderRequest& req);

    struct Stats {
        int total_orders = 0;
        int total_fills = 0;
        double realized_pnl = 0.0;
    };
    Stats get_stats() const;
    void reset_stats();

private:
    TradingEngine* engine_ = nullptr;
    std::atomic<bool> active_{false};
    mutable std::mutex mtx_;
    std::string account_id_;
    std::atomic<uint32_t> paper_ref_seq_{900000};
    Stats stats_;
};

} // namespace hft
