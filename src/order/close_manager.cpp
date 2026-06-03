// ============================================
// close_manager.cpp - Close position manager implementation (平仓管理器实现)
// ============================================

#include "order/close_manager.h"

#include "common/logger.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace hft {

namespace {

bool contains_any(const std::string& text, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (needle && text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool looks_like_insufficient_close_today(const std::string& reason) {
    return contains_any(reason, {"平今", "今仓", "今持仓", "close today", "CloseToday", "today position"}) &&
           contains_any(reason, {"不足", "不够", "insufficient", "not enough", "exceed"});
}

bool looks_like_insufficient_close_yesterday(const std::string& reason) {
    return contains_any(reason, {"平昨", "昨仓", "昨持仓", "close yesterday", "CloseYesterday", "yesterday position"}) &&
           contains_any(reason, {"不足", "不够", "insufficient", "not enough", "exceed"});
}

} // namespace

void CloseManager::set_callbacks(SendOrderFunc send_fn, CancelOrderFunc cancel_fn, AlertFunc alert_fn) {
    std::lock_guard<std::mutex> lock(mtx_); // Lock to protect callback assignment (加锁保护回调函数的设置)
    send_fn_ = std::move(send_fn);
    cancel_fn_ = std::move(cancel_fn);
    alert_fn_ = std::move(alert_fn);
}

void CloseManager::submit_close(const char* instrument, const char* exchange_id, const char* account_id,
                                Direction pos_direction, int today_vol, int yesterday_vol,
                                double last_price, double upper_limit, double lower_limit) {
    std::vector<uint32_t> pending_task_ids;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const bool need_split = need_close_today_flag(exchange_id); // Check if exchange distinguishes CloseToday / CloseYesterday (检查交易所是否区分平今平昨)

        if (need_split) {
            // Create or update separate CloseToday and CloseYesterday tasks (分别创建或更新平今和平昨任务)
            upsert_task_locked(instrument, exchange_id, account_id, pos_direction, Offset::CloseToday, today_vol,
                               last_price, upper_limit, lower_limit);
            upsert_task_locked(instrument, exchange_id, account_id, pos_direction, Offset::CloseYesterday, yesterday_vol,
                               last_price, upper_limit, lower_limit);
        } else {
            // Exchanges that don't distinguish: merge into a single Close task (不区分的交易所直接合并为一个平仓任务)
            upsert_task_locked(instrument, exchange_id, account_id, pos_direction, Offset::Close, today_vol + yesterday_vol,
                               last_price, upper_limit, lower_limit);
        }

        // Collect all tasks waiting to be sent or rejected and awaiting retry (收集所有等待发送或被拒绝后等待重试的任务)
        for (auto& task : tasks_) {
            if (task.state == CloseTaskState::Pending || task.state == CloseTaskState::OrderRejected) {
                pending_task_ids.push_back(task.id);
            }
        }
    }

    // Send tasks after releasing the lock to avoid deadlock in callbacks (释放锁后依次发送任务，避免在回调中死锁)
    for (uint32_t task_id : pending_task_ids) {
        send_task(task_id);
    }
}

void CloseManager::upsert_task_locked(const char* instrument, const char* exchange_id, const char* account_id,
                                      Direction pos_direction, Offset offset, int requested_volume,
                                      double last_price, double upper_limit, double lower_limit) {
    if (requested_volume <= 0) {
        return;
    }

    for (auto& task : tasks_) {
        if (task.state == CloseTaskState::Done || task.state == CloseTaskState::Failed) continue;
        if (!str_equal(task.instrument_id, instrument)) continue;
        if (!str_equal(task.account_id, account_id)) continue;
        if (task.pos_direction != pos_direction) continue;
        if (task.offset != offset) continue;

        task.target_volume = (std::max)(task.target_volume, task.filled_volume + requested_volume);
        task.last_price = last_price;
        task.upper_limit = upper_limit;
        task.lower_limit = lower_limit;
        LOG_INFO("Close task reusing existing pending task (平仓任务复用未完成任务): task=" + std::to_string(task.id) +
                 " instrument=" + std::string(task.instrument_id) +
                 " account=" + std::string(task.account_id) +
                 " target=" + std::to_string(task.target_volume) +
                 " filled=" + std::to_string(task.filled_volume));
        return;
    }

    CloseTask task{};
    task.id = next_id_++;
    safe_copy(task.instrument_id, instrument, sizeof(task.instrument_id));
    safe_copy(task.exchange_id, exchange_id, sizeof(task.exchange_id));
    safe_copy(task.account_id, account_id, sizeof(task.account_id));
    task.pos_direction = pos_direction;
    task.offset = offset;
    task.target_volume = requested_volume;
    task.last_price = last_price;
    task.upper_limit = upper_limit;
    task.lower_limit = lower_limit;
    tasks_.push_back(task);
}

// Calculate order price: use lower_limit for closing longs, upper_limit for closing shorts to guarantee fill
// (根据任务信息计算报单价格：平多用跌停价，平空用涨停价以确保成交)
double CloseManager::calc_close_price(const CloseTask& task) const {
    return task.pos_direction == Direction::Buy ? task.lower_limit : task.upper_limit;
}

// Actually send the close order to the exchange (实际发送平仓委托 / 发单)
void CloseManager::send_task(uint32_t task_id) {
    SendOrderFunc send_fn;
    OrderRequest req{};

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!send_fn_) return; // Return if callback not set (未设置回调函数则直接返回)

