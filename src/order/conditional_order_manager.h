#pragma once
// ============================================
// conditional_order_manager.h - Conditional order manager (条件单管理器)
//
// Manages stop-loss (止损), take-profit (止盈), and trailing-stop (追踪止损) orders.
// Evaluates trigger conditions on each tick using a 3-phase lock-filter-commit pattern.
// 管理止损、止盈和追踪止损条件单，使用三阶段（加锁过滤-解锁回调-加锁提交）模式
// 在每个 Tick 上评估触发条件。
// ============================================

#include "common/types.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

// Result of a conditional order trigger attempt (条件单触发尝试的结果)
enum class ConditionalTriggerResult {
    RetryLater, // Trigger fired but order send failed; retry later (触发但发单失败，稍后重试)
    Sent,       // Order sent successfully (发单成功)
    Cancelled   // Order cancelled (e.g. OCO group) (订单被取消，如 OCO 互斥组)
};

// Result of a check_tick evaluation (check_tick 评估结果)
struct ConditionalCheckResult {
    std::vector<uint32_t> triggered_group_ids; // OCO groups whose trigger fired (触发的 OCO 互斥组ID列表)
    bool changed = false;                      // Whether any order state changed (是否有订单状态变更)
};

// Conditional order manager: stores, evaluates, and triggers conditional orders
// 条件单管理器：存储、评估并触发条件单 (条件单管理器)
class ConditionalOrderManager {
public:
    // Add a new conditional order; returns its assigned ID (0 if duplicate idempotency key)
    // 添加新的条件单，返回分配的 ID（幂等键重复时返回 0）(添加条件单)
    uint32_t add(const ConditionalOrder& order);
    // Allocate a new OCO group ID (分配新的 OCO 互斥组 ID / 分配分组ID)
    uint32_t allocate_group_id();
    // Restore a conditional order from persistent state (从持久化状态恢复条件单 / 恢复条件单)
    void restore(const ConditionalOrder& order);
    // Cancel a conditional order by its ID (通过 ID 取消条件单 / 取消条件单)
    void cancel(uint32_t id);
    // Cancel all conditional orders in an OCO group (取消 OCO 互斥组内所有条件单 / 取消分组)
    void cancel_group(uint32_t group_id);
    // 3-phase evaluation: locked filter -> unlocked callback -> locked commit
    // 三阶段评估：加锁过滤 → 解锁回调 → 加锁提交 (Tick检查)
    ConditionalCheckResult check_tick(
        const TickData& tick,
        const std::function<ConditionalTriggerResult(const OrderRequest&, std::string&)>& callback);
    // Get snapshot of all active conditional orders (获取所有活跃条件单快照 / 活跃订单)
    std::vector<ConditionalOrder> get_active_orders() const;
    // Get list of instruments that have active conditional orders (获取有活跃条件单的合约列表 / 活跃合约)
    std::vector<std::string> get_active_instruments() const;
    // Get count of active conditional orders (获取活跃条件单数量 / 活跃数量)
    size_t active_count() const;
    // Iterate over all active conditional orders with a callback (遍历所有活跃条件单 / 遍历)
    void for_each_active_order(const std::function<void(const ConditionalOrder&)>& fn) const;
    // Get total pending open volume from conditional orders for a given instrument+direction
    // 获取指定合约+方向上条件单的待开仓总量 (待开仓量查询)
    int get_pending_open_volume(const char* instrument_id, Direction direction) const;
    // Cancel all conditional orders (取消全部条件单 / 全部取消)
    void cancel_all();
    // Cancel all conditional orders belonging to a specific strategy (取消指定策略的所有条件单 / 按策略取消)
    size_t cancel_by_strategy(const std::string& strategy_id);

