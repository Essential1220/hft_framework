#pragma once
// ============================================
// strategy_controller.h - Strategy state and performance controller (策略状态与绩效控制器)
// Manages strategy lifecycle (running/paused/stopped), auto-pause on error,
// and per-strategy performance tracking.
// (管理策略生命周期/错误自动暂停/逐策略绩效追踪)
// ============================================

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class StrategyState { Running, Paused, Stopped };

struct StrategyPerformanceSnapshot {
    std::string strategy_id;
    size_t trade_count = 0;
    double realized_pnl = 0.0;
    double floating_pnl = 0.0;
    double total_pnl = 0.0;
    double win_rate = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double profit_factor = 0.0;
};

struct StrategyPositionStats {
    std::string open_time;
    std::string add_time;
};

struct StrategySignalStats {
    size_t signal_count = 0;
    std::string last_signal;
    std::string last_signal_time;
};

struct AutoPauseResult {
    bool paused = false;
    std::string message;
};

class StrategyController {
public:
    StrategyState get_global_state() const;
    void set_global_state(StrategyState state);

    StrategyState get_state(const std::string& strategy_id) const;
    bool set_state(const std::string& strategy_id, StrategyState state);
    void init_state(const std::string& strategy_id, StrategyState initial);
    void remove_state(const std::string& strategy_id);
    AutoPauseResult auto_pause_on_error(const std::string& strategy_id);
    void reset_error_count(const std::string& strategy_id);
    size_t non_running_count() const;
    std::unordered_map<std::string, StrategyState> snapshot_states() const;

    void record_trade(const std::string& strategy_id);
    void record_open_position(const std::string& strategy_id, const std::string& trade_time);
    void update_floating_pnl(double total_floating);
    void record_signal(const std::string& strategy_id,
                       const std::string& signal_text,
                       const std::string& time_text);

    std::vector<StrategyPerformanceSnapshot> get_performance(const std::string& filter = "") const;
    StrategyPositionStats get_position_stats(const std::string& strategy_id) const;
    StrategySignalStats get_signal_stats(const std::string& strategy_id) const;
    StrategyPerformanceSnapshot get_perf(const std::string& strategy_id) const;

    void clear_performance();
    void restore_performance(const std::vector<StrategyPerformanceSnapshot>& items);
    std::vector<StrategyPerformanceSnapshot> snapshot_performance() const;

private:
    std::atomic<StrategyState> global_state_{StrategyState::Running};

    mutable std::mutex states_mtx_;
    std::unordered_map<std::string, StrategyState> states_;
    std::unordered_map<std::string, int> error_counts_;
    std::atomic<size_t> non_running_count_{0};
    // Hot-path fast short-circuit: reset_error_count can avoid locking when no errors exist.
    // Writer (auto_pause_on_error) sets true while holding lock; reader reset loads before locking.
    // (热路径快速短路: 无错误时 reset_error_count 不必加锁。
    //  写者 auto_pause_on_error 在持锁内 set true, 读者 reset 持锁前先 load。)
    std::atomic<bool> has_error_state_{false};

    mutable std::mutex perf_mtx_;
    std::map<std::string, StrategyPerformanceSnapshot> performance_;

    mutable std::mutex position_stats_mtx_;
    std::map<std::string, StrategyPositionStats> position_stats_;

    mutable std::mutex signal_stats_mtx_;
    std::map<std::string, StrategySignalStats> signal_stats_;
};

} // namespace hft
