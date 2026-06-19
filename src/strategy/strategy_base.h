#pragma once
// ============================================
// strategy_base.h - Strategy base class (策略基类)
//
// Abstract base for all strategy types (C++ native and Python bridge).
// Defines lifecycle callbacks, event dispatch, and protected trading APIs.
// 所有策略类型（C++ 原生和 Python 桥接）的抽象基类，
// 定义生命周期回调、事件分发和受保护的交易 API。
// ============================================

#include "common/types.h"
#include "engine/i_trading_context.h"
#include "engine/kline_manager.h"
#include "market/order_book.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace hft {

class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    // Lifecycle callback: called when the engine is ready and strategy should initialize
    // 生命周期回调：引擎就绪后调用，策略在此初始化 (策略初始化)
    virtual void on_init() = 0;
    // Event callback: called on each incoming market data tick
    // 事件回调：接收到行情 Tick 数据时调用 (行情回调)
    virtual void on_tick(const TickData& tick) = 0;
    // Event callback: called on each order status update from the exchange
    // 事件回调：接收到交易所委托回报时调用 (委托回报)
    virtual void on_order(const OrderInfo& order) = 0;
    // Event callback: called on each trade fill notification from the exchange
    // 事件回调：接收到交易所成交回报时调用 (成交回报)
    virtual void on_trade(const TradeInfo& trade) = 0;
    // Event callback: called after gateway reconnects and syncs state
    // 事件回调：网关重连完成并同步状态后调用 (重连回调)
    virtual void on_reconnect() {}
    // Lifecycle callback: called before strategy is unloaded, for cleanup
    // 生命周期回调：策略被卸载前调用，用于清理资源 (策略停止)
    virtual void on_stop() {}
    // Event callback: called when a K-line bar completes (K 线完成回调)
    virtual void on_bar(const std::string& instrument, const std::string& period, const KlineBar& bar) {
        (void)instrument; (void)period; (void)bar;
    }
    // Event callback: called when a registered timer fires (定时器回调)
    virtual void on_timer(int timer_id) { (void)timer_id; }

    // GIL batch optimization: whether this strategy requires the Python interpreter lock
    // GIL 批量优化：标记策略是否需要 Python 解释器锁 (需要解释器锁)
    virtual bool is_interpreted() const { return false; }

    struct InterpreterLockGuard {
        virtual ~InterpreterLockGuard() = default;
    };
    // Acquire the interpreter lock; the lock is held while the returned guard is alive
    // 获取解释器锁；调用方持有 guard 期间锁持续生效 (获取解释器锁)
    virtual std::unique_ptr<InterpreterLockGuard> acquire_interpreter_lock() { return nullptr; }

    // Inject the trading engine pointer for downstream API calls
    // 注入交易引擎指针，供策略调用底层 API (注入引擎指针)
    void set_engine(ITradingContext* engine) { engine_ = engine; }
    // Configure strategy context: ID, account, and watched instruments
    // 配置策略上下文信息：策略ID、默认账号、关注合约列表 (配置上下文)
    void configure_context(std::string strategy_id, std::string default_account_id,
                           std::vector<std::string> watched_instruments);
    // Configure strategy metadata: type, script path, parameters
    // 配置策略元数据：类型、脚本路径、扩展参数 (配置元数据)
    void configure_metadata(std::string strategy_type, std::string script_path,
                            std::map<std::string, std::string> parameters);

    // Get strategy ID (获取策略ID)
    const std::string& strategy_id() const { return strategy_id_; }
    // Get default account ID (获取默认资金账号)
    const std::string& default_account_id() const { return default_account_id_; }
    // Get watched instruments list (获取关注的合约列表)
    const std::vector<std::string>& watched_instruments() const { return watched_instruments_; }
    // Get strategy type string (获取策略类型)
    const std::string& strategy_type() const { return strategy_type_; }
    // Whether this is a Python strategy — computed once in configure_metadata for O(1) hot-path access;
    // 是否为 Python 策略 — 在 configure_metadata 时根据 strategy_type 一次性计算，
    // avoids per-tick case-insensitive string comparison.
    // 热路径 O(1) 读取，避免每 tick 对 strategy_type 做小写化字符串比较 (是否Python策略)
    bool is_python() const { return is_python_; }
    // Get script path (获取脚本路径)
    const std::string& script_path() const { return script_path_; }
    // Get extended parameters map (获取扩展参数表)
    const std::map<std::string, std::string>& parameters() const { return parameters_; }
    const std::string& version() const { return version_; }       // Get strategy version (获取策略版本)
    void set_version(const std::string& v) { version_ = v; }      // Set strategy version (设置策略版本)
    // Get a string parameter by key, with default fallback (获取字符串参数，带默认值回退)
    std::string get_parameter(const std::string& key, const std::string& default_value = "") const;
    // Get an integer parameter by key, with default fallback (获取整数参数，带默认值回退)
    int get_parameter_int(const std::string& key, int default_value = 0) const;
    // Get a double parameter by key, with default fallback (获取浮点参数，带默认值回退)
    double get_parameter_double(const std::string& key, double default_value = 0.0) const;

    // Check if an event belongs to this strategy (based on strategy_id match)
    // 检查事件是否属于该策略（基于策略ID匹配）(策略ID匹配检查)
    bool handles_strategy(const char* strategy_id) const;
    // Check if an event belongs to this strategy's default account
    // 检查事件是否属于该策略的默认账号 (账号匹配检查)
    bool handles_account(const char* account_id) const;
    // Check if an instrument is in this strategy's watchlist
    // 检查合约是否在策略关注列表中 (合约匹配检查)
    bool handles_instrument(const char* instrument_id) const;
    // Combined check: should this event be dispatched to this strategy
    // 综合检查事件是否应该派发给该策略 (综合事件匹配检查)
    bool handles_event(const char* account_id, const char* instrument_id) const;