    // Check if a conditional order has exceeded its TTL (time-to-live). Used to skip stale
    // orders during cross-session restoration.
    // 判断条件单是否已超 TTL（用于跨会话恢复时跳过陈旧单）。
    // ttl_days <= 0 disables TTL; created_at_ms <= 0 is treated as unknown (never expired).
    // ttl_days <= 0 表示禁用 TTL；created_at_ms <= 0 视为未知，不过期。
    // Accepts now_ms parameter for easy unit test injection of a fixed time.
    // 接受 now_ms 入参便于单元测试注入固定时间。(TTL过期检查)
    static bool is_expired(int64_t created_at_ms, int ttl_days, int64_t now_ms);

private:
    using TimePoint = std::chrono::steady_clock::time_point;

    // Precomputed trigger price bounds for fast early-exit on each tick
    // 预计算的触发价格边界，用于每个 Tick 快速提前退出 (触发边界)
    struct TriggerBounds {
        bool dirty = false;                     // Whether bounds need rebuilding (是否需要重建边界)
        bool has_trailing = false;              // Whether any trailing stop exists for this instrument (是否有追踪止损)
        double min_stop_buy = std::numeric_limits<double>::infinity();   // Min stop-loss trigger for buy direction (多头止损最低触发价)
        double max_stop_sell = -std::numeric_limits<double>::infinity(); // Max stop-loss trigger for sell direction (空头止损最高触发价)
        double max_take_buy = -std::numeric_limits<double>::infinity();  // Max take-profit trigger for buy direction (多头止盈最高触发价)
        double min_take_sell = std::numeric_limits<double>::infinity();  // Min take-profit trigger for sell direction (空头止盈最低触发价)
    };

    // Insert a conditional order while holding the lock (持锁插入条件单)
    void insert_locked(const ConditionalOrder& order);
    // Clear retry state for a given order ID (清除指定订单的重试状态)
    void clear_retry_state_locked(uint32_t id);
    // Remove an order ID from the instrument index (从合约索引中移除订单ID)
    void remove_from_index_locked(const InstrumentKey& key, uint32_t id);
    // Update trigger bounds when inserting a new order (插入新订单时更新触发边界)
    void update_bounds_on_insert_locked(const ConditionalOrder& order);
    // Mark trigger bounds as dirty for a given instrument (标记指定合约的触发边界为脏)
    void mark_bounds_dirty_locked(const InstrumentKey& key);
    // Rebuild trigger bounds from live orders for a given instrument (从活跃订单重建指定合约的触发边界)
    void rebuild_bounds_locked(const InstrumentKey& key);
    // Quick check: could any conditional order for this instrument trigger at this price?
    // 快速检查：该合约是否有任何条件单可能在此价格触发？(可能触发检查)
    bool may_trigger_locked(const InstrumentKey& key, double price);

    std::unordered_map<uint32_t, ConditionalOrder> orders_;             // All conditional orders by ID (所有条件单/按ID)
    // Use InstrumentKey (stack-allocated) instead of std::string for instrument index
    // 使用栈分配 InstrumentKey 替代 std::string 做合约索引 (合约索引)
    std::unordered_map<InstrumentKey, std::vector<uint32_t>, InstrumentKeyHash> instrument_index_;
    // Precomputed trigger price bounds per instrument (每个合约的预计算触发价格边界)
    std::unordered_map<InstrumentKey, TriggerBounds, InstrumentKeyHash> trigger_bounds_;
    // Per-order retry backoff: earliest time to retry (每个订单的重试退避：最早可重试时间)
    std::unordered_map<uint32_t, TimePoint> retry_not_before_;
    // Per-order retry failure count (每个订单的重试失败计数)
    std::unordered_map<uint32_t, uint32_t> retry_failures_;
    uint32_t next_id_ = 1;       // Monotonic order ID allocator (订单ID递增分配器)
    uint32_t next_group_id_ = 1; // Monotonic OCO group ID allocator (OCO组ID递增分配器)
    mutable std::mutex mtx_;     // Mutex protecting all internal state (保护所有内部状态的互斥锁)
};

} // namespace hft
