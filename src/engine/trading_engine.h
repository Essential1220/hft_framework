#pragma once
// ============================================
// trading_engine.h - Trading engine orchestration (交易引擎编排)
// Core engine that coordinates market data, order management, position tracking,
// risk control, and strategy execution across multiple accounts.
// (协调行情、委托管理、持仓追踪、风控和多账户策略执行的核心引擎)
// ============================================

#include "common/config.h"
#include "common/config_store.h"
#include "common/event.h"
#include "common/latency.h"
#include "common/spsc_queue.h"
#include "common/types.h"
#include "engine/account_manager.h"
#include "engine/i_trading_context.h"
#include "engine/instrument_registry.h"
#include "engine/kline_manager.h"
#include "engine/session_manager.h"
#include "engine/strategy_controller.h"
#include "engine/tick_data_manager.h"
#include "engine/tick_recorder.h"
#include "market/order_book_manager.h"
#include "gateway/i_md_gateway.h"
#include "order/close_manager.h"
#include "order/conditional_order_manager.h"
#include "order/algo_order_manager.h"
#include "engine/paper_trading.h"

#ifdef ENABLE_METRICS
#include "webui/web_server.h"
#endif

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hft {

class StrategyBase;
class ThreadRegistry;

struct AccountMonitorSnapshot {
    std::string account_id;
    AccountInfo account{};
    size_t position_count = 0;
    size_t active_order_count = 0;
    bool trade_gateway_logged_in = false;
    bool snapshot_ready = false;
    bool reconnect_sync_pending = false;
    std::string trade_state;
    std::string last_reject_reason;
    std::string last_reject_message;
};

struct StrategyMonitorSnapshot {
    std::string strategy_id;
    std::string strategy_type;
    std::string script_path;
    std::string version;
    std::string default_account_id;
    std::vector<std::string> watched_instruments;
    std::map<std::string, std::string> parameters;
    size_t position_count = 0;
    size_t active_order_count = 0;
    size_t conditional_order_count = 0;
    size_t close_task_count = 0;
    size_t signal_count = 0;
    size_t trade_count = 0;
    int position_volume = 0;
    double avg_price = 0.0;
    double realized_pnl = 0.0;
    double floating_pnl = 0.0;
    double total_pnl = 0.0;
    double win_rate = 0.0;
    double profit_factor = 0.0;
    std::string open_time;
    std::string add_time;
    std::string last_signal;
    std::string last_signal_time;
    bool matches_all_accounts = false;
    bool matches_all_instruments = false;
    bool account_exists = true;
    bool script_exists = true;
    std::string status;  // "running" | "paused" | "stopped"
};

struct LatencySnapshot {
    long long tick_to_signal_us = -1;
    long long signal_to_order_us = -1;
    long long order_to_trade_us = -1;
    long long tick_process_us = -1;
    long long order_process_us = -1;
    long long trade_process_us = -1;
};

struct PnlCurvePoint {
    int64_t timestamp_ms = 0;
    std::string time;
    double balance = 0.0;
    double available = 0.0;
    double margin = 0.0;
    double position_profit = 0.0;
    double total_pnl = 0.0;
};

struct InitStatus {
    bool config_exists = false;
    bool runtime_state_exists = false;
    bool kline_store_exists = false;
    bool tick_record_file_exists = false;
    bool config_loaded = false;
    std::string config_path;
    std::string runtime_state_path;
    std::string kline_store_path;
    std::string tick_recording_path;
};

class TradingEngine : public ITradingContext {
public:
    TradingEngine();
    ~TradingEngine();

    bool init(const std::string& config_path);
    bool start();
    void stop();

    bool add_strategy(std::shared_ptr<StrategyBase> strategy);
    bool remove_strategy(const std::string& strategy_id);
    size_t strategy_count() const;

    void on_tick(const TickData& tick);
    void on_order(const OrderInfo& order);
    void on_trade(const TradeInfo& trade);
    void on_account(const AccountInfo& account);
    void on_position(const PositionInfo& pos);
    void on_gateway_error(const std::string& account_id, OrderRejectReason reason,
                          const std::string& message);
    void on_cancel_rejected(const std::string& account_id, const std::string& order_ref,
                            const std::string& reason);

    void send_order(const OrderRequest& req) override;
    void send_order(const OrderRequest& req, std::string& out_order_ref);
    std::string send_order_with_ref(const OrderRequest& req) override;
    SendOrderResult send_order_with_result(const OrderRequest& req);
    void cancel_order(const std::string& order_ref) override;
    bool cancel_order(const std::string& order_ref, const std::string& account_id) override;
    size_t cancel_all_orders(const std::string& account_id = "");
    size_t cancel_strategy_orders(const std::string& strategy_id);

    void add_conditional_order_async(const ConditionalOrder& order);
    void cancel_conditional_order_async(uint32_t id);
    bool update_monitoring_config_async(int no_tick_warn_seconds, const std::string& trading_sessions);

    uint32_t add_conditional_order(const ConditionalOrder& order) override;
    void cancel_conditional_order(uint32_t id) override;
    uint32_t allocate_cond_group_id() override;

    void pause_strategy();
    void stop_strategy();
    void emergency_close_all();
    void resume_strategy();
    StrategyState get_strategy_state() const;
    bool set_strategy_state(const std::string& strategy_id, StrategyState state);
    StrategyState get_strategy_state(const std::string& strategy_id) const;

    uint32_t last_added_cond_order_id() const { return last_cond_id_.load(std::memory_order_relaxed); }

    AccountInfo get_account(const std::string& account_id = "") const;
    std::vector<PositionInfo> get_all_positions(const std::string& account_id = "") const;
    std::vector<OrderInfo> get_active_orders(const std::string& account_id = "") const;
    std::vector<TradeInfo> get_recent_trades(const std::string& account_id = "", size_t limit = 200) const;
    TickData get_last_tick(const char* instrument) const;
    std::unordered_map<std::string, TickData> get_all_ticks() const;
    std::unordered_map<std::string, TickData> get_ticks_filtered(const std::vector<std::string>& instruments = {},
                                                       size_t limit = 0) const;
    std::vector<TickData> get_ticks_changed_since(long long since_update_seq,
                                                  size_t limit,
                                                  long long* latest_update_seq = nullptr) const;
    std::vector<TickData> get_subscribed_ticks() const;
    std::vector<KlineBar> get_kline(const std::string& instrument,
                                    const std::string& period,
                                    size_t limit = 200) const;
    std::vector<std::string> get_kline_periods(const std::string& instrument) const;
    std::vector<KlineCatalogItem> get_kline_catalog(const std::string& instrument = "",
                                                    const std::string& period = "") const;
    bool import_kline_csv(const std::string& instrument,
                          const std::string& period,
                          const std::string& csv_path,
                          bool replace_existing,
                          size_t* imported_count = nullptr,
                          std::string* error = nullptr);
    std::vector<std::string> get_market_universe() const;
    std::vector<InstrumentSpec> get_instrument_specs(const std::string& instrument = "") const;
    bool has_instrument(const std::string& instrument) const;
    void update_instrument_spec(const InstrumentSpec& spec);
    bool refresh_instrument_rates(const std::string& instrument, const std::string& account_id = "", std::string* error = nullptr);
    void on_instrument_spec_update(const InstrumentSpec& spec);
    bool register_hot_instrument(const std::string& instrument);
    std::vector<ConditionalOrder> get_active_cond_orders() const;
    std::vector<CloseTask> get_close_tasks() const;
    AlgoOrderManager& get_algo_order_mgr() { return algo_order_mgr_; }
    const AlgoOrderManager& get_algo_order_mgr() const { return algo_order_mgr_; }
    PaperTradingEngine& get_paper_engine() { return paper_engine_; }
    const PaperTradingEngine& get_paper_engine() const { return paper_engine_; }
    bool retry_failed_close_task(uint32_t task_id);
    std::vector<std::string> get_recent_alerts(size_t limit = 50) const;
    void push_runtime_alert(const std::string& message);
    RiskSnapshot get_risk_snapshot(const std::string& account_id = "") const;
    // 热重载所有账户的风控参数：从 ConfigStore 重新加载并更新 RiskManager
    // Hot-reload all accounts' risk control params: re-read from ConfigStore and update RiskManager.
    bool reload_all_risk_configs();
    // RMS 风控模式控制 / RMS Risk Mode Control
    void set_risk_mode(RiskMode mode, const std::string& reason = "");
    RiskMode get_risk_mode(const std::string& account_id = "") const;
    std::vector<RiskEvent> drain_risk_events(const std::string& account_id = "");
    LatencySnapshot get_latency_snapshot() const;
    std::vector<OrderInfo> get_recent_orders(const std::string& account_id = "", size_t limit = 200) const;
    std::vector<PnlCurvePoint> get_pnl_curve(size_t limit = 240) const;
    std::vector<StrategyPerformanceSnapshot> get_strategy_performance(const std::string& strategy_id = "") const;
    TickRecordingStatus get_tick_recording_status() const;
    bool start_tick_recording(const std::string& path = "", std::string* error = nullptr);
    bool stop_tick_recording(std::string* error = nullptr);
    bool delete_tick_recording(const std::string& instrument = "", std::string* error = nullptr, size_t* deleted_files = nullptr, uintmax_t* deleted_bytes = nullptr);
    InitStatus get_init_status() const;
    std::vector<AccountMonitorSnapshot> get_account_snapshots() const;
    std::vector<StrategyMonitorSnapshot> get_strategy_snapshots(const std::string& account_id = "") const;
    std::chrono::steady_clock::time_point get_start_time() const { return start_time_; }
    bool is_md_gateway_logged_in() const { return md_gateway_ && md_gateway_->is_logged_in(); }
    // 注入行情网关（必须在 init() 之前调用，用于测试或自定义网关）
    void set_md_gateway(std::unique_ptr<IMdGateway> gw) { md_gateway_ = std::move(gw); }
    using MdGatewayFactory = std::function<std::unique_ptr<IMdGateway>(const std::string& gateway_type)>;
    void register_md_gateway_factory(MdGatewayFactory factory) { md_gateway_factory_ = std::move(factory); }
    bool is_trade_gateway_logged_in() const;
    bool is_trading_ready() const { return trading_ready_.load(std::memory_order_relaxed); }
    bool is_running() const { return running_.load(std::memory_order_relaxed); }
    bool is_reconnect_sync_pending() const;
    bool is_md_only_mode() const { return md_only_mode_.load(std::memory_order_relaxed); }
    bool is_production_hft_mode() const { return production_hft_mode_; }
    bool has_md_queue_overflow() const { return md_queue_overflow_detected_.load(std::memory_order_relaxed); }
    bool has_trade_queue_overflow() const { return trade_queue_overflow_detected_.load(std::memory_order_relaxed); }
    bool has_command_queue_overflow() const { return command_queue_overflow_detected_.load(std::memory_order_relaxed); }
    size_t md_queue_drop_count() const { return md_queue_.drop_count(); }
    bool can_trade() const;
    bool can_trade(const std::string& account_id) const;
    int seconds_since_last_tick() const;

    AccountManager& get_account_manager() { return account_mgr_; }
    const AccountManager& get_account_manager() const { return account_mgr_; }

    void on_trade_reconnected(const std::string& account_id, int front_id, int session_id, int max_order_ref);
    void on_trading_day(const char* trading_day);
    void apply_account_snapshot(const AccountInfo& account);
    void apply_position_snapshot(const std::string& account_id, const std::vector<PositionInfo>& positions);
    void apply_active_orders_snapshot(const std::string& account_id, const std::vector<OrderInfo>& active_orders);

    PositionInfo get_position(const char* instrument, Direction dir) const;
    PositionInfo get_position(const char* instrument, Direction dir, const std::string& account_id) const override;
    int get_net_position(const char* instrument) const;
    int get_net_position(const char* instrument, const std::string& account_id) const override;

    WindowedOrderBook get_order_book(const char* instrument) const override;
    AccountInfo get_account_info(const std::string& account_id) const override;
    void strategy_log(const std::string& strategy_id, int level, const std::string& message) override;
    void save_strategy_state(const std::string& strategy_id,
                             const std::map<std::string, std::string>& state) override;
    std::map<std::string, std::string> load_strategy_state(const std::string& strategy_id) override;
    int register_timer(const std::string& strategy_id, int interval_ms) override;
    void unregister_timer(int timer_id) override;
    std::vector<KlineBar> query_klines(const std::string& instrument,
                                        const std::string& period,
                                        size_t count) const override;

    const Config& get_config() const { return config_; }
    const std::string& get_config_path() const { return config_path_; }
    void set_config_store(ConfigStore* store) { store_ = store; }
    ConfigStore* get_config_store() const { return store_; }
    std::thread& get_consumer_thread() { return consumer_thread_; }

private:
    void consumer_loop();
    void apply_config_store_overlay();
    void apply_runtime_performance_config();
    ConditionalTriggerResult submit_conditional_order(const OrderRequest& req, std::string& reason);

    void process_tick(const TickData& tick);
    void auto_pause_on_error(const std::string& strategy_id);
    void process_order(const OrderInfo& order);
    void process_trade(const TradeInfo& trade);
    void process_account(const AccountInfo& account);
    void snapshot_pnl_curve();
    void process_position(const PositionInfo& pos);

    void process_command(const EngineCommand& cmd);
    bool enqueue_command_or_fallback(const EngineCommand& cmd, const char* label);
    void exec_pause();
    void exec_resume();
    void exec_stop();
    void exec_emergency_close();
    bool wait_for_snapshots(int timeout_sec);
    void reset_snapshot_state();
    void reset_snapshot_state(AccountContext* ctx);
    void maybe_finish_reconnect_sync();
    void check_runtime_alerts();
    void schedule_active_orders_refresh(int delay_ms);
    void try_refresh_active_orders();
    void apply_monitoring_config(int no_tick_warn_seconds, const std::string& trading_sessions);
    bool is_in_configured_trading_session() const;
    void refresh_trading_session_state(bool in_session);
    void load_runtime_state();
    void save_runtime_state() const;
    void load_runtime_cache();
    void save_runtime_cache() const;
    void rebuild_hot_instruments();
    bool is_hot_instrument(const char* instrument) const;
    void send_order_unlocked(const OrderRequest& req, std::string& out_order_ref,
                             std::string* out_reject_reason = nullptr,
                             std::string* out_reject_message = nullptr);
    void record_alert(const std::string& message);
    void handle_trade_queue_overflow(AccountContext* ctx, const std::string& message);
    bool try_cancel_order(const std::string& order_ref, const std::string& account_id);

    AccountContext* resolve_account(const char* account_id);
    AccountContext* resolve_account(const std::string& account_id);

    Config config_;
    std::string config_path_;
    ConfigStore* store_ = nullptr;

    std::unique_ptr<IMdGateway> md_gateway_;
    MdGatewayFactory md_gateway_factory_;
    AccountManager account_mgr_;
    ConditionalOrderManager cond_order_mgr_;
    CloseManager close_mgr_;
    AlgoOrderManager algo_order_mgr_;
    PaperTradingEngine paper_engine_;

    std::vector<std::shared_ptr<StrategyBase>> strategies_;
    mutable std::mutex strategies_mtx_;
    // Hot-path read-only snapshot: when writer (add/remove_strategy) holds strategies_mtx_,
    // atomic_store an immutable vector; readers (process_tick etc.) use atomic_load to get
    // a shared_ptr — lock-free, no allocation.
    // (热路径只读快照：在写者 add/remove_strategy 持有 strategies_mtx_ 时
    //  atomic_store 一份不可变 vector，读者 process_tick 等用 atomic_load 拿到
    //  shared_ptr，无锁、无分配。)
    std::shared_ptr<const std::vector<std::shared_ptr<StrategyBase>>> strategies_snapshot_ptr_;
    void refresh_strategies_snapshot();  // 调用前必须持有 strategies_mtx_
    std::shared_ptr<const std::vector<std::shared_ptr<StrategyBase>>>
        load_strategies_snapshot() const;
    InstrumentRegistry instrument_registry_;
    KlineManager kline_mgr_;

    SessionManager session_mgr_;
    mutable std::mutex snapshot_mtx_;
    std::condition_variable snapshot_cv_;
    std::atomic<bool> trading_ready_{false};
    std::atomic<bool> md_only_mode_{false};
    std::filesystem::path runtime_state_path_ = "runtime_state.dat";

    // 启动时清理"前一会话遗留的挂队列单"开关与一次性标志。
    // 开启后，所有账户首次完成 active_orders 快照同步后，遍历活动单并 try_cancel。
    // 一次性：重连后再次 snapshot 不应重新清理（避免误撤策略当前正常持仓的挂单）。
    // On-startup cleanup flag for pending orders left from previous session.
    // When enabled, after all accounts complete their first active_orders snapshot sync,
    // iterate active orders and try_cancel them. One-shot: reconnect snapshots should NOT
    // re-trigger cleanup (to avoid cancelling orders for strategies' normal positions).
    bool cancel_pending_on_restart_ = false;
    std::atomic<bool> pending_cleanup_done_{false};
    // 条件单跨会话恢复的 TTL（天）。0 = 永不过期；默认 1 天，与"一个交易日上下文"对齐。
    // Conditional order cross-session recovery TTL (days). 0 = never expire; default 1 day,
    // aligned with "single trading day context".
    int conditional_order_ttl_days_ = 1;
    std::atomic<bool> active_orders_refresh_pending_{false};
    std::atomic<bool> active_orders_refresh_inflight_{false};
    std::atomic<int> active_orders_refresh_expected_{0};
    std::atomic<int> active_orders_refresh_completed_{0};
    std::atomic<long long> next_active_orders_refresh_ms_{0};
    mutable std::mutex alerts_mtx_;
    std::deque<std::string> recent_alerts_;
    mutable std::shared_mutex trades_mtx_;
    std::deque<TradeInfo> recent_trades_;
    mutable std::shared_mutex orders_history_mtx_;
    std::deque<OrderInfo> recent_orders_;
    mutable std::mutex pnl_curve_mtx_;
    std::deque<PnlCurvePoint> pnl_curve_;
    StrategyController strategy_ctrl_;
    std::atomic<size_t> processed_tick_count_{0};
    TickRecorder tick_recorder_;
    std::thread tick_recording_thread_;
    std::filesystem::path runtime_cache_path_ = "runtime_cache.dat";
    std::atomic<long long> last_tick_to_signal_us_{-1};
    std::atomic<long long> last_signal_to_order_us_{-1};
    std::atomic<long long> last_order_to_trade_us_{-1};
    std::atomic<long long> last_tick_process_us_{-1};
    std::atomic<long long> last_order_process_us_{-1};
    std::atomic<long long> last_trade_process_us_{-1};
    std::atomic<long long> last_signal_steady_us_{0};
    // Order sent -> fill return latency tracking ring.
    // Replaces std::map<std::string,long long>: eliminates per-order std::string heap allocation,
    // RB-tree node allocation, and rebalancing overhead on erase(begin()) when exceeding capacity.
    // Capacity 512 is enough to cover all in-flight orders at any moment (orders of magnitude less);
    // linear scan 512x24B is only 12KB, L1 cache friendly.
    // (订单发出 → 成交回报的延迟跟踪环。
    //  取代 std::map<string,long long>: 消除每单 string 堆分配、RB-tree 节点分配,
    //  以及超过容量时 erase(begin()) 的红黑树重平衡开销。
    //  容量 512 足够覆盖任意时刻在途订单数量级远小于此, 线性扫描 512×24B 仅 12KB, L1 缓存友好。)
    struct OrderLatencyEntry {
        char order_ref[16]{};        // FixedKey 风格,'\0' 起始表示槽位为空
        long long sent_us = 0;
    };
    static constexpr size_t kOrderLatencyRingCap = 512;
    std::array<OrderLatencyEntry, kOrderLatencyRingCap> order_latency_ring_{};
    size_t order_latency_ring_head_ = 0;
    // latency_mtx_ removed: ring is only accessed from consumer thread
    std::atomic<bool> queue_overflow_detected_{false}; // 交易队列溢出标记
    std::atomic<bool> trade_queue_overflow_detected_{false};
    std::atomic<bool> md_queue_overflow_detected_{false};
    std::atomic<bool> md_queue_drop_alerted_{false};
    std::atomic<bool> command_queue_overflow_detected_{false};
    std::atomic<bool> command_queue_drop_alerted_{false};
    bool production_hft_mode_ = false;
    bool hft_disable_python_hot_path_ = false;
    bool hft_disable_tick_recording_ = false;
    bool hft_disable_kline_hot_path_ = false;
    bool hft_strategy_hot_instruments_only_ = true;
    size_t md_batch_size_ = 1;
    int engine_cpu_core_ = -1;
    int gateway_cpu_core_ = -1;
    int logger_cpu_core_ = -1;

    static constexpr size_t kQueueSize = 65536;
    SPSCQueue<Event, kQueueSize> md_queue_;

    static constexpr size_t kCmdQueueSize = 4096;
    SPSCQueue<EngineCommand, kCmdQueueSize> cmd_queue_;
    std::mutex cmd_push_mtx_;

    std::thread consumer_thread_;
    std::thread watchdog_thread_;
    std::unique_ptr<ThreadRegistry> thread_registry_;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> consumer_heartbeat_ms_{0};

    // 终端展示用合约的延迟订阅：start() 同步只订阅 hot 合约（策略+条件单+config 显式 hot），
    // 剩余的 cold 合约交给该线程后台分批订阅，避免阻塞启动 critical path。
    // Lazy subscription for terminal display instruments: start() only subscribes hot instruments
    // (strategies + conditional orders + config explicit hot) synchronously. Remaining cold
    // instruments are delegated to this background thread in batches, avoiding blocking the startup critical path.
    std::thread lazy_md_subscribe_thread_;
    std::atomic<bool> lazy_md_subscribe_stop_{false};
    bool lazy_subscribe_non_hot_ = true;

    // 异步状态保存：consumer 线程标记 dirty，后台线程合并写入
    // Async state saving: consumer thread marks dirty, background thread merges and writes.
    std::thread async_save_thread_;
    std::atomic<bool> save_running_{false};
    std::atomic<bool> dirty_{false};
    std::mutex save_mtx_;
    std::condition_variable save_cv_;
    mutable std::mutex save_file_mtx_;
    void async_save_loop();
    void request_async_save();

    LatencyStats tick_latency_stats_{1000};
    bool was_running_before_reconnect_ = false;

    TickDataManager tick_data_mgr_;
    OrderBookManager order_book_mgr_;

    struct TimerEntry {
        int id;
        int interval_ms;
        std::string strategy_id;
        std::chrono::steady_clock::time_point next_fire;
    };
    std::vector<TimerEntry> timers_;
    std::atomic<int> next_timer_id_{1};

#ifdef ENABLE_METRICS
    WebServer web_server_;
#endif

    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint32_t> last_cond_id_{0};
};

} // namespace hft
