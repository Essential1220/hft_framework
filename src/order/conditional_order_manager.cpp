// ============================================
// conditional_order_manager.cpp - Conditional order manager implementation (条件单管理器实现)
//
// Implements stop-loss (止损), take-profit (止盈), trailing-stop (追踪止损) evaluation,
// OCO group cancellation (OCO互斥组撤销), retry with exponential backoff (指数退避重试),
// and trigger price bound precomputation for fast tick filtering (触发价格边界预计算快速过滤).
// ============================================

#include "order/conditional_order_manager.h"

#include "common/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_set>
#include <ctime>

namespace hft {

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Exponential backoff for retry: 100, 200, 400, 800, 1600ms (指数退避重试间隔)
std::chrono::milliseconds retry_backoff(uint32_t failures) {
    const uint32_t shift = (std::min)(failures > 0 ? failures - 1 : 0U, 4U);
    return std::chrono::milliseconds(100U << shift);  // 100, 200, 400, 800, 1600ms
}

} // namespace

bool ConditionalOrderManager::is_expired(int64_t created_at_ms, int ttl_days, int64_t now_ms) {
    if (ttl_days <= 0) return false;
    if (created_at_ms <= 0) return false;
    const int64_t ttl_ms = static_cast<int64_t>(ttl_days) * 86400LL * 1000LL;
    return (now_ms - created_at_ms) > ttl_ms;
}

void ConditionalOrderManager::clear_retry_state_locked(uint32_t id) {
    retry_not_before_.erase(id);
    retry_failures_.erase(id);
}

void ConditionalOrderManager::insert_locked(const ConditionalOrder& order) {
    orders_[order.id] = order;
    InstrumentKey key(order.instrument_id);
    instrument_index_[key].push_back(order.id);
    update_bounds_on_insert_locked(order);
    clear_retry_state_locked(order.id);
}

void ConditionalOrderManager::remove_from_index_locked(const InstrumentKey& key, uint32_t id) {
    auto idx_it = instrument_index_.find(key);
    if (idx_it == instrument_index_.end()) return;

    auto& ids = idx_it->second;
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    if (ids.empty()) {
        instrument_index_.erase(idx_it);
        trigger_bounds_.erase(key);
    }
}

void ConditionalOrderManager::update_bounds_on_insert_locked(const ConditionalOrder& order) {
    InstrumentKey key(order.instrument_id);
    auto& bounds = trigger_bounds_[key];
    switch (order.type) {
        case ConditionType::StopLoss:
            if (order.direction == Direction::Buy) {
                bounds.min_stop_buy = (std::min)(bounds.min_stop_buy, order.trigger_price);
            } else {
                bounds.max_stop_sell = (std::max)(bounds.max_stop_sell, order.trigger_price);
            }
            break;
        case ConditionType::TakeProfit:
            if (order.direction == Direction::Buy) {
                bounds.max_take_buy = (std::max)(bounds.max_take_buy, order.trigger_price);
            } else {
                bounds.min_take_sell = (std::min)(bounds.min_take_sell, order.trigger_price);
            }
            break;
        case ConditionType::TrailingStop:
            bounds.has_trailing = true;
            break;
    }
}

void ConditionalOrderManager::mark_bounds_dirty_locked(const InstrumentKey& key) {
    auto it = trigger_bounds_.find(key);
    if (it != trigger_bounds_.end()) {
        it->second.dirty = true;
    }
}

void ConditionalOrderManager::rebuild_bounds_locked(const InstrumentKey& key) {
    TriggerBounds rebuilt{};
    auto idx_it = instrument_index_.find(key);
    if (idx_it == instrument_index_.end()) {
        trigger_bounds_.erase(key);
        return;
    }

    std::vector<uint32_t> live_ids;
    live_ids.reserve(idx_it->second.size());
    for (const uint32_t id : idx_it->second) {
        auto order_it = orders_.find(id);
        if (order_it == orders_.end()) continue;

        live_ids.push_back(id);
        const auto& order = order_it->second;
        switch (order.type) {
            case ConditionType::StopLoss:
                if (order.direction == Direction::Buy) {
                    rebuilt.min_stop_buy = (std::min)(rebuilt.min_stop_buy, order.trigger_price);
                } else {
                    rebuilt.max_stop_sell = (std::max)(rebuilt.max_stop_sell, order.trigger_price);
                }
                break;
            case ConditionType::TakeProfit:
                if (order.direction == Direction::Buy) {
                    rebuilt.max_take_buy = (std::max)(rebuilt.max_take_buy, order.trigger_price);
                } else {
                    rebuilt.min_take_sell = (std::min)(rebuilt.min_take_sell, order.trigger_price);
                }
                break;
            case ConditionType::TrailingStop:
                rebuilt.has_trailing = true;
                break;
        }
    }

    if (live_ids.empty()) {
        instrument_index_.erase(idx_it);
        trigger_bounds_.erase(key);
        return;
    }

    idx_it->second = std::move(live_ids);
    trigger_bounds_[key] = rebuilt;
}

