// ============================================
// order_manager.cpp - Order manager (zero-alloc hot path)
// 订单管理器实现（零分配热路径）
// Uses FixedKey<16> instead of std::string for order_ref lookups
// 使用 FixedKey<16> 替代 std::string 做 order_ref 查找
// ============================================

#include "order/order_manager.h"
#include "common/logger.h"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace hft {

bool OrderManager::is_active_order(const OrderInfo& order) const {
    return order.status == OrderStatus::Pending || order.status == OrderStatus::PartTraded;
}

int OrderManager::open_remaining_of(const OrderInfo& order) {
    if (order.offset != Offset::Open) return 0;
    if (!(order.status == OrderStatus::Pending || order.status == OrderStatus::PartTraded)) return 0;
    return (std::max)(0, order.total_volume - order.traded_volume);
}

void OrderManager::apply_pending_open_delta(const OrderInfo* before, const OrderInfo* after) {
    // before / after == nullptr means the order "does not exist".
    // before / after 任意一个为 nullptr 表示订单"不存在"。
    // When either side contributes a non-zero delta, accumulate by (instrument, dir).
    // 任意一边贡献非零时，按 (instrument, dir) 累加。 (delta累加)
    if (before) {
        const int rem = open_remaining_of(*before);
        if (rem > 0) {
            PendingOpenKey k{InstrumentKey(before->instrument_id), before->direction};
            auto it = pending_open_by_inst_dir_.find(k);
            if (it != pending_open_by_inst_dir_.end()) {
                it->second -= rem;
                if (it->second <= 0) pending_open_by_inst_dir_.erase(it);
            }
        }
    }
    if (after) {
        const int rem = open_remaining_of(*after);
        if (rem > 0) {
            PendingOpenKey k{InstrumentKey(after->instrument_id), after->direction};
            pending_open_by_inst_dir_[k] += rem;
        }
    }
}

void OrderManager::rebuild_pending_open() {
    pending_open_by_inst_dir_.clear();
    for (const auto& [_, order] : orders_) {
        const int rem = open_remaining_of(order);
        if (rem > 0) {
            PendingOpenKey k{InstrumentKey(order.instrument_id), order.direction};
            pending_open_by_inst_dir_[k] += rem;
        }
    }
}

void OrderManager::init(int front_id, int session_id, int max_order_ref) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (front_id_ != 0 && (front_id_ != front_id || session_id_ != session_id)) {
        size_t cleared = 0;
        for (auto it = orders_.begin(); it != orders_.end();) {
            if (is_active_order(it->second)) {
                it = orders_.erase(it);
                ++cleared;
            } else {
                ++it;
            }
        }
        if (cleared > 0) {
            LOG_WARN("OrderManager: reconnect cleanup active orders count=" + std::to_string(cleared));
        }
    }
    front_id_ = front_id;
    session_id_ = session_id;
    order_ref_.store(max_order_ref);
    LOG_INFO("OrderManager initialized: front_id=" + std::to_string(front_id) +
             " session_id=" + std::to_string(session_id) +
             " start_order_ref=" + std::to_string(max_order_ref));
}

namespace {
// to_chars writes directly into stack buffer, avoiding std::to_string heap allocation.
// to_chars 直接写入栈缓冲区，避免 std::to_string 的堆分配。
// order_ref field is char[16]; int max 11 bytes, leaving 1 byte for '\0' is sufficient.
// order_ref 字段为 char[16]，int 最长 11 字节，留 1 字节 '\0' 完全够用。 (零分配写入)
inline void write_order_ref(char (&buf)[16], int ref) {
    auto res = std::to_chars(buf, buf + sizeof(buf) - 1, ref);
    *res.ptr = '\0';
}
} // namespace

std::string OrderManager::allocate_order_ref() {
    const int ref = order_ref_.fetch_add(1) + 1;
    char buf[16]{};
    write_order_ref(buf, ref);
    return std::string(buf);
}

OrderInfo OrderManager::create_order(const OrderRequest& req) {
    OrderInfo info{};
    safe_copy(info.instrument_id, req.instrument_id, sizeof(info.instrument_id));
    safe_copy(info.exchange_id, req.exchange_id, sizeof(info.exchange_id));
    safe_copy(info.account_id, req.account_id, sizeof(info.account_id));
    safe_copy(info.strategy_id, req.strategy_id, sizeof(info.strategy_id));

    // 直接写入 info.order_ref,跳过中间 std::string。
    const int ref = order_ref_.fetch_add(1) + 1;
    write_order_ref(info.order_ref, ref);

    info.direction = req.direction;
    info.offset = req.offset;
    info.price = req.price;
    info.total_volume = req.volume;
    info.traded_volume = 0;
    info.status = OrderStatus::Pending;
    info.front_id = front_id_;
    info.session_id = session_id_;

    std::lock_guard<std::mutex> lock(mtx_);
    OrderRefKey key(info.order_ref);
    orders_[key] = info;
    apply_pending_open_delta(nullptr, &info);
    LOG_INFO("OrderManager created order: ref=" + std::string(info.order_ref) +
             " instrument=" + std::string(info.instrument_id) +
             " volume=" + std::to_string(info.total_volume));
    return info;
}

