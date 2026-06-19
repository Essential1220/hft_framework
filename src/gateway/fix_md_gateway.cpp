// ============================================
// fix_md_gateway.cpp - FIX protocol market data gateway stub
// All methods return stub values. TODO: QuickFIX/fix8 integration
// ============================================

#include "gateway/fix_md_gateway.h"
#include "common/logger.h"

namespace hft {

void FixMdGateway::init(const Config&, const std::string&, TradingEngine*) {
    LOG_INFO("FixMdGateway: stub initialized (no FIX library linked)");
    // TODO: QuickFIX/fix8 integration
}

void FixMdGateway::subscribe(const std::vector<std::string>&) {
    // TODO: send MarketDataRequest
}

void FixMdGateway::unsubscribe(const std::vector<std::string>&) {
    // TODO: send MarketDataRequest (unsubscribe)
}

void FixMdGateway::stop() {
    status_ = MdGatewayStatus::Disconnected;
}

bool FixMdGateway::is_logged_in() const {
    return false;
}

bool FixMdGateway::wait_for_login(int) {
    return false;
}

MdGatewayStatus FixMdGateway::status() const {
    return status_;
}

} // namespace hft
