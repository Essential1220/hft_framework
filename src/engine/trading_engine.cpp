// ============================================
// trading_engine.cpp - Trading engine orchestration and state management (交易引擎编排与状态管理)
// Core engine implementation coordinating market data, order management, position tracking,
// risk control, and strategy execution across multiple accounts.
// (核心引擎实现: 协调行情、委托管理、持仓追踪、风控和多账户策略执行)
// ============================================

#include "engine/trading_engine.h"

#include "common/branch_hints.h"
#include "common/logger.h"
#include "common/binary_io.h"
#include "common/crypto.h"
#include "common/string_utils.h"
#include "common/thread_utils.h"
#include "engine/account_config.h"
#include "strategy/strategy_base.h"
#include "strategy/strategy_config.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#include <tuple>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// Cross-platform CPU pause for busy-wait loops.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
    #define HFT_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define HFT_CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#elif defined(__arm__) || defined(_M_ARM)
    #define HFT_CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
    #define HFT_CPU_PAUSE() std::this_thread::yield()
#endif

namespace hft {

class ThreadRegistry {
public:
    struct Entry {
        const char* label;
        std::thread* thread;
        std::function<void()> pre_join;
        int join_timeout_sec;
    };

    void add(const char* label, std::thread* t, std::function<void()> pre_join, int timeout_sec = 5) {
        entries_.push_back({label, t, std::move(pre_join), timeout_sec});
    }

    void shutdown_all() {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            if (it->pre_join) it->pre_join();
            timed_join(*it->thread, it->join_timeout_sec, it->label);
        }
        entries_.clear();
    }

    void clear() { entries_.clear(); }

private:
    // 安全版超时 join：超时后不 detach（detach 会导致线程继续访问已析构的成员，
    // 产生 UAF），而是给一个加倍的宽限期做最后尝试；仍然超时则记录致命错误并
    // 调用 std::terminate()——生产交易系统宁可 crash dump 也不能带着悬空线程运行。
    // Safe timeout join: on timeout, does NOT detach (detach would cause UAF on destroyed members).
    // Instead gives a doubled grace period for one last attempt; if still timed out, logs fatal
    // and calls std::terminate() — a production trading system would rather crash dump
    // than run with dangling threads.
    static bool timed_join(std::thread& t, int timeout_sec, const char* label) {
        if (!t.joinable()) return true;
        auto fut = std::async(std::launch::async, [&t] { t.join(); });
        if (fut.wait_for(std::chrono::seconds(timeout_sec)) == std::future_status::ready) {
            return true;
        }
        // 首次超时：给宽限期（2x），线程可能只是在做最后的 IO flush
        // First timeout: give a grace period (2x); thread may just be doing final IO flush.
        LOG_ERROR(std::string("Thread join timeout (") + std::to_string(timeout_sec) +
                  "s): " + (label ? label : "unknown") + " — waiting grace period...");
        const int grace_sec = timeout_sec * 2;
        if (fut.wait_for(std::chrono::seconds(grace_sec)) == std::future_status::ready) {
            LOG_ERROR(std::string("Thread '") + (label ? label : "unknown") +
                      "' joined during grace period");
            return true;
        }
        // 宽限期仍然超时：这是致命错误。
        // 绝不 detach——detach 后线程继续运行会访问已析构的 engine 成员(UAF)。
        // 生产环境 crash dump 比带着悬空线程继续运行更安全。
        // Grace period also timed out: this is a fatal error.
        // Never detach — detach'd thread would access destroyed engine members (UAF).
        // In production, a crash dump is safer than running with dangling threads.
        LOG_ERROR(std::string("FATAL: Thread '") + (label ? label : "unknown") +
                  "' failed to join after " + std::to_string(timeout_sec + grace_sec) +
                  "s total — calling std::terminate() to prevent UAF");
        std::terminate();
        return false; // unreachable, suppress compiler warning
    }

    std::vector<Entry> entries_;
};

namespace {

using namespace hft::binary_io;

constexpr char kRuntimeCacheMagic[] = "HFTRC1";
constexpr size_t kDefaultMdBatchSize = 1;
constexpr size_t kMaxMdBatchSize = 2048;

long long steady_us_now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

template<typename Fn>
auto guarded_run(const char* label, Fn&& fn) {
    return [label, f = std::forward<Fn>(fn)]() mutable {
        try {
            f();
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("Thread '") + label + "' crashed: " + e.what());
        } catch (...) {
            LOG_ERROR(std::string("Thread '") + label + "' crashed: unknown exception");
        }
    };
}

// kDefaultStrategyId reuses the "default" literal to avoid constructing std::string("default") per tick.
// (复用为 "default" 字面量, 避免每 tick 构造 std::string("default"))
const std::string kDefaultStrategyId = "default";

const char* strategy_state_name(StrategyState state) {
    switch (state) {
        case StrategyState::Running:
            return "running";
        case StrategyState::Paused:
            return "paused";
        case StrategyState::Stopped:
            return "stopped";
    }
    return "running";
}

void engine_start_trace(const std::string& step) {
    if (std::getenv("HFT_ENGINE_START_TRACE") == nullptr) return;
    std::ofstream ofs("engine_start_trace.log", std::ios::app);
    ofs << step << "\n";
}

std::string now_time_text() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

const char* direction_text(Direction direction) {
    return direction == Direction::Buy ? "买入" : "卖出";
}

const char* offset_text(Offset offset) {
    switch (offset) {
        case Offset::Open: return "开仓";
        case Offset::Close: return "平仓";
        case Offset::CloseToday: return "平今";
        case Offset::CloseYesterday: return "平昨";
    }
    return "未知";
}

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

bool parse_kline_bar_line(const std::string& line,
                          std::string* instrument,
                          std::string* period,
                          KlineBar* bar) {
    if (!instrument || !period || !bar) {
        return false;
    }

    std::istringstream iss(line);
    std::string kind;
    std::string field;
    std::getline(iss, kind, '\t');
    if (kind != "kline_bar") {
        return false;
    }

    if (!std::getline(iss, *instrument, '\t') ||
        !std::getline(iss, *period, '\t') ||
        !std::getline(iss, field, '\t')) {
        return false;
    }

    try {
        bar->timestamp_ms = std::stoll(field);
        if (!std::getline(iss, bar->time, '\t')) return false;
        if (!std::getline(iss, field, '\t')) return false;
        bar->open = std::stod(field);
        if (!std::getline(iss, field, '\t')) return false;
        bar->high = std::stod(field);
        if (!std::getline(iss, field, '\t')) return false;
        bar->low = std::stod(field);
        if (!std::getline(iss, field, '\t')) return false;
        bar->close = std::stod(field);
        if (!std::getline(iss, field, '\t')) return false;
        bar->volume = std::stoi(field);
        if (!std::getline(iss, field, '\t')) return false;
        bar->turnover = std::stod(field);
    } catch (...) {
        return false;
    }

    return !instrument->empty() && !period->empty();
}

// Append values that are not already present.
void append_unique(std::vector<std::string>& items, const std::vector<std::string>& values) {
    for (const auto& value : values) {
        if (std::find(items.begin(), items.end(), value) == items.end()) {
            items.push_back(value);
        }
    }
}

// Collect instruments subscribed by configured strategies.
std::vector<std::string> collect_strategy_instruments(const Config& config) {
    std::vector<std::string> instruments;
    append_unique(instruments, split_csv(config.get_string("Strategy", "Instruments", "")));


    if (!config.has_section("Strategies")) {
        return instruments;
    }


    for (const auto& strategy_id : split_csv(config.get_string("Strategies", "List", ""))) {
        append_unique(
            instruments,
            split_csv(config.get_string("Strategy." + strategy_id, "Instruments", "")));
    }
    return instruments;
}

std::string extract_alpha_prefix(const std::string& instrument) {
    size_t index = 0;
    while (index < instrument.size() && std::isalpha(static_cast<unsigned char>(instrument[index]))) {
        ++index;
    }
    return instrument.substr(0, index);
}

int extract_contract_ym(const std::string& instrument) {
    size_t index = 0;
    while (index < instrument.size() && std::isalpha(static_cast<unsigned char>(instrument[index]))) {
        ++index;
    }

    std::string digits;
    while (index < instrument.size() && std::isdigit(static_cast<unsigned char>(instrument[index])) && digits.size() < 4) {
        digits.push_back(instrument[index]);
        ++index;
    }
    if (digits.size() != 4) {
        return -1;
    }
    return std::stoi(digits);
}

std::vector<std::string> select_terminal_display_instruments(const std::vector<std::string>& queried_instruments) {
    static const std::vector<std::string> preferred_prefixes = {
        "IF", "IC", "IH", "IM",
        "au", "ag",
        "cu", "al", "zn",
        "rb", "hc", "i",
        "sc", "fu",
    };

    std::map<std::string, std::vector<std::pair<int, std::string>>> grouped;
    for (const auto& instrument : queried_instruments) {
        const std::string prefix = extract_alpha_prefix(instrument);
        if (std::find(preferred_prefixes.begin(), preferred_prefixes.end(), prefix) == preferred_prefixes.end()) {
            continue;
        }
        const int ym = extract_contract_ym(instrument);
        if (ym <= 0) {
            continue;
        }
        grouped[prefix].push_back({ym, instrument});
    }

    const std::tm now_tm = local_time_now();
    const int current_ym = ((now_tm.tm_year + 1900) % 100) * 100 + (now_tm.tm_mon + 1);
    std::vector<std::string> selected;

    for (const auto& prefix : preferred_prefixes) {
        auto it = grouped.find(prefix);
        if (it == grouped.end() || it->second.empty()) {
            continue;
        }

        auto& candidates = it->second;
        std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            if (left.first != right.first) {
                return left.first < right.first;
            }
            return left.second < right.second;
        });

        const auto chosen = std::find_if(candidates.begin(), candidates.end(), [current_ym](const auto& item) {
            return item.first >= current_ym;
        });
        selected.push_back((chosen != candidates.end() ? chosen : std::prev(candidates.end()))->second);
    }

    return selected;
}

// Retry startup queries such as account, positions, and active orders.
bool retry_query(const char* name,
                 const std::function<int()>& fn,
                 int max_attempts = 5,
                 int retry_delay_ms = 800) {
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        const int ret = fn();
        if (ret == 0) {
            if (attempt > 1) {
                LOG_INFO(std::string("startup query recovered: ") + name +
                         " attempt=" + std::to_string(attempt));
            }
            return true;
        }

        LOG_WARN(std::string("startup query failed: ") + name +
                 " ret=" + std::to_string(ret) +
                 " attempt=" + std::to_string(attempt) +
                 "/" + std::to_string(max_attempts));

        if (attempt < max_attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
        }
    }
    return false;
}

AccountTradeState get_account_trade_state_impl(const AccountContext* ctx, bool engine_running) {
    if (!ctx) {
        return AccountTradeState::Unknown;
    }
    if (!engine_running) {
        return AccountTradeState::Stopped;
    }
    if (!ctx->trade_gateway) {
        return AccountTradeState::Booting;
    }
    if (ctx->reconnect_sync_pending) {
        return AccountTradeState::ReconnectSync;
    }
    if (!ctx->trade_gateway->is_logged_in()) {
        if (ctx->trade_state == AccountTradeState::Ready ||
            ctx->trade_state == AccountTradeState::GatewayDown ||
            ctx->account_snapshot_ready || ctx->position_snapshot_ready || ctx->active_orders_snapshot_ready) {
            return AccountTradeState::GatewayDown;
        }
        return AccountTradeState::LoginPending;
    }
    if (ctx->trade_state == AccountTradeState::SnapshotSync ||
        !ctx->account_snapshot_ready || !ctx->position_snapshot_ready || !ctx->active_orders_snapshot_ready) {
        return AccountTradeState::SnapshotSync;
    }
    if (ctx->trade_state == AccountTradeState::Booting ||
        ctx->trade_state == AccountTradeState::LoginPending ||
        ctx->trade_state == AccountTradeState::Unknown) {
        return AccountTradeState::SnapshotSync;
    }
    return ctx->trade_state;
}

void update_account_reject_state(AccountContext* ctx, OrderRejectReason reason, const std::string& message) {
    if (!ctx) {
        return;
    }
    std::lock_guard<std::mutex> lock(ctx->reject_mtx);
    ctx->last_reject_reason = reason;
    ctx->last_reject_message = message;
}

void clear_account_reject_state(AccountContext* ctx) {
    if (!ctx) {
        return;
    }
    std::lock_guard<std::mutex> lock(ctx->reject_mtx);
    ctx->last_reject_reason = OrderRejectReason::None;
    ctx->last_reject_message.clear();
}

void set_account_trade_state(AccountContext* ctx, AccountTradeState state) {
    if (!ctx) {
        return;
    }
    const AccountTradeState old = ctx->trade_state;
    if (old == state) return;
    ctx->trade_state = state;
    const std::string label = ctx->account_id.empty() ? "default" : ctx->account_id;
    LOG_INFO("account trade state: " + label + " " +
             std::string(to_string(old)) + " -> " + std::string(to_string(state)));
}

} // namespace

TradingEngine::TradingEngine() = default;

TradingEngine::~TradingEngine() {
    stop();
}

bool TradingEngine::init(const std::string& config_path) {
    config_path_ = config_path;

    // Load configuration file (加载配置文件)
    if (!config_.load(config_path)) {
        return false;
    }
    LOG_INFO("引擎配置加载成功");

    // Apply SQLite overlay configuration (account / strategy / instrument / risk)
    // (从 SQLite 覆盖配置: 账户/策略/合约/风控)
    apply_config_store_overlay();

    // Instruments
    instrument_registry_.instruments_ref() = collect_strategy_instruments(config_);

    if (instrument_registry_.instruments_ref().empty()) {
        return false;
    }
    rebuild_hot_instruments();

    std::string all_inst;
    for (const auto& inst : instrument_registry_.instruments_ref()) all_inst += inst + " ";
    LOG_INFO("策略合约加载完成: " + all_inst);

    // Account manager
if (!account_mgr_.init(config_, this)) {
        return false;
    }
    for (auto* ctx : account_mgr_.all_accounts()) {
        set_account_trade_state(ctx, AccountTradeState::Booting);
        clear_account_reject_state(ctx);
    }

    // Account manager
    AccountContext* default_ctx = account_mgr_.default_account();
    if (!default_ctx) {
        LOG_ERROR("引擎无法获取默认账户");
        return false;
    }
    // Account manager
for (auto* ctx : account_mgr_.all_accounts()) {
        ctx->risk_mgr.init(config_, &ctx->position_mgr, &ctx->order_mgr, &tick_data_mgr_, ctx->account_id);
    }
    // Configuration
    runtime_state_path_ = config_.get_string("Runtime", "StateFile", "runtime_state.dat");
    runtime_cache_path_ = runtime_state_path_;
    runtime_cache_path_.replace_extension(".cache");
    {
        const std::string configured_kline_store = trim_copy(config_.get_string("Runtime", "KlineStoreFile", ""));
        if (!configured_kline_store.empty()) {
            kline_mgr_.set_store_path(configured_kline_store);
        } else {
            auto kline_path = runtime_state_path_;
            kline_path.replace_extension(".kdb");
            kline_mgr_.set_store_path(kline_path);
        }
    }
    {
        const std::string configured_tick_file = trim_copy(config_.get_string("Runtime", "TickRecordFile", ""));
        if (!configured_tick_file.empty()) {
            tick_recorder_.set_path(configured_tick_file);
        } else {
            auto tick_path = runtime_state_path_;
            tick_path.replace_extension(".ticks");
            tick_recorder_.set_path(tick_path);
        }
        tick_recorder_.set_alert_callback([this](const std::string& msg) { record_alert(msg); });
        const std::string storage_mode_str = config_.get_string("TickRecording", "StorageMode", "stream");
        if (storage_mode_str == "mmap") {
            tick_recorder_.set_storage_mode(TickStorageMode::Mmap);
            tick_recorder_.set_mmap_max_ticks(
                static_cast<size_t>(config_.get_int("TickRecording", "MmapMaxTicks", 5000000)));
            LOG_INFO("tick recording: mmap mode, max_ticks=" +
                     std::to_string(config_.get_int("TickRecording", "MmapMaxTicks", 5000000)));
        }
    }
    session_mgr_.apply_monitoring_config(
        config_.get_int("Runtime", "NoTickWarnSeconds", 10),
        config_.get_string("Trading", "TradingSessions", ""));
    // [Runtime] New switches — default off / conservative, do not change existing behavior.
    // ([Runtime] 新增开关 — 默认关闭/保守, 不改变现有行为。)
    cancel_pending_on_restart_ = (config_.get_int("Runtime", "CancelPendingOnRestart", 0) != 0);
    conditional_order_ttl_days_ = config_.get_int("Runtime", "ConditionalOrderTTLDays", 1);
    // [Performance] LazySubscribeNonHot: default on. Move terminal-display cold instruments
    // from startup critical path to background thread.
    // ([Performance] LazySubscribeNonHot: 默认开。把终端展示用的 cold 合约从启动路径移到后台线程。)
    lazy_subscribe_non_hot_ = (config_.get_int("Performance", "LazySubscribeNonHot", 1) != 0);
    apply_runtime_performance_config();

    // 设置平仓管理器的回调函数
    close_mgr_.set_callbacks(
        [this](const OrderRequest& req, std::string& ref) { send_order(req, ref); }, // Send order callback (发送订单回调)
        [this](const std::string& account_id, const std::string& ref) { return try_cancel_order(ref, account_id); }, // Cancel order callback (撤销订单回调)
        [this](const std::string& msg) { // Alert callback (警告回调)
            record_alert(msg);
            LOG_WARN("平仓管理告警: " + msg);
        });

    // Load runtime state
    load_runtime_state();
    load_runtime_cache();
    return true;
}

bool TradingEngine::start() {
    engine_start_trace("start:enter");
    LOG_INFO("引擎启动中");

    if (!config_path_.empty()) {
        engine_start_trace("start:reload_config");
        Config latest_config;
        if (!latest_config.load(config_path_)) {
            LOG_ERROR("引擎启动前重新加载配置失败: " + config_path_);
            return false;
        }
        config_ = std::move(latest_config);
        engine_start_trace("start:apply_config_store_overlay");
        apply_config_store_overlay();

        instrument_registry_.instruments_ref() = collect_strategy_instruments(config_);
        instrument_registry_.market_universe_ref().clear();
        instrument_registry_.specs_mut().clear();
        if (instrument_registry_.instruments_ref().empty()) {
            LOG_ERROR("引擎启动前配置重载后无策略合约");
            return false;
        }
        rebuild_hot_instruments();

        if (!account_mgr_.init(config_, this)) {
            LOG_ERROR("引擎启动前账户配置重载失败");
            return false;
        }
        engine_start_trace("start:account_mgr_init_done");
        for (auto* ctx : account_mgr_.all_accounts()) {
            set_account_trade_state(ctx, AccountTradeState::Booting);
            clear_account_reject_state(ctx);
            ctx->risk_mgr.init(config_, &ctx->position_mgr, &ctx->order_mgr, &tick_data_mgr_, ctx->account_id);
        }

        runtime_state_path_ = config_.get_string("Runtime", "StateFile", "runtime_state.dat");
        runtime_cache_path_ = runtime_state_path_;
        runtime_cache_path_.replace_extension(".cache");
        session_mgr_.apply_monitoring_config(
            config_.get_int("Runtime", "NoTickWarnSeconds", 10),
            config_.get_string("Trading", "TradingSessions", ""));
        apply_runtime_performance_config();

        LOG_INFO("引擎启动前配置已重新加载");
    }

    start_time_ = std::chrono::steady_clock::now();
    running_ = true;
    paper_engine_.init(this);
    trading_ready_.store(false, std::memory_order_relaxed);
    engine_start_trace("start:start_threads");

    thread_registry_ = std::make_unique<ThreadRegistry>();

    consumer_thread_ = std::thread(&TradingEngine::consumer_loop, this);
    thread_registry_->add("consumer_loop", &consumer_thread_, nullptr, 5);
    if (engine_cpu_core_ >= 0) {
        if (set_thread_affinity(consumer_thread_, engine_cpu_core_))
            LOG_INFO("consumer_loop bound to core " + std::to_string(engine_cpu_core_));
        else
            LOG_WARN("failed to bind consumer_loop to core " + std::to_string(engine_cpu_core_));
    }

    watchdog_thread_ = std::thread(guarded_run("watchdog", [this]() {
        const auto heartbeat_path = std::filesystem::path(config_path_).parent_path() / "heartbeat";
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!running_) break;
            const int64_t hb = consumer_heartbeat_ms_.load(std::memory_order_relaxed);
            if (hb == 0) continue;
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now - hb > 10000) {
                LOG_ERROR("consumer loop heartbeat stale: " + std::to_string(now - hb) + "ms");
                record_alert("consumer_loop heartbeat timeout (" + std::to_string(now - hb) + "ms)");
            }
            try {
                std::ofstream hbf(heartbeat_path, std::ios::trunc);
                if (hbf.is_open()) hbf << now << "\n";
            } catch (...) {}
        }
        try { std::filesystem::remove(heartbeat_path); } catch (...) {}
    }));
    thread_registry_->add("watchdog", &watchdog_thread_, nullptr, 3);

    save_running_.store(true, std::memory_order_relaxed);
    async_save_thread_ = std::thread(guarded_run("async_save", [this]() { async_save_loop(); }));
    thread_registry_->add("async_save", &async_save_thread_,
        [this]() { save_running_.store(false, std::memory_order_relaxed); save_cv_.notify_one(); }, 5);

    tick_recorder_.writer_running_ref().store(true, std::memory_order_relaxed);
    tick_recording_thread_ = std::thread(guarded_run("tick_recording", [this]() { tick_recorder_.writer_loop(); }));
    thread_registry_->add("tick_recording", &tick_recording_thread_,
        [this]() { tick_recorder_.stop_writer(); }, 5);

    if (logger_cpu_core_ >= 0) {
        auto& logger_thread = AsyncLogger::instance().worker_thread();
        if (logger_thread.joinable()) {
            if (set_thread_affinity(logger_thread, logger_cpu_core_))
                LOG_INFO("logger thread bound to core " + std::to_string(logger_cpu_core_));
            else
                LOG_WARN("failed to bind logger thread to core " + std::to_string(logger_cpu_core_));
        }
    }

    auto fail_startup = [this](const std::string& reason) {
        LOG_ERROR(reason);
        stop();
        return false;
    };

    // Market data gateway
    if (!md_gateway_) {
        if (md_gateway_factory_) {
            const std::string md_gateway_type = resolve_market_data_gateway_type(config_);
            md_gateway_ = md_gateway_factory_(md_gateway_type);
        }
        if (!md_gateway_) {
            LOG_ERROR("no MD gateway available: register a factory or call set_md_gateway() before init()");
            return false;
        }
    }
    const std::string md_section = resolve_market_data_config_section(config_);
    if (md_section.empty()) {
        return fail_startup("market data config not found");
    }
    const std::string configured_md_front = config_.get_string(md_section, "MarketFront", "");
    engine_start_trace("start:md_init_begin section=" + md_section + " front=" + configured_md_front);
    LOG_INFO("行情网关初始化开始: section=" + md_section + " front=" + configured_md_front);
    md_gateway_->init(config_, md_section, this); // Initialize market data gateway (初始化行情网关)
    engine_start_trace("start:md_wait_login front=" + configured_md_front);
    LOG_INFO("等待行情网关登录: front=" + configured_md_front + " timeout=15s");