bool ConditionalOrderManager::may_trigger_locked(const InstrumentKey& key, double price) {
    auto bounds_it = trigger_bounds_.find(key);
    if (bounds_it == trigger_bounds_.end()) return false;
    if (bounds_it->second.dirty) {
        rebuild_bounds_locked(key);
        bounds_it = trigger_bounds_.find(key);
        if (bounds_it == trigger_bounds_.end()) return false;
    }

    constexpr double kBoundsEps = 1e-4;
    const auto& b = bounds_it->second;
    return b.has_trailing ||
           price >= b.min_stop_buy - kBoundsEps ||
           price <= b.max_stop_sell + kBoundsEps ||
           price <= b.max_take_buy + kBoundsEps ||
           price >= b.min_take_sell - kBoundsEps;
}

uint32_t ConditionalOrderManager::add(const ConditionalOrder& order) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (order.idempotency_key[0] != '\0') {
        for (const auto& [id, existing] : orders_) {
            if (std::strcmp(existing.idempotency_key, order.idempotency_key) == 0) {
                return 0;
            }
        }
    }

    ConditionalOrder o = order;
    o.id = next_id_++;
    o.active = true;
    o.status = CondOrderStatus::Pending;
    o.created_at_ms = now_ms();
    if (o.type == ConditionType::TrailingStop && o.extreme_price <= 0.0) {
        o.extreme_price = 0.0;
    }

    insert_locked(o);

    return o.id;
}

uint32_t ConditionalOrderManager::allocate_group_id() {
    std::lock_guard<std::mutex> lock(mtx_);
    return next_group_id_++;
}

void ConditionalOrderManager::restore(const ConditionalOrder& order) {
    std::lock_guard<std::mutex> lock(mtx_);

    ConditionalOrder restored = order;
    restored.active = true;
    insert_locked(restored);
    next_id_ = (std::max)(next_id_, restored.id + 1);
    if (restored.group_id != 0) {
        next_group_id_ = (std::max)(next_group_id_, restored.group_id + 1);
    }

    LOG_INFO("conditional order restored: id=" + std::to_string(restored.id) +
             " instrument=" + std::string(restored.instrument_id));
}

void ConditionalOrderManager::cancel(uint32_t id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = orders_.find(id);
    if (it != orders_.end()) {
        InstrumentKey key(it->second.instrument_id);
        it->second.status = CondOrderStatus::Cancelled;
        it->second.cancelled_at_ms = now_ms();
        mark_bounds_dirty_locked(key);
        remove_from_index_locked(key, id);
        orders_.erase(it);
    }
    clear_retry_state_locked(id);
}

void ConditionalOrderManager::cancel_group(uint32_t group_id) {
    if (group_id == 0) return;

    std::lock_guard<std::mutex> lock(mtx_);
    int count = 0;
    for (auto it = orders_.begin(); it != orders_.end();) {
        if (it->second.group_id == group_id) {
            InstrumentKey key(it->second.instrument_id);
            it->second.status = CondOrderStatus::Cancelled;
            it->second.cancelled_at_ms = now_ms();
            mark_bounds_dirty_locked(key);
            remove_from_index_locked(key, it->first);
            clear_retry_state_locked(it->first);
            ++count;
            it = orders_.erase(it);
        } else {
            ++it;
        }
    }

    if (count > 0) {
        LOG_INFO("OCO group cancel: group_id=" + std::to_string(group_id) +
                 " count=" + std::to_string(count));
    }
}

