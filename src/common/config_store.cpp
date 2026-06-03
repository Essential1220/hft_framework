// ============================================
// config_store.cpp - SQLite persistent storage layer implementation (SQLite 持久化存储层实现)
// ============================================

#include "common/config_store.h"
#include "common/crypto.h"
#include "common/logger.h"
#include <sqlite3.h>
#include "engine/account_config.h"
#include "engine/trading_engine.h"
#include "strategy/strategy_config.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hft {

namespace {
std::string local_time_hms() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S");
    return ss.str();
}

std::string non_empty_time_or_now(const char* value) {
    if (value && value[0] != '\0') return value;
    return local_time_hms();
}
}

ConfigStore::~ConfigStore() {
    shutdown();
}

bool ConfigStore::init(const std::string& db_path) {
    db_path_ = db_path;

    const int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOG_ERROR("ConfigStore: failed to open database: " + db_path +
                  " error: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }

    // WAL mode: writes don't block reads (WAL 模式：写入不阻塞读取)
    exec_sql("PRAGMA journal_mode=WAL");
    exec_sql("PRAGMA synchronous=NORMAL");
    exec_sql("PRAGMA busy_timeout=3000");

    ensure_schema();

    // Start background writer thread (启动后台写入线程)
    writer_running_ = true;
    writer_thread_ = std::thread(&ConfigStore::writer_loop, this);

    LOG_INFO("ConfigStore: initialized " + db_path);
    return true;
}

void ConfigStore::shutdown() {
    if (!writer_running_) return;

    writer_running_ = false;
    queue_cv_.notify_all();

    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }

    // Flush residuals (flush 残留)
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        if (!write_queue_.empty()) {
            flush_tasks(write_queue_);
        }
    }

    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }

    LOG_INFO("ConfigStore: shutdown complete, total_writes=" +
             std::to_string(total_writes_.load()) +
             " emergency=" + std::to_string(emergency_writes_.load()));
}

// ============================================
// Schema
// ============================================

