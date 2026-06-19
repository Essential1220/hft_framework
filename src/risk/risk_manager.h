#pragma once
// ============================================
// risk_manager.h - 风控管理器
//
// 职责：作为交易引擎的最后一道防线，在每笔下单(发往交易所)之前进行多维度风控检查，
// 拦截并拒绝可能导致超限、违规或高风险的报单请求。
//
// 检查维度（按优先级）：
//   1. 基础合规检查：单笔委托数量上限（max_order_size_），防止胖手指(Fat Finger)错误。
//   2. 交易时段检查：非交易时段拒绝开仓（trading_sessions_），防止无效报单消耗资源。
//   3. 资金风控：日内最大亏损限制（max_daily_loss_），基于动态更新的 day_start_balance_。
//   4. 敞口风控：单合约净持仓上限（max_net_position_），包含正在挂单但未成交的“投影持仓”。
//   5. 可平仓位校验：防止超卖（Over-selling）导致交易所拒单，严格区分今仓/昨仓/总仓。
//   6. 流控限制（Rate Limiting）：报单频率限制（max_orders_per_minute_），使用滑动窗口实现。
//   7. 撤单率限制：撤单率（max_cancel_rate_），防止因频繁撤单被交易所认定为异常交易行为（如晃骗 Spoofing）。
//
// 线程安全：所有公共方法均通过 mtx_ 保护，确保可被策略消费线程、GUI查询线程等安全并发调用。
//
// 特殊机制 (is_risk_reduction)：
//   当操作本质上是“降低风险”的减仓/平仓时（is_risk_reduction = true），
//   系统允许绕过诸如频率限制、时段限制或日内亏损限制。
//   原则：风控的目的是控制风险，绝不能因为触发了风控而阻止系统平仓自救。
// ============================================

#include "common/config.h"
#include "common/types.h"
#include "position/position_manager.h"
#include "engine/tick_data_manager.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace hft {

struct RiskSnapshot {
    int max_order_size = 0;
    int max_net_position = 0;
    int order_freq_limit = 0;
    int current_order_freq = 0;
    int cancel_rate_window_minutes = 0;
    double cancel_rate_limit = 0.0;
    double current_cancel_rate = 0.0;
    double max_daily_loss = 0.0;
    double current_daily_loss = 0.0;
    double margin_usage_ratio = 0.0;
    std::string risk_level = "normal";
    std::string last_reject_reason;
    int total_rejects_today = 0;
    RiskMode rms_mode = RiskMode::Normal;  // 当前 RMS 风控模式
    RiskErrorCode last_error_code = RiskErrorCode::None;
};

class OrderManager;

class RiskManager {
public:
    // 初始化风控参数，从配置文件中读取各项硬性阈值并绑定相关管理器
    void init(const Config& config, PositionManager* pos_mgr, OrderManager* order_mgr,
              TickDataManager* tick_mgr = nullptr,
              const std::string& account_id = "");

    // 核心检查函数：在下单前执行所有风控校验
    // @param req: 待发送的委托请求
    // @param reject_reason: 出参，如果被拒绝，填入具体原因供日志记录和前端展示
    // @param is_risk_reduction: 标志位，如果为 true（例如止损平仓单），则放宽部分限制
    // @return: true 表示通过风控允许发送，false 表示拦截
    bool check_order(const OrderRequest& req, std::string& reject_reason, bool is_risk_reduction = false,
                     int cond_pending_buy = 0, int cond_pending_sell = 0,
                     bool cancel_rate_exempt = false);

    // 判断给定 strategy_id 是否命中 CancelRateExemptStrategies 配置的 glob pattern。
    // 命中则跳过撤单率检查（用于 dev/test 策略反复 send→cancel 把 60min 窗口打满的场景）。
    bool is_cancel_rate_exempt(const char* strategy_id) const;

    // 接收撤单回报的回调：用于更新撤单滑动窗口中的时间戳，以便计算撤单率
    void on_cancel();

    // 报单成功发送的回调：用于更新发单滑动窗口中的时间戳，以便计算发单频率和撤单分母
    void on_order_sent();

    // 接收资金查询回报的回调：动态更新账户的最新余额，以计算日内浮动盈亏
    void update_account(const AccountInfo& account);

    // 交易日切换回调：用于在夜盘开盘或跨日时重置日内亏损基准和资金状态
    void update_trading_day(const std::string& trading_day);

    RiskSnapshot get_snapshot() const;

    // ---- RMS 风控模式 ----
    // 设置 RMS 风控模式，触发 RiskEvent 推送
    void set_risk_mode(RiskMode mode, const std::string& reason = "");
    // 获取当前 RMS 风控模式
    RiskMode get_risk_mode() const;
    // 取出并清空待推送的 RiskEvent 列表（SSE 线程调用）
    std::vector<RiskEvent> drain_risk_events();
    // 判断给定的 OrderRequest 在当前 RMS 模式下是否允许
    // 返回 true = 允许，false = 拒绝（error_code 填充拒绝原因）
    bool check_rms_mode(const OrderRequest& req, RiskErrorCode& error_code, bool is_risk_reduction) const;

    // 热重载风控参数：从 Config 重新读取阈值，无需重启引擎即生效
    void reload_risk_config(const Config& config);