        // Look up the corresponding close task (查找对应的平仓任务)
        auto it = std::find_if(tasks_.begin(), tasks_.end(),
                               [task_id](const CloseTask& task) { return task.id == task_id; });
        if (it == tasks_.end()) return;

        CloseTask& task = *it;
        // Only tasks in Pending or OrderRejected state can be sent (只有 Pending 或 OrderRejected 状态的任务才能发送)
        if (task.state != CloseTaskState::Pending && task.state != CloseTaskState::OrderRejected) {
            return;
        }

        // Calculate remaining volume to close (计算剩余需要平仓的数量)
        const int remaining = task.target_volume - task.filled_volume;
        if (remaining <= 0) {
            task.state = CloseTaskState::Done; // Already fully filled, mark done (已经平完，更新状态)
            return;
        }

        // Build the order request (构造报单请求)
        safe_copy(req.instrument_id, task.instrument_id, sizeof(req.instrument_id));
        safe_copy(req.exchange_id, task.exchange_id, sizeof(req.exchange_id));
        safe_copy(req.account_id, task.account_id, sizeof(req.account_id));
        // Order direction is opposite of position direction (报单方向与持仓方向相反)
        req.direction = task.pos_direction == Direction::Buy ? Direction::Sell : Direction::Buy;
        req.offset = task.offset;
        req.price = calc_close_price(task);
        req.volume = remaining;
        send_fn = send_fn_;
    }

    // Send order after releasing lock to avoid deadlock in callback (释放锁后发送订单，避免回调死锁)
    std::string order_ref;
    send_fn(req, order_ref);

    // Re-acquire lock to update task state (重新加锁更新任务状态)
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = std::find_if(tasks_.begin(), tasks_.end(),
                           [task_id](const CloseTask& task) { return task.id == task_id; });
    if (it == tasks_.end()) return;

    CloseTask& task = *it;
    if (task.state != CloseTaskState::Pending && task.state != CloseTaskState::OrderRejected) {
        return;
    }

    if (order_ref.empty()) {
        task.state = CloseTaskState::OrderRejected; // Order send failed, awaiting retry (发单失败，等待重试)
        return;
    }

    task.order_ref = order_ref;
    task.state = CloseTaskState::Sent; // Update to Sent state (更新为已发送)
    task.sent_time = std::chrono::steady_clock::now(); // Record send time for timeout checking (记录发送时间，用于超时检查)
}

