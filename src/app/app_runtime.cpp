// ============================================
// app_runtime.cpp - Application runtime implementation (应用运行时实现)
// Handles config migration, strategy loading (simple + Python), engine lifecycle with retry.
// 处理配置迁移、策略加载 (simple + Python)、引擎生命周期及重试。
// ============================================

#include "app/app_runtime.h"

#include "common/logger.h"
#include "engine/account_config.h"
#include "gateway/ctp_md_gateway.h"
#include "gateway/ctp_trade_gateway.h"
#ifdef HFT_HAS_QDP
#include "gateway/qdp_md_gateway.h"
#include "gateway/qdp_trade_gateway.h"
#endif
#include "gateway/shm_md_gateway.h"
#include "gateway/dual_md_gateway.h"
#include "gateway/fix_md_gateway.h"
#include "gateway/fix_trade_gateway.h"
#include "gateway/udp_md_gateway.h"
#include "strategy/simple_strategy.h"
#include "strategy/strategy_config.h"
#include "common/config_validator.h"
#ifdef ENABLE_PYTHON
#include "strategy/py_strategy.h"
#include <pybind11/embed.h>
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

namespace hft {

#ifdef ENABLE_PYTHON
namespace {

bool python_runtime_enabled(const Config& config) {
    return config.get_int("Runtime", "EnablePython", 1) > 0;
}

std::string detect_python_home(const Config& config) {
    std::string home = config.get_string("Runtime", "PythonHome", "");
    if (!home.empty() && std::filesystem::exists(std::filesystem::path(home) / "Lib" / "site.py")) {
        return home;
    }

    if (const char* env_home = std::getenv("PYTHONHOME")) {
        home = env_home;
        if (!home.empty() && std::filesystem::exists(std::filesystem::path(home) / "Lib" / "site.py")) {
            return home;
        }
    }

#ifdef _WIN32
    std::filesystem::path exe_dir;
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        exe_dir = std::filesystem::path(buf).parent_path();
    }
    const std::vector<std::string> candidates = {
        (exe_dir / "python").string(),
        (exe_dir / "Python39").string(),
        "C:\\Python39",
        "C:\\Users\\Administrator\\AppData\\Local\\Programs\\Python\\Python39"
    };
#else
    const std::vector<std::string> candidates = {
        "/usr",
        "/usr/local"
    };
#endif
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(std::filesystem::path(candidate) / "Lib" / "site.py")) {
            return candidate;
        }
        if (std::filesystem::exists(std::filesystem::path(candidate) / "lib" / "python3.9" / "site.py")) {
            return candidate;
        }
    }
    return "";
}

void set_env_var(const char* key, const std::string& value) {
    if (value.empty()) {
        return;
    }
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

void configure_python_environment(const Config& config) {
    const std::string home = detect_python_home(config);
    if (home.empty()) {
        LOG_WARN("PythonHome not found; embedded Python may fail to locate standard library.");
        return;
    }

    const std::filesystem::path home_path(home);
    std::vector<std::string> python_paths;
    const auto add_if_exists = [&](const std::filesystem::path& path) {
        if (std::filesystem::exists(path)) {
            python_paths.push_back(path.string());
        }
    };
#ifdef _WIN32
    add_if_exists(home_path / "Lib");
    add_if_exists(home_path / "DLLs");
    add_if_exists(home_path / "Lib" / "site-packages");
    const char separator = ';';
#else
    add_if_exists(home_path / "lib" / "python3.9");
    add_if_exists(home_path / "lib" / "python3.9" / "site-packages");
    const char separator = ':';
#endif

    std::string python_path;
    for (const auto& path : python_paths) {
        if (!python_path.empty()) {
            python_path.push_back(separator);
        }
        python_path += path;
    }

    set_env_var("PYTHONHOME", home);
    set_env_var("PYTHONPATH", python_path);
    LOG_INFO("Embedded Python environment configured: PYTHONHOME=" + home);
}

class PythonRuntimeGuard {
public:
    explicit PythonRuntimeGuard(const Config& config) {
        configure_python_environment(config);
        interpreter_ = std::make_unique<pybind11::scoped_interpreter>();
        release_gil_ = std::make_unique<pybind11::gil_scoped_release>();
        LOG_INFO("Python interpreter initialized.");
    }

private:
    std::unique_ptr<pybind11::scoped_interpreter> interpreter_;
    std::unique_ptr<pybind11::gil_scoped_release> release_gil_;
};

} // namespace
#endif