void ConfigStore::ensure_schema() {
    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS orders (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            client_order_id TEXT DEFAULT '',
            request_id      TEXT DEFAULT '',
            order_ref       TEXT NOT NULL,
            order_sys_id    TEXT DEFAULT '',
            account_id      TEXT NOT NULL,
            strategy_id     TEXT DEFAULT '',
            instrument_id   TEXT NOT NULL,
            exchange_id     TEXT DEFAULT '',
            direction       TEXT NOT NULL,
            offset_flag     TEXT NOT NULL,
            price_type      TEXT DEFAULT 'limit',
            price           REAL NOT NULL,
            total_volume    INTEGER NOT NULL,
            traded_volume   INTEGER DEFAULT 0,
            status          INTEGER DEFAULT 0,
            status_msg      TEXT DEFAULT '',
            source          TEXT DEFAULT 'manual',
            risk_checked    INTEGER DEFAULT 0,
            reject_reason   TEXT DEFAULT '',
            insert_time     TEXT DEFAULT '',
            update_time     TEXT DEFAULT '',
            trading_day     TEXT DEFAULT '',
            front_id        INTEGER DEFAULT 0,
            session_id      INTEGER DEFAULT 0,
            created_at      TEXT DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_orders_day ON orders(trading_day);
        CREATE INDEX IF NOT EXISTS idx_orders_ref ON orders(order_ref);
        CREATE INDEX IF NOT EXISTS idx_orders_inst ON orders(instrument_id);

        CREATE TABLE IF NOT EXISTS trades (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            trade_id        TEXT NOT NULL,
            order_ref       TEXT NOT NULL,
            account_id      TEXT NOT NULL,
            strategy_id     TEXT DEFAULT '',
            instrument_id   TEXT NOT NULL,
            exchange_id     TEXT DEFAULT '',
            direction       TEXT NOT NULL,
            offset_flag     TEXT NOT NULL,
            price           REAL NOT NULL,
            volume          INTEGER NOT NULL,
            commission      REAL DEFAULT 0,
            trade_time      TEXT DEFAULT '',
            trading_day     TEXT DEFAULT '',
            created_at      TEXT DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_trades_day ON trades(trading_day);
        CREATE INDEX IF NOT EXISTS idx_trades_ref ON trades(order_ref);

        CREATE TABLE IF NOT EXISTS audit_logs (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            action          TEXT NOT NULL,
            source          TEXT DEFAULT '',
            operator        TEXT DEFAULT 'user',
            detail          TEXT DEFAULT '',
            result          TEXT DEFAULT '',
            request_id      TEXT DEFAULT '',
            remote_ip       TEXT DEFAULT '',
            success         INTEGER DEFAULT 1,
            error_message   TEXT DEFAULT '',
            created_at      TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS risk_events (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            event_type      TEXT NOT NULL,
            scope           TEXT DEFAULT '',
            instrument_id   TEXT DEFAULT '',
            order_ref       TEXT DEFAULT '',
            reason          TEXT NOT NULL,
            detail_json     TEXT DEFAULT '{}',
            trading_day     TEXT DEFAULT '',
            created_at      TEXT DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_risk_day ON risk_events(trading_day);

        CREATE TABLE IF NOT EXISTS pnl_curve (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_ms    INTEGER NOT NULL,
            time_str        TEXT DEFAULT '',
            balance         REAL DEFAULT 0,
            available       REAL DEFAULT 0,
            margin_used     REAL DEFAULT 0,
            position_profit REAL DEFAULT 0,
            total_pnl       REAL DEFAULT 0,
            trading_day     TEXT DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_pnl_day ON pnl_curve(trading_day);

        CREATE TABLE IF NOT EXISTS accounts (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            account_id      TEXT NOT NULL UNIQUE,
            gateway_type    TEXT DEFAULT 'CTP',
            broker_id       TEXT DEFAULT '',
            user_id         TEXT DEFAULT '',
            password_enc    TEXT DEFAULT '',
            app_id          TEXT DEFAULT '',
            auth_code_enc   TEXT DEFAULT '',
            trade_front     TEXT DEFAULT '',
            market_front    TEXT DEFAULT '',
            is_market_data  INTEGER DEFAULT 0,
            enabled         INTEGER DEFAULT 1
        );

        CREATE TABLE IF NOT EXISTS config_kv (
            key             TEXT PRIMARY KEY,
            value           TEXT DEFAULT '',
            category        TEXT DEFAULT 'system'
        );

        CREATE TABLE IF NOT EXISTS daily_reports (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            trading_day     TEXT NOT NULL UNIQUE,
            report_json     TEXT NOT NULL,
            created_at      TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS strategies (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            strategy_id     TEXT NOT NULL UNIQUE,
            type            TEXT DEFAULT 'python',
            script_path     TEXT DEFAULT '',
            account_id      TEXT DEFAULT '',
            instruments     TEXT DEFAULT '',
            order_size      INTEGER DEFAULT 1,
            momentum_ticks  INTEGER DEFAULT 3,
            cooldown_seconds INTEGER DEFAULT 5,
            params_json     TEXT DEFAULT '{}',
            enabled         INTEGER DEFAULT 1,
            created_at      TEXT DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS instruments (
            id                      INTEGER PRIMARY KEY AUTOINCREMENT,
            instrument_id           TEXT NOT NULL UNIQUE,
            exchange_id             TEXT DEFAULT '',
            product_id              TEXT DEFAULT '',
            expire_date             TEXT DEFAULT '',
            start_deliv_date        TEXT DEFAULT '',
            end_deliv_date          TEXT DEFAULT '',
            inst_life_phase         TEXT DEFAULT '',
            is_trading              INTEGER DEFAULT 0,
            price_tick              REAL DEFAULT 0.2,
            volume_multiple         INTEGER DEFAULT 1,
            long_margin_ratio       REAL DEFAULT 0.12,
            short_margin_ratio      REAL DEFAULT 0.12,
            open_commission_rate    REAL DEFAULT 0.00002,
            close_commission_rate   REAL DEFAULT 0.00002,
            close_today_commission_rate REAL DEFAULT 0.00002,
            created_at              TEXT DEFAULT (datetime('now'))
        );
    )SQL";

    char* err = nullptr;
    if (sqlite3_exec(db_, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR("ConfigStore: schema error: " + std::string(err ? err : "unknown"));
        sqlite3_free(err);
    }

    auto add_column_if_missing = [this](const char* sql) {
        char* alter_err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &alter_err) != SQLITE_OK) {
            const std::string msg = alter_err ? alter_err : "";
            sqlite3_free(alter_err);
            if (msg.find("duplicate column name") != std::string::npos) {
                return;
            }
            LOG_ERROR("ConfigStore: schema migration error: " + msg);
        }
    };
    add_column_if_missing("ALTER TABLE instruments ADD COLUMN expire_date TEXT DEFAULT ''");
    add_column_if_missing("ALTER TABLE instruments ADD COLUMN start_deliv_date TEXT DEFAULT ''");
    add_column_if_missing("ALTER TABLE instruments ADD COLUMN end_deliv_date TEXT DEFAULT ''");
    add_column_if_missing("ALTER TABLE instruments ADD COLUMN inst_life_phase TEXT DEFAULT ''");
    add_column_if_missing("ALTER TABLE instruments ADD COLUMN is_trading INTEGER DEFAULT 0");
}

// ============================================
// Async write queue
// ============================================

void ConfigStore::enqueue(WriteTask task) {
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        if (write_queue_.size() < kMaxQueueSize) {
            write_queue_.push_back(std::move(task));
            queue_cv_.notify_one();
            return;
        }
    }
    emergency_writes_.fetch_add(1, std::memory_order_relaxed);
    struct EmergencyInfo { std::string key1, key2; };
    auto info = std::visit([](const auto& t) -> EmergencyInfo {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, InsertOrderTask>)
            return {t.order_ref, t.account_id};
        else if constexpr (std::is_same_v<T, UpdateOrderTask>)
            return {t.order_ref, t.account_id};
        else if constexpr (std::is_same_v<T, InsertTradeTask>)
            return {t.trade_id, t.account_id};
        else if constexpr (std::is_same_v<T, LogAuditTask>)
            return {t.action, t.source};
        else if constexpr (std::is_same_v<T, LogRiskEventTask>)
            return {t.event_type, t.scope};
        else
            return {t.time_str, t.trading_day};
    }, task);
    write_emergency("QUEUE_FULL type=" + std::to_string(task.index()) +
                    " k1=" + info.key1 + " k2=" + info.key2);
}

void ConfigStore::write_emergency(const std::string& line) {
    const std::string path = db_path_ + ".emergency.journal";
    std::ofstream ofs(path, std::ios::app);
    if (ofs.is_open()) {
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        ofs << ms << " " << line << "\n";
    }
}

void ConfigStore::writer_loop() {
    std::deque<WriteTask> batch;
    while (writer_running_) {
        {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return !write_queue_.empty() || !writer_running_;
            });
            if (write_queue_.empty()) continue;
            batch.swap(write_queue_);
        }
        flush_tasks(batch);
        batch.clear();
    }
    // drain remaining
    std::lock_guard<std::mutex> lock(queue_mtx_);
    if (!write_queue_.empty()) {
        flush_tasks(write_queue_);
        write_queue_.clear();
    }
}

void ConfigStore::flush_tasks(std::deque<WriteTask>& tasks) {
    if (!db_ || tasks.empty()) return;

    std::lock_guard<std::mutex> lock(read_mtx_);
    exec_sql("BEGIN TRANSACTION");
    for (const auto& task : tasks) {
        execute_task(task);
        total_writes_.fetch_add(1, std::memory_order_relaxed);
    }
    exec_sql("COMMIT");
}

void ConfigStore::execute_task(const WriteTask& task) {
    std::visit([this](const auto& t) {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, InsertOrderTask>)   exec_insert_order(t);
        else if constexpr (std::is_same_v<T, UpdateOrderTask>)  exec_update_order(t);
        else if constexpr (std::is_same_v<T, InsertTradeTask>)  exec_insert_trade(t);
        else if constexpr (std::is_same_v<T, LogAuditTask>)     exec_log_audit(t);
        else if constexpr (std::is_same_v<T, LogRiskEventTask>) exec_log_risk_event(t);
        else if constexpr (std::is_same_v<T, InsertPnlTask>)    exec_insert_pnl(t);
    }, task);
}