// Increment retry count; mark as Failed if max retries exceeded (增加重试计数，如果超过最大重试次数则标记为失败)
void CloseManager::retry_task(CloseTask& task, const std::string& reason) {
    task.retry_count++;
    if (task.retry_count > kMaxRetry) {
        task.state = CloseTaskState::Failed;
        const int remaining = task.target_volume - task.filled_volume;
        std::string msg =
            "Close task final failure (平仓任务最终失败): task=" + std::to_string(task.id) +
            " 合约=" + std::string(task.instrument_id) +
            " 账户=" + std::string(task.account_id) +
            " 目标=" + std::to_string(task.target_volume) +
            " 已平=" + std::to_string(task.filled_volume) +
            " 剩余=" + std::to_string(remaining) +
            " 原因=" + reason;
        if (remaining > 0 && task.filled_volume > 0) {
            msg += " [警告: 存在" + std::to_string(remaining) + "手孤儿持仓需人工处理]";
        }
        LOG_ERROR(msg);
        if (alert_fn_) alert_fn_(msg);
        return;
    }

    task.state = CloseTaskState::Pending;
}

void CloseManager::on_order(const OrderInfo& order) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& task : tasks_) {
        if (task.state != CloseTaskState::Sent && task.state != CloseTaskState::Cancelling) continue;
        if (!str_equal(task.account_id, order.account_id)) continue;
        if (task.order_ref != std::string(order.order_ref)) continue;

        if (order.status == OrderStatus::Error) {
            std::string reason = "Order rejected (报单被拒绝)";
            if (task.offset == Offset::CloseToday) {
                // CloseToday rejected (e.g. SHFE doesn't support it), downgrade to CloseYesterday for one retry
                // 平今被拒绝（如上交所不支持平今），降级为平昨重试一次
                task.offset = Offset::CloseYesterday;
                reason = "CloseToday rejected, switching to CloseYesterday (平今被拒绝，切换为平昨重试)";
            }
            // Note: when CloseYesterday is rejected, do NOT switch back to CloseToday,
            // to avoid infinite alternating retries that would exhaust the retry limit.
            // 注意：平昨被拒绝时不再切回平今，避免两种 offset 无限轮转耗尽重试次数。
            // CloseYesterday or plain Close rejected: retry with the original offset (retry_task handles limits).
            // CloseYesterday 或普通 Close 被拒绝，直接按原 offset 继续重试（由 retry_task 判断是否超限）。
            retry_task(task, reason);
            return;
        }

        if (order.status == OrderStatus::Cancelled) {
            const bool was_cancelling = (task.state == CloseTaskState::Cancelling);
            const int remaining = task.target_volume - task.filled_volume;
            if (remaining <= 0) {
                task.state = CloseTaskState::Done;
                LOG_INFO("close task done on cancel (fully filled), task=" + std::to_string(task.id));
                return;
            }
            if (task.filled_volume > 0) {
                LOG_INFO("close task partial fill on cancel: task=" + std::to_string(task.id) +
                         " filled=" + std::to_string(task.filled_volume) +
                         " remaining=" + std::to_string(remaining));
            }
            retry_task(task, was_cancelling ? "超时撤单后重新报单" : "委托被撤销");
            return;
        }
    }
}

void CloseManager::on_trade(const TradeInfo& trade) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& task : tasks_) {
        if (task.state != CloseTaskState::Sent && task.state != CloseTaskState::Cancelling) continue;
        if (!str_equal(task.account_id, trade.account_id)) continue;
        if (!str_equal(task.instrument_id, trade.instrument_id)) continue;
        if (task.order_ref != std::string(trade.order_ref)) continue;

        task.filled_volume += trade.volume;
        if (task.filled_volume >= task.target_volume) {
            task.state = CloseTaskState::Done;
        }
        return;
    }
}

void CloseManager::on_cancel_rejected(const char* account_id, const char* order_ref, const char* reason) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& task : tasks_) {
        if (task.state != CloseTaskState::Cancelling) continue;
        if (!str_equal(task.account_id, account_id)) continue;
        if (task.order_ref != std::string(order_ref)) continue;

        if (task.filled_volume >= task.target_volume) {
            task.state = CloseTaskState::Done;
            LOG_INFO("close task done on cancel reject (already filled), task=" + std::to_string(task.id));
            return;
        }
        // Cancel was rejected by the exchange — the order is still alive.
        // This is typically a race condition, not a real failure, so do not
        // increment retry_count; just reset the timeout window.
        task.state = CloseTaskState::Sent;
        task.sent_time = std::chrono::steady_clock::now();

        std::string msg = "close task cancel rejected by gateway, task=" + std::to_string(task.id) +
                          " account=" + std::string(task.account_id) +
                          " ref=" + task.order_ref;
        if (reason && reason[0] != '\0') {
            msg += " reason=" + std::string(reason);
        }
        LOG_WARN(msg);
        return;
    }
}