namespace {

void migrate_instrument_specs(const Config& cfg, ConfigStore& store) {
    for (const auto& sec : cfg.get_sections()) {
        if (sec.rfind("Instrument.", 0) != 0) continue;
        InstrumentSpec spec;
        spec.instrument_id = sec.substr(10);
        spec.exchange_id = cfg.get_string(sec, "ExchangeID", "");
        spec.product_id = cfg.get_string(sec, "ProductID", "");
        spec.expire_date = cfg.get_string(sec, "ExpireDate", "");
        spec.start_deliv_date = cfg.get_string(sec, "StartDelivDate", "");
        spec.end_deliv_date = cfg.get_string(sec, "EndDelivDate", "");
        const std::string life_phase = cfg.get_string(sec, "InstLifePhase", "");
        spec.inst_life_phase = life_phase.empty() ? '\0' : life_phase[0];
        spec.is_trading = cfg.get_int(sec, "IsTrading", 0) != 0;
        spec.price_tick = cfg.get_double(sec, "PriceTick", 0.2);
        spec.volume_multiple = cfg.get_int(sec, "VolumeMultiple", 1);
        spec.long_margin_ratio = cfg.get_double(sec, "LongMarginRatio", 0.12);
        spec.short_margin_ratio = cfg.get_double(sec, "ShortMarginRatio", 0.12);
        spec.open_commission_rate = cfg.get_double(sec, "OpenCommissionRate", 0.00002);
        spec.close_commission_rate = cfg.get_double(sec, "CloseCommissionRate", 0.00002);
        spec.close_today_commission_rate = cfg.get_double(sec, "CloseTodayCommissionRate", 0.00002);
        store.save_instrument_spec(spec);
    }
}

} // namespace

AppRuntime::AppRuntime() = default;

AppRuntime::~AppRuntime() {
    stop();
}

