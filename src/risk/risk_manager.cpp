// ============================================
// risk_manager.cpp - 风控管理器实现
//
// 核心函数 check_order() 的多维度瀑布流检查顺序：
//   1. volume > 0                          — 基本合法性 (防止负数或零报单)
//   2. volume <= max_order_size             — 单笔上限 (防胖手指)
//   3. in_trading_session()                 — 交易时段 (过滤非交易时间请求)
//   4. daily_loss < max_daily_loss          — 日亏损限额 (保护本金)
//   5. projected_net <= max_net_position    — 净持仓限制 (控制风险敞口，包含挂单)
//   6. closeable_position >= volume         — 可平仓位充足 (防止废单/超卖)
//   7. order_rate < max_orders_per_minute   — 报单频率 (防流控熔断)
//   8. cancel_rate < max_cancel_rate        — 撤单率 (防交易所警告或封号)
//
// 豁免机制：当 is_risk_reduction=true（明确为平仓降敞口操作）时，
//          将跳过 2/3/4/7/8 这五项检查，确保在任何恶劣情况下系统都有能力平仓自救。
// ============================================

#include "risk/risk_manager.h"

#include "common/logger.h"
#include "order/order_manager.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>

namespace hft {

namespace {

// 解析 "HH:MM" 格式的时间字符串，将其转换为自当天 00:00 起的分钟数 (0-1439)
// 如果格式不合法，返回 -1
int parse_hhmm(const std::string& text) {
    if (text.size() != 5 || text[2] != ':') {
        return -1;
    }
    const int hour = std::stoi(text.substr(0, 2));
    const int minute = std::stoi(text.substr(3, 2));
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return -1;
    }
    return hour * 60 + minute;
}

// 获取当前系统的本地时间，并以线程安全的方式填充到 std::tm 结构体中
// 屏蔽了 Windows (localtime_s) 和 Linux/Unix (localtime_r) 的平台差异
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

// glob 匹配：仅支持 pattern 开头/结尾的 *（如 *_test、test_*、*core*、精确串）。
// 设计取舍：避免引入完整正则；HFT 风控热路径，最廉价匹配即可满足"测试策略前后缀豁免"诉求。
bool glob_match(const std::string& pat, const char* str) {
    if (pat.empty() || str == nullptr) return false;
    const std::string s(str);
    const bool head = !pat.empty() && pat.front() == '*';
    const bool tail = !pat.empty() && pat.back() == '*';
    const std::string core = pat.substr(head ? 1 : 0,
                                        pat.size() - (head ? 1 : 0) - (tail ? 1 : 0));
    if (core.empty()) return true;  // pure "*" matches all
    if (head && tail) return s.find(core) != std::string::npos;
    if (head) return s.size() >= core.size() && s.compare(s.size() - core.size(), core.size(), core) == 0;
    if (tail) return s.size() >= core.size() && s.compare(0, core.size(), core) == 0;
    return s == core;
}

// 把逗号分隔的 pattern 列表解析成 vector，去掉前后空白和空段。
std::vector<std::string> parse_pattern_list(const std::string& csv) {
    std::vector<std::string> out;
    std::istringstream iss(csv);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        const auto lpos = tok.find_first_not_of(" \t\r\n");
        const auto rpos = tok.find_last_not_of(" \t\r\n");
        if (lpos == std::string::npos) continue;
        out.emplace_back(tok.substr(lpos, rpos - lpos + 1));
    }
    return out;
}

} // namespace

void RiskManager::clamp_risk_params() {
    if (max_order_size_ < 1) max_order_size_ = 1;
    if (max_order_size_ > 10000) max_order_size_ = 10000;
    if (max_net_position_ < 1) max_net_position_ = 1;
    if (max_net_position_ > 50000) max_net_position_ = 50000;
    if (max_total_position_ < 0) max_total_position_ = 0;
    if (max_total_position_ > 500000) max_total_position_ = 500000;
    if (max_orders_per_minute_ < 1) max_orders_per_minute_ = 1;
    if (max_orders_per_minute_ > 600) max_orders_per_minute_ = 600;
    if (cancel_rate_window_minutes_ < 1) cancel_rate_window_minutes_ = 1;
    if (cancel_rate_window_minutes_ > 1440) cancel_rate_window_minutes_ = 1440;
    if (cancel_rate_min_sample_ < 1) cancel_rate_min_sample_ = 1;
    if (cancel_rate_min_sample_ > 1000) cancel_rate_min_sample_ = 1000;
    if (max_cancel_rate_ < 0.0) max_cancel_rate_ = 0.0;
    if (max_cancel_rate_ > 1.0) max_cancel_rate_ = 1.0;
    if (max_daily_loss_ < 0.0) max_daily_loss_ = 0.0;
}