size_t ConfigStore::pending_writes() const {
    std::lock_guard<std::mutex> lock(queue_mtx_);
    return write_queue_.size();
}

// ============================================
// Async API implementations
// ============================================

void ConfigStore::async_insert_order(const OrderInfo& o, const std::string& source) {
    InsertOrderTask t;
    t.order_ref = o.order_ref;
    t.account_id = o.account_id;
    t.strategy_id = o.strategy_id;
    t.instrument_id = o.instrument_id;
    t.exchange_id = o.exchange_id;
    t.direction = (o.direction == Direction::Buy) ? "buy" : "sell";
    switch (o.offset) {
        case Offset::Open: t.offset_flag = "open"; break;
        case Offset::Close: t.offset_flag = "close"; break;
        case Offset::CloseToday: t.offset_flag = "close_today"; break;
        case Offset::CloseYesterday: t.offset_flag = "close_yesterday"; break;
    }
    t.source = source;
    t.insert_time = non_empty_time_or_now(o.insert_time);
    t.price = o.price;
    t.total_volume = o.total_volume;
    t.status = static_cast<int>(o.status);
    enqueue(std::move(t));
}

void ConfigStore::async_update_order(const OrderInfo& order) {
    UpdateOrderTask t;
    t.order_ref = order.order_ref;
    t.status_msg = order.status_msg;
    t.account_id = order.account_id;
    t.instrument_id = order.instrument_id;
    t.exchange_id = order.exchange_id;
    t.order_sys_id = order.order_sys_id;
    t.insert_time = non_empty_time_or_now(order.insert_time);
    t.status = static_cast<int>(order.status);
    t.traded_volume = order.traded_volume;
    t.front_id = order.front_id;
    t.session_id = order.session_id;
    enqueue(std::move(t));
}

void ConfigStore::async_insert_trade(const TradeInfo& trade) {
    InsertTradeTask t;
    t.trade_id = trade.trade_id;
    t.order_ref = trade.order_ref;
    t.account_id = trade.account_id;
    t.strategy_id = trade.strategy_id;
    t.instrument_id = trade.instrument_id;
    t.exchange_id = trade.exchange_id;
    t.direction = (trade.direction == Direction::Buy) ? "buy" : "sell";
    switch (trade.offset) {
        case Offset::Open: t.offset_flag = "open"; break;
        case Offset::Close: t.offset_flag = "close"; break;
        case Offset::CloseToday: t.offset_flag = "close_today"; break;
        case Offset::CloseYesterday: t.offset_flag = "close_yesterday"; break;
    }
    t.trade_time = non_empty_time_or_now(trade.trade_time);
    t.price = trade.price;
    t.volume = trade.volume;
    enqueue(std::move(t));
}

void ConfigStore::async_log_audit(const std::string& action, const std::string& source,
                                  const std::string& detail, const std::string& result,
                                  const std::string& remote_ip) {
    LogAuditTask t;
    t.action = action;
    t.source = source;
    t.detail = detail;
    t.result = result;
    t.remote_ip = remote_ip;
    enqueue(std::move(t));
}