void OrderManager::on_order_return(const OrderInfo& order) {
    std::lock_guard<std::mutex> lock(mtx_);
    OrderRefKey key(order.order_ref);
    auto it = orders_.find(key);
    if (it != orders_.end()) {
        // Terminal state protection: AllTraded/Cancelled/Error cannot be overwritten by non-terminal states
        // 终态保护：AllTraded/Cancelled/Error 不允许被非终态覆盖 (终态保护)
        const OrderStatus cur = it->second.status;
        const OrderStatus incoming = order.status;
        const bool cur_terminal = (cur == OrderStatus::AllTraded ||
                                   cur == OrderStatus::Cancelled ||
                                   cur == OrderStatus::Error);
        const bool in_terminal = (incoming == OrderStatus::AllTraded ||
                                  incoming == OrderStatus::Cancelled ||
                                  incoming == OrderStatus::Error);
        if (cur_terminal && !in_terminal) {
            return; // Reject non-terminal state overwriting terminal state (拒绝非终态覆盖终态)
        }
        OrderInfo before = it->second;
        it->second.status = order.status;
        safe_copy(it->second.exchange_id, order.exchange_id, sizeof(it->second.exchange_id));
        safe_copy(it->second.order_sys_id, order.order_sys_id, sizeof(it->second.order_sys_id));
        it->second.traded_volume = (std::max)(it->second.traded_volume, order.traded_volume);
        it->second.total_volume = order.total_volume;
        it->second.front_id = order.front_id;
        it->second.session_id = order.session_id;
        safe_copy(it->second.status_msg, order.status_msg, sizeof(it->second.status_msg));
        safe_copy(it->second.insert_time, order.insert_time, sizeof(it->second.insert_time));
        if (order.account_id[0] != '\0') {
            safe_copy(it->second.account_id, order.account_id, sizeof(it->second.account_id));
        }
        if (order.strategy_id[0] != '\0') {
            safe_copy(it->second.strategy_id, order.strategy_id, sizeof(it->second.strategy_id));
        }
        apply_pending_open_delta(&before, &it->second);
    } else {
        // Only accept orders from the current session to prevent ghost orders from old sessions during reconnect replay
        // 只接受当前 session 的订单，防止重连重放引入旧 session 的幽灵订单 (幽灵订单防护)
        if (order.front_id == front_id_ && order.session_id == session_id_) {
            orders_[key] = order;
            apply_pending_open_delta(nullptr, &orders_[key]);
        }
    }
}

void OrderManager::on_trade_return(const TradeInfo& trade) {
    std::lock_guard<std::mutex> lock(mtx_);

    // Trade deduplication: CTP THOST_TERT_QUICK mode may replay already-confirmed fills
    // 成交去重：CTP THOST_TERT_QUICK 模式可能重放已确认的成交 (成交去重)
    if (trade.trade_id[0] != '\0') {
        if (seen_trade_ids_.size() > 100000) {
            seen_trade_ids_.clear();
        }
        TradeIdKey tid(trade.trade_id);
        if (!seen_trade_ids_.insert(tid).second) {
            return; // Duplicate fill, skip (重复成交，跳过)
        }
    }

    OrderRefKey key(trade.order_ref);
    auto it = orders_.find(key);
    if (it == orders_.end()) {
        return;
    }

    if (std::strncmp(it->second.instrument_id, trade.instrument_id, sizeof(trade.instrument_id)) != 0) {
        LOG_ERROR("trade instrument mismatch: order_ref=" + std::string(trade.order_ref) +
                  " order=" + it->second.instrument_id +
                  " trade=" + std::string(trade.instrument_id));
        return;
    }

    OrderInfo before = it->second;
    it->second.traded_volume += trade.volume;
    if (it->second.traded_volume > it->second.total_volume) {
        it->second.traded_volume = it->second.total_volume;
    }
    // Terminal state protection: Cancelled/Error status should not be overwritten by volume-based deduction
    // 终态保护：Cancelled/Error 状态不被成交量推算覆盖 (终态保护)
    const OrderStatus cur = it->second.status;
    if (cur != OrderStatus::Cancelled && cur != OrderStatus::Error) {
        if (it->second.traded_volume >= it->second.total_volume) {
            it->second.status = OrderStatus::AllTraded;
        } else {
            it->second.status = OrderStatus::PartTraded;
        }
    }
    apply_pending_open_delta(&before, &it->second);
}

