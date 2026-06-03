#pragma once
// ============================================
// session_manager.h - Trading session and tick monitoring (交易时段与行情监控)
// Tracks trading session state and monitors tick data flow for staleness detection.
// (追踪交易时段状态并监控行情数据流以检测 stale 行情)
// ============================================

#include <atomic>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace hft {

class SessionManager {
public:
    void apply_monitoring_config(int no_tick_warn_seconds, const std::string& trading_sessions);
    bool is_in_configured_trading_session() const;

    struct SessionTransition {
        bool changed = false;
        bool leaving_session = false;
    };
    SessionTransition refresh_trading_session_state(bool in_session);

    int seconds_since_last_tick() const;
    void update_last_tick_time();

    int no_tick_warn_seconds() const { return no_tick_warn_seconds_; }
    bool no_tick_alerted() const { return no_tick_alerted_; }
    void set_no_tick_alerted(bool v) { no_tick_alerted_ = v; }
    bool last_in_trading_session() const { return last_in_trading_session_; }
    bool last_md_logged_in() const { return last_md_logged_in_; }
    void set_last_md_logged_in(bool v) { last_md_logged_in_ = v; }
    bool last_td_logged_in() const { return last_td_logged_in_; }
    void set_last_td_logged_in(bool v) { last_td_logged_in_ = v; }
    long long trading_session_enter_steady_ms() const { return trading_session_enter_steady_ms_; }

private:
    int no_tick_warn_seconds_ = 10;
    std::vector<std::pair<int, int>> monitor_trading_sessions_;
    std::atomic<long long> last_tick_steady_ms_{0};
    long long trading_session_enter_steady_ms_ = 0;
    bool no_tick_alerted_ = false;
    bool last_in_trading_session_ = false;
    bool last_md_logged_in_ = false;
    bool last_td_logged_in_ = false;
};

} // namespace hft
