#pragma once
// ============================================
// dual_trade_gateway.h - Dual-active trade gateway
// Wraps primary + backup ITradeGateway instances with automatic failover.
// ============================================

#include "gateway/i_trade_gateway.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hft {

class DualTradeGateway : public ITradeGateway {
public:
    using GatewayFactory = std::function<std::unique_ptr<ITradeGateway>(const std::string& type)>;

    void set_gateway_factory(GatewayFactory factory) { factory_ = std::move(factory); }
    void set_primary(std::unique_ptr<ITradeGateway> gw) { primary_ = std::move(gw); }
    void set_backup(std::unique_ptr<ITradeGateway> gw) { backup_ = std::move(gw); }

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

    void failover();
    void failback();
    bool is_using_primary() const { return using_primary_.load(std::memory_order_relaxed); }

private:
    ITradeGateway* active() const;

    std::unique_ptr<ITradeGateway> primary_;
    std::unique_ptr<ITradeGateway> backup_;
    GatewayFactory factory_;
    std::atomic<bool> using_primary_{true};
};

} // namespace hft