void OrderManager::replace_active_orders(const std::vector<OrderInfo>& active_orders) {
    std::lock_guard<std::mutex> lock(mtx_);
    OrderMap existing_orders = orders_;

    for (auto it = orders_.begin(); it != orders_.end();) {
        if (is_active_order(it->second)) {
            it = orders_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto order : active_orders) {
        OrderRefKey key(order.order_ref);
        auto existing_it = existing_orders.find(key);
        if (existing_it != existing_orders.end()) {
            if (order.strategy_id[0] == '\0' && existing_it->second.strategy_id[0] != '\0') {
                safe_copy(order.strategy_id, existing_it->second.strategy_id, sizeof(order.strategy_id));
            }
            if (order.account_id[0] == '\0' && existing_it->second.account_id[0] != '\0') {
                safe_copy(order.account_id, existing_it->second.account_id, sizeof(order.account_id));
            }
        }
        orders_[key] = order;
    }
    // Batch replacement involves deleting + inserting many active orders; rebuilding the counter wholesale is more robust.
    // 批量替换涉及多笔活动单删除+新增，统一重建计数器更稳。 (批量重建)
    rebuild_pending_open();
}

void OrderManager::remove_order(const std::string& order_ref) {
    std::lock_guard<std::mutex> lock(mtx_);
    OrderRefKey key(order_ref.c_str());
    auto it = orders_.find(key);
    if (it == orders_.end()) return;
    apply_pending_open_delta(&it->second, nullptr);
    orders_.erase(it);
}

bool OrderManager::get_order_copy(const std::string& order_ref, OrderInfo& out_order) const {
    std::lock_guard<std::mutex> lock(mtx_);
    OrderRefKey key(order_ref.c_str());
    auto it = orders_.find(key);
    if (it == orders_.end()) {
        return false;
    }
    out_order = it->second;
    return true;
}

bool OrderManager::enrich_order_info(OrderInfo& order) const {
    std::lock_guard<std::mutex> lock(mtx_);
    OrderRefKey key(order.order_ref);
    auto it = orders_.find(key);
    if (it == orders_.end()) {
        return false;
    }
    if (order.strategy_id[0] == '\0' && it->second.strategy_id[0] != '\0') {
        safe_copy(order.strategy_id, it->second.strategy_id, sizeof(order.strategy_id));
    }
    if (order.account_id[0] == '\0' && it->second.account_id[0] != '\0') {
        safe_copy(order.account_id, it->second.account_id, sizeof(order.account_id));
    }
    return true;
}

bool OrderManager::enrich_trade_info(TradeInfo& trade) const {
    std::lock_guard<std::mutex> lock(mtx_);
    OrderRefKey key(trade.order_ref);
    auto it = orders_.find(key);
    if (it == orders_.end()) {
        return false;
    }
    if (trade.strategy_id[0] == '\0' && it->second.strategy_id[0] != '\0') {
        safe_copy(trade.strategy_id, it->second.strategy_id, sizeof(trade.strategy_id));
    }
    if (trade.account_id[0] == '\0' && it->second.account_id[0] != '\0') {
        safe_copy(trade.account_id, it->second.account_id, sizeof(trade.account_id));
    }
    return true;
}

std::vector<OrderInfo> OrderManager::get_active_orders() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<OrderInfo> result;
    for (const auto& [key, order] : orders_) {
        if (is_active_order(order)) {
            result.push_back(order);
        }
    }
    return result;
}

int OrderManager::get_pending_open_volume(const char* instrument, Direction dir) const {
    if (!instrument || instrument[0] == '\0') return 0;
    std::lock_guard<std::mutex> lock(mtx_);
    PendingOpenKey k{InstrumentKey(instrument), dir};
    auto it = pending_open_by_inst_dir_.find(k);
    return it == pending_open_by_inst_dir_.end() ? 0 : it->second;
}

int OrderManager::get_pending_close_volume(const char* instrument, Direction pos_dir, Offset offset) const {
    std::lock_guard<std::mutex> lock(mtx_);
    int pending = 0;
    const Direction close_order_dir = (pos_dir == Direction::Buy) ? Direction::Sell : Direction::Buy;

    for (const auto& [key, order] : orders_) {
        if (!is_active_order(order) || order.offset == Offset::Open) continue;
        if (!str_equal(order.instrument_id, instrument) || order.direction != close_order_dir) continue;

        const int remaining = (std::max)(0, order.total_volume - order.traded_volume);
        if (offset == Offset::Close) {
            pending += remaining;
        } else if (order.offset == offset) {
            pending += remaining;
        }
    }
    return pending;
}

} // namespace hft