if (!md_gateway_->wait_for_login(15)) { // Wait for MD gateway login, 15-second timeout (等待行情网关登录, 超时15秒)
        engine_start_trace("start:md_login_timeout front=" + configured_md_front);
        return fail_startup("market data gateway login timeout");
    }
    engine_start_trace("start:md_login_ok");
    LOG_INFO("行情网关登录成功");

    for (auto* ctx : account_mgr_.all_accounts()) {
        set_account_trade_state(ctx, AccountTradeState::LoginPending);
    }

    // Engine startup trace
    engine_start_trace("start:td_start_all_begin");
    LOG_INFO("交易网关启动开始: account_count=" + std::to_string(account_mgr_.all_accounts().size()));
    auto start_result = account_mgr_.start_all(config_, this);
    engine_start_trace("start:td_start_all_done");
    LOG_INFO("交易网关启动完成: all_ok=" + std::to_string(start_result.all_ok ? 1 : 0) +
             " failed_count=" + std::to_string(start_result.failed_accounts.size()));
    if (!start_result.all_ok) {
        md_only_mode_.store(true, std::memory_order_relaxed);
        for (auto& name : start_result.failed_accounts) {
            LOG_WARN("交易网关登录失败，账户=" + name + "，进入仅行情模式");
            auto* ctx = account_mgr_.find_account(name);
            if (ctx) set_account_trade_state(ctx, AccountTradeState::GatewayDown);
        }
        std::string failed_list;
        for (size_t i = 0; i < start_result.failed_accounts.size(); ++i) {
            if (i > 0) failed_list += ",";
            failed_list += start_result.failed_accounts[i];
        }
        record_alert("仅行情模式：交易网关登录失败，账户=" + failed_list);
    } else {
        md_only_mode_.store(false, std::memory_order_relaxed);
    }

    // Engine startup trace
    engine_start_trace("start:query_snapshots_begin");
    LOG_INFO("启动账户快照同步开始");
    reset_snapshot_state();
    for (auto* ctx : account_mgr_.all_accounts()) {
        // Skip accounts that failed login (跳过登录失败的账户)
        if (ctx->trade_state == AccountTradeState::GatewayDown) continue;

        const std::string label_acct = ctx->account_id.empty() ? "default" : ctx->account_id;
        LOG_INFO("启动同步账户快照: " + label_acct);
        // Retry query
        if (!retry_query(("资金[" + label_acct + "]").c_str(),
                         [ctx]() { return ctx->trade_gateway->query_account(); })) {
            set_account_trade_state(ctx, AccountTradeState::GatewayDown);
            LOG_WARN("启动资金查询失败，标记账户网关不可用: " + label_acct);
            continue;
        }
        // Retry query
        if (!retry_query(("持仓[" + label_acct + "]").c_str(),
                         [ctx]() { return ctx->trade_gateway->query_position(); })) {
            set_account_trade_state(ctx, AccountTradeState::GatewayDown);
            LOG_WARN("启动持仓查询失败，标记账户网关不可用: " + label_acct);
            continue;
        }

        // Retry query
        if (!retry_query(("活动委托[" + label_acct + "]").c_str(),
                         [ctx]() { return ctx->trade_gateway->query_active_orders(); })) {
            LOG_WARN("启动活动委托查询失败，已安排刷新: " + label_acct);
            record_alert("启动活动委托查询失败: " + label_acct);
            schedule_active_orders_refresh(0); // Schedule immediate active orders refresh (安排立即刷新活动委托)
        }
        LOG_INFO("启动快照请求已发出: " + label_acct);
    }
    engine_start_trace("start:query_snapshots_sent");
    LOG_INFO("启动账户快照请求已发送");
    snapshot_cv_.notify_all(); // Notify threads waiting for snapshots (通知等待快照的线程)
schedule_active_orders_refresh(1500); // Schedule active orders refresh in 1.5 seconds (安排1.5秒后刷新活动委托)

    // Wait
    if (!wait_for_snapshots(15)) {
        if (md_only_mode_.load(std::memory_order_relaxed)) {
            LOG_WARN("启动快照同步超时（仅行情模式，继续运行）");
        } else {
            return fail_startup("启动快照同步超时");
        }
    }
    engine_start_trace("start:snapshots_ready");
    LOG_INFO("启动账户快照同步完成");

    // Print positions for each account (打印每个账户的持仓)
for (const auto* ctx : account_mgr_.all_accounts()) {
        ctx->position_mgr.log_positions("startup[" + (ctx->account_id.empty() ? "default" : ctx->account_id) + "]");
    }

    // Account manager
    // Account manager
    if (auto* discovery_account = account_mgr_.default_account();
        discovery_account && discovery_account->trade_gateway &&
        discovery_account->trade_state != AccountTradeState::GatewayDown) {
        engine_start_trace("start:query_instruments_begin");
        LOG_INFO("启动合约列表查询开始");
        const auto queried_instruments = discovery_account->trade_gateway->query_instruments(20);
        engine_start_trace("start:query_instruments_done count=" + std::to_string(queried_instruments.size()));
        LOG_INFO("启动合约列表查询完成: count=" + std::to_string(queried_instruments.size()));
        const auto terminal_instruments = queried_instruments;
        instrument_registry_.market_universe_ref() = terminal_instruments;
        for (const auto& inst : terminal_instruments) {
            auto spec_it = instrument_registry_.specs_mut().find(inst);
            if (spec_it != instrument_registry_.specs_mut().end()) {
                spec_it->second = apply_instrument_spec_overrides(config_, spec_it->second);
            } else {
                instrument_registry_.specs_mut()[inst] = apply_instrument_spec_overrides(config_, infer_instrument_spec(inst));
            }
        }
        // Filter expired contracts: instruments with expire_date < today are excluded from market_universe
        // (过滤已过期合约: expire_date < 今天的不进入 market_universe)
        {
            auto now = std::chrono::system_clock::now();
            auto tt = std::chrono::system_clock::to_time_t(now);
            struct tm local_tm{};
#ifdef _WIN32
            localtime_s(&local_tm, &tt);
#else
            localtime_r(&tt, &local_tm);
#endif
            char today_buf[16];
            std::snprintf(today_buf, sizeof(today_buf), "%04d%02d%02d",
                local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday);
            const std::string today_str(today_buf);
            instrument_registry_.market_universe_ref().erase(
                std::remove_if(instrument_registry_.market_universe_ref().begin(), instrument_registry_.market_universe_ref().end(),
                    [&](const std::string& inst) {
                        auto it = instrument_registry_.specs_mut().find(inst);
                        if (it == instrument_registry_.specs_mut().end()) return false;
                        const auto& ed = it->second.expire_date;
                        return !ed.empty() && ed < today_str;
                    }),
                instrument_registry_.market_universe_ref().end());
        }
        // Market data software needs full-market subscription; HFT hot path must later split into strategy-watched contract channels.
        // (行情软件需要全市场订阅; 高频交易热路径后续必须拆分为策略关注合约通道。)
        append_unique(instrument_registry_.instruments_ref(), terminal_instruments);
        rebuild_hot_instruments();
        std::string terminal_inst;
        const size_t preview_count = (std::min)(terminal_instruments.size(), size_t{32});
        for (size_t index = 0; index < preview_count; ++index) {
            const auto& inst = terminal_instruments[index];
            terminal_inst += inst + " ";
        }
        LOG_INFO("终端合约池生成完成: queried=" +
                 std::to_string(queried_instruments.size()) +
                 " selected=" + std::to_string(terminal_instruments.size()) +
                 " strategy=" + std::to_string(collect_strategy_instruments(config_).size()) +
                 " total_subscribe=" + std::to_string(instrument_registry_.instruments_ref().size()));
        if (!terminal_inst.empty()) {
            LOG_INFO("终端合约池预览: " + terminal_inst);
        } else {
            LOG_WARN("合约查询后终端合约池为空");
        }
        LOG_INFO("动态订阅池生成完成: strategy=" +
                 std::to_string(collect_strategy_instruments(config_).size()) +
                 " terminal=" + std::to_string(terminal_instruments.size()) +
                 " total=" + std::to_string(instrument_registry_.instruments_ref().size()));
    } else {
        LOG_WARN("没有可用于查询合约列表的交易账户，行情订阅池仅包含策略合约");
        instrument_registry_.market_universe_ref().clear();
        for (const auto& inst : instrument_registry_.instruments_ref()) instrument_registry_.specs_mut()[inst] = apply_instrument_spec_overrides(config_, infer_instrument_spec(inst));
    }

    for (const auto& inst : instrument_registry_.instruments_ref()) {
        if (instrument_registry_.specs_mut().find(inst) == instrument_registry_.specs_mut().end()) instrument_registry_.specs_mut()[inst] = apply_instrument_spec_overrides(config_, infer_instrument_spec(inst));
    }

    if (instrument_registry_.market_universe_ref().empty()) {
        LOG_WARN("市场合约池为空；终端行情列表将在合约查询成功后更新");
    }

    std::string final_inst_preview;
    const size_t final_preview_count = (std::min)(instrument_registry_.instruments_ref().size(), size_t{32});
    for (size_t index = 0; index < final_preview_count; ++index) {
        final_inst_preview += instrument_registry_.instruments_ref()[index] + " ";
    }
    LOG_INFO("最终行情订阅合约: count=" + std::to_string(instrument_registry_.instruments_ref().size()) +
             (final_inst_preview.empty() ? "" : " preview=" + final_inst_preview +
                                                 (instrument_registry_.instruments_ref().size() > final_preview_count ? "..." : "")));

    // Strategies
    std::vector<std::shared_ptr<StrategyBase>> startup_strategies;
    {
        std::lock_guard<std::mutex> lock(strategies_mtx_);
        startup_strategies = strategies_;
    }
    for (auto& strategy : startup_strategies) {
        strategy->set_engine(this);
        strategy->on_init();
    }

    // Subscribe market data (订阅行情)
    engine_start_trace("start:subscribe_begin count=" + std::to_string(instrument_registry_.instruments_ref().size()));
    LOG_INFO("行情订阅开始: count=" + std::to_string(instrument_registry_.instruments_ref().size()));
    if (!lazy_subscribe_non_hot_) {
        md_gateway_->subscribe(instrument_registry_.instruments_ref());
    } else {
        // Split subscription pool by is_hot: hot instruments subscribe synchronously (few, ~10-50)
        // to ensure fast startup ready; cold instruments (terminal display, large count) go to
        // background thread in batches, avoiding blocking the critical path.
        // (把订阅池按 is_hot 拆开: hot 同步订阅 少量 ~10-50 合约 保证启动 ready 快;
        //  cold 合约 终端展示用 量大 放到后台线程慢慢分批送 避免阻塞 critical path)
        std::vector<std::string> hot_instruments;
        std::vector<std::string> cold_instruments;
        const auto& all_instruments = instrument_registry_.instruments_ref();
        hot_instruments.reserve(all_instruments.size());
        cold_instruments.reserve(all_instruments.size());
        for (const auto& inst : all_instruments) {
            if (instrument_registry_.is_hot(inst.c_str())) {
                hot_instruments.push_back(inst);
            } else {
                cold_instruments.push_back(inst);
            }
        }
        LOG_INFO("行情订阅分流: hot=" + std::to_string(hot_instruments.size()) +
                 " cold=" + std::to_string(cold_instruments.size()));
        if (!hot_instruments.empty()) {
            md_gateway_->subscribe(hot_instruments);
        }
        if (!cold_instruments.empty()) {
            lazy_md_subscribe_stop_.store(false, std::memory_order_relaxed);
            lazy_md_subscribe_thread_ = std::thread(guarded_run("lazy_md_subscribe",
                [this, cold = std::move(cold_instruments)]() {
                    // Wait for MD login before subscribing, to avoid the reconnect-resubscribe path
                    // triggered by OnRspUserLogin clashing with the background incremental subscriptions.
                    // (等行情登录完成再下发, 避免 OnRspUserLogin 触发的 reconnect-resubscribe
                    //  路径和后台增量订阅同时下发请求。)
                    for (int i = 0; i < 100 && !lazy_md_subscribe_stop_.load(std::memory_order_relaxed); ++i) {
                        if (md_gateway_ && md_gateway_->is_logged_in()) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (lazy_md_subscribe_stop_.load(std::memory_order_relaxed)) {
                        LOG_INFO("lazy_md_subscribe stopped before start, pending=" +
                                 std::to_string(cold.size()));
                        return;
                    }
                    // Batch of 200 + 50ms interval — gentler rate than startup sync path,
                    // avoids impacting hot-path tick delivery.
                    // (每批 200 个 + 50ms 间隔, 速率比启动同步路径更温和, 避免影响热路径 tick 推送)
                    constexpr size_t kLazyChunk = 200;
                    constexpr int kInterChunkSleepMs = 50;
                    size_t sent = 0;
                    const auto t0 = std::chrono::steady_clock::now();
                    for (size_t offset = 0;
                         offset < cold.size() &&
                         !lazy_md_subscribe_stop_.load(std::memory_order_relaxed);
                         offset += kLazyChunk) {
                        const size_t end = std::min(offset + kLazyChunk, cold.size());
                        std::vector<std::string> chunk(cold.begin() + offset, cold.begin() + end);
                        if (md_gateway_) md_gateway_->subscribe_append(chunk);
                        sent += chunk.size();
                        if (end < cold.size()) {
                            // Segmented sleep so stop signal can interrupt in time (分段 sleep 以便停止信号能及时打断)
                            for (int slept = 0;
                                 slept < kInterChunkSleepMs &&
                                 !lazy_md_subscribe_stop_.load(std::memory_order_relaxed);
                                 slept += 10) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            }
                        }
                    }
                    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    LOG_INFO("lazy_md_subscribe done: sent=" + std::to_string(sent) +
                             "/" + std::to_string(cold.size()) +
                             " elapsed_ms=" + std::to_string(elapsed_ms));
                }));
            thread_registry_->add("lazy_md_subscribe", &lazy_md_subscribe_thread_,
                [this]() { lazy_md_subscribe_stop_.store(true, std::memory_order_relaxed); }, 10);
        }
    }
    engine_start_trace("start:subscribe_done");
    LOG_INFO("行情订阅完成");
    session_mgr_.set_last_md_logged_in(md_gateway_->is_logged_in());
    // Check if all accounts are logged in (检查所有账户是否登录)
    close_mgr_.resume_pending_tasks(); // Resume pending close tasks (恢复挂起的平仓任务)
try_refresh_active_orders(); // Attempt to refresh active orders (尝试刷新活动委托)

    LOG_INFO("引擎启动完成");
    engine_start_trace("start:done");

#ifdef ENABLE_METRICS
    {
        int metrics_port = config_.get_int("Metrics", "Port", 9090);
        bool enable_control = config_.get_int("WebUI", "EnableControl", 0) != 0;
        if (metrics_port > 0) {
            web_server_.start(metrics_port, this, enable_control);
        }
    }
#endif

    return true;
}

void TradingEngine::stop() {
#ifdef ENABLE_METRICS
    web_server_.stop();
#endif
    strategy_ctrl_.set_global_state(StrategyState::Stopped); // Set strategy state to stopped (设置策略状态为停止)
    trading_ready_.store(false, std::memory_order_relaxed); // Set trade ready state to false (设置交易状态为未就绪)
if (running_) {
        LOG_INFO("引擎停止中");
        running_ = false;
        if (thread_registry_) {
            thread_registry_->shutdown_all();
            thread_registry_.reset();
        }
        if (md_gateway_) md_gateway_->stop(); // Stop market data gateway (停止行情网关)
        account_mgr_.stop_all(); // Stop all account trade gateways (停止所有账户的交易网关)
        {
            std::lock_guard<std::mutex> lock(snapshot_mtx_);
            for (auto* ctx : account_mgr_.all_accounts()) {
                set_account_trade_state(ctx, AccountTradeState::Stopped);
            }
        }
        save_runtime_state(); // Save runtime state (保存运行时状态)
LOG_INFO("引擎已停止");
    }
}

bool TradingEngine::add_strategy(std::shared_ptr<StrategyBase> strategy) {
    if (!strategy) {
        return false;
    }

    const std::string new_id = strategy->strategy_id().empty() ? "default" : strategy->strategy_id();
    std::lock_guard<std::mutex> lock(strategies_mtx_);
    for (const auto& existing : strategies_) {
        if (!existing) {
            continue;
        }
        const std::string existing_id = existing->strategy_id().empty() ? "default" : existing->strategy_id();
        if (existing_id == new_id) {
            LOG_WARN("strategy already loaded, skip duplicate: id=" + new_id);
            return false;
        }
    }

    strategy->set_engine(this);
    if (running_.load(std::memory_order_relaxed)) {
        try {
            strategy->on_init();
        } catch (const std::exception& e) {
            LOG_ERROR("strategy hotload on_init failed: id=" + new_id + " error=" + e.what());
            strategy->set_engine(nullptr);
            return false;
        } catch (...) {
            LOG_ERROR("strategy hotload on_init unknown failure: id=" + new_id);
            strategy->set_engine(nullptr);
            return false;
        }
    }

    strategies_.push_back(std::move(strategy));
    {
        const StrategyState initial_state = strategy_ctrl_.get_global_state();
        strategy_ctrl_.init_state(new_id, initial_state);
    }
    refresh_strategies_snapshot();
    return true;
}

bool TradingEngine::remove_strategy(const std::string& strategy_id) {
    const std::string target_id = trim_copy(strategy_id).empty() ? "default" : trim_copy(strategy_id);

    // Phase 1: find and extract the strategy, call on_stop()
    std::shared_ptr<StrategyBase> removed_strategy;
    {
        std::lock_guard<std::mutex> lock(strategies_mtx_);
        auto it = std::find_if(strategies_.begin(), strategies_.end(), [&](const auto& strategy) {
            if (!strategy) return false;
            const std::string existing_id = strategy->strategy_id().empty() ? "default" : strategy->strategy_id();
            return existing_id == target_id;
        });
        if (it == strategies_.end()) {
            LOG_WARN("strategy unload skipped, not loaded: id=" + target_id);
            return false;
        }
        removed_strategy = std::move(*it);
        strategies_.erase(it);
        refresh_strategies_snapshot();
    }

    // Phase 2: notify strategy before cleanup (outside strategies_mtx_)
    if (removed_strategy) {
        try {
            removed_strategy->on_stop();
        } catch (const std::exception& e) {
            LOG_ERROR("strategy on_stop exception: id=" + target_id + " error=" + e.what());
        } catch (...) {
            LOG_ERROR("strategy on_stop unknown exception: id=" + target_id);
        }
    }

    // Phase 3: cancel pending orders and conditional orders
    cancel_strategy_orders(target_id);

    // Phase 4: clean up strategy state
    strategy_ctrl_.remove_state(target_id);
    LOG_INFO("strategy unloaded: id=" + target_id);
    return true;
}

