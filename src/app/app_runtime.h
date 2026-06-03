#pragma once
// ============================================
// app_runtime.h - Application runtime: lifecycle, hot-reload, retry (应用运行时: 生命周期/热加载/重试)
// Orchestrates TradingEngine startup with config migration, strategy loading, and gateway registration.
// 编排 TradingEngine 启动, 包括配置迁移、策略加载和网关注册。
// ============================================

#include "common/config.h"
#include "common/config_store.h"
#include "engine/trading_engine.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace hft {

class AppRuntime {
public:
    AppRuntime();
    ~AppRuntime();

    bool initialize(const std::string& config_path, std::function<void()> shutdown_callback);
    bool start(int startup_delay_ms = 0);
    void stop();

    TradingEngine* engine() { return engine_.get(); }
    const TradingEngine* engine() const { return engine_.get(); }
    ConfigStore* store() { return store_.get(); }
    const Config& config() const;

    // Hot-load a single strategy (takes effect immediately, no engine restart needed)
    // 热加载单个策略 (导入后立即生效, 无需重启引擎)
    bool load_single_strategy(const StrategyConfigSpec& spec);

private:
    bool load_strategies();
    bool start_engine_with_retry();
    void start_engine_async(int startup_delay_ms);

    std::string config_path_;
    std::unique_ptr<ConfigStore> store_;
    std::unique_ptr<TradingEngine> engine_;
    std::thread engine_start_thread_;
    std::atomic<bool> stopping_{false};
};

} // namespace hft
