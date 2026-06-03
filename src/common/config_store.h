#pragma once
// ============================================
// config_store.h - SQLite persistent storage layer (SQLite 持久化存储层)
//
// Responsibilities (职责)：
//   1. Trading data persistence: orders / trades / audit / risk events / PnL curve
//      (交易数据持久化：订单/成交/审计/风控事件/盈亏曲线)
//   2. Async write queue (non-blocking for trading thread) (异步写入队列，不阻塞交易线程)
//   3. Sync query API for Web API (同步查询接口，供 Web API 调用)
//
// Design principles (设计原则)：
//   - Trading thread only calls async_* methods, returns immediately (交易线程只调 async_* 方法，立即返回)
//   - Background writer thread batch-flushes to SQLite (后台 writer 线程批量 flush 到 SQLite)
//   - SQLite WAL mode, writes don't block reads (SQLite WAL 模式，写入不阻塞读取)
//   - Write to emergency ring buffer when queue is full, never block (队列满时写 emergency ring buffer，绝不阻塞)
// ============================================

#include "common/types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

struct sqlite3;

namespace hft {

// Forward declarations
struct AccountConfigSpec;
struct AccountConfigBundle;
struct StrategyConfigSpec;
struct InstrumentSpec;

class ConfigStore {
public:
    ConfigStore() = default;
    ~ConfigStore();

    // Initialize database (create/open + create tables) (初始化数据库：创建/打开 + 建表)
    bool init(const std::string& db_path);

    // Graceful shutdown: flush queue then close database (优雅关闭：flush 队列后关闭数据库)
    void shutdown();

    // ---- Async writes (trading path, non-blocking) (异步写入，交易路径，不阻塞) ----
    void async_insert_order(const OrderInfo& order, const std::string& source);
    void async_update_order(const OrderInfo& order);
    void async_insert_trade(const TradeInfo& trade);
    void async_log_audit(const std::string& action, const std::string& source,
                         const std::string& detail, const std::string& result,
                         const std::string& remote_ip = "");
    void async_log_risk_event(const std::string& event_type, const std::string& scope,
                              const std::string& instrument, const std::string& reason,
                              const std::string& detail_json = "{}");
    void async_insert_pnl(int64_t timestamp_ms, const std::string& time_str,
                          double balance, double available, double margin,
                          double position_profit, double total_pnl,
                          const std::string& trading_day);

    // ---- Sync queries (Web API path, may block) (同步查询，Web API 路径，可以阻塞) ----
    std::vector<OrderInfo> query_orders(const std::string& trading_day = "",
                                        size_t limit = 500) const;
    std::vector<TradeInfo> query_trades(const std::string& trading_day = "",
                                        size_t limit = 500) const;
    std::vector<std::string> query_recent_audits(size_t limit = 100) const;
    std::vector<std::string> query_risk_events(const std::string& trading_day = "",
                                                size_t limit = 200) const;

    // ---- Config CRUD (sync, for Web API and startup flow) (配置 CRUD，同步，供 Web API 和启动流程调用) ----

    // Account config (password encrypted storage) (账户配置，密码加密存储)
    bool save_account_bundle(const AccountConfigBundle& bundle);
    AccountConfigBundle load_account_bundle() const;

    // Risk control config (风控配置)
    bool save_risk_config(const std::string& key, const std::string& value);
    std::string load_risk_config(const std::string& key, const std::string& default_val = "") const;
    std::vector<std::pair<std::string, std::string>> load_all_risk_config() const;

    // System config (AI/alerts/Web auth etc. generic key-value) (系统配置：AI/告警/Web认证等通用 key-value)
    bool save_system_config(const std::string& key, const std::string& value);
    std::string load_system_config(const std::string& key, const std::string& default_val = "") const;
    bool save_encrypted_system_config(const std::string& key, const std::string& value);
    std::string load_encrypted_system_config(const std::string& key, const std::string& default_val = "") const;

    // Check if there is already migrated config data (检查是否有已迁移的配置数据)
    bool has_migrated_config() const;
    void mark_migrated();

