#pragma once
// ============================================
// position_manager.h - Position manager (zero-alloc hot path)
// 持仓管理器（零分配热路径）
//
// Tracks positions on each trade fill, supports CloseToday/CloseYesterday split,
// and provides net position, total absolute position, and reconciliation flag.
// 在每笔成交时更新持仓，支持平今/平昨区分，提供净持仓、总绝对持仓和对账标记。
// ============================================

#include "common/types.h"
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace hft {

class PositionManager {
public:
    void on_trade(const TradeInfo& trade);
    PositionInfo get_position(const char* instrument, Direction dir) const;
    int get_net_position(const char* instrument) const;
    void update_position(const char* instrument, Direction dir, const PositionInfo& pos);
    void replace_positions(const std::vector<PositionInfo>& snapshot);
    void clear();
    std::vector<PositionInfo> get_all_positions() const;
    int get_total_absolute_position() const;
    void log_positions(const std::string& tag) const;

    // Position mismatch flag: set when close volume exceeds held position; triggers immediate reconciliation query
    // 持仓不一致标记：当平仓量超过持仓时触发，需要立即查询对账 (对账标记)
    bool needs_reconciliation() const { return reconciliation_needed_.load(std::memory_order_relaxed); }
    // Clear the reconciliation flag (清除对账标记 / 清除对账标记)
    void clear_reconciliation_flag() { reconciliation_needed_.store(false, std::memory_order_relaxed); }

private:
    using Map = std::unordered_map<InstrumentKey, PositionInfo, InstrumentKeyHash>;
    mutable std::mutex mtx_;                   // Mutex protecting the position map (保护持仓映射表的互斥锁)
    Map positions_;                            // Position map keyed by instrument+direction (按合约+方向索引的持仓表)
    std::atomic<bool> reconciliation_needed_{false}; // Atomic flag for reconciliation needed (对账需求原子标志)
};

} // namespace hft
