// ============================================
// strategy_base.cpp - Strategy base class implementation (策略基类实现)
//
// Implements event filtering, order proxy methods, and context configuration.
// 实现事件过滤、下单代理方法、上下文配置。
// ============================================

#include "strategy/strategy_base.h"

#include "common/logger.h"

#include <algorithm>
#include <cstdlib>

namespace hft {

// Configure strategy context (配置策略上下文)
void StrategyBase::configure_context(std::string strategy_id, std::string default_account_id,
                                     std::vector<std::string> watched_instruments) {
    strategy_id_ = std::move(strategy_id);
    default_account_id_ = std::move(default_account_id);
    watched_instruments_ = std::move(watched_instruments);
    watched_hashes_.clear();
    for (const auto& inst : watched_instruments_) {
        watched_hashes_.insert(instrument_hash(inst.c_str()));
    }
}

void StrategyBase::configure_metadata(std::string strategy_type, std::string script_path,
                                      std::map<std::string, std::string> parameters) {
    strategy_type_ = std::move(strategy_type);
    script_path_ = std::move(script_path);
    parameters_ = std::move(parameters);
    // Hot path reads is_python_ directly; do case-insensitive comparison once here (热路径用is_python_直读，这里一次性做不区分大小写比较)
    std::string lower = strategy_type_;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    is_python_ = (lower == "python");
}

std::string StrategyBase::get_parameter(const std::string& key, const std::string& default_value) const {
    const auto it = parameters_.find(key);
    return it == parameters_.end() ? default_value : it->second;
}

int StrategyBase::get_parameter_int(const std::string& key, int default_value) const {
    const auto it = parameters_.find(key);
    if (it == parameters_.end() || it->second.empty()) {
        return default_value;
    }
    try {
        return std::stoi(it->second);
    } catch (...) {
        return default_value;
    }
}

double StrategyBase::get_parameter_double(const std::string& key, double default_value) const {
    const auto it = parameters_.find(key);
    if (it == parameters_.end() || it->second.empty()) {
        return default_value;
    }
    try {
        return std::stod(it->second);
    } catch (...) {
        return default_value;
    }
}

// Check if the event targets this strategy (检查是否处理指定策略的事件 / 策略匹配)
bool StrategyBase::handles_strategy(const char* strategy_id) const {
    if (!strategy_id || strategy_id[0] == '\0') {
        return true; // If event has no strategy_id, handle by default (如果事件未指定策略ID，默认处理)
    }
    return strategy_id_ == strategy_id;
}

// Check if the event targets this strategy's account (检查是否处理指定账户的事件 / 账户匹配)
bool StrategyBase::handles_account(const char* account_id) const {
    if (default_account_id_.empty()) {
        return true; // If strategy is not bound to a specific account, handle all (如果策略未绑定特定账户，默认处理所有账户的事件)
    }
    if (!account_id || account_id[0] == '\0') {
        return true;
    }
    return default_account_id_ == account_id;
}

// Check if the instrument is in this strategy's watchlist (检查是否处理指定合约的事件 / 合约匹配)
bool StrategyBase::handles_instrument(const char* instrument_id) const {
    if (watched_instruments_.empty()) {
        return true;
    }
    if (!instrument_id || instrument_id[0] == '\0') {
        return false;
    }
    return watched_hashes_.count(instrument_hash(instrument_id)) > 0;
}

// Combined check: account AND instrument must both match (综合检查是否处理该事件 / 综合事件匹配)
bool StrategyBase::handles_event(const char* account_id, const char* instrument_id) const {
    return handles_account(account_id) && handles_instrument(instrument_id);
}

// ---- Order and query proxy methods (下单与查询的代理方法) ----
// Auto-fill strategy_id, account_id, exchange_id from context, then delegate to engine.
// 自动填充策略 ID、账户 ID 等上下文信息，并调用引擎接口。

std::string StrategyBase::send_order(const OrderRequest& req) {
    if (!engine_) return "";
    OrderRequest final_req = req;
    safe_copy(final_req.strategy_id, strategy_id_.c_str(), sizeof(final_req.strategy_id));
    if (final_req.account_id[0] == '\0' && !default_account_id_.empty()) {
        safe_copy(final_req.account_id, default_account_id_.c_str(), sizeof(final_req.account_id));
    }
    if (final_req.exchange_id[0] == '\0') {
        safe_copy(final_req.exchange_id, get_exchange_id(final_req.instrument_id), sizeof(final_req.exchange_id));
    }
    return engine_->send_order_with_ref(final_req);
}

void StrategyBase::cancel_order(const std::string& order_ref) {
    if (!engine_) return;
    if (default_account_id_.empty()) {
        engine_->cancel_order(order_ref);
    } else {
        engine_->cancel_order(order_ref, default_account_id_);
    }
}

uint32_t StrategyBase::add_conditional_order(const ConditionalOrder& order) {
    if (!engine_) return 0;
    ConditionalOrder final_order = order;
    safe_copy(final_order.strategy_id, strategy_id_.c_str(), sizeof(final_order.strategy_id));
    if (final_order.account_id[0] == '\0' && !default_account_id_.empty()) {
        safe_copy(final_order.account_id, default_account_id_.c_str(), sizeof(final_order.account_id));
    }
    return engine_->add_conditional_order(final_order);
}

void StrategyBase::cancel_conditional_order(uint32_t id) {
    if (engine_) {
        engine_->cancel_conditional_order(id);
    }
}

uint32_t StrategyBase::allocate_cond_group_id() {
    if (engine_) {
        return engine_->allocate_cond_group_id();
    }
    return 0;
}

PositionInfo StrategyBase::get_position(const char* instrument, Direction dir) {
    if (!engine_) return PositionInfo{};
    return engine_->get_position(instrument, dir, default_account_id_.c_str());
}

PositionInfo StrategyBase::get_position(const char* instrument, Direction dir, const char* account_id) {
    if (!engine_) return PositionInfo{};
    return engine_->get_position(instrument, dir, account_id ? account_id : "");
}

int StrategyBase::get_net_position(const char* instrument) {
    if (!engine_) return 0;
    return engine_->get_net_position(instrument, default_account_id_.c_str());
}

int StrategyBase::get_net_position(const char* instrument, const char* account_id) {
    if (!engine_) return 0;
    return engine_->get_net_position(instrument, account_id ? account_id : "");
}

WindowedOrderBook StrategyBase::get_order_book(const char* instrument) {
    if (!engine_) return WindowedOrderBook{};
    return engine_->get_order_book(instrument);
}

AccountInfo StrategyBase::get_account_info(const std::string& account_id) {
    if (!engine_) return AccountInfo{};
    const std::string& aid = account_id.empty() ? default_account_id_ : account_id;
    return engine_->get_account_info(aid);
}

void StrategyBase::log_info(const std::string& msg) {
    if (engine_) engine_->strategy_log(strategy_id_, 0, msg);
}

void StrategyBase::log_warn(const std::string& msg) {
    if (engine_) engine_->strategy_log(strategy_id_, 1, msg);
}

void StrategyBase::log_error(const std::string& msg) {
    if (engine_) engine_->strategy_log(strategy_id_, 2, msg);
}

void StrategyBase::save_state(const std::map<std::string, std::string>& state) {
    if (engine_) engine_->save_strategy_state(strategy_id_, state);
}

std::map<std::string, std::string> StrategyBase::load_state() {
    if (!engine_) return {};
    return engine_->load_strategy_state(strategy_id_);
}

int StrategyBase::register_timer(int interval_ms) {
    if (!engine_) return -1;
    return engine_->register_timer(strategy_id_, interval_ms);
}

void StrategyBase::unregister_timer(int timer_id) {
    if (engine_) engine_->unregister_timer(timer_id);
}

std::vector<KlineBar> StrategyBase::query_klines(const std::string& instrument,
                                                  const std::string& period,
                                                  size_t count) {
    if (!engine_) return {};
    return engine_->query_klines(instrument, period, count);
}

} // namespace hft