    // Strategy config (SQLite primary storage) (策略配置，SQLite 主存储)
    bool save_strategy_specs(const std::vector<StrategyConfigSpec>& specs);
    std::vector<StrategyConfigSpec> load_strategy_specs() const;
    bool delete_strategy(const std::string& strategy_id);

    // Instrument config (SQLite primary storage) (合约配置，SQLite 主存储)
    bool save_instrument_spec(const InstrumentSpec& spec);
    std::vector<InstrumentSpec> load_instrument_specs() const;
    InstrumentSpec load_instrument_spec(const std::string& instrument_id) const;
    bool has_instrument_spec(const std::string& instrument_id) const;

    // ---- Trading day review report (交易日复盘报告) ----
    bool save_daily_report(const std::string& trading_day, const std::string& report_json);
    std::string load_daily_report(const std::string& trading_day) const;
    std::vector<std::string> list_daily_reports(size_t limit = 30) const;

    // Database path (数据库路径)
    const std::string& db_path() const { return db_path_; }

    // Statistics (统计)
    size_t pending_writes() const;
    size_t total_writes() const { return total_writes_.load(std::memory_order_relaxed); }
    size_t emergency_writes() const { return emergency_writes_.load(std::memory_order_relaxed); }

private:
    // Internal write task types (内部写入任务类型)
    struct InsertOrderTask {
        std::string order_ref, account_id, strategy_id, instrument_id;
        std::string exchange_id, direction, offset_flag, source, insert_time;
        double price = 0;
        int total_volume = 0, status = 0;
    };
    struct UpdateOrderTask {
        std::string order_ref, status_msg, account_id, instrument_id;
        std::string exchange_id, order_sys_id, insert_time;
        int status = 0, traded_volume = 0, front_id = 0, session_id = 0;
    };
    struct InsertTradeTask {
        std::string trade_id, order_ref, account_id, strategy_id;
        std::string instrument_id, exchange_id, direction, offset_flag, trade_time;
        double price = 0;
        int volume = 0;
    };
    struct LogAuditTask {
        std::string action, source, detail, result, remote_ip;
    };
    struct LogRiskEventTask {
        std::string event_type, scope, instrument, reason, detail_json;
    };
    struct InsertPnlTask {
        int64_t timestamp_ms = 0;
        std::string time_str, trading_day;
        double balance = 0, available = 0, margin = 0;
        double position_profit = 0, total_pnl = 0;
    };

    using WriteTask = std::variant<InsertOrderTask, UpdateOrderTask, InsertTradeTask,
                                   LogAuditTask, LogRiskEventTask, InsertPnlTask>;

    void ensure_schema();
    void writer_loop();
    void flush_tasks(std::deque<WriteTask>& tasks);
    void execute_task(const WriteTask& task);
    void enqueue(WriteTask task);
    void write_emergency(const std::string& line);

    // Concrete SQL execution (具体 SQL 执行)
    void exec_insert_order(const InsertOrderTask& t);
    void exec_update_order(const UpdateOrderTask& t);
    void exec_insert_trade(const InsertTradeTask& t);
    void exec_log_audit(const LogAuditTask& t);
    void exec_log_risk_event(const LogRiskEventTask& t);
    void exec_insert_pnl(const InsertPnlTask& t);

    bool exec_sql(const char* sql);
    bool exec_sql(const std::string& sql);

    sqlite3* db_ = nullptr;
    std::string db_path_;

    // Async write queue (异步写入队列)
    mutable std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::deque<WriteTask> write_queue_;
    static constexpr size_t kMaxQueueSize = 10000;

    // Background writer thread (后台写入线程)
    std::thread writer_thread_;
    std::atomic<bool> writer_running_{false};

    // Statistics (统计)
    std::atomic<size_t> total_writes_{0};
    std::atomic<size_t> emergency_writes_{0};

    // Query mutex (read/write separation: writes go to queue, reads go to this lock)
    // (查询用锁：读写分离，写走队列，读走这个锁)
    mutable std::mutex read_mtx_;
};

} // namespace hft
