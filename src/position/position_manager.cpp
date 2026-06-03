// ============================================
// position_manager.cpp - Position manager (zero-alloc hot path)
// 持仓管理器（零分配热路径）
// Uses InstrumentKey (stack-allocated, 20 bytes) instead of std::string
// 使用栈分配 InstrumentKey (20字节) 替代 std::string
// ============================================

#include "position/position_manager.h"
#include "common/logger.h"

namespace hft {

void PositionManager::on_trade(const TradeInfo& trade) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (trade.offset == Offset::Open) {
        // Open: increase position in trade direction (开仓：按成交方向增加持仓)
        InstrumentKey key(trade.instrument_id, trade.direction);
        auto& pos = positions_[key];
        safe_copy(pos.instrument_id, trade.instrument_id, sizeof(pos.instrument_id));
        safe_copy(pos.account_id, trade.account_id, sizeof(pos.account_id));
        pos.direction = trade.direction;
        pos.today += trade.volume;
        pos.total += trade.volume;
        if (pos.total > 0) {
            pos.avg_price = ((pos.avg_price * (pos.total - trade.volume)) +
                             trade.price * trade.volume) / pos.total;
        }
    } else {
        // Close: decrease position in opposite direction (平仓：按反方向减少持仓)
        Direction close_dir = (trade.direction == Direction::Buy) ? Direction::Sell : Direction::Buy;
        InstrumentKey key(trade.instrument_id, close_dir);
        auto it = positions_.find(key);
        if (it != positions_.end()) {
            auto& pos = it->second;
            if (pos.account_id[0] == '\0' && trade.account_id[0] != '\0') {
                safe_copy(pos.account_id, trade.account_id, sizeof(pos.account_id));
            }
            if (trade.volume > pos.total) {
                LOG_ERROR("CRITICAL: close volume exceeds position! instrument=" +
                          std::string(trade.instrument_id) +
                          " close_vol=" + std::to_string(trade.volume) +
                          " pos_total=" + std::to_string(pos.total) +
                          " - position divergence detected, reconciliation needed");
                reconciliation_needed_.store(true, std::memory_order_relaxed);
            }
            pos.total -= trade.volume;
            if (trade.offset == Offset::CloseToday) {
                pos.today -= trade.volume;
            } else if (trade.offset == Offset::CloseYesterday) {
                pos.yesterday -= trade.volume;
            } else {
                int yd_reduce = (std::min)(pos.yesterday, trade.volume);
                pos.yesterday -= yd_reduce;
                pos.today -= (trade.volume - yd_reduce);
            }
            pos.total = (std::max)(0, pos.total);
            pos.today = (std::max)(0, pos.today);
            pos.yesterday = (std::max)(0, pos.yesterday);
            // Invariant check: total must equal today + yesterday (不变量校验：total == today + yesterday)
            if (pos.total != pos.today + pos.yesterday) {
                LOG_ERROR("INVARIANT VIOLATION: total!=" + std::to_string(pos.total) +
                          " today+yesterday=" + std::to_string(pos.today + pos.yesterday) +
                          " instrument=" + std::string(trade.instrument_id));
                reconciliation_needed_.store(true, std::memory_order_relaxed);
            }
            if (pos.total == 0) {
                pos.avg_price = 0.0;
            }
        }
    }
    // Hot path: no LOG here to avoid string allocation per trade (热路径：不做日志记录，避免每次成交堆分配字符串)
}

PositionInfo PositionManager::get_position(const char* instrument, Direction dir) const {
    std::lock_guard<std::mutex> lock(mtx_);
    InstrumentKey key(instrument, dir);
    auto it = positions_.find(key);
    if (it != positions_.end()) return it->second;

    PositionInfo empty{};
    safe_copy(empty.instrument_id, instrument, sizeof(empty.instrument_id));
    empty.direction = dir;
    return empty;
}

int PositionManager::get_net_position(const char* instrument) const {
    std::lock_guard<std::mutex> lock(mtx_);
    int long_total = 0;
    int short_total = 0;

    InstrumentKey key_l(instrument, Direction::Buy);
    auto it_long = positions_.find(key_l);
    if (it_long != positions_.end()) {
        long_total = it_long->second.total;
    }

    InstrumentKey key_s(instrument, Direction::Sell);
    auto it_short = positions_.find(key_s);
    if (it_short != positions_.end()) {
        short_total = it_short->second.total;
    }

    return long_total - short_total;
}

void PositionManager::update_position(const char* instrument, Direction dir, const PositionInfo& pos) {
    std::lock_guard<std::mutex> lock(mtx_);
    InstrumentKey key(instrument, dir);
    positions_[key] = pos;
}

void PositionManager::replace_positions(const std::vector<PositionInfo>& snapshot) {
    std::lock_guard<std::mutex> lock(mtx_);
    positions_.clear();
    for (const auto& pos : snapshot) {
        if (pos.total <= 0) continue;
        InstrumentKey key(pos.instrument_id, pos.direction);
        positions_[key] = pos;
    }
}

void PositionManager::clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    positions_.clear();
}

std::vector<PositionInfo> PositionManager::get_all_positions() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<PositionInfo> result;
    for (const auto& [key, pos] : positions_) {
        if (pos.total > 0) {
            result.push_back(pos);
        }
    }
    return result;
}

int PositionManager::get_total_absolute_position() const {
    std::lock_guard<std::mutex> lock(mtx_);
    int total = 0;
    for (const auto& [key, pos] : positions_) {
        total += pos.total;
    }
    return total;
}

void PositionManager::log_positions(const std::string& tag) const {
    auto positions = get_all_positions();
    if (positions.empty()) {
        LOG_WARN(tag + " no positions");
        return;
    }
    LOG_WARN(tag + " position snapshot (" + std::to_string(positions.size()) + "):");
    for (auto& pos : positions) {
        LOG_WARN("  " + std::string(pos.instrument_id) +
                 " dir=" + (pos.direction == Direction::Buy ? "L" : "S") +
                 " total=" + std::to_string(pos.total) +
                 " today=" + std::to_string(pos.today) +
                 " yd=" + std::to_string(pos.yesterday) +
                 " avg=" + std::to_string(pos.avg_price));
    }
}

} // namespace hft