// 3-phase check_tick: locked filter -> unlocked callback -> locked commit
// 三阶段 Tick 检查：加锁过滤 → 解锁回调 → 加锁提交 (条件单触发主入口)
ConditionalCheckResult ConditionalOrderManager::check_tick(
    const TickData& tick,
    const std::function<ConditionalTriggerResult(const OrderRequest&, std::string&)>& callback) {

    ConditionalCheckResult result;

    // ---- Phase 1: evaluate and build snapshot (locked) ----
    // 第一阶段：加锁评估并构建触发快照
    struct TriggerInfo {
        uint32_t id;
        OrderRequest req;
        uint32_t group_id;
    };
    std::vector<TriggerInfo> trigger_list;

    {
        std::lock_guard<std::mutex> lock(mtx_);

        const InstrumentKey instr(tick.instrument_id);
        auto idx_it = instrument_index_.find(instr);
        if (idx_it == instrument_index_.end()) {
            return result;
        }
        if (!may_trigger_locked(instr, tick.last_price)) {
            return result;
        }

        std::vector<uint32_t>& ids = idx_it->second;
        std::vector<uint32_t> live_ids;
        live_ids.reserve(ids.size());

        const auto now = std::chrono::steady_clock::now();

        for (uint32_t id : ids) {
            auto oit = orders_.find(id);
            if (oit == orders_.end()) continue;

            live_ids.push_back(id);

            ConditionalOrder& order = oit->second;

            if (order.expire_at_ms > 0 && now_ms() >= order.expire_at_ms) {
                order.status = CondOrderStatus::Expired;
                order.cancelled_at_ms = now_ms();
                mark_bounds_dirty_locked(instr);
                result.changed = true;
                live_ids.pop_back();
                orders_.erase(oit);
                continue;
            }

            const double price = tick.last_price;
            constexpr double kPriceEps = 1e-4;
            bool triggered = false;

            switch (order.type) {
                case ConditionType::StopLoss:
                    triggered = (order.direction == Direction::Buy)
                        ? price >= order.trigger_price - kPriceEps
                        : price <= order.trigger_price + kPriceEps;
                    break;
                case ConditionType::TakeProfit:
                    triggered = (order.direction == Direction::Buy)
                        ? price <= order.trigger_price + kPriceEps
                        : price >= order.trigger_price - kPriceEps;
                    break;
                case ConditionType::TrailingStop:
                    if (order.direction == Direction::Buy) {
                        if (order.extreme_price <= 0.0 || price > order.extreme_price) {
                            order.extreme_price = price;
                        }
                        triggered = price <= order.extreme_price - order.trail_offset + kPriceEps;
                    } else {
                        if (order.extreme_price <= 0.0 || price < order.extreme_price) {
                            order.extreme_price = price;
                        }
                        triggered = price >= order.extreme_price + order.trail_offset - kPriceEps;
                    }
                    break;
            }

            if (!triggered) continue;

            const auto retry_it = retry_not_before_.find(id);
            if (retry_it != retry_not_before_.end() && retry_it->second > now) continue;

            TriggerInfo info{};
            info.id = id;
            info.group_id = order.group_id;
            safe_copy(info.req.instrument_id, order.instrument_id, sizeof(info.req.instrument_id));
            safe_copy(info.req.account_id, order.account_id, sizeof(info.req.account_id));
            safe_copy(info.req.strategy_id, order.strategy_id, sizeof(info.req.strategy_id));
            safe_copy(info.req.exchange_id, get_exchange_id(order.instrument_id), sizeof(info.req.exchange_id));
            info.req.direction = order.direction;
            info.req.price = (order.order_price > 0) ? order.order_price : tick.last_price;
            info.req.volume = order.volume;
            info.req.offset = order.offset;
            trigger_list.push_back(std::move(info));
        }

        ids = std::move(live_ids);
    }  // release mtx_

    if (trigger_list.empty()) return result;

    // ---- Phase 2: execute callback (unlocked) ----
    // 第二阶段：解锁后执行回调（避免死锁）
    struct CallbackResult {
        uint32_t id;
        uint32_t group_id;
        ConditionalTriggerResult result;
        std::string reason;
    };
    std::vector<CallbackResult> callback_results;
    callback_results.reserve(trigger_list.size());

    std::unordered_set<uint32_t> fired_groups;
    for (const auto& info : trigger_list) {
        if (info.group_id != 0 && fired_groups.count(info.group_id)) continue;

        std::string reason;
        const auto cb_result = callback(info.req, reason);
        callback_results.push_back({info.id, info.group_id, cb_result, std::move(reason)});

        // OCO: both Sent and Cancelled should prevent the other order in the same group from triggering
        // OCO 互斥组：Sent 或 Cancelled 都应阻止同组的另一个条件单触发
        if (info.group_id != 0 &&
            (cb_result == ConditionalTriggerResult::Sent ||
             cb_result == ConditionalTriggerResult::Cancelled)) {
            fired_groups.insert(info.group_id);
        }
    }

    // ---- Phase 3: commit results (locked) ----
    // 第三阶段：加锁提交结果
    {
        std::lock_guard<std::mutex> lock(mtx_);
        std::unordered_set<InstrumentKey, InstrumentKeyHash> changed_instruments;

        for (const auto& cr : callback_results) {
            auto oit = orders_.find(cr.id);
            if (oit == orders_.end()) continue;

            ConditionalOrder& order = oit->second;
            InstrumentKey key(order.instrument_id);

            if (cr.result == ConditionalTriggerResult::Sent) {
                order.status = CondOrderStatus::Triggered;
                order.triggered_at_ms = now_ms();
                if (cr.group_id != 0) {
                    result.triggered_group_ids.push_back(cr.group_id);
                }
                clear_retry_state_locked(cr.id);
                changed_instruments.insert(key);
                mark_bounds_dirty_locked(key);
                orders_.erase(oit);
                result.changed = true;
            } else if (cr.result == ConditionalTriggerResult::Cancelled) {
                order.status = CondOrderStatus::Rejected;
                order.triggered_at_ms = now_ms();
                safe_copy(order.reject_reason, cr.reason.c_str(), sizeof(order.reject_reason) - 1);
                LOG_WARN("conditional order auto-cancelled: id=" + std::to_string(cr.id) +
                         (cr.reason.empty() ? "" : " reason=" + cr.reason));
                clear_retry_state_locked(cr.id);
                changed_instruments.insert(key);
                mark_bounds_dirty_locked(key);
                orders_.erase(oit);
                result.changed = true;
            } else {
                // RetryLater
                constexpr uint32_t kMaxRetries = 10;
                const uint32_t failures = retry_failures_[cr.id] + 1;
                if (failures >= kMaxRetries) {
                    // Max retries exceeded: mark as Rejected and remove (超过最大重试次数，标记为 Rejected 并移除)
                    order.status = CondOrderStatus::Rejected;
                    order.triggered_at_ms = now_ms();
                    safe_copy(order.reject_reason, "max retries exceeded", sizeof(order.reject_reason) - 1);
                    LOG_WARN("conditional order max retries exceeded: id=" + std::to_string(cr.id) +
                             " failures=" + std::to_string(failures));
                    clear_retry_state_locked(cr.id);
                    changed_instruments.insert(key);
                    mark_bounds_dirty_locked(key);
                    orders_.erase(oit);
                    result.changed = true;
                } else {
                    retry_failures_[cr.id] = failures;
                    const auto backoff = retry_backoff(failures);
                    retry_not_before_[cr.id] = std::chrono::steady_clock::now() + backoff;
                    LOG_WARN("conditional order retry: id=" + std::to_string(cr.id) +
                             (cr.reason.empty() ? "" : " reason=" + cr.reason) +
                             " failures=" + std::to_string(failures) +
                             " next_retry_ms=" + std::to_string(backoff.count()));
                }
            }
        }

        for (const auto& key : changed_instruments) {
            rebuild_bounds_locked(key);
        }
    }

    return result;
}