bool AppRuntime::initialize(const std::string& config_path, std::function<void()> shutdown_callback) {
#ifdef ENABLE_PYTHON
    Config bootstrap_config;
    bootstrap_config.load(config_path);
    if (python_runtime_enabled(bootstrap_config)) {
        static std::unique_ptr<PythonRuntimeGuard> python_runtime_guard;
        if (!python_runtime_guard) {
            python_runtime_guard = std::make_unique<PythonRuntimeGuard>(bootstrap_config);
        }
    } else {
        LOG_INFO("Python runtime disabled by Runtime.EnablePython=0; skipping embedded Python initialization.");
    }
#endif

    config_path_ = config_path;
    engine_ = std::make_unique<TradingEngine>();

    engine_->get_account_manager().register_gateway_factory(
        [](const std::string& gateway_type) -> std::unique_ptr<ITradeGateway> {
            if (gateway_type == "CTP") {
                return std::make_unique<CtpTradeGateway>();
            }
#ifdef HFT_HAS_QDP
            if (gateway_type == "QDP") {
                return std::make_unique<QdpTradeGateway>();
            }
#endif
            LOG_ERROR("unsupported trade gateway type: " + gateway_type);
            return nullptr;
        });

    engine_->register_md_gateway_factory(
        [](const std::string& gateway_type) -> std::unique_ptr<IMdGateway> {
            if (gateway_type == "CTP" || gateway_type.empty()) {
                return std::make_unique<CtpMdGateway>();
            }
#ifdef HFT_HAS_QDP
            if (gateway_type == "QDP") {
                return std::make_unique<QdpMdGateway>();
            }
#endif
            if (gateway_type == "SHM") {
                return std::make_unique<ShmMdGateway>();
            }
            if (gateway_type == "FIX") {
                return std::make_unique<FixMdGateway>();
            }
            if (gateway_type == "UDP") {
                return std::make_unique<UdpMdGateway>();
            }
            if (gateway_type == "CTP_DUAL") {
                return std::make_unique<DualMdGateway>();
            }
            LOG_ERROR("unsupported md gateway type: " + gateway_type + ", falling back to CTP");
            return std::make_unique<CtpMdGateway>();
        });

    store_ = std::make_unique<ConfigStore>();
    {
        std::string db_dir = config_path_;
        const auto sep = db_dir.find_last_of("/\\");
        if (sep != std::string::npos) {
            db_dir = db_dir.substr(0, sep + 1);
        } else {
            db_dir.clear();
        }

        const std::string db_path = db_dir + "hft_data.db";
        if (!store_->init(db_path)) {
            LOG_ERROR("ConfigStore initialization failed: " + db_path);
            store_.reset();
        } else {
            engine_->set_config_store(store_.get());
        }
    }

    if (!engine_->init(config_path_)) {
        LOG_ERROR("engine initialization failed, check config.ini");
        engine_.reset();
        return false;
    }

    {
        ConfigValidator validator;
        std::vector<ConfigValidationError> validation_errors;
        if (!validator.validate(engine_->get_config(), validation_errors)) {
            for (const auto& e : validation_errors) {
                LOG_ERROR("config validation: [" + e.section + "] " + e.key + " — " + e.message);
            }
            LOG_ERROR("config validation failed with " +
                      std::to_string(validation_errors.size()) + " error(s), aborting startup");
            engine_.reset();
            return false;
        }
        LOG_INFO("config validation passed");
    }

    if (store_ && engine_) {
        if (!store_->has_migrated_config()) {
            LOG_WARN("ConfigStore: first startup, migrating config.ini to SQLite");
            const auto& cfg = engine_->get_config();

            {
                std::ifstream src(config_path_, std::ios::binary);
                std::ofstream dst(config_path_ + ".bak", std::ios::binary);
                if (src.is_open() && dst.is_open()) {
                    dst << src.rdbuf();
                    LOG_INFO("ConfigStore: backed up config.ini to config.ini.bak");
                }
            }

            const auto bundle = load_account_configs(cfg);
            if (!bundle.accounts.empty()) {
                store_->save_account_bundle(bundle);
                LOG_INFO("ConfigStore: migrated " + std::to_string(bundle.accounts.size()) + " account(s)");
            }

            store_->save_risk_config("MaxOrderSize", cfg.get_string("Risk", "MaxOrderSize", "5"));
            store_->save_risk_config("MaxNetPosition", cfg.get_string("Risk", "MaxNetPosition", "10"));
            store_->save_risk_config("MaxOrdersPerMinute", cfg.get_string("Risk", "MaxOrdersPerMinute", "30"));
            store_->save_risk_config("MaxCancelRate", cfg.get_string("Risk", "MaxCancelRate", "0.5"));
            store_->save_risk_config("MaxDailyLoss", cfg.get_string("Risk", "MaxDailyLoss", "5000"));
            store_->save_risk_config("CancelRateWindowMinutes", cfg.get_string("Risk", "CancelRateWindowMinutes", "60"));
            store_->save_risk_config("TradingSessions", cfg.get_string("Trading", "TradingSessions", ""));

            const std::string ai_key = cfg.get_string("AI", "ApiKey", "");
            if (!ai_key.empty()) store_->save_encrypted_system_config("ai_api_key", ai_key);
            store_->save_system_config("ai_enabled", cfg.get_string("AI", "Enabled", "0"));
            store_->save_system_config("ai_provider", cfg.get_string("AI", "Provider", ""));
            store_->save_system_config("ai_endpoint", cfg.get_string("AI", "Endpoint", ""));
            store_->save_system_config("ai_model", cfg.get_string("AI", "Model", ""));
            store_->save_system_config("ai_timeout_seconds", cfg.get_string("AI", "TimeoutSeconds", "20"));
            store_->save_system_config("ai_max_prompt_chars", cfg.get_string("AI", "MaxPromptChars", "12000"));

            const std::string webhook_secret = cfg.get_string("Alerts", "WebhookSecret", "");
            if (!webhook_secret.empty()) store_->save_encrypted_system_config("alert_webhook_secret", webhook_secret);
            store_->save_system_config("alert_webhook_enabled", cfg.get_string("Alerts", "WebhookEnabled", "0"));
            store_->save_system_config("alert_webhook_url", cfg.get_string("Alerts", "WebhookUrl", ""));
            store_->save_system_config("alert_min_level", cfg.get_string("Alerts", "MinLevel", "WARN"));

            const auto strategy_specs = load_strategy_specs(cfg);
            if (!strategy_specs.empty()) {
                store_->save_strategy_specs(strategy_specs);
                LOG_INFO("ConfigStore: migrated " + std::to_string(strategy_specs.size()) + " strategy spec(s)");
            }

            migrate_instrument_specs(cfg, *store_);

            Config slim;
            slim.set_string("Log", "Level", cfg.get_string("Log", "Level", "INFO"));
            slim.set_string("Log", "Directory", cfg.get_string("Log", "Directory", "logs"));
            slim.set_string("Log", "FilePrefix", cfg.get_string("Log", "FilePrefix", "hft"));
            slim.set_string("Log", "QueueCapacity", cfg.get_string("Log", "QueueCapacity", "8192"));
            slim.set_string("Log", "RecentBufferSize", cfg.get_string("Log", "RecentBufferSize", "400"));
            slim.set_string("Log", "FlushIntervalMs", cfg.get_string("Log", "FlushIntervalMs", "1000"));
            slim.set_string("Log", "RetentionDays", cfg.get_string("Log", "RetentionDays", "7"));
            slim.set_string("Performance", "EngineCpuCore", cfg.get_string("Performance", "EngineCpuCore", "2"));
            slim.set_string("Performance", "LoggerCpuCore", cfg.get_string("Performance", "LoggerCpuCore", "-1"));
            slim.set_string("Performance", "ProductionHftMode", cfg.get_string("Performance", "ProductionHftMode", "0"));
            slim.set_string("Performance", "MdBatchSize", cfg.get_string("Performance", "MdBatchSize", "512"));
            slim.set_string("Performance", "DisablePythonHotPath", cfg.get_string("Performance", "DisablePythonHotPath", "0"));
            slim.set_string("Performance", "DisableTickRecordingHotPath", cfg.get_string("Performance", "DisableTickRecordingHotPath", "0"));
            slim.set_string("Performance", "DisableKlineHotPath", cfg.get_string("Performance", "DisableKlineHotPath", "0"));
            slim.set_string("Performance", "StrategyHotInstrumentsOnly", cfg.get_string("Performance", "StrategyHotInstrumentsOnly", "1"));
            slim.set_string("Runtime", "RunMode", cfg.get_string("Runtime", "RunMode", "service"));
            slim.set_string("Runtime", "StateFile", cfg.get_string("Runtime", "StateFile", "runtime_state.dat"));
            slim.set_string("Runtime", "NoTickWarnSeconds", cfg.get_string("Runtime", "NoTickWarnSeconds", "10"));
            const auto global_instruments = cfg.get_string("Strategy", "Instruments", "");
            if (!global_instruments.empty()) {
                slim.set_string("Strategy", "Instruments", global_instruments);
            }
            slim.save(config_path_);

            store_->mark_migrated();
            LOG_WARN("ConfigStore: migration completed with " +
                     std::to_string(bundle.accounts.size()) + " account(s)");
        }

        if (store_->has_migrated_config() &&
            store_->load_system_config("config_migrated_v2", "") != "1") {
            const auto& cfg = engine_->get_config();
            const auto specs = load_strategy_specs(cfg);
            if (!specs.empty()) {
                store_->save_strategy_specs(specs);
                LOG_INFO("ConfigStore: v2 migrated " + std::to_string(specs.size()) + " strategy spec(s)");
            }

            migrate_instrument_specs(cfg, *store_);

            store_->save_system_config("config_migrated_v2", "1");
            LOG_INFO("ConfigStore: v2 migration completed");
        }
    }

    if (engine_ && !load_strategies()) {
        LOG_WARN("strategy loading failed; trading engine stays available for market data and manual trading");
    }

    return true;
}