void ConfigStore::async_log_risk_event(const std::string& event_type, const std::string& scope,
                                       const std::string& instrument, const std::string& reason,
                                       const std::string& detail_json) {
    LogRiskEventTask t;
    t.event_type = event_type;
    t.scope = scope;
    t.instrument = instrument;
    t.reason = reason;
    t.detail_json = detail_json;
    enqueue(std::move(t));
}

void ConfigStore::async_insert_pnl(int64_t timestamp_ms, const std::string& time_str,
                                   double balance, double available, double margin,
                                   double position_profit, double total_pnl,
                                   const std::string& trading_day) {
    InsertPnlTask t;
    t.timestamp_ms = timestamp_ms;
    t.time_str = time_str;
    t.trading_day = trading_day;
    t.balance = balance;
    t.available = available;
    t.margin = margin;
    t.position_profit = position_profit;
    t.total_pnl = total_pnl;
    enqueue(std::move(t));
}

// ============================================
// SQL execution helpers
// ============================================

bool ConfigStore::exec_sql(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        LOG_ERROR("ConfigStore SQL error: " + std::string(err ? err : "unknown") +
                  " sql: " + std::string(sql).substr(0, 200));
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool ConfigStore::exec_sql(const std::string& sql) {
    return exec_sql(sql.c_str());
}

// ============================================
// Task executors (called from writer thread)
// ============================================

void ConfigStore::exec_insert_order(const InsertOrderTask& t) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO orders (order_ref, account_id, strategy_id, instrument_id, "
                      "exchange_id, direction, offset_flag, price, total_volume, status, source, "
                      "front_id, session_id, insert_time) "
                      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, t.order_ref.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, t.account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, t.strategy_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, t.instrument_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, t.exchange_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, t.direction.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, t.offset_flag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 8, t.price);
    sqlite3_bind_int(stmt, 9, t.total_volume);
    sqlite3_bind_int(stmt, 10, t.status);
    sqlite3_bind_text(stmt, 11, t.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 12, 0);
    sqlite3_bind_int(stmt, 13, 0);
    sqlite3_bind_text(stmt, 14, t.insert_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void ConfigStore::exec_update_order(const UpdateOrderTask& t) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE orders SET status=?, traded_volume=?, status_msg=?, exchange_id=?, "
        "order_sys_id=?, front_id=?, session_id=?, insert_time=CASE WHEN insert_time='' THEN ? ELSE insert_time END, "
        "update_time=datetime('now') "
        "WHERE id=(SELECT id FROM orders WHERE order_ref=? AND account_id=? "
        "AND instrument_id=? ORDER BY id DESC LIMIT 1)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, t.status);
    sqlite3_bind_int(stmt, 2, t.traded_volume);
    sqlite3_bind_text(stmt, 3, t.status_msg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, t.exchange_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, t.order_sys_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, t.front_id);
    sqlite3_bind_int(stmt, 7, t.session_id);
    sqlite3_bind_text(stmt, 8, t.insert_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, t.order_ref.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, t.account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, t.instrument_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void ConfigStore::exec_insert_trade(const InsertTradeTask& t) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO trades (trade_id, order_ref, account_id, strategy_id, "
                      "instrument_id, exchange_id, direction, offset_flag, price, volume, trade_time) "
                      "VALUES (?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, t.trade_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, t.order_ref.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, t.account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, t.strategy_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, t.instrument_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, t.exchange_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, t.direction.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, t.offset_flag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 9, t.price);
    sqlite3_bind_int(stmt, 10, t.volume);
    sqlite3_bind_text(stmt, 11, t.trade_time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void ConfigStore::exec_log_audit(const LogAuditTask& t) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO audit_logs (action, source, detail, result, remote_ip) "
                      "VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, t.action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, t.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, t.detail.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, t.result.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, t.remote_ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void ConfigStore::exec_log_risk_event(const LogRiskEventTask& t) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO risk_events (event_type, scope, instrument_id, reason, detail_json) "
                      "VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, t.event_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, t.scope.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, t.instrument.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, t.reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, t.detail_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void ConfigStore::exec_insert_pnl(const InsertPnlTask& t) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO pnl_curve (timestamp_ms, time_str, balance, available, "
                      "margin_used, position_profit, total_pnl, trading_day) "
                      "VALUES (?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, t.timestamp_ms);
    sqlite3_bind_text(stmt, 2, t.time_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, t.balance);
    sqlite3_bind_double(stmt, 4, t.available);
    sqlite3_bind_double(stmt, 5, t.margin);
    sqlite3_bind_double(stmt, 6, t.position_profit);
    sqlite3_bind_double(stmt, 7, t.total_pnl);
    sqlite3_bind_text(stmt, 8, t.trading_day.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// ============================================
// Sync query implementations
// ============================================

std::vector<OrderInfo> ConfigStore::query_orders(const std::string& trading_day,
                                                  size_t limit) const {
    std::vector<OrderInfo> result;
    if (!db_) return result;

    std::lock_guard<std::mutex> lock(read_mtx_);
    std::string sql = "SELECT order_ref, account_id, strategy_id, instrument_id, exchange_id, "
                      "direction, offset_flag, price, total_volume, traded_volume, status, "
                      "status_msg, insert_time, order_sys_id, front_id, session_id FROM orders";
    if (!trading_day.empty()) {
        sql += " WHERE trading_day='" + trading_day + "'";
    }
    sql += " ORDER BY id DESC LIMIT " + std::to_string(limit);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OrderInfo o{};
        const auto col_text = [&](int col) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return p ? p : "";
        };
        std::strncpy(o.order_ref, col_text(0).c_str(), sizeof(o.order_ref) - 1);
        std::strncpy(o.account_id, col_text(1).c_str(), sizeof(o.account_id) - 1);
        std::strncpy(o.strategy_id, col_text(2).c_str(), sizeof(o.strategy_id) - 1);
        std::strncpy(o.instrument_id, col_text(3).c_str(), sizeof(o.instrument_id) - 1);
        std::strncpy(o.exchange_id, col_text(4).c_str(), sizeof(o.exchange_id) - 1);
        o.direction = (col_text(5) == "buy") ? Direction::Buy : Direction::Sell;
        const auto off = col_text(6);
        if (off == "close") o.offset = Offset::Close;
        else if (off == "close_today") o.offset = Offset::CloseToday;
        else if (off == "close_yesterday") o.offset = Offset::CloseYesterday;
        else o.offset = Offset::Open;
        o.price = sqlite3_column_double(stmt, 7);
        o.total_volume = sqlite3_column_int(stmt, 8);
        o.traded_volume = sqlite3_column_int(stmt, 9);
        o.status = static_cast<OrderStatus>(sqlite3_column_int(stmt, 10));
        std::strncpy(o.status_msg, col_text(11).c_str(), sizeof(o.status_msg) - 1);
        std::strncpy(o.insert_time, col_text(12).c_str(), sizeof(o.insert_time) - 1);
        std::strncpy(o.order_sys_id, col_text(13).c_str(), sizeof(o.order_sys_id) - 1);
        o.front_id = sqlite3_column_int(stmt, 14);
        o.session_id = sqlite3_column_int(stmt, 15);
        result.push_back(o);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<TradeInfo> ConfigStore::query_trades(const std::string& trading_day,
                                                  size_t limit) const {
    std::vector<TradeInfo> result;
    if (!db_) return result;

    std::lock_guard<std::mutex> lock(read_mtx_);
    std::string sql = "SELECT trade_id, order_ref, account_id, strategy_id, instrument_id, "
                      "exchange_id, direction, offset_flag, price, volume, trade_time FROM trades";
    if (!trading_day.empty()) {
        sql += " WHERE trading_day='" + trading_day + "'";
    }
    sql += " ORDER BY id DESC LIMIT " + std::to_string(limit);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TradeInfo t{};
        const auto col_text = [&](int col) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return p ? p : "";
        };
        std::strncpy(t.trade_id, col_text(0).c_str(), sizeof(t.trade_id) - 1);
        std::strncpy(t.order_ref, col_text(1).c_str(), sizeof(t.order_ref) - 1);
        std::strncpy(t.account_id, col_text(2).c_str(), sizeof(t.account_id) - 1);
        std::strncpy(t.strategy_id, col_text(3).c_str(), sizeof(t.strategy_id) - 1);
        std::strncpy(t.instrument_id, col_text(4).c_str(), sizeof(t.instrument_id) - 1);
        std::strncpy(t.exchange_id, col_text(5).c_str(), sizeof(t.exchange_id) - 1);
        t.direction = (col_text(6) == "buy") ? Direction::Buy : Direction::Sell;
        const auto off = col_text(7);
        if (off == "close") t.offset = Offset::Close;
        else if (off == "close_today") t.offset = Offset::CloseToday;
        else if (off == "close_yesterday") t.offset = Offset::CloseYesterday;
        else t.offset = Offset::Open;
        t.price = sqlite3_column_double(stmt, 8);
        t.volume = sqlite3_column_int(stmt, 9);
        std::strncpy(t.trade_time, col_text(10).c_str(), sizeof(t.trade_time) - 1);
        result.push_back(t);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<std::string> ConfigStore::query_recent_audits(size_t limit) const {
    std::vector<std::string> result;
    if (!db_) return result;

    std::lock_guard<std::mutex> lock(read_mtx_);
    const std::string sql = "SELECT action, source, detail, result, created_at "
                            "FROM audit_logs ORDER BY id DESC LIMIT " + std::to_string(limit);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto col = [&](int c) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
            return p ? p : "";
        };
        result.push_back(col(0) + " source=" + col(1) + " " + col(2) +
                         " result=" + col(3) + " at=" + col(4));
    }
    sqlite3_finalize(stmt);
    return result;
}

// ============================================
std::vector<std::string> ConfigStore::query_risk_events(const std::string& trading_day,
                                                         size_t limit) const {
    std::vector<std::string> result;
    if (!db_) return result;

    std::lock_guard<std::mutex> lock(read_mtx_);
    std::string sql = "SELECT event_type, scope, instrument_id, reason, detail_json, created_at "
                      "FROM risk_events";
    if (!trading_day.empty()) {
        sql += " WHERE trading_day='" + trading_day + "'";
    }
    sql += " ORDER BY id DESC LIMIT " + std::to_string(limit);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto col = [&](int c) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
            return p ? p : "";
        };
        // JSON format output (JSON 格式输出)
        std::string entry = "{\"type\":\"" + col(0) + "\",\"scope\":\"" + col(1) +
                            "\",\"instrument\":\"" + col(2) + "\",\"reason\":\"" + col(3) +
                            "\",\"detail\":" + col(4) + ",\"time\":\"" + col(5) + "\"}";
        result.push_back(std::move(entry));
    }
    sqlite3_finalize(stmt);
    return result;
}

// Config CRUD implementations (配置 CRUD 实现)
// ============================================

bool ConfigStore::save_account_bundle(const AccountConfigBundle& bundle) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lock(read_mtx_);

    exec_sql("BEGIN TRANSACTION");

    // Clear old data (清除旧数据)
    exec_sql("DELETE FROM accounts");

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO accounts (account_id, gateway_type, broker_id, user_id, "
                      "password_enc, app_id, auth_code_enc, trade_front, market_front, is_market_data) "
                      "VALUES (?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        exec_sql("ROLLBACK");
        return false;
    }

    for (const auto& acct : bundle.accounts) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, acct.account_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, acct.gateway_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, acct.broker_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, acct.user_id.c_str(), -1, SQLITE_TRANSIENT);
        // Password encrypted storage (密码加密存储)
        const std::string pwd_enc = acct.password.empty() ? "" : crypto::encrypt(acct.password);
        sqlite3_bind_text(stmt, 5, pwd_enc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, acct.app_id.c_str(), -1, SQLITE_TRANSIENT);
        const std::string auth_enc = acct.auth_code.empty() ? "" : crypto::encrypt(acct.auth_code);
        sqlite3_bind_text(stmt, 7, auth_enc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, acct.trade_front.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, acct.market_front.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 10, acct.account_id == bundle.market_data_account_id ? 1 : 0);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);

    exec_sql("COMMIT");
    return true;
}

