#pragma once
// ============================================
// dual_md_gateway.h - Dual-active market data gateway
// Wraps primary + backup IMdGateway instances with automatic failover.
// ============================================

#include "gateway/i_md_gateway.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hft {

class DualMdGateway : public IMdGateway {
public:
    using GatewayFactory = std::function<std::unique_ptr<IMdGateway>(const std::string& type)>;

    void set_gateway_factory(GatewayFactory factory) { factory_ = std::move(factory); }

    void init(const Config& config, const std::string& section, TradingEngine* engine) override;
    void subscribe(const std::vector<std::string>& instruments) override;
    void unsubscribe(const std::vector<std::string>& instruments) override;
    void subscribe_append(const std::vector<std::string>& instruments) override;
    void stop() override;
    bool is_logged_in() const override;
    bool wait_for_login(int timeout_sec = 10) override;
    MdGatewayStatus status() const override;

private:
    void on_primary_status(MdGatewayStatus old_s, MdGatewayStatus new_s);
    void on_backup_status(MdGatewayStatus old_s, MdGatewayStatus new_s);
    void failover_to_backup();
    void failback_to_primary();

    std::unique_ptr<IMdGateway> primary_;
    std::unique_ptr<IMdGateway> backup_;
    GatewayFactory factory_;
    std::atomic<bool> using_primary_{true};
    std::vector<std::string> subscribed_;
    mutable std::mutex sub_mtx_;
    TradingEngine* engine_ = nullptr;
};

} // namespace hft