bool AppRuntime::start(int startup_delay_ms) {
    if (engine_) {
        start_engine_async(startup_delay_ms);
    } else {
        LOG_ERROR("trading engine not initialized");
        return false;
    }
    return true;
}

void AppRuntime::stop() {
    stopping_.store(true, std::memory_order_relaxed);
    if (engine_start_thread_.joinable()) {
        engine_start_thread_.join();
    }
    if (engine_) {
        engine_->stop();
    }
    if (store_) {
        store_->shutdown();
    }
}

const Config& AppRuntime::config() const {
    static Config empty_config;
    if (engine_) return engine_->get_config();
    empty_config.load(config_path_);
    return empty_config;
}

bool AppRuntime::load_strategies() {
    const Config& config = engine_->get_config();
    const auto strategy_specs = load_strategy_specs(config);
    if (strategy_specs.empty()) {
        LOG_WARN("no strategy configured; engine will run without strategy instances");
        return true;
    }

    size_t loaded_count = 0;
    for (const auto& spec : strategy_specs) {
        if (spec.type == "python") {
#ifdef ENABLE_PYTHON
            if (!python_runtime_enabled(config)) {
                LOG_WARN("python strategy skipped because Runtime.EnablePython is disabled, id=" + spec.id);
                continue;
            }
            if (spec.script_path.empty()) {
                LOG_ERROR("python strategy missing ScriptPath, id=" + spec.id);
                return false;
            }
            const std::string resolved_script_path = resolve_strategy_script_path(config_path_, spec.script_path);
            auto strategy = std::make_shared<PyStrategy>(resolved_script_path);
            strategy->configure_context(spec.id, spec.account_id, spec.instruments);
            strategy->configure_metadata(spec.type, resolved_script_path, build_runtime_param_map(spec));
            strategy->set_version(spec.version);
            if (engine_->add_strategy(strategy)) {
                ++loaded_count;
                LOG_INFO("loaded python strategy: id=" + spec.id + " path=" + resolved_script_path);
            } else {
                LOG_WARN("python strategy skipped because it is already loaded, id=" + spec.id);
            }
#else
            LOG_ERROR("Python 策略已配置但当前程序未启用 Python 支持，策略已跳过: id=" + spec.id);
            continue;
#endif
            continue;
        }

        if (spec.instruments.empty()) {
            LOG_ERROR("simple strategy missing Instruments, strategy skipped: id=" + spec.id);
            continue;
        }
        const std::string instrument = spec.instruments.front();
        auto strategy = std::make_shared<SimpleStrategy>(
            instrument.c_str(), spec.order_size, spec.momentum_ticks, spec.cooldown_seconds);
        strategy->configure_context(spec.id, spec.account_id, spec.instruments);
        strategy->configure_metadata(spec.type, "", build_runtime_param_map(spec));
        strategy->set_version(spec.version);
        if (engine_->add_strategy(strategy)) {
            ++loaded_count;
            LOG_INFO("loaded simple strategy: id=" + spec.id + " instrument=" + instrument);
        } else {
            LOG_WARN("simple strategy skipped because it is already loaded, id=" + spec.id);
        }
    }

    if (loaded_count == 0) {
        LOG_WARN("no strategy loaded; configured strategy count=" + std::to_string(strategy_specs.size()) +
                 ". Engine will run without strategy instances.");
        return true;
    }
    return true;
}