AccountConfigBundle ConfigStore::load_account_bundle() const {
    AccountConfigBundle bundle;
    if (!db_) return bundle;
    std::lock_guard<std::mutex> lock(read_mtx_);

    const char* sql = "SELECT account_id, gateway_type, broker_id, user_id, password_enc, "
                      "app_id, auth_code_enc, trade_front, market_front, is_market_data FROM accounts";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return bundle;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto col = [&](int c) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
            return p ? p : "";
        };
        AccountConfigSpec acct;
        acct.account_id = col(0);
        acct.gateway_type = col(1);
        acct.broker_id = col(2);
        acct.user_id = col(3);
        // Password decryption (密码解密)
        const std::string pwd_enc = col(4);
        acct.password = pwd_enc.empty() ? "" : crypto::decrypt(pwd_enc);
        acct.app_id = col(5);
        const std::string auth_enc = col(6);
        acct.auth_code = auth_enc.empty() ? "" : crypto::decrypt(auth_enc);
        acct.trade_front = col(7);
        acct.market_front = col(8);
        if (sqlite3_column_int(stmt, 9)) {
            bundle.market_data_account_id = acct.account_id;
        }
        bundle.accounts.push_back(std::move(acct));
    }
    sqlite3_finalize(stmt);
    return bundle;
}

