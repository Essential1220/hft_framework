#pragma once
// ============================================
// fix_md_gateway.h - FIX protocol market data gateway stub
// Placeholder for QuickFIX/fix8 integration.
// ============================================

#include "gateway/i_md_gateway.h"

namespace hft {

class FixMdGateway : public IMdGateway {
public:
    void init(const Config& config, const std::string& section, TradingEngine* engine) override;
    void subscribe(const std::vector<std::string>& instruments) override;
    void unsubscribe(const std::vector<std::string>& instruments) override;
    void stop() override;
    bool is_logged_in() const override;
    bool wait_for_login(int timeout_sec = 10) override;
    MdGatewayStatus status() const override;

private:
    MdGatewayStatus status_ = MdGatewayStatus::Disconnected;
};

} // namespace hft