protected:
    // ---- APIs provided to derived strategies (提供给派生策略调用的 API) ----
    // Send a regular order; returns the order_ref string (发送普通委托；返回报单引用)
    std::string send_order(const OrderRequest& req);
    // Cancel a regular order by its order_ref (通过报单引用撤销普通委托 / 撤单)
    void cancel_order(const std::string& order_ref);
    // Add a conditional order (stop loss, take profit, trailing stop) (添加条件单)
    uint32_t add_conditional_order(const ConditionalOrder& order);
    // Cancel a conditional order by its ID (通过ID取消条件单 / 取消条件单)
    void cancel_conditional_order(uint32_t id);
    // Allocate an OCO group ID for conditional orders (分配 OCO 互斥组ID / 分配条件单分组ID)
    uint32_t allocate_cond_group_id();

    // Query aggregated position across all accounts (查询聚合持仓)
    PositionInfo get_position(const char* instrument, Direction dir);
    // Query position for a specific account (查询指定账号的持仓)
    PositionInfo get_position(const char* instrument, Direction dir, const char* account_id);
    // Query aggregated net position across all accounts (查询聚合净持仓)
    int get_net_position(const char* instrument);
    // Query net position for a specific account (查询指定账号的净持仓)
    int get_net_position(const char* instrument, const char* account_id);

    // Query order book snapshot (查询盘口深度快照)
    WindowedOrderBook get_order_book(const char* instrument);
    // Query account info (查询资金账户信息)
    AccountInfo get_account_info(const std::string& account_id = "");
    // Strategy logging through AsyncLogger (通过异步日志记录策略日志)
    void log_info(const std::string& msg);
    void log_warn(const std::string& msg);
    void log_error(const std::string& msg);
    // Strategy state persistence (策略状态持久化)
    void save_state(const std::map<std::string, std::string>& state);
    std::map<std::string, std::string> load_state();
    // Timer registration (定时器注册)
    int register_timer(int interval_ms);
    void unregister_timer(int timer_id);
    // Query historical K-lines (查询历史 K 线)
    std::vector<KlineBar> query_klines(const std::string& instrument,
                                        const std::string& period,
                                        size_t count = 200);

    ITradingContext* engine_ = nullptr;                 // Trading engine instance (交易引擎实例)
    std::string strategy_id_;                         // Unique strategy identifier (策略唯一标识)
    std::string default_account_id_;                  // Default account used for orders (默认使用的资金账号)
    std::vector<std::string> watched_instruments_;    // Instruments this strategy monitors (该策略关注的合约列表)
    std::unordered_set<uint32_t> watched_hashes_;    // FNV32 hashes of watched_instruments_ for O(1) lookup
    std::string strategy_type_;                       // Strategy type string, e.g. "simple" or "python" (策略类型)
    bool is_python_ = false;                          // Cached: whether strategy_type_ is "python" (缓存是否为python)
    std::string script_path_;                         // Path to the strategy script file (策略脚本路径)
    std::string version_;                             // Strategy version string (策略版本)
    std::map<std::string, std::string> parameters_;   // Extended key-value parameters (扩展键值参数)
};

} // namespace hft
