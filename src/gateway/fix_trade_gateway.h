#pragma once
// ============================================
// fix_trade_gateway.h - FIX protocol trade gateway stub
// Placeholder for QuickFIX/fix8 integration.
// ============================================

#include "gateway/i_trade_gateway.h"

namespace hft {

class FixTradeGateway : public ITradeGateway {
public:
    void init(const Config& config, const std::string& section,
              TradingEngine* engine, const std::string& account_id) override;
    void stop() override;
    bool wait_for_login(int timeout_sec = 30) override;
    bool is_logged_in() const override;

    int send_order(const OrderRequest& req, const std::string& order_ref) override;
    int cancel_order(const std::string& instrument_id, const std::string& exchange_id,
                     const std::string& order_ref, int front_id, int session_id) override;
    int query_account() override;
    int query_position(const std::string& instrument_id = "") override;
    int query_active_orders() override;
    std::vector<std::string> query_instruments(int timeout_sec = 30) override;

    int get_front_id() const override;
    int get_session_id() const override;
    int get_max_order_ref() const override;
};

} // namespace hft