bool AppRuntime::load_single_strategy(const StrategyConfigSpec& spec) {
    if (!engine_) return false;

    const Config& config = engine_->get_config();

    if (spec.type == "python") {
#ifdef ENABLE_PYTHON
        if (!python_runtime_enabled(config)) {
            LOG_WARN("python strategy skipped because Runtime.EnablePython is disabled, id=" + spec.id);
            return false;
        }
        if (spec.script_path.empty()) {
            LOG_ERROR("python strategy missing ScriptPath, id=" + spec.id);
            return false;
        }
        const std::string resolved_script_path = resolve_strategy_script_path(config_path_, spec.script_path);
        try {
            py::gil_scoped_acquire gil;
            py::module_ builtins = py::module_::import("builtins");
            std::ifstream ifs(resolved_script_path);
            if (!ifs.is_open()) {
                LOG_ERROR("python strategy script not found: " + resolved_script_path);
                return false;
            }
            std::string source((std::istreambuf_iterator<char>(ifs)),
                               std::istreambuf_iterator<char>());
            builtins.attr("compile")(source, resolved_script_path, "exec");
        } catch (const py::error_already_set& e) {
            LOG_ERROR("python strategy syntax error: id=" + spec.id + " error=" + e.what());
            return false;
        }
        auto strategy = std::make_shared<PyStrategy>(resolved_script_path);
        strategy->configure_context(spec.id, spec.account_id, spec.instruments);
        strategy->configure_metadata(spec.type, resolved_script_path, build_runtime_param_map(spec));
        strategy->set_version(spec.version);
        if (!engine_->add_strategy(strategy)) {
            LOG_WARN("hot-loaded python strategy skipped because it is already loaded, id=" + spec.id);
            return false;
        }
        LOG_INFO("hot-loaded python strategy: id=" + spec.id + " path=" + resolved_script_path);
        return true;
#else
        LOG_ERROR("Python 策略已配置但当前程序未启用 Python 支持，策略已跳过: id=" + spec.id);
        return false;
#endif
    }

    if (spec.instruments.empty()) {
        LOG_ERROR("simple strategy missing Instruments, hot-load rejected: id=" + spec.id);
        return false;
    }
    const std::string instrument = spec.instruments.front();
    auto strategy = std::make_shared<SimpleStrategy>(
        instrument.c_str(), spec.order_size, spec.momentum_ticks, spec.cooldown_seconds);
    strategy->configure_context(spec.id, spec.account_id, spec.instruments);
    strategy->configure_metadata(spec.type, "", build_runtime_param_map(spec));
    strategy->set_version(spec.version);
    if (!engine_->add_strategy(strategy)) {
        LOG_WARN("hot-loaded simple strategy skipped because it is already loaded, id=" + spec.id);
        return false;
    }
    LOG_INFO("hot-loaded simple strategy: id=" + spec.id + " instrument=" + instrument);
    return true;
}