// ============================================
// Strategy CRUD (策略 CRUD)
// ============================================

bool ConfigStore::save_strategy_specs(const std::vector<StrategyConfigSpec>& specs) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lock(read_mtx_);

    exec_sql("BEGIN TRANSACTION");
    exec_sql("DELETE FROM strategies");

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO strategies (strategy_id, type, script_path, account_id, "
                      "instruments, order_size, momentum_ticks, cooldown_seconds, params_json) "
                      "VALUES (?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        exec_sql("ROLLBACK");
        return false;
    }

    for (const auto& spec : specs) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, spec.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, spec.type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, spec.script_path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, spec.account_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, join_csv(spec.instruments).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 6, spec.order_size);
        sqlite3_bind_int(stmt, 7, spec.momentum_ticks);
        sqlite3_bind_int(stmt, 8, spec.cooldown_seconds);
        // Serialize params map to JSON (将参数 map 序列化为 JSON)
        std::string params_json = "{";
        bool first = true;
        for (const auto& [k, v] : spec.params) {
            if (!first) params_json += ",";
            first = false;
            params_json += "\"" + k + "\":\"" + v + "\"";
        }
        params_json += "}";
        sqlite3_bind_text(stmt, 9, params_json.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);

    exec_sql("COMMIT");
    return true;
}