void RiskManager::init(const Config& config, PositionManager* pos_mgr, OrderManager* order_mgr,
                       const std::string& account_id) {
    pos_mgr_ = pos_mgr;
    order_mgr_ = order_mgr;
    account_id_ = account_id;

    const std::string acct_section = account_id.empty() ? "" : "Risk." + account_id;
    const bool has_acct = !acct_section.empty() && config.has_section(acct_section);
    const std::string& section = has_acct ? acct_section : std::string("Risk");

    max_order_size_ = config.get_int(section, "MaxOrderSize", 5);
    max_net_position_ = config.get_int(section, "MaxNetPosition", 10);
    max_total_position_ = config.get_int(section, "MaxTotalPosition", 0);
    max_orders_per_minute_ = config.get_int(section, "MaxOrdersPerMinute", 30);
    cancel_rate_window_minutes_ = config.get_int(section, "CancelRateWindowMinutes", 60);
    cancel_rate_min_sample_ = config.get_int(section, "CancelRateMinSample", 10);
    max_cancel_rate_ = config.get_double(section, "MaxCancelRate", 0.5);
    max_daily_loss_ = config.get_double(section, "MaxDailyLoss", 0.0);
    cancel_rate_exempt_patterns_ = parse_pattern_list(
        config.get_string(section, "CancelRateExemptStrategies", ""));
    clamp_risk_params();

    // 解析交易时段配置 [Trading] TradingSessions
    // 格式例如："09:00-11:30,13:30-15:00,21:00-02:30"
    trading_sessions_.clear();
    std::istringstream iss(config.get_string("Trading", "TradingSessions", ""));
    std::string token;
    // 按逗号分割出多个时间段
    while (std::getline(iss, token, ',')) {
        const auto dash = token.find('-');
        if (dash == std::string::npos) {
            continue; // 格式不合法，跳过
        }

        // 解析起始和结束时间
        const int start = parse_hhmm(token.substr(0, dash));
        const int end = parse_hhmm(token.substr(dash + 1));
        if (start >= 0 && end >= 0) {
            trading_sessions_.push_back({start, end});
        }
    }

    LOG_INFO("RiskManager initialized: order_size=" + std::to_string(max_order_size_) +
             " net_position=" + std::to_string(max_net_position_) +
             " orders_per_min=" + std::to_string(max_orders_per_minute_) +
             " cancel_window_min=" + std::to_string(cancel_rate_window_minutes_) +
             " cancel_rate=" + std::to_string(max_cancel_rate_) +
             " daily_loss=" + std::to_string(max_daily_loss_) +
             " cancel_rate_exempt_patterns=" + std::to_string(cancel_rate_exempt_patterns_.size()));
}