void CloseManager::check_timeout() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> pending_task_ids;
    std::vector<std::pair<uint32_t, std::pair<std::string, std::string>>> cancel_requests;

    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& task : tasks_) {
            if (task.state == CloseTaskState::Pending || task.state == CloseTaskState::OrderRejected) {
                pending_task_ids.push_back(task.id);
                continue;
            }

            if (task.state != CloseTaskState::Sent) continue;

            if (task.filled_volume >= task.target_volume) {
                task.state = CloseTaskState::Done;
                continue;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - task.sent_time).count();
            if (elapsed >= kTimeoutSeconds) {
                task.state = CloseTaskState::Cancelling;
                cancel_requests.push_back({task.id, {task.account_id, task.order_ref}});
            }
        }

        cleanup_finished_tasks_locked();
    }

    for (uint32_t task_id : pending_task_ids) {
        send_task(task_id);
    }

    for (const auto& item : cancel_requests) {
        const uint32_t task_id = item.first;
        const std::string& account_id = item.second.first;
        const std::string& order_ref = item.second.second;
        const bool cancel_issued = cancel_fn_ && cancel_fn_(account_id, order_ref);

        std::lock_guard<std::mutex> lock(mtx_);
        auto it = std::find_if(tasks_.begin(), tasks_.end(),
                               [task_id](const CloseTask& task) { return task.id == task_id; });
        if (it == tasks_.end()) {
            continue;
        }

        CloseTask& task = *it;
        if (cancel_issued) {
            continue;
        }

        if (task.state != CloseTaskState::Cancelling) {
            continue;
        }

        ++task.retry_count;
        task.state = CloseTaskState::Sent;
        task.sent_time = now;
        if (task.retry_count > kMaxRetry) {
            task.state = CloseTaskState::Failed;
            const std::string msg =
                "close task cancel request failed repeatedly, task=" + std::to_string(task.id) +
                " instrument=" + std::string(task.instrument_id) +
                " account=" + std::string(task.account_id) +
                " ref=" + task.order_ref;
            LOG_ERROR(msg);
            if (alert_fn_) alert_fn_(msg);
        } else {
            LOG_WARN("close task cancel request was not issued, will retry later: task=" +
                     std::to_string(task.id) +
                     " account=" + std::string(task.account_id) +
                     " ref=" + task.order_ref +
                     " retry=" + std::to_string(task.retry_count));
        }
    }
}

void CloseManager::cleanup_finished_tasks() {
    std::lock_guard<std::mutex> lock(mtx_);
    cleanup_finished_tasks_locked();
}

bool CloseManager::is_same_active_close_order(const CloseTask& task, const OrderInfo& order) const {
    if (!str_equal(task.account_id, order.account_id)) return false;
    if (!str_equal(task.instrument_id, order.instrument_id)) return false;
    if (task.offset != order.offset) return false;

    const Direction expected_direction =
        task.pos_direction == Direction::Buy ? Direction::Sell : Direction::Buy;
    if (order.direction != expected_direction) return false;

    const int remaining = (std::max)(0, task.target_volume - task.filled_volume);
    const int order_remaining = (std::max)(0, order.total_volume - order.traded_volume);
    if (remaining <= 0 || order_remaining <= 0 || order_remaining > remaining) {
        return false;
    }
    if (order.traded_volume < 0 || order.traded_volume > task.target_volume) {
        return false;
    }

    const double expected_price = calc_close_price(task);
    return std::fabs(order.price - expected_price) < 1e-6;
}

