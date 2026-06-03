// ============================================
// strategy_controller.cpp - Strategy state and performance controller implementation (策略状态与绩效控制器实现)
// ============================================

#include "engine/strategy_controller.h"

#include "common/logger.h"
#include "common/string_utils.h"

#include <algorithm>

namespace hft {

StrategyState StrategyController::get_global_state() const {
    return global_state_.load(std::memory_order_relaxed);
}

void StrategyController::set_global_state(StrategyState state) {
    global_state_.store(state, std::memory_order_relaxed);
}

StrategyState StrategyController::get_state(const std::string& strategy_id) const {
    const std::string target = trim_copy(strategy_id).empty() ? "default" : trim_copy(strategy_id);
    std::lock_guard<std::mutex> lock(states_mtx_);
    const auto it = states_.find(target);
    if (it != states_.end()) return it->second;
    return global_state_.load(std::memory_order_relaxed);
}

bool StrategyController::set_state(const std::string& strategy_id, StrategyState state) {
    const std::string target = trim_copy(strategy_id).empty() ? "default" : trim_copy(strategy_id);
    std::lock_guard<std::mutex> lock(states_mtx_);
    const auto it = states_.find(target);
    if (it == states_.end()) return false;

    const StrategyState old_state = it->second;
    it->second = state;
    if (old_state == StrategyState::Running && state != StrategyState::Running) {
        non_running_count_.fetch_add(1, std::memory_order_relaxed);
    } else if (old_state != StrategyState::Running && state == StrategyState::Running) {
        non_running_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    return true;
}

void StrategyController::init_state(const std::string& strategy_id, StrategyState initial) {
    std::lock_guard<std::mutex> lock(states_mtx_);
    const auto [_, inserted] = states_.try_emplace(strategy_id, initial);
    if (inserted && initial != StrategyState::Running) {
        non_running_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

void StrategyController::remove_state(const std::string& strategy_id) {
    std::lock_guard<std::mutex> lock(states_mtx_);
    const auto it = states_.find(strategy_id);
    if (it != states_.end()) {
        if (it->second != StrategyState::Running) {
            non_running_count_.fetch_sub(1, std::memory_order_relaxed);
        }
        states_.erase(it);
    }
}

// Auto-pause a strategy after N consecutive errors (连续 N 次错误后自动暂停策略)
AutoPauseResult StrategyController::auto_pause_on_error(const std::string& strategy_id) {
    constexpr int kMaxConsecutiveErrors = 10;
    std::lock_guard<std::mutex> lock(states_mtx_);
    int& count = error_counts_[strategy_id];
    ++count;
    has_error_state_.store(true, std::memory_order_release);
    if (count >= kMaxConsecutiveErrors) {
        states_[strategy_id] = StrategyState::Paused;
        non_running_count_.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("strategy auto-paused after " + std::to_string(count) +
                  " consecutive errors: " + strategy_id);
        return {true, "strategy auto-paused: " + strategy_id +
                      " (" + std::to_string(count) + " consecutive errors)"};
    }
    return {};
}

void StrategyController::reset_error_count(const std::string& strategy_id) {
    // Hot-path fast short-circuit: the vast majority of ticks have no error state,
    // just atomic load and return, avoiding states_mtx_ lock/unlock overhead.
    // (热路径快速短路: 绝大多数 tick 没有错误状态, 直接 atomic load 即返回,
    //  避免 states_mtx_ 的 lock/unlock 开销。)
    if (!has_error_state_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(states_mtx_);
    error_counts_.erase(strategy_id);
    if (error_counts_.empty()) {
        has_error_state_.store(false, std::memory_order_release);
    }
}

size_t StrategyController::non_running_count() const {
    return non_running_count_.load(std::memory_order_relaxed);
}

std::unordered_map<std::string, StrategyState> StrategyController::snapshot_states() const {
    std::lock_guard<std::mutex> lock(states_mtx_);
    return states_;
}

void StrategyController::record_trade(const std::string& strategy_id) {
    std::lock_guard<std::mutex> lock(perf_mtx_);
    auto& perf = performance_[strategy_id];
    perf.strategy_id = strategy_id;
    ++perf.trade_count;
}

void StrategyController::record_open_position(const std::string& strategy_id,
                                               const std::string& trade_time) {
    std::lock_guard<std::mutex> lock(position_stats_mtx_);
    auto& stats = position_stats_[strategy_id];
    if (stats.open_time.empty()) {
        stats.open_time = trade_time;
    }
    stats.add_time = trade_time;
}

void StrategyController::update_floating_pnl(double total_floating) {
    std::lock_guard<std::mutex> lock(perf_mtx_);
    for (auto& [_, perf] : performance_) {
        perf.floating_pnl = total_floating;
        perf.total_pnl = perf.realized_pnl + perf.floating_pnl;
    }
}

void StrategyController::record_signal(const std::string& strategy_id,
                                        const std::string& signal_text,
                                        const std::string& time_text) {
    std::lock_guard<std::mutex> lock(signal_stats_mtx_);
    auto& stats = signal_stats_[strategy_id];
    ++stats.signal_count;
    stats.last_signal = signal_text;
    stats.last_signal_time = time_text;
}

std::vector<StrategyPerformanceSnapshot> StrategyController::get_performance(
    const std::string& filter) const {
    const std::string f = trim_copy(filter);
    std::vector<StrategyPerformanceSnapshot> result;
    std::lock_guard<std::mutex> lock(perf_mtx_);
    for (const auto& [id, snapshot] : performance_) {
        if (!f.empty() && id != f) continue;
        result.push_back(snapshot);
    }
    return result;
}

StrategyPositionStats StrategyController::get_position_stats(const std::string& strategy_id) const {
    std::lock_guard<std::mutex> lock(position_stats_mtx_);
    const auto it = position_stats_.find(strategy_id);
    if (it != position_stats_.end()) return it->second;
    return {};
}

StrategySignalStats StrategyController::get_signal_stats(const std::string& strategy_id) const {
    std::lock_guard<std::mutex> lock(signal_stats_mtx_);
    const auto it = signal_stats_.find(strategy_id);
    if (it != signal_stats_.end()) return it->second;
    return {};
}

StrategyPerformanceSnapshot StrategyController::get_perf(const std::string& strategy_id) const {
    std::lock_guard<std::mutex> lock(perf_mtx_);
    const auto it = performance_.find(strategy_id);
    if (it != performance_.end()) return it->second;
    return {};
}

void StrategyController::clear_performance() {
    {
        std::lock_guard<std::mutex> lock(perf_mtx_);
        performance_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(position_stats_mtx_);
        position_stats_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(signal_stats_mtx_);
        signal_stats_.clear();
    }
}

void StrategyController::restore_performance(const std::vector<StrategyPerformanceSnapshot>& items) {
    std::lock_guard<std::mutex> lock(perf_mtx_);
    for (const auto& p : items) {
        performance_[p.strategy_id] = p;
    }
}

std::vector<StrategyPerformanceSnapshot> StrategyController::snapshot_performance() const {
    std::lock_guard<std::mutex> lock(perf_mtx_);
    std::vector<StrategyPerformanceSnapshot> result;
    result.reserve(performance_.size());
    for (const auto& [_, perf] : performance_) result.push_back(perf);
    return result;
}

} // namespace hft