// ---- 风控核心函数：下单前校验 ----
// 返回 true = 通过风控，允许下发到交易所
// 返回 false = 被风控拦截，reject_reason 填充人类可读的拒绝原因
bool RiskManager::check_order(const OrderRequest& req, std::string& reject_reason, bool is_risk_reduction,
                              int cond_pending_buy, int cond_pending_sell,
                              bool cancel_rate_exempt) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    const auto reject = [&](const std::string& reason, RiskErrorCode code = RiskErrorCode::None) {
        reject_reason = reason;
        ++total_rejects_today_;
        last_error_code_ = code;
        // 推送 RiskEvent
        RiskEvent evt{};
        evt.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        evt.mode = rms_mode_;
        evt.error_code = code;
        safe_copy(evt.instrument_id, req.instrument_id, sizeof(evt.instrument_id));
        safe_copy(evt.account_id, req.account_id, sizeof(evt.account_id));
        safe_copy(evt.message, reason.c_str(), sizeof(evt.message));
        pending_risk_events_.push_back(evt);
        LOG_WARN("Risk reject: " + reject_reason);
        return false;
    };

    // ---- 检查0：RMS 风控模式 ----
    {
        RiskErrorCode rms_code = RiskErrorCode::None;
        if (!check_rms_mode_locked(req, rms_code, is_risk_reduction)) {
            return reject(std::string("RMS: ") + to_string(rms_code), rms_code);
        }
    }

    // ---- 检查1：基本合法性 ----
    // 拦截手数为 0 或负数的非法报单
    if (req.volume <= 0) {
        return reject("order volume must be greater than 0", RiskErrorCode::None);
    }

    // ---- 以下检查组：如果是为了降低风险（如止损平仓），则予以豁免 ----
    if (!is_risk_reduction) {
        // ---- 检查2：单笔委托量上限 (防胖手指) ----
        if (req.volume > max_order_size_) {
            return reject("order volume exceeds max_order_size", RiskErrorCode::MAX_ORDER_SIZE);
        }

        // ---- 检查3：交易时段检查 ----
        // 防止策略在非交易时间（如中午休市、周末）发出废单
        if (!in_trading_session()) {
            return reject("outside configured trading session");
        }

        // ---- 检查4：日内绝对亏损限额 ----
        // 计算公式：当日初始资金 - 当前最新资金。如果超过了设置的最大回撤金额，则禁止开新仓
        if (max_daily_loss_ > 0.0 && account_initialized_) {
            const double current_loss = day_start_balance_ - latest_balance_;
            if (current_loss >= max_daily_loss_) {
                return reject("daily loss limit reached", RiskErrorCode::DAILY_LOSS_LIMIT);
            }
        }
    }

    // ---- 检查5：开仓时净持仓投影检查 (Projected Net Position) ----
    // 只有开仓(Open)才需要检查是否超出最大持仓敞口
    if (pos_mgr_ && order_mgr_ && req.offset == Offset::Open) {
        // 核心算法：当前实际净仓位 + 未成交的买单量 - 未成交的卖单量
        const int current_net = pos_mgr_->get_net_position(req.instrument_id);
        const int pending_buy = order_mgr_->get_pending_open_volume(req.instrument_id, Direction::Buy);
        const int pending_sell = order_mgr_->get_pending_open_volume(req.instrument_id, Direction::Sell);
        int projected = current_net + pending_buy + cond_pending_buy - pending_sell - cond_pending_sell;
        
        // 加上本次报单的预估影响
        projected += (req.direction == Direction::Buy) ? req.volume : -req.volume;

        // 如果投影后的绝对值超出限制，则拦截
        if (std::abs(projected) > max_net_position_) {
            return reject("projected net position exceeds limit", RiskErrorCode::MAX_POSITION);
        }

        if (max_total_position_ > 0) {
            const int total_abs = pos_mgr_->get_total_absolute_position();
            if (total_abs + req.volume > max_total_position_) {
                return reject("total absolute position exceeds limit", RiskErrorCode::MAX_POSITION);
            }
        }
    }

    // ---- 检查6：平仓时可平仓位校验 (防止超卖) ----
    // 只有平仓指令(非Open)才需要检查
    if (pos_mgr_ && order_mgr_ && req.offset != Offset::Open) {
        // 注意：平仓单的方向是它要平掉的持仓的“反方向”。
        // 如果发出了买入平仓(Buy)，那么它要平的是空头持仓(Sell)。
        const Direction pos_dir = (req.direction == Direction::Buy) ? Direction::Sell : Direction::Buy;
        const PositionInfo pos = pos_mgr_->get_position(req.instrument_id, pos_dir);

        int available = 0;
        // 严格区分上期所/能源中心的平今、平昨规则，并扣除已经在途（挂单中）的同类型平仓量
        if (req.offset == Offset::CloseToday) {
            const int reserved = order_mgr_->get_pending_close_volume(req.instrument_id, pos_dir, Offset::CloseToday);
            available = (std::max)(0, pos.today - reserved);
        } else if (req.offset == Offset::CloseYesterday) {
            const int reserved = order_mgr_->get_pending_close_volume(req.instrument_id, pos_dir, Offset::CloseYesterday);
            available = (std::max)(0, pos.yesterday - reserved);
        } else {
            // 普通平仓 (如大商所/郑商所)，看总仓位
            const int reserved = order_mgr_->get_pending_close_volume(req.instrument_id, pos_dir, Offset::Close);
            available = (std::max)(0, pos.total - reserved);
        }

        // 如果想平的手数大于当前可用手数，必然被交易所拒单，直接在本地拦截
        if (req.volume > available) {
            return reject("insufficient closeable position");
        }
    }

    // 更新流控时间窗口
    const auto now = std::chrono::steady_clock::now();
    prune_order_rate_window_locked(now);
    prune_cancel_order_window_locked(now);
    prune_cancel_window_locked(now);

    // ---- 检查7、8：报单频率和撤单率限制（同样对减仓单豁免） ----
    if (!is_risk_reduction) {
        // 检查7：限制每分钟报单数 (1分钟滑动窗口内的订单数量)
        // 注意：频率检查对 cancel_rate_exempt 策略仍然生效——避免测试策略疯狂发单耗尽柜台流控。
        if (static_cast<int>(order_rate_timestamps_.size()) >= max_orders_per_minute_) {
            return reject("order rate exceeds max_orders_per_minute", RiskErrorCode::ORDER_RATE_LIMIT);
        }

        // 检查8：撤单率限制
        // 为了避免分母太小导致误判（例如刚发1单撤1单，撤单率就是100%），设置最小样本数
        // cancel_rate_exempt：测试策略反复 send→cancel 不应污染窗口，由 strategy_id pattern 命中决定。
        if (!cancel_rate_exempt &&
            static_cast<int>(cancel_rate_order_timestamps_.size()) >= cancel_rate_min_sample_) {
            const double rate = static_cast<double>(cancel_timestamps_.size()) /
                                static_cast<double>(cancel_rate_order_timestamps_.size());
            if (rate > max_cancel_rate_) {
                return reject("cancel rate exceeds configured window limit");
            }
        }
    }

    // 所有风控检查通过
    return true;
}