    // 日亏损基准持久化支持
    double get_day_start_balance() const {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        return day_start_balance_;
    }
    void set_day_start_balance(double balance) {
        std::unique_lock<std::shared_mutex> lock(mtx_);
        if (balance > 0.0) {
            day_start_balance_ = balance;
            account_initialized_ = true;
        }
    }
    std::string get_trading_day() const {
        std::shared_lock<std::shared_mutex> lock(mtx_);
        return trading_day_;
    }

private:
    // 使用高精度单调时钟，不受系统时间修改影响，适合做滑动窗口的流控计算
    using TimePoint = std::chrono::steady_clock::time_point;

    // 辅助方法：检查当前系统时间是否处于配置允许的交易时段内
    bool in_trading_session() const;

    // 辅助方法：将 std::tm 格式的时间转换为"自午夜 00:00 以来的分钟数" (0-1439)，便于进行时间段比较
    static int minutes_since_midnight(const std::tm& tm_buf);

    // 以下方法用于清理滑动窗口中过期的历史数据（流控核心）
    // 注意：调用方必须持有 mtx_ 锁
    void prune_order_rate_window_locked(TimePoint now);         // 清理报单频率窗口 (通常1分钟)
    void prune_cancel_order_window_locked(TimePoint now);       // 清理撤单率计算的"报单"分母窗口
    void prune_cancel_window_locked(TimePoint now);             // 清理撤单率计算的"撤单"分子窗口

    // ---- 静态风控参数 (由 init() 从配置加载) ----
    int max_order_size_ = 5;              // 限制单笔报单的最大手数（防胖手指）
    int max_net_position_ = 10;           // 限制单合约(多空相抵后)的最大净敞口手数
    int max_total_position_ = 0;          // 全品种总持仓上限（0=不限制）
    int max_orders_per_minute_ = 30;      // 限制每分钟的最大发单笔数（防死循环疯狂发单）
    int cancel_rate_window_minutes_ = 60; // 撤单率统计的时间窗口长度（分钟）
    int cancel_rate_min_sample_ = 10;    // 撤单率计算的最小样本数（防小样本误判）
    double max_cancel_rate_ = 0.5;        // 窗口内允许的最大撤单率 (撤单次数 / 发单次数)，例如 50%
    double max_daily_loss_ = 0.0;         // 限制日内最大绝对亏损金额（0 表示不限制）
    double max_price_deviation_ = 0.1;    // 限价单价格偏离最新价的最大比例（默认 10%，0 表示不限制）

    // 撤单率豁免 strategy_id pattern 列表（glob：开头/结尾的 * 通配，例如 *_test、test_*）。
    // 命中的策略在 check_order 中跳过撤单率分支，但仍受 MaxOrderSize/MaxNetPosition 等其它检查约束。
    std::vector<std::string> cancel_rate_exempt_patterns_;

    // ---- 依赖的子系统指针 ----
    PositionManager* pos_mgr_ = nullptr;  // 用于查询当前实际持仓
    OrderManager* order_mgr_ = nullptr;   // 用于查询未成交的挂单(投影敞口)
    TickDataManager* tick_mgr_ = nullptr; // 用于获取最新行情(价格校验)
    
    // 保护风控管理器内部状态的读写锁
    // 写操作(check_order/on_order_sent/on_cancel)用 unique_lock
    // 读操作(get_snapshot)用 shared_lock，避免阻塞 SSE 推送线程
    mutable std::shared_mutex mtx_;  

    // ---- 流控计算的滑动窗口数据结构 ----
    // 使用双端队列(deque)保存时间戳，队头为最旧的数据，队尾为最新数据
    std::deque<TimePoint> order_rate_timestamps_;       // 用于计算"每分钟发单数"的时间戳队列
    std::deque<TimePoint> cancel_rate_order_timestamps_; // 用于计算"撤单率"分母（历史发单）的时间戳队列
    std::deque<TimePoint> cancel_timestamps_;            // 用于计算"撤单率"分子（历史撤单）的时间戳队列

    // ---- 交易时段配置 ----
    // 存储多个合法的交易时间区间，格式为：[开始分钟数, 结束分钟数]
    std::vector<std::pair<int, int>> trading_sessions_;

    // RMS 模式检查内部实现（调用方必须持有 mtx_）
    bool check_rms_mode_locked(const OrderRequest& req, RiskErrorCode& error_code, bool is_risk_reduction) const;

    void clamp_risk_params();

    // ---- 资金与亏损跟踪状态 ----
    bool account_initialized_ = false;   // 标记是否已接收到当日第一笔有效的资金回报
    double day_start_balance_ = 0.0;     // 记录当前交易日起始资金基准，用于计算绝对亏损
    double latest_balance_ = 0.0;        // 记录最新一笔资金回报中的动态权益余额
    std::string trading_day_;            // 记录当前系统所处的交易日字符串 (如 "20231024")
    std::string account_id_;             // 关联账户ID（用于读取 [Risk.AccountID] 覆盖配置）
    int total_rejects_today_ = 0;

    // ---- RMS 风控模式状态 ----
    RiskMode rms_mode_ = RiskMode::Normal;
    RiskErrorCode last_error_code_ = RiskErrorCode::None;
    std::vector<RiskEvent> pending_risk_events_;  // 待推送的事件队列
};

} // namespace hft