size_t TradingEngine::strategy_count() const {
    std::lock_guard<std::mutex> lock(strategies_mtx_);
    return strategies_.size();
}

// Must be called while holding strategies_mtx_.
// Rebuild a new immutable vector snapshot, then atomic_store. Reader side gets
// shared_ptr<const vector> via atomic_load — zero allocation, zero lock.
// (必须在持有 strategies_mtx_ 时调用。重建一个新的不可变 vector 快照, 然后
//  atomic_store。读者侧 atomic_load 得到 shared_ptr<const vector>, 零分配、零锁。)
void TradingEngine::refresh_strategies_snapshot() {
    auto fresh = std::make_shared<std::vector<std::shared_ptr<StrategyBase>>>(strategies_);
    std::atomic_store_explicit(
        &strategies_snapshot_ptr_,
        std::shared_ptr<const std::vector<std::shared_ptr<StrategyBase>>>(std::move(fresh)),
        std::memory_order_release);
}

std::shared_ptr<const std::vector<std::shared_ptr<StrategyBase>>>
TradingEngine::load_strategies_snapshot() const {
    auto snap = std::atomic_load_explicit(&strategies_snapshot_ptr_, std::memory_order_acquire);
    if (!snap) {
        // Very early, haven't initialized snapshot yet; fall back to empty vector.
        // (极早期还没初始化过快照, 回退到空 vector。)
        static const auto kEmpty =
            std::make_shared<const std::vector<std::shared_ptr<StrategyBase>>>();
        return kEmpty;
    }
    return snap;
}

// Receive market ticks and enqueue them for the consumer thread.
void TradingEngine::on_tick(const TickData& tick) {
    Event evt{};
    evt.type = EventType::Tick;
    evt.tick = tick;
    if (!md_queue_.push(evt)) {
        const bool first_overflow = !md_queue_overflow_detected_.exchange(true, std::memory_order_relaxed);
        const size_t drops = md_queue_.drop_count();
        if (drops <= 1 || drops % 10000 == 0) {
            LOG_WARN("行情队列已满，丢弃 tick: instrument=" + std::string(tick.instrument_id) +
                     " drop_count=" + std::to_string(drops));
        }
        if (!md_queue_drop_alerted_.exchange(true, std::memory_order_relaxed)) {
            record_alert("行情队列拥塞：已开始丢弃 tick，请检查订阅量、策略耗时和前端连接数");
        }
        // P0 safety linkage: market data queue overflow means strategies see
        // discontinuous data, potentially triggering incorrect signals. On first
        // overflow, auto-escalate to NoOpen to protect — allow close/cancel,
        // but prevent opening new positions under incomplete market data.
        // (P0 安全联动: 行情队列溢出意味着策略看到的行情不连续, 可能触发错误信号。
        //  首次溢出自动升级到 NoOpen 保护, 允许平仓/撤单, 但禁止在不完整行情下开新仓。)
        if (first_overflow) {
            // Async-notify consumer thread via cmd_queue_ to set risk mode,
            // because on_tick runs on the gateway thread and cannot directly manipulate engine state.
            // (通过 cmd_queue_ 异步通知 consumer 线程设置风控模式,
            //  因为 on_tick 在网关线程调用, 不能直接操作 engine 状态。)
            EngineCommand cmd{};
            cmd.type = CommandType::SetRiskMode;
            cmd.risk_mode = RiskMode::NoOpen;
            safe_copy(cmd.reason, "md queue overflow: tick data incomplete, open blocked",
                      sizeof(cmd.reason));
            cmd_queue_.push(cmd);
            push_runtime_alert("行情队列溢出：自动禁止开仓（NoOpen），平仓/撤单仍可用。恢复需人工确认后切回 Normal");
        }
    }
}

void TradingEngine::handle_trade_queue_overflow(AccountContext* ctx, const std::string& message) {
    const bool first_overflow = !trade_queue_overflow_detected_.exchange(true, std::memory_order_relaxed);
    queue_overflow_detected_.store(true, std::memory_order_relaxed);
    if (!first_overflow) {
        return;
    }
    strategy_ctrl_.set_global_state(StrategyState::Paused);
    if (ctx) {
        ctx->risk_mgr.set_risk_mode(RiskMode::Halted, "trade queue overflow");
        update_account_reject_state(ctx, OrderRejectReason::EngineNotReady, message);
        set_account_trade_state(ctx, AccountTradeState::GatewayDown);
    } else {
        set_risk_mode(RiskMode::Halted, "trade queue overflow");
    }
    push_runtime_alert(message + "，策略已暂停，风控已熔断，请人工核对委托/成交/持仓");
    request_async_save();
}

// Receive order returns and enqueue them for the matching account.
void TradingEngine::on_order(const OrderInfo& order) {
    AccountContext* ctx = resolve_account(order.account_id);
    if (!ctx) {
        LOG_WARN("收到未知账户的委托回报: " + std::string(order.account_id));
        return;
    }
    Event evt{};
    evt.type = EventType::Order;
    evt.order = order;
    if (!ctx->trade_queue.push(evt)) {
        const size_t drops = ctx->trade_queue.drop_count();
        const std::string message = "严重：交易队列已满，丢弃委托回报 ref=" + std::string(order.order_ref) +
            " instrument=" + std::string(order.instrument_id) +
            " acct=" + std::string(order.account_id) +
            " drop_count=" + std::to_string(drops) +
            " queue_size=" + std::to_string(ctx->trade_queue.size());
        if (drops <= 1 || drops % 1000 == 0) {
            LOG_ERROR(message + " - 持仓或委托状态可能不一致");
        }
        handle_trade_queue_overflow(ctx, message);
    }
}

// Receive trade returns and enqueue them for the matching account.
void TradingEngine::on_trade(const TradeInfo& trade) {
    AccountContext* ctx = resolve_account(trade.account_id);
    if (!ctx) {
        LOG_WARN("收到未知账户的成交回报: " + std::string(trade.account_id));
        return;
    }
    Event evt{};
    evt.type = EventType::Trade;
    evt.trade = trade;
    if (!ctx->trade_queue.push(evt)) {
        const size_t drops = ctx->trade_queue.drop_count();
        const std::string message = "严重：交易队列已满，丢弃成交回报 trade_id=" + std::string(trade.trade_id) +
            " instrument=" + std::string(trade.instrument_id) +
            " acct=" + std::string(trade.account_id) +
            " drop_count=" + std::to_string(drops) +
            " queue_size=" + std::to_string(ctx->trade_queue.size());
        if (drops <= 1 || drops % 1000 == 0) {
            LOG_ERROR(message + " - 持仓或委托状态可能不一致");
        }
        handle_trade_queue_overflow(ctx, message);
    }
}

// Receive account snapshots and enqueue them for the matching account.
void TradingEngine::on_account(const AccountInfo& account) {
    AccountContext* ctx = resolve_account(account.account_id);
    if (!ctx) {
        LOG_WARN("收到未知账户的资金快照: " + std::string(account.account_id));
        return;
    }
    Event evt{};
    evt.type = EventType::Account;
    evt.account = account;
    if (!ctx->trade_queue.push(evt)) {
        const std::string message = "严重：交易队列已满，丢弃资金快照 acct=" + std::string(account.account_id);
        LOG_ERROR(message);
        handle_trade_queue_overflow(ctx, message);
    }
}

// Receive position snapshots and enqueue them for the matching account.
void TradingEngine::on_position(const PositionInfo& pos) {
    AccountContext* ctx = resolve_account(pos.account_id);
    if (!ctx) {
        LOG_WARN("收到未知账户的持仓快照: " + std::string(pos.account_id));
        return;
    }
    Event evt{};
    evt.type = EventType::Position;
    evt.position = pos;
    if (!ctx->trade_queue.push(evt)) {
        const std::string message = "严重：交易队列已满，丢弃持仓快照 acct=" + std::string(pos.account_id) +
            " instrument=" + std::string(pos.instrument_id);
        LOG_ERROR(message);
        handle_trade_queue_overflow(ctx, message);
    }
}

// Receive cancel rejects and enqueue for the consumer thread.
void TradingEngine::on_cancel_rejected(const std::string& account_id, const std::string& order_ref,
                                       const std::string& reason) {
    AccountContext* ctx = resolve_account(account_id);
    if (!ctx) return;
    Event evt{};
    evt.type = EventType::CancelRejected;
    safe_copy(evt.cancel_reject.account_id, account_id.c_str(), sizeof(evt.cancel_reject.account_id));
    safe_copy(evt.cancel_reject.order_ref, order_ref.c_str(), sizeof(evt.cancel_reject.order_ref));
    safe_copy(evt.cancel_reject.reason, reason.c_str(), sizeof(evt.cancel_reject.reason));
    if (!ctx->trade_queue.push(evt)) {
        LOG_WARN("trade queue full, cancel_rejected inline ref=" + order_ref);
        close_mgr_.on_cancel_rejected(account_id.c_str(), order_ref.c_str(), reason.c_str());
    }
}

void TradingEngine::on_gateway_error(const std::string& account_id, OrderRejectReason reason,
                                     const std::string& message) {
    AccountContext* ctx = account_mgr_.find_account(account_id);
    if (!ctx) {
        return;
    }
    update_account_reject_state(ctx, reason, message);
    set_account_trade_state(ctx, AccountTradeState::GatewayDown);
    record_alert(message);
}

// ============================================
// ConfigStore overlay: values in SQLite override config.ini.
// ============================================
void TradingEngine::apply_config_store_overlay() {
    if (!store_) return;

    //
    const auto acct_bundle = store_->load_account_bundle();
    if (!acct_bundle.accounts.empty()) {
        //
        std::string acct_list;
        for (size_t i = 0; i < acct_bundle.accounts.size(); ++i) {
            if (i > 0) acct_list += ",";
            acct_list += acct_bundle.accounts[i].account_id;
        }
        config_.set_string("Accounts", "List", acct_list);
        config_.set_string("Accounts", "MarketDataAccount", acct_bundle.market_data_account_id);
        for (const auto& acct : acct_bundle.accounts) {
            const std::string section = "CTP." + acct.account_id;
            config_.set_string("Accounts", acct.account_id + ".Gateway", acct.gateway_type);
            config_.set_string(section, "BrokerID", acct.broker_id);
            config_.set_string(section, "UserID", acct.user_id);
            config_.set_string(section, "Password", crypto::encrypt_config_value(acct.password));
            config_.set_string(section, "AppID", acct.app_id);
            if (!acct.auth_code.empty()) config_.set_string(section, "AuthCode", crypto::encrypt_config_value(acct.auth_code));
            config_.set_string(section, "TradeFront", acct.trade_front);
            config_.set_string(section, "MarketFront", acct.market_front);
        }
        LOG_INFO("配置库覆盖: " + std::to_string(acct_bundle.accounts.size()) + " 个账户");
    }

    // 2. 瑕嗙洊绛栫暐閰嶇疆
    const auto strategy_specs = store_->load_strategy_specs();
    if (!strategy_specs.empty()) {
        std::string strat_list;
        for (size_t i = 0; i < strategy_specs.size(); ++i) {
            if (i > 0) strat_list += ",";
            strat_list += strategy_specs[i].id;
        }
        config_.set_string("Strategies", "List", strat_list);
        for (const auto& spec : strategy_specs) {
            const std::string section = "Strategy." + spec.id;
            config_.set_string(section, "Type", spec.type);
            config_.set_string(section, "ScriptPath", spec.script_path);
            config_.set_string(section, "AccountID", spec.account_id);
            config_.set_string(section, "Instruments", join_csv(spec.instruments));
            config_.set_string(section, "OrderSize", std::to_string(spec.order_size));
            config_.set_string(section, "MomentumTicks", std::to_string(spec.momentum_ticks));
            config_.set_string(section, "CooldownSeconds", std::to_string(spec.cooldown_seconds));
            for (const auto& [k, v] : spec.params) {
                config_.set_string(section, "Param." + k, v);
            }
        }
        LOG_INFO("配置库覆盖: " + std::to_string(strategy_specs.size()) + " 个策略");
    }

    // 3. 瑕嗙洊鍚堢害閰嶇疆
    const auto inst_specs = store_->load_instrument_specs();
    if (!inst_specs.empty()) {
        for (const auto& spec : inst_specs) {
            const std::string section = "Instrument." + spec.instrument_id;
            config_.set_string(section, "ExchangeID", spec.exchange_id);
            config_.set_string(section, "ProductID", spec.product_id);
            config_.set_string(section, "PriceTick", std::to_string(spec.price_tick));
            config_.set_string(section, "VolumeMultiple", std::to_string(spec.volume_multiple));
            config_.set_string(section, "LongMarginRatio", std::to_string(spec.long_margin_ratio));
            config_.set_string(section, "ShortMarginRatio", std::to_string(spec.short_margin_ratio));
            config_.set_string(section, "OpenCommissionRate", std::to_string(spec.open_commission_rate));
            config_.set_string(section, "CloseCommissionRate", std::to_string(spec.close_commission_rate));
            config_.set_string(section, "CloseTodayCommissionRate", std::to_string(spec.close_today_commission_rate));
        }
        LOG_INFO("配置库覆盖: " + std::to_string(inst_specs.size()) + " 个合约参数");
    }

    // 4. 瑕嗙洊椋庢帶閰嶇疆
    const auto risk_kvs = store_->load_all_risk_config();
    if (!risk_kvs.empty()) {
        for (const auto& [k, v] : risk_kvs) {
            if (k == "TradingSessions") {
                config_.set_string("Trading", "TradingSessions", v);
            } else {
                config_.set_string("Risk", k, v);
            }
        }
        LOG_INFO("配置库覆盖: " + std::to_string(risk_kvs.size()) + " 个风控配置项");
    }
}

void TradingEngine::rebuild_hot_instruments() {
    instrument_registry_.rebuild_hot(
        collect_strategy_instruments(config_),
        split_csv(config_.get_string("MarketData", "HotInstruments", "")),
        cond_order_mgr_.get_active_instruments());
}

bool TradingEngine::is_hot_instrument(const char* instrument) const {
    return instrument_registry_.is_hot(instrument);
}

bool TradingEngine::register_hot_instrument(const std::string& instrument) {
    return instrument_registry_.register_hot(instrument);
}

void TradingEngine::apply_runtime_performance_config() {
    production_hft_mode_ = config_.get_int("Performance", "ProductionHftMode", 0) > 0;
    md_batch_size_ = static_cast<size_t>((std::max)(
        1, config_.get_int("Performance", "MdBatchSize", production_hft_mode_ ? 512 : static_cast<int>(kDefaultMdBatchSize))));
    md_batch_size_ = (std::min)(md_batch_size_, kMaxMdBatchSize);

    hft_disable_python_hot_path_ =
        config_.get_int("Performance", "DisablePythonHotPath", production_hft_mode_ ? 1 : 0) > 0;
    if (production_hft_mode_) {
        hft_disable_python_hot_path_ = true;
    }
    hft_disable_tick_recording_ =
        config_.get_int("Performance", "DisableTickRecordingHotPath", production_hft_mode_ ? 1 : 0) > 0;
    hft_disable_kline_hot_path_ =
        config_.get_int("Performance", "DisableKlineHotPath", production_hft_mode_ ? 1 : 0) > 0;
    hft_strategy_hot_instruments_only_ =
        config_.get_int("Performance", "StrategyHotInstrumentsOnly", 1) > 0;

    engine_cpu_core_ = config_.get_int("Performance", "EngineCpuCore", -1);
    gateway_cpu_core_ = config_.get_int("Performance", "GatewayCoreId", -1);
    logger_cpu_core_ = config_.get_int("Performance", "LoggerCpuCore", -1);

    LOG_INFO("performance config: production_hft=" + std::to_string(production_hft_mode_ ? 1 : 0) +
             " md_batch_size=" + std::to_string(md_batch_size_) +
             " disable_python_hot_path=" + std::to_string(hft_disable_python_hot_path_ ? 1 : 0) +
             " disable_tick_recording_hot_path=" + std::to_string(hft_disable_tick_recording_ ? 1 : 0) +
             " disable_kline_hot_path=" + std::to_string(hft_disable_kline_hot_path_ ? 1 : 0) +
             " strategy_hot_instruments_only=" + std::to_string(hft_strategy_hot_instruments_only_ ? 1 : 0) +
             " engine_cpu_core=" + std::to_string(engine_cpu_core_) +
             " logger_cpu_core=" + std::to_string(logger_cpu_core_));
}