void AppRuntime::start_engine_async(int startup_delay_ms) {
    stopping_.store(false, std::memory_order_relaxed);
    engine_start_thread_ = std::thread([this, startup_delay_ms]() {
        if (startup_delay_ms > 0) {
            LOG_INFO("startup delay before trading engine: " + std::to_string(startup_delay_ms) + " ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(startup_delay_ms));
        }
        if (stopping_.load(std::memory_order_relaxed)) {
            return;
        }
        if (!start_engine_with_retry()) {
            LOG_ERROR("engine startup failed");
        }
    });
}

bool AppRuntime::start_engine_with_retry() {
    constexpr int kEngineStartAttempts = 3;
    for (int attempt = 1; attempt <= kEngineStartAttempts; ++attempt) {
        if (engine_->start()) {
            return true;
        }

        LOG_WARN("engine startup failed, attempt=" + std::to_string(attempt) +
                 "/" + std::to_string(kEngineStartAttempts));
        engine_->stop();
        if (attempt < kEngineStartAttempts) {
            constexpr int kRetryDelaySec = 10;
            LOG_INFO("retry engine startup after " + std::to_string(kRetryDelaySec) + " seconds");
            std::this_thread::sleep_for(std::chrono::seconds(kRetryDelaySec));
        }
    }
    return false;
}

} // namespace hft