std::vector<ConditionalOrder> ConditionalOrderManager::get_active_orders() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<ConditionalOrder> result;
    result.reserve(orders_.size());
    for (const auto& kv : orders_) {
        result.push_back(kv.second);
    }
    return result;
}

std::vector<std::string> ConditionalOrderManager::get_active_instruments() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> result;
    result.reserve(instrument_index_.size());
    for (const auto& kv : instrument_index_) {
        result.emplace_back(kv.first.data);
    }
    return result;
}

size_t ConditionalOrderManager::active_count() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return orders_.size();
}

void ConditionalOrderManager::for_each_active_order(const std::function<void(const ConditionalOrder&)>& fn) const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& kv : orders_) {
        fn(kv.second);
    }
}

int ConditionalOrderManager::get_pending_open_volume(const char* instrument_id, Direction direction) const {
    std::lock_guard<std::mutex> lock(mtx_);
    int total = 0;
    for (const auto& [id, order] : orders_) {
        if (order.offset != Offset::Open) continue;
        if (order.direction != direction) continue;
        if (std::strncmp(order.instrument_id, instrument_id, sizeof(order.instrument_id)) != 0) continue;
        total += order.volume;
    }
    return total;
}

void ConditionalOrderManager::cancel_all() {
    std::lock_guard<std::mutex> lock(mtx_);
    const int count = static_cast<int>(orders_.size());
    orders_.clear();
    instrument_index_.clear();
    trigger_bounds_.clear();
    retry_not_before_.clear();
    retry_failures_.clear();
    LOG_INFO("conditional orders bulk cancel count=" + std::to_string(count));
}

size_t ConditionalOrderManager::cancel_by_strategy(const std::string& strategy_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<uint32_t> to_cancel;
    for (const auto& [id, order] : orders_) {
        if (strategy_id == order.strategy_id) {
            to_cancel.push_back(id);
        }
    }
    for (uint32_t id : to_cancel) {
        auto it = orders_.find(id);
        if (it == orders_.end()) continue;
        InstrumentKey key(it->second.instrument_id);
        remove_from_index_locked(key, id);
        mark_bounds_dirty_locked(key);
        clear_retry_state_locked(id);
        orders_.erase(it);
    }
    if (!to_cancel.empty()) {
        LOG_INFO("conditional orders cancelled for strategy=" + strategy_id +
                 " count=" + std::to_string(to_cancel.size()));
    }
    return to_cancel.size();
}

} // namespace hft