void CloseManager::cleanup_finished_tasks_locked() {
    static constexpr size_t kMaxFinishedTasks = 20;
    size_t finished_count = 0;
    for (const auto& task : tasks_) {
        if (task.state == CloseTaskState::Done || task.state == CloseTaskState::Failed) {
            ++finished_count;
        }
    }

    if (finished_count <= kMaxFinishedTasks) return;

    const size_t to_remove = finished_count - kMaxFinishedTasks;
    size_t removed = 0;
    tasks_.erase(
        std::remove_if(tasks_.begin(), tasks_.end(),
                       [&](const CloseTask& task) {
                           if (removed >= to_remove) return false;
                           if (task.state == CloseTaskState::Done || task.state == CloseTaskState::Failed) {
                               ++removed;
                               return true;
                           }
                           return false;
                       }),
        tasks_.end());
}

bool CloseManager::has_pending_tasks() const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto& task : tasks_) {
        if (task.state != CloseTaskState::Done && task.state != CloseTaskState::Failed) {
            return true;
        }
    }
    return false;
}

std::vector<CloseTask> CloseManager::get_tasks() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return tasks_;
}

bool CloseManager::retry_failed_task(uint32_t task_id) {
    uint32_t send_id = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = std::find_if(tasks_.begin(), tasks_.end(),
                               [task_id](const CloseTask& t) { return t.id == task_id; });
        if (it == tasks_.end()) return false;
        if (it->state != CloseTaskState::Failed) return false;
        const int remaining = it->target_volume - it->filled_volume;
        if (remaining <= 0) {
            it->state = CloseTaskState::Done;
            return false;
        }
        it->state = CloseTaskState::Pending;
        it->retry_count = 0;
        it->order_ref.clear();
        send_id = it->id;
        LOG_INFO("close task manual retry: task=" + std::to_string(task_id) +
                 " remaining=" + std::to_string(remaining));
    }
    if (send_id > 0) {
        send_task(send_id);
    }
    return true;
}

void CloseManager::restore_tasks(const std::vector<CloseTask>& tasks) {
    std::lock_guard<std::mutex> lock(mtx_);
    tasks_ = tasks;
    next_id_ = 1;
    for (const auto& task : tasks_) {
        next_id_ = (std::max)(next_id_, task.id + 1);
    }

    for (auto& task : tasks_) {
        if (task.state != CloseTaskState::Done && task.state != CloseTaskState::Failed) {
            task.state = CloseTaskState::Pending;
        }
    }
}

void CloseManager::reconcile_active_orders(const std::string& account_id, const std::vector<OrderInfo>& active_orders) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<bool> used_orders(active_orders.size(), false);
    const auto now = std::chrono::steady_clock::now();

    for (auto& task : tasks_) {
        if (!str_equal(task.account_id, account_id.c_str())) {
            continue;
        }
        if (task.state == CloseTaskState::Done || task.state == CloseTaskState::Failed) {
            continue;
        }

        int matched_index = -1;
        if (!task.order_ref.empty()) {
            for (size_t i = 0; i < active_orders.size(); ++i) {
                if (used_orders[i]) continue;
                const auto& order = active_orders[i];
                if (task.order_ref != order.order_ref) continue;
                if (!is_same_active_close_order(task, order)) continue;
                matched_index = static_cast<int>(i);
                break;
            }
        }

        if (matched_index < 0) {
            for (size_t i = 0; i < active_orders.size(); ++i) {
                if (used_orders[i]) continue;
                if (!is_same_active_close_order(task, active_orders[i])) continue;
                matched_index = static_cast<int>(i);
                break;
            }
        }

        if (matched_index < 0) {
            task.state = CloseTaskState::Pending;
            task.order_ref.clear();
            continue;
        }

        const auto& order = active_orders[matched_index];
        used_orders[matched_index] = true;
        task.order_ref = order.order_ref;
        task.filled_volume = (std::max)(task.filled_volume, order.traded_volume);
        if (task.filled_volume >= task.target_volume) {
            task.state = CloseTaskState::Done;
            task.order_ref.clear();
        } else {
            task.state = CloseTaskState::Sent;
            task.sent_time = now;
        }
    }
}

void CloseManager::resume_pending_tasks() {
    std::vector<uint32_t> pending_task_ids;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& task : tasks_) {
            if (task.state == CloseTaskState::Pending || task.state == CloseTaskState::OrderRejected) {
                pending_task_ids.push_back(task.id);
            }
        }
    }

    for (uint32_t task_id : pending_task_ids) {
        send_task(task_id);
    }
}

} // namespace hft