std::vector<StrategyConfigSpec> ConfigStore::load_strategy_specs() const {
    std::vector<StrategyConfigSpec> specs;
    if (!db_) return specs;
    std::lock_guard<std::mutex> lock(read_mtx_);

    const char* sql = "SELECT strategy_id, type, script_path, account_id, instruments, "
                      "order_size, momentum_ticks, cooldown_seconds, params_json "
                      "FROM strategies WHERE enabled=1 ORDER BY id";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return specs;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto col = [&](int c) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
            return p ? p : "";
        };
        StrategyConfigSpec spec;
        spec.id = col(0);
        spec.type = col(1);
        spec.script_path = col(2);
        spec.account_id = col(3);
        spec.instruments = split_csv_trimmed(col(4));
        spec.order_size = sqlite3_column_int(stmt, 5);
        spec.momentum_ticks = sqlite3_column_int(stmt, 6);
        spec.cooldown_seconds = sqlite3_column_int(stmt, 7);
        // Parse params_json (simple key-value) (解析 params_json，简单键值解析)
        const std::string pj = col(8);
        if (pj.size() > 2) {
            // Simple JSON parse: {"k":"v","k2":"v2"} (简单 JSON 解析)
            std::string key, val;
            bool in_key = false, in_val = false;
            for (size_t i = 0; i < pj.size(); ++i) {
                char c = pj[i];
                if (c == '"') {
                    if (!in_key && !in_val) { in_key = true; key.clear(); }
                    else if (in_key) { in_key = false; }
                    else if (!in_val) { in_val = true; val.clear(); }
                    else { in_val = false; spec.params[key] = val; }
                } else if (in_key) { key += c; }
                else if (in_val) { val += c; }
            }
        }
        specs.push_back(std::move(spec));
    }
    sqlite3_finalize(stmt);
    return specs;
}

bool ConfigStore::delete_strategy(const std::string& strategy_id) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lock(read_mtx_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM strategies WHERE strategy_id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, strategy_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return true;
}

// ============================================
// Instrument CRUD (合约 CRUD)
// ============================================

bool ConfigStore::save_instrument_spec(const InstrumentSpec& spec) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lock(read_mtx_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO instruments "
                      "(instrument_id, exchange_id, product_id, price_tick, volume_multiple, "
                      "expire_date, start_deliv_date, end_deliv_date, inst_life_phase, is_trading, "
                      "long_margin_ratio, short_margin_ratio, open_commission_rate, "
                      "close_commission_rate, close_today_commission_rate) "
                      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, spec.instrument_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, spec.exchange_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, spec.product_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 4, spec.price_tick);
    sqlite3_bind_int(stmt, 5, spec.volume_multiple);
    sqlite3_bind_text(stmt, 6, spec.expire_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, spec.start_deliv_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, spec.end_deliv_date.c_str(), -1, SQLITE_TRANSIENT);
    const std::string life_phase = spec.inst_life_phase ? std::string(1, spec.inst_life_phase) : "";
    sqlite3_bind_text(stmt, 9, life_phase.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 10, spec.is_trading ? 1 : 0);
    sqlite3_bind_double(stmt, 11, spec.long_margin_ratio);
    sqlite3_bind_double(stmt, 12, spec.short_margin_ratio);
    sqlite3_bind_double(stmt, 13, spec.open_commission_rate);
    sqlite3_bind_double(stmt, 14, spec.close_commission_rate);
    sqlite3_bind_double(stmt, 15, spec.close_today_commission_rate);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return true;
}

std::vector<InstrumentSpec> ConfigStore::load_instrument_specs() const {
    std::vector<InstrumentSpec> specs;
    if (!db_) return specs;
    std::lock_guard<std::mutex> lock(read_mtx_);

    const char* sql = "SELECT instrument_id, exchange_id, product_id, price_tick, volume_multiple, "
                      "expire_date, start_deliv_date, end_deliv_date, inst_life_phase, is_trading, "
                      "long_margin_ratio, short_margin_ratio, open_commission_rate, "
                      "close_commission_rate, close_today_commission_rate FROM instruments";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return specs;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto col = [&](int c) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
            return p ? p : "";
        };
        InstrumentSpec spec;
        spec.instrument_id = col(0);
        spec.exchange_id = col(1);
        spec.product_id = col(2);
        spec.price_tick = sqlite3_column_double(stmt, 3);
        spec.volume_multiple = sqlite3_column_int(stmt, 4);
        spec.expire_date = col(5);
        spec.start_deliv_date = col(6);
        spec.end_deliv_date = col(7);
        const std::string life_phase = col(8);
        spec.inst_life_phase = life_phase.empty() ? '\0' : life_phase[0];
        spec.is_trading = sqlite3_column_int(stmt, 9) != 0;
        spec.long_margin_ratio = sqlite3_column_double(stmt, 10);
        spec.short_margin_ratio = sqlite3_column_double(stmt, 11);
        spec.open_commission_rate = sqlite3_column_double(stmt, 12);
        spec.close_commission_rate = sqlite3_column_double(stmt, 13);
        spec.close_today_commission_rate = sqlite3_column_double(stmt, 14);
        specs.push_back(std::move(spec));
    }
    sqlite3_finalize(stmt);
    return specs;
}

