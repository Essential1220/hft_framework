#pragma once
// ============================================
// i_trading_context.h - Trading context interface (交易上下文接口)
// Provides abstract interface for strategy to send/cancel orders and query positions.
// (为策略提供下单、撤单、持仓查询的抽象接口)
// ============================================

#include "common/types.h"
#include "market/order_book.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hft {

struct KlineBar;

class ITradingContext {
public:
    virtual ~ITradingContext() = default;

    virtual void send_order(const OrderRequest& req) = 0;
    virtual std::string send_order_with_ref(const OrderRequest& req) = 0;
    virtual void cancel_order(const std::string& order_ref) = 0;
    virtual bool cancel_order(const std::string& order_ref, const std::string& account_id) = 0;

    virtual uint32_t add_conditional_order(const ConditionalOrder& order) = 0;
    virtual void cancel_conditional_order(uint32_t id) = 0;
    virtual uint32_t allocate_cond_group_id() = 0;

    virtual PositionInfo get_position(const char* instrument, Direction dir, const std::string& account_id) const = 0;
    virtual int get_net_position(const char* instrument, const std::string& account_id) const = 0;

    virtual WindowedOrderBook get_order_book(const char* instrument) const = 0;
    virtual AccountInfo get_account_info(const std::string& account_id) const = 0;

    virtual void strategy_log(const std::string& strategy_id, int level, const std::string& message) = 0;

    virtual void save_strategy_state(const std::string& strategy_id,
                                     const std::map<std::string, std::string>& state) = 0;
    virtual std::map<std::string, std::string> load_strategy_state(const std::string& strategy_id) = 0;

    virtual int register_timer(const std::string& strategy_id, int interval_ms) = 0;
    virtual void unregister_timer(int timer_id) = 0;

    virtual std::vector<KlineBar> query_klines(const std::string& instrument,
                                                const std::string& period,
                                                size_t count) const = 0;
};

} // namespace hft
