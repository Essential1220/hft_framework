// ============================================
// fix_trade_gateway.cpp - FIX protocol trade gateway stub
// All methods return stub values. TODO: QuickFIX/fix8 integration
// ============================================

#include "gateway/fix_trade_gateway.h"
#include "common/logger.h"

namespace hft {

void FixTradeGateway::init(const Config&, const std::string&,
                           TradingEngine*, const std::string& account_id) {
    account_id_ = account_id;
    LOG_INFO("FixTradeGateway: stub initialized for " + account_id + " (no FIX library linked)");
    // TODO: QuickFIX/fix8 integration
}

void FixTradeGateway::stop() {}

bool FixTradeGateway::wait_for_login(int) { return false; }

bool FixTradeGateway::is_logged_in() const { return false; }

int FixTradeGateway::send_order(const OrderRequest&, const std::string&) {
    return -1; // TODO: send NewOrderSingle
}

int FixTradeGateway::cancel_order(const std::string&, const std::string&,
                                   const std::string&, int, int) {
    return -1; // TODO: send OrderCancelRequest
}

int FixTradeGateway::query_account() { return -1; }
int FixTradeGateway::query_position(const std::string&) { return -1; }
int FixTradeGateway::query_active_orders() { return -1; }
std::vector<std::string> FixTradeGateway::query_instruments(int) { return {}; }

int FixTradeGateway::get_front_id() const { return 0; }
int FixTradeGateway::get_session_id() const { return 0; }
int FixTradeGateway::get_max_order_ref() const { return 0; }

} // namespace hft