InstrumentSpec ConfigStore::load_instrument_spec(const std::string& instrument_id) const {
    InstrumentSpec spec;
    spec.instrument_id = instrument_id;
    if (!db_) return spec;
    std::lock_guard<std::mutex> lock(read_mtx_);

    const char* sql = "SELECT exchange_id, product_id, price_tick, volume_multiple, "
                      "expire_date, start_deliv_date, end_deliv_date, inst_life_phase, is_trading, "
                      "long_margin_ratio, short_margin_ratio, open_commission_rate, "
                      "close_commission_rate, close_today_commission_rate "
                      "FROM instruments WHERE instrument_id=?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return spec;
    sqlite3_bind_text(stmt, 1, instrument_id.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto col = [&](int c) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
            return p ? p : "";
        };
        spec.exchange_id = col(0);
        spec.product_id = col(1);
        spec.price_tick = sqlite3_column_double(stmt, 2);
        spec.volume_multiple = sqlite3_column_int(stmt, 3);
        spec.expire_date = col(4);
        spec.start_deliv_date = col(5);
        spec.end_deliv_date = col(6);
        const std::string life_phase = col(7);
        spec.inst_life_phase = life_phase.empty() ? '\0' : life_phase[0];
        spec.is_trading = sqlite3_column_int(stmt, 8) != 0;
        spec.long_margin_ratio = sqlite3_column_double(stmt, 9);
        spec.short_margin_ratio = sqlite3_column_double(stmt, 10);
        spec.open_commission_rate = sqlite3_column_double(stmt, 11);
        spec.close_commission_rate = sqlite3_column_double(stmt, 12);
        spec.close_today_commission_rate = sqlite3_column_double(stmt, 13);
    }
    sqlite3_finalize(stmt);
    return spec;
}

bool ConfigStore::has_instrument_spec(const std::string& instrument_id) const {
    if (!db_) return false;
    std::lock_guard<std::mutex> lock(read_mtx_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1 FROM instruments WHERE instrument_id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, instrument_id.c_str(), -1, SQLITE_TRANSIENT);
    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

bool ConfigStore::save_risk_config(const std::string& key, const std::string& value) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lock(read_mtx_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO config_kv (key, value, category) VALUES (?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, "risk", -1, SQLITE_STATIC);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::string ConfigStore::load_risk_config(const std::string& key, const std::string& default_val) const {
    if (!db_) return default_val;
    std::lock_guard<std::mutex> lock(read_mtx_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT value FROM config_kv WHERE key=? AND category='risk'";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return default_val;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string result = default_val;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (p) result = p;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<std::pair<std::string, std::string>> ConfigStore::load_all_risk_config() const {
    std::vector<std::pair<std::string, std::string>> result;
    if (!db_) return result;
    std::lock_guard<std::mutex> lock(read_mtx_);
    const char* sql = "SELECT key, value FROM config_kv WHERE category='risk'";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto col = [&](int c) -> std::string {
            const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, c));
            return p ? p : "";
        };
        result.emplace_back(col(0), col(1));
    }
    sqlite3_finalize(stmt);
    return result;
}

bool ConfigStore::save_system_config(const std::string& key, const std::string& value) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lock(read_mtx_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO config_kv (key, value, category) VALUES (?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, "system", -1, SQLITE_STATIC);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::string ConfigStore::load_system_config(const std::string& key, const std::string& default_val) const {
    if (!db_) return default_val;
    std::lock_guard<std::mutex> lock(read_mtx_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT value FROM config_kv WHERE key=? AND category='system'";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return default_val;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string result = default_val;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (p) result = p;
    }
    sqlite3_finalize(stmt);
    return result;
}

bool ConfigStore::save_encrypted_system_config(const std::string& key, const std::string& value) {
    if (value.empty()) {
        return save_system_config(key + "_enc", "") && save_system_config(key, "");
    }
    const std::string encrypted = crypto::encrypt(value);
    if (encrypted.empty()) {
        return false;
    }
    return save_system_config(key + "_enc", encrypted) && save_system_config(key, "");
}

std::string ConfigStore::load_encrypted_system_config(const std::string& key, const std::string& default_val) const {
    const std::string encrypted = load_system_config(key + "_enc", "");
    if (!encrypted.empty()) {
        const std::string decrypted = crypto::decrypt(encrypted);
        return decrypted.empty() ? default_val : decrypted;
    }
    const std::string legacy_plaintext = load_system_config(key, "");
    return legacy_plaintext.empty() ? default_val : legacy_plaintext;
}

bool ConfigStore::has_migrated_config() const {
    return load_system_config("config_migrated") == "1";
}

void ConfigStore::mark_migrated() {
    save_system_config("config_migrated", "1");
}

// ============================================
// Daily report implementations (每日报告实现)
// ============================================

bool ConfigStore::save_daily_report(const std::string& trading_day, const std::string& report_json) {
    if (!db_) return false;
    std::lock_guard<std::mutex> lock(read_mtx_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT OR REPLACE INTO daily_reports (trading_day, report_json) VALUES (?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, trading_day.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, report_json.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::string ConfigStore::load_daily_report(const std::string& trading_day) const {
    if (!db_) return "";
    std::lock_guard<std::mutex> lock(read_mtx_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT report_json FROM daily_reports WHERE trading_day=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_text(stmt, 1, trading_day.c_str(), -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (p) result = p;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<std::string> ConfigStore::list_daily_reports(size_t limit) const {
    std::vector<std::string> result;
    if (!db_) return result;
    std::lock_guard<std::mutex> lock(read_mtx_);
    const std::string sql = "SELECT trading_day, report_json FROM daily_reports ORDER BY trading_day DESC LIMIT " + std::to_string(limit);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (p) result.push_back(p);
    }
    sqlite3_finalize(stmt);
    return result;
}

} // namespace hft
