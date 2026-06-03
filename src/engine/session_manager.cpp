// ============================================
// session_manager.cpp - Trading session and tick monitoring implementation (交易时段与行情监控实现)
// ============================================

#include "engine/session_manager.h"

#include "common/logger.h"

#include <algorithm>
#include <ctime>
#include <sstream>

namespace hft {

namespace {

std::tm local_time_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    return tm_buf;
}

int minutes_since_midnight(const std::tm& tm_buf) {
    return tm_buf.tm_hour * 60 + tm_buf.tm_min;
}

int parse_hhmm(const std::string& text) {
    if (text.size() < 5 || text[2] != ':') return -1;
    try {
        const int hour = std::stoi(text.substr(0, 2));
        const int minute = std::stoi(text.substr(3, 2));
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;
        return hour * 60 + minute;
    } catch (...) {
        return -1;
    }
}

std::vector<std::pair<int, int>> parse_trading_sessions(const std::string& text) {
    std::vector<std::pair<int, int>> sessions;
    std::istringstream iss(text);
    std::string token;
    while (std::getline(iss, token, ',')) {
        const auto dash = token.find('-');
        if (dash == std::string::npos) continue;
        const int start = parse_hhmm(token.substr(0, dash));
        const int end = parse_hhmm(token.substr(dash + 1));
        if (start >= 0 && end >= 0) {
            sessions.push_back({start, end});
        }
    }
    return sessions;
}

} // namespace

// Apply (possibly hot-reloaded) monitoring config: tick staleness threshold and trading session windows.
// (应用可能热更新的监控配置：行情 stale 阈值和交易时段窗口)
void SessionManager::apply_monitoring_config(int no_tick_warn_seconds,
                                             const std::string& trading_sessions) {
    no_tick_warn_seconds_ = no_tick_warn_seconds;
    monitor_trading_sessions_ = parse_trading_sessions(trading_sessions);

    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    last_in_trading_session_ = is_in_configured_trading_session();
    trading_session_enter_steady_ms_ = last_in_trading_session_ ? now_ms : 0;
    no_tick_alerted_ = false;

    // Engine monitoring config hot-updated (引擎监控配置已热更新)
    LOG_INFO("引擎监控配置已热更新: NoTickWarnSeconds=: NoTickWarnSeconds=" +
             std::to_string(no_tick_warn_seconds_) +
             " TradingSessions=" +
             (trading_sessions.empty() ? std::string("<empty>") : trading_sessions));
}

bool SessionManager::is_in_configured_trading_session() const {
    if (monitor_trading_sessions_.empty()) {
        return true;
    }

    const std::tm tm_buf = local_time_now();
    const int now_minutes = minutes_since_midnight(tm_buf);
    for (const auto& [start, end] : monitor_trading_sessions_) {
        if (start <= end) {
            if (now_minutes >= start && now_minutes <= end) return true;
        } else {
            if (now_minutes >= start || now_minutes <= end) return true;
        }
    }
    return false;
}

SessionManager::SessionTransition SessionManager::refresh_trading_session_state(bool in_session) {
    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    SessionTransition result;
    if (in_session != last_in_trading_session_) {
        result.changed = true;
        result.leaving_session = last_in_trading_session_ && !in_session;
        last_in_trading_session_ = in_session;
        no_tick_alerted_ = false;
        trading_session_enter_steady_ms_ = in_session ? now_ms : 0;
        // Engine trading session state changed (引擎交易时段状态变化)
        LOG_INFO(std::string("引擎交易时段状态变化") + (in_session ? "进入交易时段" : "离开交易时段"));
        return result;
    }

    if (in_session && trading_session_enter_steady_ms_ <= 0) {
        trading_session_enter_steady_ms_ = now_ms;
    }
    return result;
}

int SessionManager::seconds_since_last_tick() const {
    const long long last_ms = last_tick_steady_ms_.load(std::memory_order_relaxed);
    if (last_ms <= 0) return -1;
    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<int>((std::max)(0LL, now_ms - last_ms) / 1000);
}

void SessionManager::update_last_tick_time() {
    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    last_tick_steady_ms_.store(now_ms, std::memory_order_relaxed);
}

} // namespace hft
