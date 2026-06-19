// ============================================
// dual_trade_gateway.cpp - Dual-active trade gateway implementation
// ============================================

#include "gateway/dual_trade_gateway.h"
#include "common/logger.h"

namespace hft {

void DualTradeGateway::init(const Config& config, const std::string& section,
                            TradingEngine* engine, const std::string& account_id) {
    account_id_ = account_id;

    if (primary_) {
        primary_->init(config, section, engine, account_id);
    }

    std::string backup_front = config.get_string(section, "TradeFrontBackup", "");
    if (backup_ && !backup_front.empty()) {
        Config backup_config = config;
        backup_config.set_string(section, "TradeFront", backup_front);
        backup_->init(backup_config, section, engine, account_id);
    }
}

void DualTradeGateway::stop() {
    if (primary_) primary_->stop();
    if (backup_) backup_->stop();
}

bool DualTradeGateway::wait_for_login(int timeout_sec) {
    if (primary_ && primary_->wait_for_login(timeout_sec)) return true;
    if (backup_) return backup_->wait_for_login(timeout_sec);
    return false;
}

bool DualTradeGateway::is_logged_in() const {
    auto* a = active();
    return a && a->is_logged_in();
}

int DualTradeGateway::send_order(const OrderRequest& req, const std::string& order_ref) {
    auto* a = active();
    if (!a) return -1;
    return a->send_order(req, order_ref);
}

int DualTradeGateway::cancel_order(const std::string& instrument_id, const std::string& exchange_id,
                                    const std::string& order_ref, int front_id, int session_id) {
    auto* a = active();
    if (!a) return -1;
    return a->cancel_order(instrument_id, exchange_id, order_ref, front_id, session_id);
}

int DualTradeGateway::query_account() {
    auto* a = active();
    if (!a) return -1;
    return a->query_account();
}

int DualTradeGateway::query_position(const std::string& instrument_id) {
    auto* a = active();
    if (!a) return -1;
    return a->query_position(instrument_id);
}

int DualTradeGateway::query_active_orders() {
    auto* a = active();
    if (!a) return -1;
    return a->query_active_orders();
}

std::vector<std::string> DualTradeGateway::query_instruments(int timeout_sec) {
    auto* a = active();
    if (!a) return {};
    return a->query_instruments(timeout_sec);
}

int DualTradeGateway::get_front_id() const {
    auto* a = active();
    return a ? a->get_front_id() : 0;
}

int DualTradeGateway::get_session_id() const {
    auto* a = active();
    return a ? a->get_session_id() : 0;
}

int DualTradeGateway::get_max_order_ref() const {
    auto* a = active();
    return a ? a->get_max_order_ref() : 0;
}

void DualTradeGateway::failover() {
    if (!backup_) return;
    LOG_WARN("DualTradeGateway: failing over to backup for " + account_id_);
    using_primary_.store(false, std::memory_order_relaxed);
}

void DualTradeGateway::failback() {
    LOG_INFO("DualTradeGateway: failing back to primary for " + account_id_);
    using_primary_.store(true, std::memory_order_relaxed);
}

ITradeGateway* DualTradeGateway::active() const {
    if (using_primary_.load(std::memory_order_relaxed) && primary_) {
        return primary_.get();
    }
    return backup_.get();
}

} // namespace hft