bool RiskManager::is_cancel_rate_exempt(const char* strategy_id) const {
    if (strategy_id == nullptr || strategy_id[0] == '\0') return false;
    std::shared_lock<std::shared_mutex> lock(mtx_);
    for (const auto& pat : cancel_rate_exempt_patterns_) {
        if (glob_match(pat, strategy_id)) return true;
    }
    return false;
}

void RiskManager::on_cancel() {
    // 发生撤单时，将当前时间戳压入撤单窗口队列
    std::unique_lock<std::shared_mutex> lock(mtx_);
    const auto now = std::chrono::steady_clock::now();
    cancel_timestamps_.push_back(now);
    prune_cancel_window_locked(now); // 顺便清理过期数据
}

void RiskManager::on_order_sent() {
    // 发送报单时，将时间戳压入发单频率窗口和撤单率分母窗口
    std::unique_lock<std::shared_mutex> lock(mtx_);
    const auto now = std::chrono::steady_clock::now();
    order_rate_timestamps_.push_back(now);
    cancel_rate_order_timestamps_.push_back(now);
    prune_order_rate_window_locked(now);
    prune_cancel_order_window_locked(now);
}

void RiskManager::update_account(const AccountInfo& account) {
    // 收到资金回报，更新最新余额
    std::unique_lock<std::shared_mutex> lock(mtx_);
    latest_balance_ = account.balance;
    // 如果是启动后的第一笔资金回报，将其设为当日基准资金，用于计算日内绝对亏损
    if (!account_initialized_) {
        account_initialized_ = true;
        day_start_balance_ = account.balance;
    }
}

void RiskManager::update_trading_day(const std::string& trading_day) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    if (trading_day.empty() || trading_day_ == trading_day) {
        return;
    }

    // 发生交易日切换（如夜盘开盘时，CTP 会推送新的交易日）
    trading_day_ = trading_day;
    if (account_initialized_) {
        // 重置日内资金基准为当前的最新余额
        day_start_balance_ = latest_balance_;
    }
    total_rejects_today_ = 0;
    LOG_INFO("RiskManager trading_day updated: " + trading_day_ +
             " day_start_balance=" + std::to_string(day_start_balance_));
}