// Consumer loop
void TradingEngine::consumer_loop() {
    LOG_INFO("consumer_loop thread started");
    LOG_INFO("consumer_loop 线程启动");
  try {
    Event evt{};
    EngineCommand cmd{};
    int idle_spins = 0; // Idle spin counter (空转计数器)
    auto last_timeout_check = std::chrono::steady_clock::now();
    auto last_pnl_snapshot = std::chrono::steady_clock::now();
while (running_) {
        consumer_heartbeat_ms_.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            std::memory_order_relaxed);
        bool got_event = false;

        // 1. 处理所有积压的命令
        while (cmd_queue_.pop(cmd)) {
            got_event = true;
            process_command(cmd);
        }

        // Account manager
        for (auto* ctx : account_mgr_.all_accounts()) {
            while (ctx->trade_queue.pop(evt)) {
                got_event = true;
                switch (evt.type) {
                    case EventType::Order: process_order(evt.order); break;
                    case EventType::Trade: process_trade(evt.trade); break;
                    case EventType::Account: process_account(evt.account); break;
                    case EventType::Position: process_position(evt.position); break;
                    case EventType::CancelRejected:
                        close_mgr_.on_cancel_rejected(
                            evt.cancel_reject.account_id,
                            evt.cancel_reject.order_ref,
                            evt.cancel_reject.reason);
                        break;
                    default: break;
                }
            }
        }

        // 3. 批量处理行情事件，避免极端行情 burst 时每轮只消费 1 个 tick 造成积压。
        //    每 64 个 tick 穿插检查命令队列，确保紧急命令（如 EmergencyClose）不被大批量行情阻塞。
        size_t md_processed = 0;
        while (md_processed < md_batch_size_ && md_queue_.pop(evt)) {
            got_event = true;
            ++md_processed;
            if (evt.type == EventType::Tick) {
                process_tick(evt.tick);
            }
            if ((md_processed & 63) == 0) {
                while (cmd_queue_.pop(cmd)) {
                    process_command(cmd);
                }
            }
        }

        // Periodic tasks: use absolute time trigger to ensure execution even during active market data periods.
        // (定时任务: 使用绝对时间触发, 确保行情活跃期也能执行)
        const auto now = std::chrono::steady_clock::now();
        if (now - last_timeout_check >= std::chrono::seconds(1)) {
            last_timeout_check = now;
            close_mgr_.check_timeout();
            check_runtime_alerts();
            algo_order_mgr_.tick([this](const OrderRequest& req) {
                return send_order_with_result(req);
            });

            // Check strategy timers
            if (!timers_.empty()) {
                auto snapshot = load_strategies_snapshot();
                for (auto& timer : timers_) {
                    if (now >= timer.next_fire) {
                        timer.next_fire = now + std::chrono::milliseconds(timer.interval_ms);
                        if (snapshot) {
                            for (auto& s : *snapshot) {
                                if (s->strategy_id() == timer.strategy_id) {
                                    try {
                                        s->on_timer(timer.id);
                                    } catch (const std::exception& e) {
                                        LOG_ERROR("[" + timer.strategy_id + "] on_timer exception: " + e.what());
                                        auto_pause_on_error(timer.strategy_id);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (now - last_pnl_snapshot >= std::chrono::seconds(60)) {
            last_pnl_snapshot = now;
            snapshot_pnl_curve();
        }

        // 4. 如果没有事件处理，执行后台任务并稍作休眠
        if (HFT_UNLIKELY(!got_event)) {
            try_refresh_active_orders();
            ++idle_spins;
            if (session_mgr_.last_in_trading_session()) {
                if (idle_spins < 100) {
                    HFT_CPU_PAUSE();
                } else if (idle_spins < 1000) {
                    std::this_thread::yield();
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        } else {
            idle_spins = 0;
        }
    }
  } catch (const std::exception& e) {
    running_.store(false, std::memory_order_release);
    LOG_ERROR(std::string("consumer_loop 异常崩溃: ") + e.what());
    record_alert("FATAL: consumer_loop crashed: " + std::string(e.what()));
    for (auto* ctx : account_mgr_.all_accounts()) {
        ctx->risk_mgr.set_risk_mode(RiskMode::Halted, "consumer_loop crash");
        try {
            for (const auto& order : ctx->order_mgr.get_active_orders()) {
                try_cancel_order(std::string(order.order_ref), ctx->account_id);
            }
        } catch (...) {}
    }
    try { save_runtime_state(); } catch (...) {}
  } catch (...) {
    running_.store(false, std::memory_order_release);
    LOG_ERROR("consumer_loop 未知异常崩溃");
    record_alert("FATAL: consumer_loop crashed with unknown exception");
    for (auto* ctx : account_mgr_.all_accounts()) {
        ctx->risk_mgr.set_risk_mode(RiskMode::Halted, "consumer_loop crash");
        try {
            for (const auto& order : ctx->order_mgr.get_active_orders()) {
                try_cancel_order(std::string(order.order_ref), ctx->account_id);
            }
        } catch (...) {}
    }
    try { save_runtime_state(); } catch (...) {}
  }
    LOG_INFO("consumer_loop exit");
}

void TradingEngine::auto_pause_on_error(const std::string& strategy_id) {
    const auto result = strategy_ctrl_.auto_pause_on_error(strategy_id);
    if (result.paused) {
        record_alert(result.message);
        request_async_save();
    }
}

void TradingEngine::process_tick(const TickData& tick) {
    // ZT-05: Basic tick data validation to filter corrupt CTP data
    if (HFT_UNLIKELY(tick.instrument_id[0] == '\0' ||
        !std::isfinite(tick.last_price) || tick.last_price <= 0.0 ||
        tick.volume < 0)) {
        return;
    }
    if (HFT_UNLIKELY(tick.upper_limit > 0.0 && tick.last_price > tick.upper_limit * 1.01)) return;
    if (HFT_UNLIKELY(tick.lower_limit > 0.0 && tick.last_price < tick.lower_limit * 0.99)) return;

    const long long process_start_us = steady_us_now();
    processed_tick_count_.fetch_add(1, std::memory_order_relaxed);

    // Hot path: only do necessary tick caching and conditional order checks; move rest outside lock.
    // (热路径: 只做必要的 tick 缓存和条件单检查, 其余移到锁外)
    tick_data_mgr_.update(tick);
    order_book_mgr_.update(tick);

    // Update the last tick receive timestamp.
    session_mgr_.update_last_tick_time();
    session_mgr_.set_no_tick_alerted(false);

    const bool hot = is_hot_instrument(tick.instrument_id);
    if (HFT_UNLIKELY(!hot && hft_strategy_hot_instruments_only_)) {
        const long long process_elapsed_us = (std::max)(0LL, steady_us_now() - process_start_us);
        last_tick_process_us_.store(process_elapsed_us, std::memory_order_relaxed);
        return;
    }

    if (hot) {
        // Order manager
            // 3-phase check_tick
        auto cond_result = cond_order_mgr_.check_tick(tick, [this](const OrderRequest& req, std::string& reason) {
            return submit_conditional_order(req, reason);
        });

        for (const uint32_t gid : cond_result.triggered_group_ids) {
            cond_order_mgr_.cancel_group(gid);
        }
        if (cond_result.changed) {
        // async_save: periodic timer handles this
        }
        // close_mgr_.check_timeout() 已由 consumer_loop 的 1 秒 periodic 调用 (line ~1281)
        // Removed per-tick duplicate call to reduce hot-path mutex + vector allocation.
        // (删除每 tick 的重复调用以减少热路径 mutex + vector 分配)
    }

    // dispatch tick to running strategies only
    if (hot && !md_only_mode_.load(std::memory_order_relaxed) && strategy_ctrl_.get_global_state() == StrategyState::Running) {
        // Hot-path lock-free snapshot: atomic_load gets immutable strategies vector, zero allocation.
        // (热路径无锁快照: atomic_load 拿到不可变 strategies vector, 零分配)
        auto strategies_snapshot = load_strategies_snapshot();
        const bool need_per_strategy_state =
            strategy_ctrl_.non_running_count() > 0;

        // GIL batch: acquire interpreter lock once for all Python strategies
        std::unique_ptr<StrategyBase::InterpreterLockGuard> interp_lock;
        if (!hft_disable_python_hot_path_) {
            for (const auto& strategy : *strategies_snapshot) {
                if (strategy->is_interpreted()) {
                    interp_lock = strategy->acquire_interpreter_lock();
                    break;
                }
            }
        }

        for (const auto& strategy : *strategies_snapshot) {
            if (!strategy->handles_instrument(tick.instrument_id)) {
                continue;
            }
            if (hft_disable_python_hot_path_ && strategy->is_python()) {
                continue;
            }
            if (need_per_strategy_state) {
                const std::string& sid =
                    strategy->strategy_id().empty() ? kDefaultStrategyId : strategy->strategy_id();
                if (strategy_ctrl_.get_state(sid) != StrategyState::Running) {
                    continue;
                }
            }
            try {
                strategy->on_tick(tick);
                strategy_ctrl_.reset_error_count(strategy->strategy_id());
            } catch (const std::exception& e) {
                LOG_ERROR("on_tick exception: strategy=" + strategy->strategy_id() +
                          " instrument=" + std::string(tick.instrument_id) +
                          " error=" + e.what());
                auto_pause_on_error(strategy->strategy_id());
            } catch (...) {
                LOG_ERROR("on_tick unknown: strategy=" + strategy->strategy_id() +
                          " instrument=" + std::string(tick.instrument_id));
                auto_pause_on_error(strategy->strategy_id());
            }
        }
    }

    // Full-path: include kline/tick-recording in measured latency unless production mode removes them.
    if (!hft_disable_kline_hot_path_) {
        auto completed_bars = kline_mgr_.update_from_tick(tick);
        kline_mgr_.maybe_persist(process_start_us);

        if (!completed_bars.empty() && !md_only_mode_.load(std::memory_order_relaxed)
            && strategy_ctrl_.get_global_state() == StrategyState::Running) {
            auto bar_snapshot = load_strategies_snapshot();
            if (bar_snapshot) {
                for (const auto& cb : completed_bars) {
                    for (auto& strategy : *bar_snapshot) {
                        if (!strategy->handles_instrument(cb.instrument.c_str())) continue;
                        if (strategy_ctrl_.get_state(strategy->strategy_id()) != StrategyState::Running) continue;
                        try {
                            strategy->on_bar(cb.instrument, cb.period, cb.bar);
                        } catch (const std::exception& e) {
                            LOG_ERROR("on_bar exception: strategy=" + strategy->strategy_id() +
                                      " instrument=" + cb.instrument + " period=" + cb.period +
                                      " error=" + e.what());
                            auto_pause_on_error(strategy->strategy_id());
                        }
                    }
                }
            }
        }
    }
    if (!hft_disable_tick_recording_) {
        tick_recorder_.record(tick);
    }

    const long long end_us = steady_us_now();
    const long long process_elapsed_us = (std::max)(0LL, end_us - process_start_us);
    last_tick_process_us_.store(process_elapsed_us, std::memory_order_relaxed);
    last_tick_to_signal_us_.store(process_elapsed_us, std::memory_order_relaxed);
    last_signal_steady_us_.store(end_us, std::memory_order_relaxed);
}

void TradingEngine::process_order(const OrderInfo& order) {
    const long long process_start_us = steady_us_now();
    OrderInfo routed_order = order;

    AccountContext* ctx = resolve_account(routed_order.account_id);
    if (!ctx) {
        LOG_INFO("runtime message");
        return;
    }

    ctx->order_mgr.on_order_return(routed_order);
    ctx->order_mgr.enrich_order_info(routed_order);
    close_mgr_.on_order(routed_order);
    if (store_ && !production_hft_mode_) {
        store_->async_update_order(routed_order);
    }
    {
        std::unique_lock<std::shared_mutex> lock(orders_history_mtx_);
        recent_orders_.push_front(routed_order);
        while (recent_orders_.size() > 1000) {
            recent_orders_.pop_back();
        }
    }
    request_async_save();  // Async save, don't block hot path (异步保存, 不阻塞热路径)

    if (routed_order.status == OrderStatus::Cancelled) {
        ctx->risk_mgr.on_cancel();
    }

    auto strategies_snapshot = load_strategies_snapshot();
    if (!strategies_snapshot) return;

    // GIL batch: acquire interpreter lock once for all Python strategies
    std::unique_ptr<StrategyBase::InterpreterLockGuard> interp_lock;
    for (const auto& strategy : *strategies_snapshot) {
        if (strategy->is_interpreted()) {
            interp_lock = strategy->acquire_interpreter_lock();
            break;
        }
    }

    if (routed_order.strategy_id[0] == '\0' && strategies_snapshot->size() > 1) {
        LOG_WARN("skip unscoped order callback in multi-strategy mode: ref=" +
                 std::string(routed_order.order_ref) +
                 " instrument=" + std::string(routed_order.instrument_id) +
                 " acct=" + ctx->account_id);
    } else {
        for (const auto& strategy : *strategies_snapshot) {
            if (!strategy->handles_strategy(routed_order.strategy_id)) {
                continue;
            }
            if (!strategy->handles_event(routed_order.account_id, routed_order.instrument_id)) {
                continue;
            }
            try {
                strategy->on_order(routed_order);
            } catch (const std::exception& e) {
                LOG_ERROR("策略 on_order 异常: strategy=" + strategy->strategy_id() +
                          " ref=" + std::string(routed_order.order_ref) +
                          " error=" + e.what());
            } catch (...) {
                LOG_ERROR("未知策略 on_order 异常: strategy=" + strategy->strategy_id() +
                          " ref=" + std::string(routed_order.order_ref));
            }
        }
    }

    last_order_process_us_.store((std::max)(0LL, steady_us_now() - process_start_us), std::memory_order_relaxed);

}

void TradingEngine::process_trade(const TradeInfo& trade) {
    const long long process_start_us = steady_us_now();
    TradeInfo routed_trade = trade;

    AccountContext* ctx = resolve_account(routed_trade.account_id);
    if (!ctx) {
        LOG_INFO("runtime message");
        return;
    }

    ctx->order_mgr.on_trade_return(routed_trade);
    ctx->order_mgr.enrich_trade_info(routed_trade);
    ctx->position_mgr.on_trade(routed_trade);
    close_mgr_.on_trade(routed_trade);
    algo_order_mgr_.on_trade(std::string(routed_trade.order_ref), routed_trade.volume);
    if (store_ && !production_hft_mode_) {
        store_->async_insert_trade(routed_trade);
    }
    {
        const std::string strategy_id = routed_trade.strategy_id[0] == '\0' ? "manual" : routed_trade.strategy_id;
        strategy_ctrl_.record_trade(strategy_id);
    }
    if (routed_trade.offset == Offset::Open) {
        const std::string strategy_id = routed_trade.strategy_id[0] == '\0' ? "manual" : routed_trade.strategy_id;
        const std::string trade_time = routed_trade.trade_time[0] ? routed_trade.trade_time : "";
        strategy_ctrl_.record_open_position(strategy_id, trade_time);
    }
    {
        for (auto& entry : order_latency_ring_) {
            if (entry.order_ref[0] != '\0' &&
                std::strncmp(entry.order_ref, routed_trade.order_ref, sizeof(entry.order_ref)) == 0) {
                last_order_to_trade_us_.store(
                    (std::max)(0LL, process_start_us - entry.sent_us),
                    std::memory_order_relaxed);
                entry.order_ref[0] = '\0';
                break;
            }
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(trades_mtx_);
        recent_trades_.push_front(routed_trade);
        while (recent_trades_.size() > 500) {
            recent_trades_.pop_back();
        }
    }
    request_async_save();  // Async save, don't block hot path (异步保存, 不阻塞热路径)

    auto strategies_snap = load_strategies_snapshot();
    if (!strategies_snap) {
        last_trade_process_us_.store((std::max)(0LL, steady_us_now() - process_start_us),
                                     std::memory_order_relaxed);
        return;
    }

    // GIL batch: acquire interpreter lock once for all Python strategies
    std::unique_ptr<StrategyBase::InterpreterLockGuard> interp_lock;
    for (const auto& strategy : *strategies_snap) {
        if (strategy->is_interpreted()) {
            interp_lock = strategy->acquire_interpreter_lock();
            break;
        }
    }

    if (routed_trade.strategy_id[0] == '\0' && strategies_snap->size() > 1) {
        LOG_WARN("skip unscoped trade callback in multi-strategy mode: instrument=" +
                 std::string(routed_trade.instrument_id) +
                 " trade_id=" + std::string(routed_trade.trade_id) +
                 " acct=" + ctx->account_id);
    } else {
        for (const auto& strategy : *strategies_snap) {
            if (!strategy->handles_strategy(routed_trade.strategy_id)) {
                continue;
            }
            if (!strategy->handles_event(routed_trade.account_id, routed_trade.instrument_id)) {
                continue;
            }
            try {
                strategy->on_trade(routed_trade);
            } catch (const std::exception& e) {
                LOG_ERROR("策略 on_trade 异常: strategy=" + strategy->strategy_id() +
                          " instrument=" + std::string(routed_trade.instrument_id) +
                          " error=" + e.what());
            } catch (...) {
                LOG_ERROR("未知策略 on_trade 异常: strategy=" + strategy->strategy_id() +
                          " instrument=" + std::string(routed_trade.instrument_id));
            }
        }
    }

    last_trade_process_us_.store((std::max)(0LL, steady_us_now() - process_start_us), std::memory_order_relaxed);

}

void TradingEngine::process_account(const AccountInfo& account) {
    AccountContext* ctx = resolve_account(account.account_id);
    if (!ctx) {
        LOG_INFO("runtime message");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(ctx->account_mtx);
        ctx->account_info = account;
    }
    ctx->risk_mgr.update_account(account);

    PnlCurvePoint point;
    point.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    point.balance = account.balance;
    point.available = account.available;
    point.margin = account.margin;
    point.position_profit = account.position_profit;
    point.total_pnl = account.close_profit + account.position_profit;
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &now);
#else
        localtime_r(&now, &tm_buf);
#endif
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                      tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        point.time = buf;
    }
    {
        std::lock_guard<std::mutex> lock(pnl_curve_mtx_);
        pnl_curve_.push_back(point);
        while (pnl_curve_.size() > 1440) {
            pnl_curve_.pop_front();
        }
    }
    save_runtime_cache();
}

void TradingEngine::snapshot_pnl_curve() {
    AccountInfo account = get_account("");
    if (account.balance <= 0 && account.available <= 0) return;

    PnlCurvePoint point;
    point.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    point.balance = account.balance;
    point.available = account.available;
    point.margin = account.margin;
    point.position_profit = account.position_profit;
    point.total_pnl = account.close_profit + account.position_profit;
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &now);
#else
        localtime_r(&now, &tm_buf);
#endif
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                      tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        point.time = buf;
    }
    {
        std::lock_guard<std::mutex> lock(pnl_curve_mtx_);
        pnl_curve_.push_back(point);
        while (pnl_curve_.size() > 1440) {
            pnl_curve_.pop_front();
        }
    }
}

void TradingEngine::process_position(const PositionInfo& pos) {
    AccountContext* ctx = resolve_account(pos.account_id);
    if (!ctx) {
        LOG_INFO("runtime message");
        return;
    }
    ctx->position_mgr.update_position(pos.instrument_id, pos.direction, pos);
    {
        double total_floating = 0.0;
        const auto positions = account_mgr_.get_all_positions();
        for (const auto& item : positions) {
            total_floating += item.position_profit;
        }
        strategy_ctrl_.update_floating_pnl(total_floating);
    }
}

// Block
void TradingEngine::send_order(const OrderRequest& req) {
    std::string unused_ref;
    send_order(req, unused_ref);
}

std::string TradingEngine::send_order_with_ref(const OrderRequest& req) {
    std::string order_ref;
    send_order(req, order_ref);
    return order_ref;
}

namespace {
// Terminal reject reason whitelist: conditional orders encountering these rejects
// go directly to Cancelled instead of entering retry-backoff storm.
// Design principle: only reasons that won't resolve over time (config error / position exhausted /
// engine Halt) are terminal; rate-limit, cancel-rate, transient gateway errors are transient
// and still use RetryLater.
// (终态拒单原因白名单: 条件单遇到这些 reject 直接 Cancelled, 不进入 retry-backoff 风暴。
//  设计原则: 原因不会随时间消失 配置错误/持仓已耗尽/引擎 Halt 才算终态;
//  流控、撤单率、网关短暂错误属于瞬态, 仍走 RetryLater。)
bool is_terminal_reject_reason(const std::string& reason) {
    if (reason.empty()) return false;
    static const char* kTerminalSubstrings[] = {
        "insufficient closeable position",  // Insufficient closeable position — from CTP or local (可平持仓不足, CTP/local 都可能返回)
        "可用持仓不足",
        "平今仓位不足",
        "max_order_size",                   // Per-order limit is a config item; retry won't make it smaller (单笔上限是配置项, 重试不会变小)
        "InvalidParam",
        "empty instrument_id",
        "invalid volume",
        "RMS:",                             // Engine is Halt/Frozen; retry won't pass either (引擎被 Halt/Frozen, 重试也通不过)
    };
    for (const char* needle : kTerminalSubstrings) {
        if (reason.find(needle) != std::string::npos) return true;
    }
    return false;
}
} // namespace

ConditionalTriggerResult TradingEngine::submit_conditional_order(const OrderRequest& req, std::string& reason) {
    std::string order_ref;
    std::string reject_reason;
    std::string reject_message;
    send_order_unlocked(req, order_ref, &reject_reason, &reject_message);
    if (!order_ref.empty()) {
        return ConditionalTriggerResult::Sent;
    }

    AccountContext* ctx = resolve_account(req.account_id);
    if (!ctx) {
        reason = "account not found";
        return ConditionalTriggerResult::Cancelled;
    }

    // Position fallback: even if risk doesn't block it, retry shouldn't happen when local position is insufficient.
    // (持仓兜底: 即使 risk 未拦截, 本地持仓不足时也不应重试。)
    if (req.offset != Offset::Open) {
        const Direction pos_direction = (req.direction == Direction::Buy) ? Direction::Sell : Direction::Buy;
        const PositionInfo pos = ctx->position_mgr.get_position(req.instrument_id, pos_direction);
        if (pos.total < req.volume) {
            reason = "可用持仓不足";
            record_alert("条件单自动撤单：可用持仓不足  account=" + ctx->account_id +
                         ", instrument=" + std::string(req.instrument_id));
            return ConditionalTriggerResult::Cancelled;
        }
    }

    // Distinguish terminal vs transient reject — avoid retrying 10 times on the same config/position error and flooding logs.
    // (区分终态 vs 瞬态 reject — 避免被相同的配置/持仓错误重试 10 次刷屏。)
    if (is_terminal_reject_reason(reject_message) || is_terminal_reject_reason(reject_reason)) {
        reason = reject_message.empty() ? reject_reason : reject_message;
        record_alert("条件单自动撤单：" + reason + "  account=" + ctx->account_id +
                     ", instrument=" + std::string(req.instrument_id));
        return ConditionalTriggerResult::Cancelled;
    }

    reason = reject_message.empty() ? "order failed, retry later" : reject_message;
    return ConditionalTriggerResult::RetryLater;
}

//
void TradingEngine::send_order(const OrderRequest& req, std::string& out_order_ref) {
    // Architecture constraint: all send_order calls come from the consumer thread (strategy
    // on_tick/on_order/on_trade callbacks, conditional order triggers, close tasks). Under
    // consumer single-threaded access, the original send_order_mtx_ is no longer necessary;
    // lower-level order_mgr/risk_mgr fine-grained locks are only for cross-thread query APIs.
    // (架构约束: 所有 send_order 调用均来自 consumer 线程 策略 on_tick/on_order/on_trade
    //  回调、条件单触发、平仓任务。consumer 单线程访问下, 原 send_order_mtx_ 不再必要;
    //  下层 order_mgr/risk_mgr 各自的细粒度锁仅用于跨线程的查询接口。)
    send_order_unlocked(req, out_order_ref);
}

void TradingEngine::send_order_unlocked(const OrderRequest& req, std::string& out_order_ref,
                                        std::string* out_reject_reason,
                                        std::string* out_reject_message) {
    out_order_ref.clear();

    auto set_reject = [&](const std::string& reason, const std::string& message) {
        if (out_reject_reason) *out_reject_reason = reason;
        if (out_reject_message) *out_reject_message = message;
    };

    if (HFT_UNLIKELY(req.instrument_id[0] == '\0')) {
        LOG_ERROR("send_order rejected: empty instrument_id");
        set_reject("InvalidParam", "empty instrument_id");
        return;
    }
    if (HFT_UNLIKELY(req.volume <= 0)) {
        LOG_ERROR("send_order rejected: invalid volume=" + std::to_string(req.volume));
        set_reject("InvalidParam", "invalid volume=" + std::to_string(req.volume));
        return;
    }
    if (HFT_UNLIKELY(req.price_type == OrderRequest::PriceType::Limit && req.price <= 0.0)) {
        LOG_ERROR("send_order rejected: limit order with invalid price=" + std::to_string(req.price));
        set_reject("InvalidParam", "limit order with invalid price");
        return;
    }

    // Account manager
    AccountContext* ctx = account_mgr_.route_order(req);
    if (!ctx) {
        LOG_INFO("runtime message");
        return;
    }

    if (md_only_mode_.load(std::memory_order_relaxed)) {
        const std::string message = "engine in md-only mode, account=" +
            (ctx->account_id.empty() ? std::string("default") : ctx->account_id);
        update_account_reject_state(ctx, OrderRejectReason::EngineNotReady, message);
        set_reject("EngineNotReady", message);
        LOG_INFO("引擎拒绝下单，原因：当前为纯行情模式");
        return;
    }

    const AccountTradeState trade_state =
        get_account_trade_state_impl(ctx, running_.load(std::memory_order_relaxed));
    if (trade_state != AccountTradeState::Ready) {
        OrderRejectReason reason = OrderRejectReason::AccountNotReady;
        if (trade_state == AccountTradeState::ReconnectSync) {
            reason = OrderRejectReason::ReconnectSync;
        } else if (trade_state == AccountTradeState::GatewayDown ||
                   trade_state == AccountTradeState::LoginPending) {
            reason = OrderRejectReason::GatewayDisconnected;
        }

        const std::string message = "account trade state is " + std::string(to_string(trade_state)) +
            ", account=" + (ctx->account_id.empty() ? std::string("default") : ctx->account_id);
        update_account_reject_state(ctx, reason, message);
        set_reject(to_string(reason), message);
        LOG_INFO("runtime message");
        return;
    }

    clear_account_reject_state(ctx);

    OrderRequest actual_req = req;
    //
    if (actual_req.offset == Offset::Close && need_close_today_flag(actual_req.exchange_id)) {
        //
const Direction close_dir = (actual_req.direction == Direction::Buy) ? Direction::Sell : Direction::Buy;
        const PositionInfo pos = ctx->position_mgr.get_position(actual_req.instrument_id, close_dir);
        //
        const int reserved_today =
            ctx->order_mgr.get_pending_close_volume(actual_req.instrument_id, close_dir, Offset::CloseToday);
        const int reserved_yesterday =
            ctx->order_mgr.get_pending_close_volume(actual_req.instrument_id, close_dir, Offset::CloseYesterday);
        //
        const int available_today = (std::max)(0, pos.today - reserved_today);
        const int available_yesterday = (std::max)(0, pos.yesterday - reserved_yesterday);

        if (actual_req.volume <= available_today) {
            actual_req.offset = Offset::CloseToday;
        } else if (actual_req.volume <= available_yesterday) {
            actual_req.offset = Offset::CloseYesterday;
        } else if (actual_req.volume <= available_today + available_yesterday &&
                   available_today > 0 && available_yesterday > 0) {
            // Logging
LOG_INFO("引擎自动拆单平仓: 平今=" + std::to_string(available_today) +
                     " 平昨=" + std::to_string(actual_req.volume - available_today));

            OrderRequest today_req = actual_req;
            today_req.offset = Offset::CloseToday;
            today_req.volume = available_today;

            OrderRequest yd_req = actual_req;
            yd_req.offset = Offset::CloseYesterday;
            yd_req.volume = actual_req.volume - available_today;

            std::string ref1;
            send_order_unlocked(today_req, ref1, out_reject_reason, out_reject_message);

            std::string ref2;
            send_order_unlocked(yd_req, ref2, out_reject_reason, out_reject_message);

            if (ref1.empty() || ref2.empty()) {
                // Split order partially failed: cancel the one that was already submitted (拆单部分失败: 撤销已成功提交的那一单)
                if (!ref1.empty()) {
                    LOG_WARN("拆单平仓: 平昨失败，撤回平今单 ref=" + ref1);
                    cancel_order(ref1, ctx->account_id);
                }
                if (!ref2.empty()) {
                    LOG_WARN("拆单平仓: 平今失败，撤回平昨单 ref=" + ref2);
                    cancel_order(ref2, ctx->account_id);
                }
                LOG_ERROR("引擎自动拆单平仓部分失败: acct=" + ctx->account_id +
                          " instrument=" + std::string(actual_req.instrument_id) +
                          " ref_today=" + ref1 +
                          " ref_yesterday=" + ref2);
                record_alert("split close order partially failed, account=" + ctx->account_id +
                             ", instrument=" + std::string(actual_req.instrument_id));
                if (!ref1.empty()) {
                    out_order_ref = ref1;
                } else if (!ref2.empty()) {
                    out_order_ref = ref2;
                } else {
                    out_order_ref.clear();
                }
                return;
            }

            out_order_ref = ref1; // Return close-today order reference (返回平今单的引用)
            return;
        } else {
            const std::string message = "insufficient closeable position, account=" + ctx->account_id +
                                        ", instrument=" + std::string(actual_req.instrument_id);
            update_account_reject_state(ctx, OrderRejectReason::PositionUnavailable, message);
            set_reject("PositionUnavailable", message);
            LOG_INFO("引擎拒绝平仓，原因：可用持仓不足");
            return;
        }
    }

    std::string reject_reason;
    const bool is_close = (actual_req.offset != Offset::Open);
    const int cond_buy = cond_order_mgr_.get_pending_open_volume(actual_req.instrument_id, Direction::Buy);
    const int cond_sell = cond_order_mgr_.get_pending_open_volume(actual_req.instrument_id, Direction::Sell);
    const bool cancel_rate_exempt = ctx->risk_mgr.is_cancel_rate_exempt(actual_req.strategy_id);
if (!ctx->risk_mgr.check_order(actual_req, reject_reason, is_close, cond_buy, cond_sell, cancel_rate_exempt)) {
        update_account_reject_state(ctx, OrderRejectReason::RiskCheckFailed, reject_reason);
        set_reject("RiskCheckFailed", reject_reason);
        LOG_INFO("runtime message");
        return;
    }

    //
const OrderInfo order = ctx->order_mgr.create_order(actual_req);
    ctx->risk_mgr.on_order_sent();
    const std::string order_ref(order.order_ref);
    // Trade gateway
    const int ret = ctx->trade_gateway->send_order(actual_req, order_ref);
    if (ret != 0) {
        const std::string message = "gateway rejected local send, ret=" + std::to_string(ret) +
                                    ", ref=" + order_ref;
        update_account_reject_state(ctx, OrderRejectReason::GatewaySendFailed, message);
        set_reject("GatewaySendFailed", message);
        LOG_INFO("runtime message");
        return;
    }

    const long long now_us = steady_us_now();
    const long long signal_us = last_signal_steady_us_.load(std::memory_order_relaxed);
    if (signal_us > 0) {
        last_signal_to_order_us_.store((std::max)(0LL, now_us - signal_us), std::memory_order_relaxed);
    }
    {
        auto& slot = order_latency_ring_[order_latency_ring_head_];
        std::strncpy(slot.order_ref, order.order_ref, sizeof(slot.order_ref) - 1);
        slot.order_ref[sizeof(slot.order_ref) - 1] = '\0';
        slot.sent_us = now_us;
        order_latency_ring_head_ = (order_latency_ring_head_ + 1) % kOrderLatencyRingCap;
    }
    clear_account_reject_state(ctx);
    out_order_ref = order_ref;
    if (actual_req.strategy_id[0] != '\0') {
        char sig_buf[128];
        std::snprintf(sig_buf, sizeof(sig_buf), "%s%s %s %d手 @%.2f ref=%s",
                      direction_text(actual_req.direction),
                      offset_text(actual_req.offset),
                      actual_req.instrument_id,
                      actual_req.volume,
                      actual_req.price,
                      order.order_ref);
        strategy_ctrl_.record_signal(actual_req.strategy_id, sig_buf, now_time_text());
    }
    if (store_ && !production_hft_mode_) {
        store_->async_insert_order(order, "manual");
    }
}

SendOrderResult TradingEngine::send_order_with_result(const OrderRequest& req) {
    if (paper_engine_.is_active()) {
        return paper_engine_.simulate_order(req);
    }

    SendOrderResult result;

    AccountContext* ctx = account_mgr_.route_order(req);
    if (!ctx) {
        result.reject_reason = "NoAccount";
        result.reject_message = "no matching account for order";
        return result;
    }

    if (md_only_mode_.load(std::memory_order_relaxed)) {
        result.reject_reason = "EngineNotReady";
        result.reject_message = "engine in md-only mode";
        return result;
    }

    const AccountTradeState trade_state =
        get_account_trade_state_impl(ctx, running_.load(std::memory_order_relaxed));
    if (trade_state != AccountTradeState::Ready) {
        result.reject_reason = "AccountNotReady";
        result.reject_message = "account trade state is " + std::string(to_string(trade_state));
        return result;
    }

    // Same as above: consumer single-threaded, no need for send_order_mtx_ serialization.
    // (同上: consumer 单线程, 无需 send_order_mtx_ 序列化)
    std::string order_ref;
    send_order_unlocked(req, order_ref, &result.reject_reason, &result.reject_message);
    if (order_ref.empty()) {
        if (result.reject_reason.empty() || result.reject_reason == "None") {
            result.reject_reason = "Unknown";
            result.reject_message = "order rejected by gateway";
        }
        // Risk management
        if (result.reject_reason == "RiskCheckFailed" && store_) {
            store_->async_log_risk_event("risk_reject", "system", req.instrument_id, result.reject_message);
        }
    } else {
        result.order_ref = order_ref;
    }
    return result;
}

 // Cancel specified order — no account specified, search globally (撤销指定订单 — 不指定账户, 需全局搜索)
void TradingEngine::cancel_order(const std::string& order_ref) {
    AccountContext* matched_ctx = nullptr;
    OrderInfo matched_order{};
    // Account manager
for (auto* ctx : account_mgr_.all_accounts()) {
        OrderInfo order{};
        if (!ctx->order_mgr.get_order_copy(order_ref, order)) {
            continue;
        }
        // Block
        if (matched_ctx != nullptr) {
            LOG_ERROR("引擎拒绝按全局 order_ref 撤单，原因：多账户存在重复ref=" + order_ref +
                      " accounts=" + matched_ctx->account_id + "," + ctx->account_id);
            return;
        }
        matched_ctx = ctx;
        matched_order = order;
    }
    // Block
if (matched_ctx != nullptr) {
        const int ret = matched_ctx->trade_gateway->cancel_order(std::string(matched_order.instrument_id),
                                                                 std::string(matched_order.exchange_id),
                                                                 order_ref,
                                                                 matched_order.front_id,
                                                                 matched_order.session_id);
        if (ret != 0) {
            LOG_WARN("cancel request failed to send, ref=" + order_ref +
                     " acct=" + matched_ctx->account_id +
                     " ret=" + std::to_string(ret));
        }
        return;
    }
    LOG_INFO("runtime message");
}

 // Cancel order under the specified account (撤销指定账户下的订单)
bool TradingEngine::cancel_order(const std::string& order_ref, const std::string& account_id) {
    return try_cancel_order(order_ref, account_id);
}

size_t TradingEngine::cancel_all_orders(const std::string& account_id) {
    const auto orders = get_active_orders(account_id);
    size_t submitted = 0;
    for (const auto& order : orders) {
        if (try_cancel_order(std::string(order.order_ref), std::string(order.account_id))) {
            ++submitted;
        }
    }
    return submitted;
}

size_t TradingEngine::cancel_strategy_orders(const std::string& strategy_id) {
    size_t submitted = 0;
    for (auto* ctx : account_mgr_.all_accounts()) {
        const auto active_orders = ctx->order_mgr.get_active_orders();
        for (const auto& order : active_orders) {
            if (strategy_id == order.strategy_id) {
                if (try_cancel_order(std::string(order.order_ref), ctx->account_id)) {
                    ++submitted;
                }
            }
        }
    }
    submitted += cond_order_mgr_.cancel_by_strategy(strategy_id);
    if (submitted > 0) {
        LOG_INFO("cancel_strategy_orders: strategy=" + strategy_id +
                 " cancelled=" + std::to_string(submitted));
    }
    return submitted;
}

 // Attempt to cancel order under the specified account (尝试撤销指定账户下的订单)
bool TradingEngine::try_cancel_order(const std::string& order_ref, const std::string& account_id) {
    AccountContext* ctx = resolve_account(account_id);
    if (!ctx) {
        LOG_INFO("close task cancel skipped: account not found, acct=" + account_id +
                 " ref=" + order_ref);
        return false;
    }

    OrderInfo order{};
    if (!ctx->order_mgr.get_order_copy(order_ref, order)) {
        LOG_INFO("close task cancel skipped: order not found locally, acct=" + account_id +
                 " ref=" + order_ref);
        return false;
    }

    // Trade gateway
const int ret = ctx->trade_gateway->cancel_order(std::string(order.instrument_id),
                                                     std::string(order.exchange_id),
                                                     order_ref,
                                                     order.front_id,
                                                     order.session_id);
    if (ret != 0) {
        LOG_WARN("close task cancel request failed to send, acct=" + account_id +
                 " ref=" + order_ref +
                 " ret=" + std::to_string(ret));
        return false;
    }
    return true;
}

// Add conditional order (添加条件单)
uint32_t TradingEngine::add_conditional_order(const ConditionalOrder& order) {
    ConditionalOrder o = order;
    if (o.type == ConditionType::TrailingStop && o.extreme_price < 1e-9) {
        TickData tick = get_last_tick(o.instrument_id);
        if (tick.last_price > 0.0) {
            o.extreme_price = tick.last_price;
        }
    }
    const uint32_t id = cond_order_mgr_.add(o);
    register_hot_instrument(o.instrument_id);
    request_async_save();
    return id;
}

// Allocate conditional order group ID — for OCO orders (分配条件单分组ID, 用于 OCO 订单)
uint32_t TradingEngine::allocate_cond_group_id() {
    return cond_order_mgr_.allocate_group_id();
}

// Cancel conditional order (取消条件单)
void TradingEngine::cancel_conditional_order(uint32_t id) {
    cond_order_mgr_.cancel(id);
    request_async_save();
}

 // Trade gateway reconnected callback (交易网关重连回调)
void TradingEngine::on_trade_reconnected(const std::string& account_id, int front_id, int session_id, int max_order_ref) {
    LOG_INFO("引擎收到交易重连通知: account=: account=" + account_id +
             " front_id=" + std::to_string(front_id) +
             " session_id=" + std::to_string(session_id) +
             " max_order_ref=" + std::to_string(max_order_ref));

    AccountContext* ctx = resolve_account(account_id);
    if (!ctx) {
        LOG_INFO("runtime message");
        return;
    }

    // Initialize
    ctx->order_mgr.init(front_id, session_id, max_order_ref);
    {
        std::lock_guard<std::mutex> lock(snapshot_mtx_);
        const bool any_pending = std::any_of(
            account_mgr_.all_accounts().begin(), account_mgr_.all_accounts().end(),
            [](const AccountContext* c) { return c->reconnect_sync_pending; });
        if (!any_pending) {
            was_running_before_reconnect_ = (strategy_ctrl_.get_global_state() == StrategyState::Running);
            strategy_ctrl_.set_global_state(StrategyState::Paused);
        }
        ctx->resume_after_reconnect = was_running_before_reconnect_;
        ctx->reconnect_sync_pending = true;
        ctx->reconnect_start_time = std::chrono::steady_clock::now();
    }
    reset_snapshot_state(ctx); // Reset snapshot state (重置快照状态)
    //
const std::string label = ctx->account_id.empty() ? "default" : ctx->account_id;
    if (!retry_query(("重连后资金[" + label + "]").c_str(),
                     [ctx]() { return ctx->trade_gateway->query_account(); }, 5, 1000)) {
        LOG_INFO("runtime message");
    }
    if (!retry_query(("重连后持仓[" + label + "]").c_str(),
                     [ctx]() { return ctx->trade_gateway->query_position(); }, 5, 1000)) {
        LOG_INFO("runtime message");
    }

    // Retry query
    if (!retry_query(("重连后活动委托[" + label + "]").c_str(),
                     [ctx]() { return ctx->trade_gateway->query_active_orders(); }, 5, 1000)) {
        LOG_INFO("runtime message");
        // Logging
        LOG_INFO("runtime message");
        record_alert("runtime alert");
        schedule_active_orders_refresh(0); // Schedule immediate refresh (安排立即刷新)
    }
    schedule_active_orders_refresh(1500); // Schedule refresh slightly later (安排稍后刷新)
}

// Update trading day (更新交易日)
void TradingEngine::on_trading_day(const char* trading_day) {
    if (!trading_day || trading_day[0] == '\0') return;
    for (auto* ctx : account_mgr_.all_accounts()) {
        ctx->risk_mgr.update_trading_day(trading_day);
    }
}

// Block
void TradingEngine::apply_account_snapshot(const AccountInfo& account) {
    AccountContext* ctx = resolve_account(account.account_id);
    if (!ctx) {
        LOG_INFO("runtime message");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(ctx->account_mtx);
        ctx->account_info = account;
    }
    ctx->risk_mgr.update_account(account); // Update risk manager with account funds (更新风控资金)
    {
        std::lock_guard<std::mutex> lock(snapshot_mtx_);
        ctx->account_snapshot_ready = true; // Mark account fund snapshot ready (标记资金快照就绪)
    }
    snapshot_cv_.notify_all();
    maybe_finish_reconnect_sync();
}

//
void TradingEngine::apply_position_snapshot(const std::string& account_id, const std::vector<PositionInfo>& positions) {
    AccountContext* ctx = resolve_account(account_id);
    if (!ctx) {
        LOG_INFO("runtime message");
        return;
    }
    ctx->position_mgr.replace_positions(positions); // Replace local positions (替换本地持仓)
    {
        std::lock_guard<std::mutex> lock(snapshot_mtx_);
        ctx->position_snapshot_ready = true; // Mark position snapshot ready (标记持仓快照就绪)
    }
    snapshot_cv_.notify_all();
    maybe_finish_reconnect_sync();
}

//
void TradingEngine::apply_active_orders_snapshot(const std::string& account_id, const std::vector<OrderInfo>& active_orders) {
    AccountContext* ctx = resolve_account(account_id);
    if (!ctx) {
        LOG_INFO("runtime message");
        return;
    }
    ctx->order_mgr.replace_active_orders(active_orders); // Replace local active orders (替换本地活动委托)
    close_mgr_.reconcile_active_orders(account_id, active_orders); // Sync close manager's active orders (同步平仓管理器的活动委托)

    // Handle refresh progress (处理刷新进度)
    const int expected = active_orders_refresh_expected_.load(std::memory_order_relaxed);
    bool all_accounts_refreshed = false;
    if (expected > 0) {
        const int completed =
            active_orders_refresh_completed_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (completed >= expected) { // If all accounts' refreshes have completed (如果所有账户的刷新都完成了)
            active_orders_refresh_pending_.store(false, std::memory_order_relaxed);
            active_orders_refresh_inflight_.store(false, std::memory_order_relaxed);
            active_orders_refresh_expected_.store(0, std::memory_order_relaxed);
            active_orders_refresh_completed_.store(0, std::memory_order_relaxed);
            all_accounts_refreshed = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(snapshot_mtx_);
        ctx->active_orders_snapshot_ready = true; // Mark active orders snapshot ready (标记活动委托快照就绪)
    }
    snapshot_cv_.notify_all();
    maybe_finish_reconnect_sync();

    // [HIGH fix] On first full-account snapshot completion after startup, clean up "pending orders
    // left from previous session" per config. One-shot (pending_cleanup_done_): reconnection
    // snapshots do NOT re-trigger, avoiding accidentally canceling current session's normal pending orders.
    // ([HIGH 修复] 启动后首次全账户 snapshot 完成时, 按配置清理"前一会话遗留的挂队列单"。
    //  一次性 pending_cleanup_done_: 重连后的 snapshot 不再触发, 避免误撤当前会话的正常挂单。)
    if (cancel_pending_on_restart_ && all_accounts_refreshed) {
        bool expected_done = false;
        if (pending_cleanup_done_.compare_exchange_strong(expected_done, true)) {
            int cancelled = 0;
            for (auto* a_ctx : account_mgr_.all_accounts()) {
                for (const auto& order : a_ctx->order_mgr.get_active_orders()) {
                    if (try_cancel_order(std::string(order.order_ref), a_ctx->account_id)) {
                        ++cancelled;
                    }
                }
            }
            if (cancelled > 0) {
                LOG_INFO("CancelPendingOnRestart: cancelled " + std::to_string(cancelled) +
                         " stale active order(s) from previous session");
            }
        }
    }
}

// Reconnect
bool TradingEngine::is_reconnect_sync_pending() const {
    std::lock_guard<std::mutex> lock(snapshot_mtx_);
    for (const auto* ctx : account_mgr_.all_accounts()) {
        if (ctx->reconnect_sync_pending) return true;
    }
    return false;
}

// Get seconds since last tick.
int TradingEngine::seconds_since_last_tick() const {
    return session_mgr_.seconds_since_last_tick();
}

bool TradingEngine::is_in_configured_trading_session() const {
    return session_mgr_.is_in_configured_trading_session();
}

void TradingEngine::refresh_trading_session_state(bool in_session) {
    const auto transition = session_mgr_.refresh_trading_session_state(in_session);
    if (transition.leaving_session) {
        request_async_save();
    }
}

void TradingEngine::apply_monitoring_config(int no_tick_warn_seconds,
                                            const std::string& trading_sessions) {
    session_mgr_.apply_monitoring_config(no_tick_warn_seconds, trading_sessions);
}

//
PositionInfo TradingEngine::get_position(const char* instrument, Direction dir) const {
    return get_position(instrument, dir, "");
}

// Get aggregated net position.
int TradingEngine::get_net_position(const char* instrument) const {
    // Aggregate net positions across all accounts (汇总所有账户的净持仓)
    int net = 0;
    for (const auto* ctx : account_mgr_.all_accounts()) {
        net += ctx->position_mgr.get_net_position(instrument);
    }
    return net;
}

//
PositionInfo TradingEngine::get_position(const char* instrument, Direction dir, const std::string& account_id) const {
    if (!account_id.empty()) {
        const AccountContext* ctx = account_mgr_.find_account(account_id);
        return ctx ? ctx->position_mgr.get_position(instrument, dir) : PositionInfo{};
    }

    // Aggregate positions across all accounts (聚合所有账户的持仓)
    PositionInfo aggregated{};
    safe_copy(aggregated.instrument_id, instrument, sizeof(aggregated.instrument_id));
    safe_copy(aggregated.account_id, "aggregated", sizeof(aggregated.account_id));
    aggregated.direction = dir;
    double weighted_price = 0.0; // Used to compute weighted average price (用于计算加权平均价)
    for (const auto* ctx : account_mgr_.all_accounts()) {
        PositionInfo pos = ctx->position_mgr.get_position(instrument, dir);
        if (pos.total <= 0) {
            continue;
        }
        aggregated.total += pos.total;
        aggregated.today += pos.today;
        aggregated.yesterday += pos.yesterday;
        aggregated.position_profit += pos.position_profit;
        aggregated.use_margin += pos.use_margin;
        weighted_price += pos.avg_price * static_cast<double>(pos.total);
    }

    if (aggregated.total > 0) {
        aggregated.avg_price = weighted_price / static_cast<double>(aggregated.total);
        return aggregated;
    }

    const AccountContext* default_ctx = account_mgr_.default_account();
    return default_ctx ? default_ctx->position_mgr.get_position(instrument, dir) : aggregated;
}

//
int TradingEngine::get_net_position(const char* instrument, const std::string& account_id) const {
    if (!account_id.empty()) {
        const AccountContext* ctx = account_mgr_.find_account(account_id);
        return ctx ? ctx->position_mgr.get_net_position(instrument) : 0;
    }

    int net = 0;
    for (const auto* ctx : account_mgr_.all_accounts()) {
        net += ctx->position_mgr.get_net_position(instrument);
    }
    return net;
}

WindowedOrderBook TradingEngine::get_order_book(const char* instrument) const {
    return order_book_mgr_.get_book(instrument);
}

AccountInfo TradingEngine::get_account_info(const std::string& account_id) const {
    return account_mgr_.get_account(account_id);
}

void TradingEngine::strategy_log(const std::string& strategy_id, int level, const std::string& message) {
    const std::string prefixed = "[" + strategy_id + "] " + message;
    switch (level) {
        case 0:  LOG_INFO(prefixed);  break;
        case 1:  LOG_WARN(prefixed);  break;
        default: LOG_ERROR(prefixed); break;
    }
}

void TradingEngine::save_strategy_state(const std::string& strategy_id,
                                         const std::map<std::string, std::string>& state) {
    if (!store_) return;
    for (const auto& [k, v] : state) {
        store_->save_system_config("strategy_state." + strategy_id + "." + k, v);
    }
}

std::map<std::string, std::string> TradingEngine::load_strategy_state(const std::string& strategy_id) {
    std::map<std::string, std::string> result;
    if (!store_) return result;
    const std::string prefix = "strategy_state." + strategy_id + ".";
    for (const auto& [k, v] : store_->load_system_config_by_prefix(prefix)) {
        result[k] = v;
    }
    return result;
}

int TradingEngine::register_timer(const std::string& strategy_id, int interval_ms) {
    const int id = next_timer_id_.fetch_add(1, std::memory_order_relaxed);
    TimerEntry entry;
    entry.id = id;
    entry.interval_ms = interval_ms;
    entry.strategy_id = strategy_id;
    entry.next_fire = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval_ms);
    timers_.push_back(std::move(entry));
    return id;
}

void TradingEngine::unregister_timer(int timer_id) {
    timers_.erase(
        std::remove_if(timers_.begin(), timers_.end(),
                        [timer_id](const TimerEntry& e) { return e.id == timer_id; }),
        timers_.end());
}

std::vector<KlineBar> TradingEngine::query_klines(const std::string& instrument,
                                                    const std::string& period,
                                                    size_t count) const {
    return kline_mgr_.get_bars(instrument, period, count);
}

// Block
void TradingEngine::pause_strategy() {
    EngineCommand cmd{};
    cmd.type = CommandType::Pause;
    enqueue_command_or_fallback(cmd, "暂停策略");
}

// Block
void TradingEngine::resume_strategy() {
    EngineCommand cmd{};
    cmd.type = CommandType::Resume;
    enqueue_command_or_fallback(cmd, "恢复策略");
}

// Stop
void TradingEngine::stop_strategy() {
    EngineCommand cmd{};
    cmd.type = CommandType::Stop;
    enqueue_command_or_fallback(cmd, "停止策略");
}

// Emergency
void TradingEngine::emergency_close_all() {
    EngineCommand cmd{};
    cmd.type = CommandType::EmergencyClose;
    enqueue_command_or_fallback(cmd, "紧急全平");
}

// Block
void TradingEngine::add_conditional_order_async(const ConditionalOrder& order) {
    EngineCommand cmd{};
    cmd.type = CommandType::AddCondOrder;
    cmd.cond_order = order;
    enqueue_command_or_fallback(cmd, "添加条件单");
}

// Block
void TradingEngine::cancel_conditional_order_async(uint32_t id) {
    EngineCommand cmd{};
    cmd.type = CommandType::CancelCondOrder;
    cmd.cond_order_id = id;
    enqueue_command_or_fallback(cmd, "撤销条件单");
}

bool TradingEngine::update_monitoring_config_async(int no_tick_warn_seconds,
                                                   const std::string& trading_sessions) {
    if (!running_.load(std::memory_order_relaxed)) {
        LOG_WARN("引擎未运行，监控配置热更新被跳过");
        return false;
    }

    EngineCommand cmd{};
    cmd.type = CommandType::UpdateMonitorConfig;
    cmd.monitor_config.no_tick_warn_seconds = no_tick_warn_seconds;
    safe_copy(cmd.monitor_config.trading_sessions, trading_sessions.c_str(),
              sizeof(cmd.monitor_config.trading_sessions));
    return enqueue_command_or_fallback(cmd, "更新监控配置");
}

// State
StrategyState TradingEngine::get_strategy_state() const {
    return strategy_ctrl_.get_global_state();
}

bool TradingEngine::set_strategy_state(const std::string& strategy_id, StrategyState state) {
    const std::string target_id = trim_copy(strategy_id).empty() ? "default" : trim_copy(strategy_id);
    bool exists = false;
    {
        std::lock_guard<std::mutex> lock(strategies_mtx_);
        for (const auto& strategy : strategies_) {
            if (!strategy) continue;
            const std::string existing_id = strategy->strategy_id().empty() ? "default" : strategy->strategy_id();
            if (existing_id == target_id) {
                exists = true;
                break;
            }
        }
    }
    if (!exists) return false;
    if (!strategy_ctrl_.set_state(target_id, state)) return false;
    request_async_save();
    LOG_INFO("strategy state changed: id=" + target_id + " state=" + strategy_state_name(state));
    return true;
}

StrategyState TradingEngine::get_strategy_state(const std::string& strategy_id) const {
    return strategy_ctrl_.get_state(strategy_id);
}

bool TradingEngine::enqueue_command_or_fallback(const EngineCommand& cmd, const char* label) {
    {
        std::lock_guard<std::mutex> lock(cmd_push_mtx_);
        if (cmd_queue_.push(cmd)) {
            return true;
        }
    }

    command_queue_overflow_detected_.store(true, std::memory_order_relaxed);
    const std::string action = label ? label : "引擎命令";
    const std::string message = "严重：引擎命令队列已满，" + action + "切换为同步兜底执行";
    const bool first_overflow = !command_queue_drop_alerted_.exchange(true, std::memory_order_relaxed);
    if (first_overflow || cmd.type == CommandType::Pause ||
        cmd.type == CommandType::Stop || cmd.type == CommandType::EmergencyClose) {
        LOG_ERROR(message);
    }
    if (first_overflow) {
        record_alert(message);
    }

    switch (cmd.type) {
        case CommandType::Pause:
        case CommandType::Resume:
        case CommandType::Stop:
        case CommandType::EmergencyClose:
            process_command(cmd);
            return true;
        case CommandType::AddCondOrder:
        case CommandType::CancelCondOrder:
        case CommandType::UpdateMonitorConfig:
            if (first_overflow) {
                LOG_ERROR("非关键异步命令因队列满未执行: " + action);
            }
            return false;
    }
    return false;
}

// Block
void TradingEngine::process_command(const EngineCommand& cmd) {
    switch (cmd.type) {
        case CommandType::Pause:
            exec_pause();
            break;
        case CommandType::Resume:
            exec_resume();
            break;
        case CommandType::Stop:
            exec_stop();
            break;
        case CommandType::EmergencyClose:
            exec_emergency_close();
            break;
        case CommandType::AddCondOrder: {
            const uint32_t id = cond_order_mgr_.add(cmd.cond_order);
            register_hot_instrument(cmd.cond_order.instrument_id);
            last_cond_id_.store(id, std::memory_order_relaxed); // Record last added conditional order ID (记录最后添加的条件单ID)
            request_async_save();
            LOG_INFO("runtime message");
            break;
        }
        case CommandType::CancelCondOrder:
            cond_order_mgr_.cancel(cmd.cond_order_id);
            rebuild_hot_instruments();
            request_async_save();
            LOG_INFO("runtime message");
            break;
        case CommandType::UpdateMonitorConfig:
            apply_monitoring_config(cmd.monitor_config.no_tick_warn_seconds,
                                    cmd.monitor_config.trading_sessions);
            break;
        case CommandType::SetRiskMode:
            // Async risk mode switch from gateway thread (e.g. market data queue overflow auto-degradation)
            // (来自网关线程的异步风控模式切换, 如行情队列溢出自动降级)
            set_risk_mode(cmd.risk_mode, cmd.reason);
            LOG_WARN("风控模式异步切换: mode=" + std::string(to_string(cmd.risk_mode)) +
                     " reason=" + std::string(cmd.reason));
            break;
    }
}

// Block
void TradingEngine::exec_pause() {
    if (strategy_ctrl_.get_global_state() == StrategyState::Running) {
        strategy_ctrl_.set_global_state(StrategyState::Paused);
        request_async_save();
        LOG_INFO("runtime message");
    }
}

void TradingEngine::exec_resume() {
    strategy_ctrl_.set_global_state(StrategyState::Running);
    request_async_save();
    LOG_INFO("runtime message");
}

void TradingEngine::exec_stop() {
    strategy_ctrl_.set_global_state(StrategyState::Stopped);
    LOG_INFO("runtime message");

    // Cancel all accounts' active orders (撤销所有账户的活动委托)
    for (auto* ctx : account_mgr_.all_accounts()) {
        const auto active_orders = ctx->order_mgr.get_active_orders();
        for (const auto& order : active_orders) {
            cancel_order(std::string(order.order_ref), ctx->account_id);
        }
    }

    cond_order_mgr_.cancel_all();
    request_async_save();
}

void TradingEngine::exec_emergency_close() {
    exec_stop();

    //
int close_count = 0;
    int skip_count = 0;
    for (auto* ctx : account_mgr_.all_accounts()) {
        const auto positions = ctx->position_mgr.get_all_positions();
        for (const auto& pos : positions) {
            if (pos.total <= 0) continue;

            double last_price = 0.0;
            double upper = 0.0;
            double lower = 0.0;
            {
                const TickData last_tick = tick_data_mgr_.get_last_tick(pos.instrument_id);
                last_price = last_tick.last_price;
                upper = last_tick.upper_limit;
                lower = last_tick.lower_limit;
            }

            // Block
            if (upper <= 0.0 || lower <= 0.0) {
                LOG_ERROR("emergency close skipped: missing limit price");
                record_alert("紧急全平跳过:"
                             " 持仓=" + std::to_string(pos.total) + "手，请手动处理！");
                ++skip_count;
                continue;
            }

            close_mgr_.submit_close(pos.instrument_id, get_exchange_id(pos.instrument_id), ctx->account_id.c_str(),
                                    pos.direction, pos.today, pos.yesterday, last_price, upper, lower);
            ++close_count;
        }
    }

    LOG_INFO("引擎已提交紧急全平: " + std::to_string(close_count) +
             " 跳过=" + std::to_string(skip_count));
    if (skip_count > 0) {
        LOG_INFO("runtime message");
    }
}

AccountInfo TradingEngine::get_account(const std::string& account_id) const {
    return account_mgr_.get_account(account_id);
}

std::vector<PositionInfo> TradingEngine::get_all_positions(const std::string& account_id) const {
    return account_mgr_.get_all_positions(account_id);
}

std::vector<OrderInfo> TradingEngine::get_active_orders(const std::string& account_id) const {
    return account_mgr_.get_all_active_orders(account_id);
}

TickData TradingEngine::get_last_tick(const char* instrument) const {
    return tick_data_mgr_.get_last_tick(instrument);
}

std::unordered_map<std::string, TickData> TradingEngine::get_all_ticks() const {
    return tick_data_mgr_.get_all_ticks();
}

std::unordered_map<std::string, TickData> TradingEngine::get_ticks_filtered(const std::vector<std::string>& instruments,
                                                                  size_t limit) const {
    return tick_data_mgr_.get_ticks_filtered(instruments, limit);
}

std::vector<TickData> TradingEngine::get_ticks_changed_since(long long since_update_seq,
                                                             size_t limit,
                                                             long long* latest_update_seq) const {
    return tick_data_mgr_.get_ticks_changed_since(since_update_seq, limit, latest_update_seq);
}

std::vector<TickData> TradingEngine::get_subscribed_ticks() const {
    return tick_data_mgr_.get_ticks_for(instrument_registry_.instruments_ref());
}

std::vector<KlineBar> TradingEngine::get_kline(const std::string& instrument,
                                               const std::string& period,
                                               size_t limit) const {
    return kline_mgr_.get_bars(instrument, period, limit);
}

std::vector<std::string> TradingEngine::get_kline_periods(const std::string& instrument) const {
    return kline_mgr_.get_periods(instrument);
}

std::vector<KlineCatalogItem> TradingEngine::get_kline_catalog(const std::string& instrument,
                                                               const std::string& period) const {
    return kline_mgr_.get_catalog(instrument, period);
}

std::vector<TradeInfo> TradingEngine::get_recent_trades(const std::string& account_id, size_t limit) const {
    const std::string account_filter = trim_copy(account_id);
    limit = (std::max)(size_t{1}, (std::min)(limit, size_t{500}));

    std::vector<TradeInfo> trades;
    std::shared_lock<std::shared_mutex> lock(trades_mtx_);
    for (const auto& trade : recent_trades_) {
        if (!account_filter.empty() && account_filter != trade.account_id) {
            continue;
        }
        trades.push_back(trade);
        if (trades.size() >= limit) {
            break;
        }
    }
    return trades;
}

std::vector<OrderInfo> TradingEngine::get_recent_orders(const std::string& account_id, size_t limit) const {
    const std::string account_filter = trim_copy(account_id);
    limit = (std::max)(size_t{1}, (std::min)(limit, size_t{1000}));

    std::vector<OrderInfo> orders;
    std::shared_lock<std::shared_mutex> lock(orders_history_mtx_);
    for (const auto& order : recent_orders_) {
        if (!account_filter.empty() && account_filter != order.account_id) {
            continue;
        }
        orders.push_back(order);
        if (orders.size() >= limit) {
            break;
        }
    }
    return orders;
}

std::vector<PnlCurvePoint> TradingEngine::get_pnl_curve(size_t limit) const {
    limit = (std::max)(size_t{1}, (std::min)(limit, size_t{1440}));
    std::lock_guard<std::mutex> lock(pnl_curve_mtx_);
    const size_t take = (std::min)(limit, pnl_curve_.size());
    return std::vector<PnlCurvePoint>(pnl_curve_.end() - take, pnl_curve_.end());
}

std::vector<StrategyPerformanceSnapshot> TradingEngine::get_strategy_performance(const std::string& strategy_id) const {
    return strategy_ctrl_.get_performance(strategy_id);
}

TickRecordingStatus TradingEngine::get_tick_recording_status() const {
    return tick_recorder_.get_status();
}

bool TradingEngine::start_tick_recording(const std::string& path, std::string* error) {
    const bool ok = tick_recorder_.start(path, error);
    if (ok) save_runtime_cache();
    return ok;
}

bool TradingEngine::stop_tick_recording(std::string* error) {
    const bool ok = tick_recorder_.stop(error);
    save_runtime_cache();
    return ok;
}

bool TradingEngine::delete_tick_recording(const std::string& instrument,
                                          std::string* error,
                                          size_t* deleted_files,
                                          uintmax_t* deleted_bytes) {
    return tick_recorder_.delete_files(instrument, error, deleted_files, deleted_bytes);
}

InitStatus TradingEngine::get_init_status() const {
    InitStatus status;
    status.config_path = config_path_;
    status.runtime_state_path = runtime_state_path_.string();
    status.kline_store_path = kline_mgr_.store_path().string();
    status.tick_recording_path = tick_recorder_.path_string();
    status.config_exists = !config_path_.empty() && std::filesystem::exists(config_path_);
    status.runtime_state_exists = std::filesystem::exists(runtime_state_path_);
    status.kline_store_exists = std::filesystem::exists(kline_mgr_.store_path());
    status.tick_record_file_exists = std::filesystem::exists(tick_recorder_.path());
    status.config_loaded = !config_path_.empty();
    return status;
}

bool TradingEngine::import_kline_csv(const std::string& instrument,
                                     const std::string& period,
                                     const std::string& csv_path,
                                     bool replace_existing,
                                     size_t* imported_count,
                                     std::string* error) {
    if (!kline_mgr_.import_csv(instrument, period, csv_path, replace_existing, imported_count, error)) {
        return false;
    }
    request_async_save();
    return true;
}

std::vector<InstrumentSpec> TradingEngine::get_instrument_specs(const std::string& instrument) const {
    const std::string filter = trim_copy(instrument);
    if (!filter.empty()) {
        auto specs = instrument_registry_.get_specs(filter);
        if (!specs.empty() && specs[0].instrument_id.empty()) {
            specs[0] = apply_instrument_spec_overrides(config_, infer_instrument_spec(filter));
        }
        return specs;
    }
    return instrument_registry_.get_specs();
}

bool TradingEngine::has_instrument(const std::string& instrument) const {
    return instrument_registry_.has_instrument(instrument);
}
void TradingEngine::update_instrument_spec(const InstrumentSpec& spec) {
    instrument_registry_.update_spec(spec);
}
bool TradingEngine::refresh_instrument_rates(const std::string& instrument, const std::string& account_id, std::string* error) {
    const std::string normalized = trim_copy(instrument);
    if (normalized.empty()) {
        if (error) *error = "missing_instrument";
        return false;
    }
    AccountContext* ctx = resolve_account(account_id);
    if (!ctx || !ctx->trade_gateway) {
        if (error) *error = "trade_gateway_unavailable";
        return false;
    }
    if (!ctx->trade_gateway->is_logged_in()) {
        if (error) *error = "trade_gateway_not_logged_in";
        return false;
    }
    auto specs = get_instrument_specs(normalized);
    const std::string exchange = specs.empty() ? std::string(get_exchange_id(normalized.c_str())) : specs.front().exchange_id;
    const int ret = ctx->trade_gateway->query_instrument_rates(normalized, exchange);
    if (ret != 0) {
        if (error) *error = "query_instrument_rates_failed:" + std::to_string(ret);
        return false;
    }
    if (error) error->clear();
    return true;
}

void TradingEngine::on_instrument_spec_update(const InstrumentSpec& spec) {
    instrument_registry_.update_spec(spec);
}
std::vector<std::string> TradingEngine::get_market_universe() const {
    return instrument_registry_.get_market_universe();
}

std::vector<ConditionalOrder> TradingEngine::get_active_cond_orders() const {
    return cond_order_mgr_.get_active_orders();
}

std::vector<CloseTask> TradingEngine::get_close_tasks() const {
    return close_mgr_.get_tasks();
}

bool TradingEngine::retry_failed_close_task(uint32_t task_id) {
    return close_mgr_.retry_failed_task(task_id);
}

void TradingEngine::push_runtime_alert(const std::string& message) {
    record_alert(message);
}

std::vector<std::string> TradingEngine::get_recent_alerts(size_t limit) const {
    std::lock_guard<std::mutex> lock(alerts_mtx_);
    std::vector<std::string> result;
    const size_t take = (std::min)(limit, recent_alerts_.size());
    result.reserve(take);
    for (size_t i = recent_alerts_.size() - take; i < recent_alerts_.size(); ++i) {
        result.push_back(recent_alerts_[i]);
    }
    return result;
}

RiskSnapshot TradingEngine::get_risk_snapshot(const std::string& account_id) const {
    const AccountContext* ctx = account_mgr_.find_account(account_id);
    if (!ctx) {
        ctx = account_mgr_.default_account();
    }
    if (!ctx) {
        return {};
    }

    RiskSnapshot snapshot = ctx->risk_mgr.get_snapshot();
    {
        std::lock_guard<std::mutex> lock(ctx->account_mtx);
        snapshot.margin_usage_ratio = ctx->account_info.balance > 0.0
            ? (std::max)(0.0, ctx->account_info.margin / ctx->account_info.balance)
            : 0.0;
    }
    {
        std::lock_guard<std::mutex> lock(ctx->reject_mtx);
        if (!ctx->last_reject_message.empty()) {
            snapshot.last_reject_reason = ctx->last_reject_message;
            snapshot.risk_level = "warning";
        } else if (ctx->last_reject_reason != OrderRejectReason::None) {
            snapshot.last_reject_reason = to_string(ctx->last_reject_reason);
            snapshot.risk_level = "warning";
        }
    }
    return snapshot;
}

bool TradingEngine::reload_all_risk_configs() {
    // Reload risk control configuration from ConfigStore into Config object (从 ConfigStore 重新加载风控配置到 Config 对象)
    apply_config_store_overlay();
    // Account manager
    auto accounts = account_mgr_.all_accounts();
    for (auto* ctx : accounts) {
        if (ctx) {
            ctx->risk_mgr.reload_risk_config(config_);
        }
    }
    LOG_INFO("All risk configs reloaded for " + std::to_string(accounts.size()) + " accounts");
    return true;
}

void TradingEngine::set_risk_mode(RiskMode mode, const std::string& reason) {
    auto accounts = account_mgr_.all_accounts();
    for (auto* ctx : accounts) {
        if (ctx) {
            ctx->risk_mgr.set_risk_mode(mode, reason);
        }
    }
    LOG_INFO("RMS mode set to " + std::string(to_string(mode)) +
             " for " + std::to_string(accounts.size()) + " accounts" +
             (reason.empty() ? "" : " reason=" + reason));
}

RiskMode TradingEngine::get_risk_mode(const std::string& account_id) const {
    const AccountContext* ctx = account_mgr_.find_account(account_id);
    if (ctx) {
        return ctx->risk_mgr.get_risk_mode();
    }
    return RiskMode::Normal;
}

std::vector<RiskEvent> TradingEngine::drain_risk_events(const std::string& account_id) {
    AccountContext* ctx = account_mgr_.find_account(account_id);
    if (ctx) {
        return ctx->risk_mgr.drain_risk_events();
    }
    return {};
}

LatencySnapshot TradingEngine::get_latency_snapshot() const {
    LatencySnapshot snapshot;
    snapshot.tick_to_signal_us = last_tick_to_signal_us_.load(std::memory_order_relaxed);
    snapshot.signal_to_order_us = last_signal_to_order_us_.load(std::memory_order_relaxed);
    snapshot.order_to_trade_us = last_order_to_trade_us_.load(std::memory_order_relaxed);
    snapshot.tick_process_us = last_tick_process_us_.load(std::memory_order_relaxed);
    snapshot.order_process_us = last_order_process_us_.load(std::memory_order_relaxed);
    snapshot.trade_process_us = last_trade_process_us_.load(std::memory_order_relaxed);
    return snapshot;
}

std::vector<AccountMonitorSnapshot> TradingEngine::get_account_snapshots() const {
    std::vector<AccountMonitorSnapshot> snapshots;
    const auto accounts = account_mgr_.all_accounts();
    snapshots.reserve(accounts.size());

    for (const auto* ctx : accounts) {
        AccountMonitorSnapshot snapshot;
        snapshot.account_id = ctx->account_id.empty() ? "default" : ctx->account_id;
        {
            std::lock_guard<std::mutex> lock(ctx->account_mtx);
            snapshot.account = ctx->account_info;
        }
        {
            std::lock_guard<std::mutex> lock(ctx->reject_mtx);
            snapshot.last_reject_reason = to_string(ctx->last_reject_reason);
            snapshot.last_reject_message = ctx->last_reject_message;
        }
        snapshot.position_count = ctx->position_mgr.get_all_positions().size();
        snapshot.active_order_count = ctx->order_mgr.get_active_orders().size();
        snapshot.trade_gateway_logged_in = ctx->trade_gateway && ctx->trade_gateway->is_logged_in();
        {
            std::lock_guard<std::mutex> lock(snapshot_mtx_);
            snapshot.trade_state = to_string(get_account_trade_state_impl(
                ctx, running_.load(std::memory_order_relaxed)));
            snapshot.snapshot_ready =
                ctx->account_snapshot_ready && ctx->position_snapshot_ready && ctx->active_orders_snapshot_ready;
            snapshot.reconnect_sync_pending = ctx->reconnect_sync_pending;
        }
        snapshots.push_back(std::move(snapshot));
    }

    return snapshots;
}

std::vector<StrategyMonitorSnapshot> TradingEngine::get_strategy_snapshots(const std::string& account_id) const {
    const auto positions = account_mgr_.get_all_positions(account_id);
    const auto active_orders = account_mgr_.get_all_active_orders(account_id);
    const auto cond_orders = cond_order_mgr_.get_active_orders();
    const auto close_tasks = close_mgr_.get_tasks();

    std::vector<std::shared_ptr<StrategyBase>> strategies_snapshot;
    {
        std::lock_guard<std::mutex> lock(strategies_mtx_);
        strategies_snapshot = strategies_;
    }

    std::vector<StrategyMonitorSnapshot> snapshots;
    snapshots.reserve(strategies_snapshot.size());

    for (const auto& strategy : strategies_snapshot) {
        StrategyMonitorSnapshot snapshot;
        snapshot.strategy_id = strategy->strategy_id().empty() ? "default" : strategy->strategy_id();
        snapshot.strategy_type = strategy->strategy_type();
        snapshot.script_path = strategy->script_path();
        snapshot.version = strategy->version();
        snapshot.default_account_id = strategy->default_account_id();
        snapshot.watched_instruments = strategy->watched_instruments();
        snapshot.parameters = strategy->parameters();
        snapshot.matches_all_accounts = snapshot.default_account_id.empty();
        snapshot.matches_all_instruments = snapshot.watched_instruments.empty();
        snapshot.account_exists =
            snapshot.matches_all_accounts || account_mgr_.find_account(snapshot.default_account_id) != nullptr;
        snapshot.script_exists =
            snapshot.script_path.empty() || std::filesystem::exists(snapshot.script_path);

        snapshot.status = strategy_state_name(get_strategy_state(snapshot.strategy_id));

        double weighted_price = 0.0;
        for (const auto& pos : positions) {
            if (strategy->handles_event(pos.account_id, pos.instrument_id)) {
                ++snapshot.position_count;
                snapshot.position_volume += pos.total;
                weighted_price += pos.avg_price * static_cast<double>(pos.total);
            }
        }
        if (snapshot.position_volume > 0) {
            snapshot.avg_price = weighted_price / static_cast<double>(snapshot.position_volume);
        }
        for (const auto& order : active_orders) {
            if (order.strategy_id[0] == '\0' && strategies_snapshot.size() > 1) {
                continue;
            }
            if (!strategy->handles_strategy(order.strategy_id)) {
                continue;
            }
            if (strategy->handles_event(order.account_id, order.instrument_id)) {
                ++snapshot.active_order_count;
            }
        }
        for (const auto& cond_order : cond_orders) {
            if (!account_id.empty() && std::string(cond_order.account_id) != account_id) {
                continue;
            }
            if (!strategy->handles_strategy(cond_order.strategy_id)) {
                continue;
            }
            if (strategy->handles_event(cond_order.account_id, cond_order.instrument_id)) {
                ++snapshot.conditional_order_count;
            }
        }
        for (const auto& task : close_tasks) {
            if (!account_id.empty() && std::string(task.account_id) != account_id) {
                continue;
            }
            if (strategy->handles_event(task.account_id, task.instrument_id)) {
                ++snapshot.close_task_count;
            }
        }

        {
            const auto pos_stats = strategy_ctrl_.get_position_stats(snapshot.strategy_id);
            snapshot.open_time = pos_stats.open_time;
            snapshot.add_time = pos_stats.add_time;
        }
        {
            const auto sig_stats = strategy_ctrl_.get_signal_stats(snapshot.strategy_id);
            snapshot.signal_count = sig_stats.signal_count;
            snapshot.last_signal = sig_stats.last_signal;
            snapshot.last_signal_time = sig_stats.last_signal_time;
        }
        {
            const auto perf = strategy_ctrl_.get_perf(snapshot.strategy_id);
            snapshot.trade_count = perf.trade_count;
            snapshot.realized_pnl = perf.realized_pnl;
            snapshot.floating_pnl = perf.floating_pnl;
            snapshot.total_pnl = perf.total_pnl;
            snapshot.win_rate = perf.win_rate;
            snapshot.profit_factor = perf.profit_factor;
        }

        snapshots.push_back(std::move(snapshot));
    }

    return snapshots;
}

bool TradingEngine::wait_for_snapshots(int timeout_sec) {
    std::unique_lock<std::mutex> lock(snapshot_mtx_);
    return snapshot_cv_.wait_for(lock, std::chrono::seconds(timeout_sec), [this] {
        for (const auto* ctx : account_mgr_.all_accounts()) {
            // State
            if (ctx->trade_state == AccountTradeState::GatewayDown) continue;
            if (!ctx->account_snapshot_ready || !ctx->position_snapshot_ready || !ctx->active_orders_snapshot_ready) {
                return false;
            }
        }
        return true;
    });
}

// State
void TradingEngine::reset_snapshot_state() {
    std::lock_guard<std::mutex> lock(snapshot_mtx_);
    for (auto* ctx : account_mgr_.all_accounts()) {
        if (ctx->trade_state == AccountTradeState::GatewayDown) continue;
        ctx->account_snapshot_ready = false;
        ctx->position_snapshot_ready = false;
        ctx->active_orders_snapshot_ready = false;
        set_account_trade_state(ctx, AccountTradeState::SnapshotSync);
    }
}

// State
void TradingEngine::reset_snapshot_state(AccountContext* ctx) {
    if (!ctx) {
        return;
    }

    std::lock_guard<std::mutex> lock(snapshot_mtx_);
    if (ctx->trade_state == AccountTradeState::GatewayDown) return;
    ctx->account_snapshot_ready = false;
    ctx->position_snapshot_ready = false;
    ctx->active_orders_snapshot_ready = false;
    set_account_trade_state(ctx, ctx->reconnect_sync_pending ? AccountTradeState::ReconnectSync
                                                             : AccountTradeState::SnapshotSync);
}

// Reconnect
void TradingEngine::maybe_finish_reconnect_sync() {
    bool global_reconnect_completed = false;
    bool should_resume = false;
    {
        std::lock_guard<std::mutex> lock(snapshot_mtx_);

        //
        bool all_ready = true;
        bool any_reconnect_transition = false; // Whether any account completed transition from syncing to done (是否有账户完成了从同步中到同步完成的转换)
        for (const auto* ctx : account_mgr_.all_accounts()) {
            if (ctx->trade_state == AccountTradeState::GatewayDown) continue;
            if (!ctx->account_snapshot_ready || !ctx->position_snapshot_ready || !ctx->active_orders_snapshot_ready) {
                all_ready = false;
                break;
            }
        }

        if (all_ready) {
            trading_ready_.store(true, std::memory_order_relaxed); // Mark trading engine as ready (标记交易引擎为就绪状态)
            for (auto* ctx : account_mgr_.all_accounts()) {
                if (ctx->trade_state == AccountTradeState::GatewayDown) continue;
                if (!ctx->reconnect_sync_pending) {
                    set_account_trade_state(ctx, AccountTradeState::Ready);
                }
            }
        }

        // Account manager
        for (auto* ctx : account_mgr_.all_accounts()) {
            if (ctx->trade_state == AccountTradeState::GatewayDown) continue;
            if (ctx->reconnect_sync_pending &&
                ctx->account_snapshot_ready && ctx->position_snapshot_ready && ctx->active_orders_snapshot_ready) {
                ctx->reconnect_sync_pending = false; // Clear sync-in-progress flag (清除同步中标记)
                set_account_trade_state(ctx, AccountTradeState::Ready);
                any_reconnect_transition = true;
            }
        }

        // Reconnect
        bool any_reconnect_pending = false;
        for (const auto* ctx : account_mgr_.all_accounts()) {
            if (ctx->reconnect_sync_pending) {
                any_reconnect_pending = true;
                break;
            }
        }

        // Reconnect
if (all_ready && any_reconnect_transition && !any_reconnect_pending) {
            global_reconnect_completed = true;
            // Check if any account needs to resume strategy after reconnect (检查是否有账户需要在重连后恢复策略运行)
for (auto* ctx : account_mgr_.all_accounts()) {
                if (ctx->resume_after_reconnect) {
                    should_resume = true;
                    ctx->resume_after_reconnect = false;
                }
            }
        }
    }

    if (!global_reconnect_completed) return; // If global reconnect sync not done, return directly (如果全局重连同步未完成, 直接返回)

    // Print post-reconnect position info (打印重连后的持仓信息)
    for (auto* ctx : account_mgr_.all_accounts()) {
        ctx->position_mgr.log_positions("reconnect[" + (ctx->account_id.empty() ? "default" : ctx->account_id) + "]");
    }
    
    // Block
    if (should_resume) {
        strategy_ctrl_.set_global_state(StrategyState::Running);
        request_async_save();
        LOG_INFO("引擎重连对账完成后已恢复策略运行");
    }
    close_mgr_.resume_pending_tasks(); // Resume pending close tasks (恢复挂起的平仓任务)
    // Notify strategies of reconnect completion (通知策略重连完成)
    std::vector<std::shared_ptr<StrategyBase>> strategies_snapshot;
    {
        std::lock_guard<std::mutex> lock(strategies_mtx_);
        strategies_snapshot = strategies_;
    }
    for (auto& strategy : strategies_snapshot) {
        strategy->on_reconnect();
    }
}

 // Check whether trading is possible (at least one account ready) (检查是否可以进行交易, 至少一个账户就绪)
bool TradingEngine::can_trade() const {
    if (md_only_mode_.load(std::memory_order_relaxed)) return false;
    if (!trading_ready_.load(std::memory_order_relaxed)) return false;
    for (const auto* ctx : account_mgr_.all_accounts()) {
        if (ctx->trade_state == AccountTradeState::Ready) return true;
    }
    return false;
}

// Check whether a specific account can trade (检查指定账户是否可以交易)
bool TradingEngine::can_trade(const std::string& account_id) const {
    if (md_only_mode_.load(std::memory_order_relaxed)) return false;
    const AccountContext* ctx = account_mgr_.find_account(account_id);
    if (!ctx) return false;
    return ctx->trade_state == AccountTradeState::Ready;
}

// Trade gateway
bool TradingEngine::is_trade_gateway_logged_in() const {
    // All accounts' trade gateways must be logged in to count as logged in (所有账户的交易网关都登录才算登录)
for (const auto* ctx : account_mgr_.all_accounts()) {
        if (!ctx->trade_gateway || !ctx->trade_gateway->is_logged_in()) {
            return false;
        }
    }
    return account_mgr_.account_count() > 0;
}

// Alert
void TradingEngine::check_runtime_alerts() {
    // Market data gateway
const bool md_logged_in = md_gateway_->is_logged_in();
    if (md_logged_in != session_mgr_.last_md_logged_in()) {
        session_mgr_.set_last_md_logged_in(md_logged_in);
        LOG_INFO(std::string("market gateway state changed: ") + (md_logged_in ? "connected" : "disconnected"));
        if (!md_logged_in) {
            record_alert("行情网关已断开");
        }
    }

    // Check trade gateway state changes (检查交易网关状态变化)
const bool td_logged_in = is_trade_gateway_logged_in();
    if (td_logged_in != session_mgr_.last_td_logged_in()) {
        session_mgr_.set_last_td_logged_in(td_logged_in);
        LOG_INFO(std::string("trade gateway state changed: ") + (td_logged_in ? "connected" : "disconnected"));
        if (!td_logged_in) {
            record_alert("交易网关存在断开连接");
        }
    }
    {
        std::lock_guard<std::mutex> lock(snapshot_mtx_);
        for (auto* ctx : account_mgr_.all_accounts()) {
            if (!ctx->trade_gateway) {
                continue;
            }
            if (!ctx->trade_gateway->is_logged_in()) {
                if (ctx->reconnect_sync_pending) {
                    set_account_trade_state(ctx, AccountTradeState::ReconnectSync);
                } else if (ctx->trade_state == AccountTradeState::Ready) {
                    set_account_trade_state(ctx, AccountTradeState::GatewayDown);
                }
            } else if (!ctx->reconnect_sync_pending &&
                       ctx->account_snapshot_ready && ctx->position_snapshot_ready && ctx->active_orders_snapshot_ready &&
                       ctx->trade_state == AccountTradeState::GatewayDown) {
                set_account_trade_state(ctx, AccountTradeState::Ready);
            }
        }
    }

    const bool in_trading_session = is_in_configured_trading_session();
    refresh_trading_session_state(in_trading_session);

    // No-tick alert check
    if (in_trading_session && session_mgr_.no_tick_warn_seconds() > 0) {
        const int idle_seconds = session_mgr_.seconds_since_last_tick();
        const long long enter_ms = session_mgr_.trading_session_enter_steady_ms();
        int effective_idle = idle_seconds;
        if (idle_seconds < 0 && enter_ms > 0) {
            const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            effective_idle = static_cast<int>(((std::max)(0LL, now_ms - enter_ms)) / 1000);
        }
        if (effective_idle >= 0 && effective_idle >= session_mgr_.no_tick_warn_seconds() && !session_mgr_.no_tick_alerted()) {
            session_mgr_.set_no_tick_alerted(true);
            record_alert("当前交易时段内超过阈值时间未收到行情");
            LOG_INFO("runtime message");
        }
    }

    // Reconnect sync timeout check: if not completed within 30 seconds, enter safe mode (重连同步超时检查: 30秒内未完成则切入安全模式)
    {
        static constexpr int kReconnectTimeoutSeconds = 30;
        const auto now_tp = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(snapshot_mtx_);
        for (auto* ctx : account_mgr_.all_accounts()) {
            if (!ctx->reconnect_sync_pending) continue;
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now_tp - ctx->reconnect_start_time).count();
            if (elapsed >= kReconnectTimeoutSeconds) {
                ctx->reconnect_sync_pending = false;
                ctx->resume_after_reconnect = false;
                set_account_trade_state(ctx, AccountTradeState::Ready);
                ctx->risk_mgr.set_risk_mode(RiskMode::NoOpen,
                    "reconnect sync timeout after " + std::to_string(elapsed) + "s");
                const std::string msg = "重连同步超时(" + std::to_string(elapsed) +
                    "s), 账户=" + (ctx->account_id.empty() ? "default" : ctx->account_id) +
                    " 切入禁开仓模式";
                LOG_ERROR(msg);
                record_alert(msg);
            }
        }
    }

    try_refresh_active_orders(); // Attempt to refresh active orders (尝试刷新活动委托)
}

 // Schedule active orders refresh after specified delay (安排在指定延迟后刷新活动委托)
void TradingEngine::schedule_active_orders_refresh(int delay_ms) {
    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    active_orders_refresh_pending_.store(true, std::memory_order_relaxed);
    active_orders_refresh_inflight_.store(false, std::memory_order_relaxed);
    active_orders_refresh_expected_.store(0, std::memory_order_relaxed);
    active_orders_refresh_completed_.store(0, std::memory_order_relaxed);
    next_active_orders_refresh_ms_.store(now_ms + delay_ms, std::memory_order_relaxed);
}

// Block
void TradingEngine::try_refresh_active_orders() {
    if (!active_orders_refresh_pending_.load(std::memory_order_relaxed)) return;
    if (active_orders_refresh_inflight_.load(std::memory_order_relaxed)) return;
    if (!running_ || !is_trade_gateway_logged_in()) return;

    const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now_ms < next_active_orders_refresh_ms_.load(std::memory_order_relaxed)) return;

    active_orders_refresh_inflight_.store(true, std::memory_order_relaxed);
    active_orders_refresh_completed_.store(0, std::memory_order_relaxed);
    active_orders_refresh_expected_.store(static_cast<int>(account_mgr_.account_count()),
                                          std::memory_order_relaxed);

    //
bool any_failed = false;
    for (auto* ctx : account_mgr_.all_accounts()) {
        const int ret = ctx->trade_gateway->query_active_orders();
        if (ret != 0) {
            any_failed = true;
            LOG_INFO("active order sync failed: ret=" + std::to_string(ret) + " account=" + ctx->account_id);
        }
    }

    // If any queries failed, schedule a retry later (如果有查询失败的, 安排稍后重试)
if (any_failed) {
        active_orders_refresh_inflight_.store(false, std::memory_order_relaxed);
        active_orders_refresh_expected_.store(0, std::memory_order_relaxed);
        active_orders_refresh_completed_.store(0, std::memory_order_relaxed);
        next_active_orders_refresh_ms_.store(now_ms + 2000, std::memory_order_relaxed);
    }
}

// Load runtime state from file: strategy states, conditional orders, close tasks
// (从文件加载运行时状态: 策略状态、条件单、平仓任务)
void TradingEngine::load_runtime_state() {
    std::string line;
    std::vector<CloseTask> restored_close_tasks;
    std::vector<std::tuple<std::string, std::string, KlineBar>> restored_kline_bars;
    bool loaded_text_state = false;
    std::ifstream ifs(runtime_state_path_);
    if (!ifs.is_open()) {
        const std::filesystem::path bak_path = runtime_state_path_.string() + ".bak";
        if (std::filesystem::exists(bak_path)) {
            LOG_WARN("runtime_state.dat not found, trying .bak fallback");
            ifs.open(bak_path);
        }
    }
    if (ifs.is_open()) {
        // Pre-validate checksum before loading state
        {
            std::string check_line;
            uint64_t checksum_expected = 0;
            bool has_checksum = false;
            std::streampos checksum_pos = 0;
            while (std::getline(ifs, check_line)) {
                if (check_line.substr(0, 10) == "#CHECKSUM\t") {
                    checksum_pos = ifs.tellg() - static_cast<std::streamoff>(check_line.size() + 1);
                    try {
                        checksum_expected = std::stoull(check_line.substr(10));
                        has_checksum = true;
                    } catch (...) {}
                    break;
                }
            }
            if (has_checksum && static_cast<uint64_t>(checksum_pos) != checksum_expected) {
                LOG_ERROR("runtime_state.dat checksum mismatch: expected=" +
                          std::to_string(checksum_expected) + " actual=" +
                          std::to_string(static_cast<uint64_t>(checksum_pos)));
                record_alert("runtime_state.dat 完整性校验失败，可能已损坏，跳过加载");
                ifs.close();
                return;
            }
            ifs.clear();
            ifs.seekg(0);
        }

        loaded_text_state = true;
        std::string header_line;
        if (std::getline(ifs, header_line)) {
            if (header_line != "#HFTS_V1") {
                ifs.clear();
                ifs.seekg(0);
                LOG_WARN("runtime_state.dat missing version header, loading as legacy format");
            }
        }
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;

            std::istringstream iss(line);
            std::string kind;
            std::getline(iss, kind, '\t'); // First column is record type (第一列是记录类型)

            if (kind == "#CHECKSUM") {
                continue;
            }

            // State
if (kind == "strategy_state") {
                std::string value;
                std::getline(iss, value, '\t');
                if (value == "paused") {
                    strategy_ctrl_.set_global_state(StrategyState::Paused);
                } else if (value == "stopped") {
                    strategy_ctrl_.set_global_state(StrategyState::Stopped);
                } else {
                    strategy_ctrl_.set_global_state(StrategyState::Running);
                }
                continue;
            }

            if (kind == "strategy_instance_state") {
                std::string strategy_id;
                std::string value;
                std::getline(iss, strategy_id, '\t');
                std::getline(iss, value, '\t');
                StrategyState state = StrategyState::Running;
                if (value == "paused") {
                    state = StrategyState::Paused;
                } else if (value == "stopped") {
                    state = StrategyState::Stopped;
                }
                strategy_id = trim_copy(strategy_id);
                if (!strategy_id.empty()) {
                    strategy_ctrl_.set_state(strategy_id, state);
                }
                continue;
            }

            // Restore day_start_balance for daily loss tracking (S0-05 fix)
            if (kind == "day_start_balance") {
                std::string account_id, dsb_str, trading_day;
                std::getline(iss, account_id, '\t');
                std::getline(iss, dsb_str, '\t');
                std::getline(iss, trading_day, '\t');
                account_id = trim_copy(account_id);
                trading_day = trim_copy(trading_day);
                if (!account_id.empty() && !dsb_str.empty()) {
                    try {
                        double dsb = std::stod(dsb_str);
                        auto* ctx = account_mgr_.find_account(account_id);
                        if (ctx) {
                            const std::string current_td = ctx->risk_mgr.get_trading_day();
                            if (current_td.empty() || current_td == trading_day) {
                                ctx->risk_mgr.set_day_start_balance(dsb);
                                LOG_INFO("恢复日亏损基准: account=" + account_id +
                                         " day_start_balance=" + std::to_string(dsb) +
                                         " trading_day=" + trading_day);
                            }
                        }
                    } catch (...) {}
                }
                continue;
            }

            if (kind == "kline_bar") {
                std::string instrument;
                std::string period;
                KlineBar bar;
                if (parse_kline_bar_line(line, &instrument, &period, &bar)) {
                    restored_kline_bars.emplace_back(std::move(instrument), std::move(period), std::move(bar));
                }
                continue;
            }

            // Block
if (kind == "cond_order") {
                ConditionalOrder order{};
                std::string field;

                std::getline(iss, field, '\t');
                order.id = static_cast<uint32_t>(std::stoul(field));

                std::getline(iss, field, '\t');
                safe_copy(order.instrument_id, field.c_str(), sizeof(order.instrument_id));

                std::getline(iss, field, '\t');
                order.type = static_cast<ConditionType>(std::stoi(field));

                std::getline(iss, field, '\t');
                order.direction = static_cast<Direction>(std::stoi(field));

                std::getline(iss, field, '\t');
                order.trigger_price = std::stod(field);

                std::getline(iss, field, '\t');
                order.trail_offset = std::stod(field);

                std::getline(iss, field, '\t');
                order.volume = std::stoi(field);

                std::getline(iss, field, '\t');
                order.extreme_price = std::stod(field);

                // Block
if (std::getline(iss, field, '\t') && !field.empty()) {
                    order.group_id = static_cast<uint32_t>(std::stoul(field));
                }

                // Block
                if (std::getline(iss, field, '\t') && !field.empty()) {
                    safe_copy(order.account_id, field.c_str(), sizeof(order.account_id));
                }

                // Block
                if (std::getline(iss, field, '\t') && !field.empty()) {
                    safe_copy(order.strategy_id, field.c_str(), sizeof(order.strategy_id));
                }

                // Block
                if (std::getline(iss, field, '\t') && !field.empty()) {
                    order.offset = static_cast<Offset>(std::stoi(field));
                }

                // Block
                if (std::getline(iss, field, '\t') && !field.empty()) {
                    order.order_price = std::stod(field);
                }

                // Block
                if (std::getline(iss, field, '\t') && !field.empty()) {
                    safe_copy(order.idempotency_key, field.c_str(), sizeof(order.idempotency_key));
                }

                // Block
                if (std::getline(iss, field, '\t') && !field.empty()) {
                    order.status = static_cast<CondOrderStatus>(std::stoi(field));
                }

                // Block
                if (std::getline(iss, field, '\t') && !field.empty()) {
                    order.created_at_ms = std::stoll(field);
                }

                // Block
                if (std::getline(iss, field, '\t') && !field.empty()) {
                    order.triggered_at_ms = std::stoll(field);
                }

                // Block
                if (std::getline(iss, field, '\t') && !field.empty()) {
                    order.cancelled_at_ms = std::stoll(field);
                }

                // Block
                if (std::getline(iss, field, '\t') && !field.empty()) {
                    safe_copy(order.reject_reason, field.c_str(), sizeof(order.reject_reason));
                }

                // [MEDIUM fix] Conditional order TTL: when restoring across trading days, old
                // conditional orders' strategy/position context may have already expired;
                // hard restore would trigger retry-then-fail loops. Set ConditionalOrderTTLDays=0 to disable.
                // ([MEDIUM 修复] 条件单 TTL: 跨交易日恢复时, 老条件单的策略/持仓上下文可能已经
                //  失效, 硬恢复会触发 retry-then-fail 循环。配置 ConditionalOrderTTLDays=0 关闭。)
                {
                    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    if (ConditionalOrderManager::is_expired(order.created_at_ms,
                                                            conditional_order_ttl_days_, now_ms)) {
                        LOG_INFO("conditional order expired by TTL: id=" +
                                 std::to_string(order.id) +
                                 " age_ms=" + std::to_string(now_ms - order.created_at_ms) +
                                 " ttl_days=" + std::to_string(conditional_order_ttl_days_));
                        continue;
                    }
                }

                cond_order_mgr_.restore(order);
                continue;
            }

            // Block
            if (kind == "close_task") {
                CloseTask task{};
                std::string field;

                std::getline(iss, field, '\t');
                task.id = static_cast<uint32_t>(std::stoul(field));

                std::getline(iss, field, '\t');
                safe_copy(task.instrument_id, field.c_str(), sizeof(task.instrument_id));

                std::getline(iss, field, '\t');
                safe_copy(task.exchange_id, field.c_str(), sizeof(task.exchange_id));

                //
                if (!field.empty() && (field[0] == '-' || std::isdigit(static_cast<unsigned char>(field[0])))) {
                    //
                    task.pos_direction = static_cast<Direction>(std::stoi(field));
                } else {
                    safe_copy(task.account_id, field.c_str(), sizeof(task.account_id));
                    std::getline(iss, field, '\t');
                    task.pos_direction = static_cast<Direction>(std::stoi(field));
                }

                std::getline(iss, field, '\t');
                task.offset = static_cast<Offset>(std::stoi(field));

                std::getline(iss, field, '\t');
                task.target_volume = std::stoi(field);

                std::getline(iss, field, '\t');
                task.filled_volume = std::stoi(field);

                std::getline(iss, field, '\t');
                task.retry_count = std::stoi(field);

                std::getline(iss, field, '\t');
                task.last_price = std::stod(field);

                std::getline(iss, field, '\t');
                task.upper_limit = std::stod(field);

                std::getline(iss, field, '\t');
                task.lower_limit = std::stod(field);

                std::string tail_field;
                std::getline(iss, field, '\t');
                if (std::getline(iss, tail_field, '\t')) {
                    task.order_ref = field;
                    task.state = tail_field.empty() ? CloseTaskState::Pending
                                                    : static_cast<CloseTaskState>(std::stoi(tail_field));
                } else {
                    task.state = field.empty() ? CloseTaskState::Pending
                                               : static_cast<CloseTaskState>(std::stoi(field));
                }

                restored_close_tasks.push_back(task);
            }
        }
    }

    // Block
    if (!restored_close_tasks.empty()) {
        close_mgr_.restore_tasks(restored_close_tasks);
    }

    const bool loaded_kline_store = kline_mgr_.load_store();
    if (!loaded_kline_store && !restored_kline_bars.empty()) {
        kline_mgr_.restore_legacy_bars(restored_kline_bars);
    }

    if (!loaded_text_state && !loaded_kline_store) {
        return;
    }

    LOG_INFO("runtime message");
}

// Block
void TradingEngine::request_async_save() {
    dirty_.store(true, std::memory_order_relaxed);
    save_cv_.notify_one();
}

// Block
void TradingEngine::async_save_loop() {
    std::unique_lock<std::mutex> lock(save_mtx_);
    while (save_running_.load(std::memory_order_relaxed)) {
        save_cv_.wait_for(lock, std::chrono::seconds(5));
        if (dirty_.exchange(false, std::memory_order_relaxed)) {
            //
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            lock.lock();
            try {
                save_runtime_state();
            } catch (const std::exception& e) {
                LOG_ERROR(std::string("异步保存失败: ") + e.what());
            } catch (...) {
                LOG_ERROR("异步保存未知异常");
            }
        }
    }
    // Block
    if (dirty_.exchange(false, std::memory_order_relaxed)) {
        try {
            save_runtime_state();
        } catch (...) {}
    }
}

// Save runtime state to file (将运行时状态保存到文件)
void TradingEngine::save_runtime_state() const {
    std::lock_guard<std::mutex> lock(save_file_mtx_);
    const std::filesystem::path tmp_path = runtime_state_path_.string() + ".tmp";
    std::ofstream ofs(tmp_path, std::ios::trunc);
    if (!ofs.is_open()) {
        LOG_INFO("runtime message");
        return;
    }

    ofs << "#HFTS_V1\n";

    // Persist global and per-strategy lifecycle states.
    const StrategyState state = strategy_ctrl_.get_global_state();
    ofs << "strategy_state\t" << strategy_state_name(state) << "\n";
    {
        const auto states = strategy_ctrl_.snapshot_states();
        for (const auto& [strategy_id, strategy_state] : states) {
            ofs << "strategy_instance_state\t" << strategy_id << "\t"
                << strategy_state_name(strategy_state) << "\n";
        }
    }

    // Persist day_start_balance for each account (S0-05 fix)
    for (const auto* ctx : account_mgr_.all_accounts()) {
        const double dsb = ctx->risk_mgr.get_day_start_balance();
        const std::string td = ctx->risk_mgr.get_trading_day();
        if (dsb > 0.0) {
            ofs << "day_start_balance\t" << ctx->account_id << "\t"
                << std::fixed << std::setprecision(2) << dsb << "\t"
                << td << "\n";
        }
    }

    // Order manager
    cond_order_mgr_.for_each_active_order([&ofs](const ConditionalOrder& order) {
        ofs << "cond_order\t"
            << order.id << "\t"
            << order.instrument_id << "\t"
            << static_cast<int>(order.type) << "\t"
            << static_cast<int>(order.direction) << "\t"
            << order.trigger_price << "\t"
            << order.trail_offset << "\t"
            << order.volume << "\t"
            << order.extreme_price << "\t"
            << order.group_id << "\t"
            << order.account_id << "\t"
            << order.strategy_id << "\t"
            << static_cast<int>(order.offset) << "\t"
            << order.order_price << "\t"
            << order.idempotency_key << "\t"
            << static_cast<int>(order.status) << "\t"
            << order.created_at_ms << "\t"
            << order.triggered_at_ms << "\t"
            << order.cancelled_at_ms << "\t"
            << order.reject_reason << "\n";
    });

    // Save unfinished close tasks (保存尚未完成的平仓任务)
for (const auto& task : close_mgr_.get_tasks()) {
        if (task.state == CloseTaskState::Done || task.state == CloseTaskState::Failed) {
            continue;
        }
        ofs << "close_task\t"
            << task.id << "\t"
            << task.instrument_id << "\t"
            << task.exchange_id << "\t"
            << task.account_id << "\t"
            << static_cast<int>(task.pos_direction) << "\t"
            << static_cast<int>(task.offset) << "\t"
            << task.target_volume << "\t"
            << task.filled_volume << "\t"
            << task.retry_count << "\t"
            << task.last_price << "\t"
            << task.upper_limit << "\t"
            << task.lower_limit << "\t"
            << task.order_ref << "\t"
            << static_cast<int>(task.state) << "\n";
    }

    // Write integrity checksum: byte count before this line
    const auto pos = ofs.tellp();
    const uint64_t byte_count = static_cast<uint64_t>(pos);
    ofs << "#CHECKSUM\t" << byte_count << "\n";

    ofs.flush();
    ofs.close();

    // Atomic replace: back up old file first, then rename (原子替换: 先备份旧文件, 再 rename)
    std::error_code ec;
    const std::filesystem::path bak_path = runtime_state_path_.string() + ".bak";
    if (std::filesystem::exists(runtime_state_path_)) {
        std::filesystem::copy_file(runtime_state_path_, bak_path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    std::filesystem::rename(tmp_path, runtime_state_path_, ec);
    if (ec) {
        LOG_INFO("runtime message");
        // fallback: copy then remove the temporary file.
        std::filesystem::copy_file(tmp_path, runtime_state_path_,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp_path, ec);
    }

    kline_mgr_.save_store();
}

void TradingEngine::load_runtime_cache() {
    std::ifstream ifs(runtime_cache_path_, std::ios::binary);
    if (!ifs.is_open()) {
        return;
    }
    char magic[sizeof(kRuntimeCacheMagic) - 1]{};
    if (!ifs.read(magic, static_cast<std::streamsize>(sizeof(magic))) ||
        std::string(magic, sizeof(magic)) != kRuntimeCacheMagic) {
        return;
    }
    uint64_t recorded = 0;
    if (!read_varuint(ifs, &recorded)) return;
    processed_tick_count_.store(static_cast<size_t>(recorded), std::memory_order_relaxed);

    uint64_t count = 0;
    if (!read_varuint(ifs, &count)) return;
    {
        std::lock_guard<std::mutex> lock(pnl_curve_mtx_);
        pnl_curve_.clear();
        for (uint64_t i = 0; i < count; ++i) {
            PnlCurvePoint p{};
            uint64_t ts = 0;
            int64_t balance = 0, available = 0, margin = 0, pos = 0, total = 0;
            if (!read_varuint(ifs, &ts) || !read_string(ifs, &p.time) ||
                !read_varint(ifs, &balance) || !read_varint(ifs, &available) ||
                !read_varint(ifs, &margin) || !read_varint(ifs, &pos) || !read_varint(ifs, &total)) return;
            p.timestamp_ms = static_cast<int64_t>(ts);
            p.balance = restore_turnover(balance);
            p.available = restore_turnover(available);
            p.margin = restore_turnover(margin);
            p.position_profit = restore_turnover(pos);
            p.total_pnl = restore_turnover(total);
            pnl_curve_.push_back(p);
        }
    }

    if (!read_varuint(ifs, &count)) return;
    {
        std::vector<StrategyPerformanceSnapshot> perf_items;
        for (uint64_t i = 0; i < count; ++i) {
            StrategyPerformanceSnapshot p{};
            uint64_t trades = 0;
            int64_t realized = 0, floating = 0;
            if (!read_string(ifs, &p.strategy_id) || !read_varuint(ifs, &trades) ||
                !read_varint(ifs, &realized) || !read_varint(ifs, &floating)) return;
            p.trade_count = static_cast<size_t>(trades);
            p.realized_pnl = restore_turnover(realized);
            p.floating_pnl = restore_turnover(floating);
            p.total_pnl = p.realized_pnl + p.floating_pnl;
            perf_items.push_back(std::move(p));
        }
        strategy_ctrl_.clear_performance();
        strategy_ctrl_.restore_performance(perf_items);
    }
}

void TradingEngine::save_runtime_cache() const {
    std::vector<PnlCurvePoint> pnl_snapshot;
    {
        std::lock_guard<std::mutex> lock(pnl_curve_mtx_);
        pnl_snapshot.assign(pnl_curve_.begin(), pnl_curve_.end());
    }
    std::vector<StrategyPerformanceSnapshot> perf_snapshot = strategy_ctrl_.snapshot_performance();

    const std::filesystem::path tmp_path = runtime_cache_path_.string() + ".tmp";
    std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return;
    ofs.write(kRuntimeCacheMagic, static_cast<std::streamsize>(sizeof(kRuntimeCacheMagic) - 1));
    if (!write_varuint(ofs, processed_tick_count_.load(std::memory_order_relaxed)) ||
        !write_varuint(ofs, pnl_snapshot.size())) return;
    for (const auto& p : pnl_snapshot) {
        if (!write_varuint(ofs, static_cast<uint64_t>((std::max)(int64_t{0}, p.timestamp_ms))) ||
            !write_string(ofs, p.time) ||
            !write_varint(ofs, fixed_turnover(p.balance)) ||
            !write_varint(ofs, fixed_turnover(p.available)) ||
            !write_varint(ofs, fixed_turnover(p.margin)) ||
            !write_varint(ofs, fixed_turnover(p.position_profit)) ||
            !write_varint(ofs, fixed_turnover(p.total_pnl))) return;
    }
    if (!write_varuint(ofs, perf_snapshot.size())) return;
    for (const auto& p : perf_snapshot) {
        if (!write_string(ofs, p.strategy_id) ||
            !write_varuint(ofs, p.trade_count) ||
            !write_varint(ofs, fixed_turnover(p.realized_pnl)) ||
            !write_varint(ofs, fixed_turnover(p.floating_pnl))) return;
    }
    ofs.close();
    std::error_code ec;
    std::filesystem::rename(tmp_path, runtime_cache_path_, ec);
    if (ec) {
        std::filesystem::copy_file(tmp_path, runtime_cache_path_,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp_path, ec);
    }
}

// Alert
void TradingEngine::record_alert(const std::string& message) {
    std::lock_guard<std::mutex> lock(alerts_mtx_);
    recent_alerts_.push_back(message);
    while (recent_alerts_.size() > 100) { // Keep at most 100 recent alerts (最多保留最近100条告警)
        recent_alerts_.pop_front();
    }
}

// Resolve account context from char* — if null/empty, returns default account.
// (根据 char* 解析账户上下文, 为空则返回默认账户)
AccountContext* TradingEngine::resolve_account(const char* account_id) {
    if (!account_id || account_id[0] == '\0') {
        return account_mgr_.default_account();
    }
    return account_mgr_.find_account(std::string(account_id));
}

// Resolve account context from string — if empty, returns default account.
// (根据 string 解析账户上下文, 为空则返回默认账户)
AccountContext* TradingEngine::resolve_account(const std::string& account_id) {
    if (account_id.empty()) {
        return account_mgr_.default_account();
    }
    return account_mgr_.find_account(account_id);
}

} // namespace hft