void RiskManager::reload_risk_config(const Config& config) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    const std::string acct_section = account_id_.empty() ? "" : "Risk." + account_id_;
    const bool has_acct = !acct_section.empty() && config.has_section(acct_section);
    const std::string& section = has_acct ? acct_section : std::string("Risk");
    max_order_size_ = config.get_int(section, "MaxOrderSize", 5);
    max_net_position_ = config.get_int(section, "MaxNetPosition", 10);
    max_total_position_ = config.get_int(section, "MaxTotalPosition", 0);
    max_orders_per_minute_ = config.get_int(section, "MaxOrdersPerMinute", 30);
    cancel_rate_window_minutes_ = config.get_int(section, "CancelRateWindowMinutes", 60);
    cancel_rate_min_sample_ = config.get_int(section, "CancelRateMinSample", 10);
    max_cancel_rate_ = config.get_double(section, "MaxCancelRate", 0.5);
    max_daily_loss_ = config.get_double(section, "MaxDailyLoss", 0.0);
    cancel_rate_exempt_patterns_ = parse_pattern_list(
        config.get_string(section, "CancelRateExemptStrategies", ""));
    clamp_risk_params();

    trading_sessions_.clear();
    std::istringstream iss(config.get_string("Trading", "TradingSessions", ""));
    std::string token;
    while (std::getline(iss, token, ',')) {
        const auto dash = token.find('-');
        if (dash == std::string::npos) {
            continue;
        }

        const int start = parse_hhmm(token.substr(0, dash));
        const int end = parse_hhmm(token.substr(dash + 1));
        if (start >= 0 && end >= 0) {
            trading_sessions_.push_back({start, end});
        }
    }

    LOG_INFO("RiskManager reloaded: order_size=" + std::to_string(max_order_size_) +
             " net_position=" + std::to_string(max_net_position_) +
             " orders_per_min=" + std::to_string(max_orders_per_minute_) +
             " cancel_rate=" + std::to_string(max_cancel_rate_) +
             " daily_loss=" + std::to_string(max_daily_loss_) +
             " trading_sessions=" + std::to_string(trading_sessions_.size()));
}

RiskSnapshot RiskManager::get_snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    RiskSnapshot snapshot;
    snapshot.max_order_size = max_order_size_;
    snapshot.max_net_position = max_net_position_;
    snapshot.order_freq_limit = max_orders_per_minute_;
    snapshot.current_order_freq = static_cast<int>(order_rate_timestamps_.size());
    snapshot.cancel_rate_window_minutes = cancel_rate_window_minutes_;
    snapshot.cancel_rate_limit = max_cancel_rate_;
    snapshot.current_cancel_rate = cancel_rate_order_timestamps_.empty()
        ? 0.0
        : static_cast<double>(cancel_timestamps_.size()) /
            static_cast<double>(cancel_rate_order_timestamps_.size());
    snapshot.max_daily_loss = max_daily_loss_;
    snapshot.current_daily_loss = account_initialized_
        ? (std::max)(0.0, day_start_balance_ - latest_balance_)
        : 0.0;
    snapshot.total_rejects_today = total_rejects_today_;
    snapshot.rms_mode = rms_mode_;
    snapshot.last_error_code = last_error_code_;

    const bool loss_warning = snapshot.max_daily_loss > 0.0 &&
        snapshot.current_daily_loss >= snapshot.max_daily_loss * 0.8;
    const bool order_warning = snapshot.order_freq_limit > 0 &&
        snapshot.current_order_freq >= static_cast<int>(snapshot.order_freq_limit * 0.8);
    const bool cancel_warning = snapshot.cancel_rate_limit > 0.0 &&
        snapshot.current_cancel_rate >= snapshot.cancel_rate_limit * 0.8;
    // RMS 模式优先级高于自动计算的 risk_level
    if (rms_mode_ != RiskMode::Normal && rms_mode_ != RiskMode::Warning) {
        snapshot.risk_level = to_string(rms_mode_);
    } else {
        snapshot.risk_level = (loss_warning || order_warning || cancel_warning) ? "warning" : "normal";
    }
    return snapshot;
}

// ---- RMS 风控模式实现 ----

void RiskManager::set_risk_mode(RiskMode mode, const std::string& reason) {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    if (rms_mode_ == mode) return;

    const auto prev = rms_mode_;
    rms_mode_ = mode;

    RiskEvent evt{};
    evt.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    evt.mode = mode;
    if (!reason.empty()) {
        safe_copy(evt.message, reason.c_str(), sizeof(evt.message));
    }
    pending_risk_events_.push_back(evt);

    LOG_INFO("RMS mode changed: " + std::string(to_string(prev)) +
             " -> " + std::string(to_string(mode)) +
             (reason.empty() ? "" : " reason=" + reason));
}

RiskMode RiskManager::get_risk_mode() const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return rms_mode_;
}

std::vector<RiskEvent> RiskManager::drain_risk_events() {
    std::unique_lock<std::shared_mutex> lock(mtx_);
    std::vector<RiskEvent> events;
    events.swap(pending_risk_events_);
    return events;
}

bool RiskManager::check_rms_mode(const OrderRequest& req, RiskErrorCode& error_code, bool is_risk_reduction) const {
    std::shared_lock<std::shared_mutex> lock(mtx_);
    return check_rms_mode_locked(req, error_code, is_risk_reduction);
}

bool RiskManager::check_rms_mode_locked(const OrderRequest& req, RiskErrorCode& error_code, bool is_risk_reduction) const {
    if (rms_mode_ == RiskMode::Halted) {
        if (is_risk_reduction || req.offset != Offset::Open) {
            return true;
        }
        error_code = RiskErrorCode::RISK_HALTED;
        return false;
    }
    if (rms_mode_ == RiskMode::Liquidating) {
        if (req.offset == Offset::Open) {
            error_code = RiskErrorCode::RISK_LIQUIDATING;
            return false;
        }
        return true;
    }
    if (rms_mode_ == RiskMode::ReduceOnly) {
        if (is_risk_reduction) return true;
        if (req.offset == Offset::Open) {
            error_code = RiskErrorCode::RISK_REDUCE_ONLY;
            return false;
        }
        return true;
    }
    if (rms_mode_ == RiskMode::NoOpen) {
        if (req.offset == Offset::Open) {
            error_code = RiskErrorCode::RISK_NO_OPEN;
            return false;
        }
        return true;
    }
    error_code = RiskErrorCode::None;
    return true;
}

// 交易时段检查逻辑
// 支持跨午夜时段配置，例如夜盘 "21:00-02:30"
// 如果配置文件中 TradingSessions 为空，则默认全天 24 小时允许交易
bool RiskManager::in_trading_session() const {
    if (trading_sessions_.empty()) {
        return true;
    }

    const std::tm tm_buf = local_time_now();
    const int now_minutes = minutes_since_midnight(tm_buf);
    
    for (const auto& [start, end] : trading_sessions_) {
        if (start <= end) {
            // 普通时段，如 "09:00-11:30"
            if (now_minutes >= start && now_minutes <= end) return true;
        } else {
            // 跨午夜时段，如 "21:00-02:30" (即 start=1260, end=150)
            // 满足 大于等于21:00 或者 小于等于02:30 均可
            if (now_minutes >= start || now_minutes <= end) return true;
        }
    }
    return false;
}

int RiskManager::minutes_since_midnight(const std::tm& tm_buf) {
    return tm_buf.tm_hour * 60 + tm_buf.tm_min;
}

void RiskManager::prune_order_rate_window_locked(TimePoint now) {
    // 清理发单频率窗口：移除 1 分钟之前的时间戳
    const auto cutoff = now - std::chrono::minutes(1);
    while (!order_rate_timestamps_.empty() && order_rate_timestamps_.front() < cutoff) {
        order_rate_timestamps_.pop_front();
    }
}

void RiskManager::prune_cancel_order_window_locked(TimePoint now) {
    // 清理撤单率分母窗口：移除 cancel_rate_window_minutes_ 之前的时间戳
    const auto cutoff = now - std::chrono::minutes((std::max)(1, cancel_rate_window_minutes_));
    while (!cancel_rate_order_timestamps_.empty() && cancel_rate_order_timestamps_.front() < cutoff) {
        cancel_rate_order_timestamps_.pop_front();
    }
}

void RiskManager::prune_cancel_window_locked(TimePoint now) {
    // 清理撤单率分子窗口：移除 cancel_rate_window_minutes_ 之前的时间戳
    const auto cutoff = now - std::chrono::minutes((std::max)(1, cancel_rate_window_minutes_));
    while (!cancel_timestamps_.empty() && cancel_timestamps_.front() < cutoff) {
        cancel_timestamps_.pop_front();
    }
}

} // namespace hft
