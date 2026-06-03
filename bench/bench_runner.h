#pragma once
// ============================================
// bench/bench_runner.h — Core harness for standalone benchmark binary (独立 benchmark 二进制核心 harness)
// Reuses QPCLatencyStats from src/common/qpc_timer.h for scene registration,
// config passing, and Markdown/CSV/JSON output generation.
// 复用 src/common/qpc_timer.h 的 QPCLatencyStats, 负责场景注册、配置传递、Markdown/CSV/JSON 输出生成。
// ============================================

#include "common/qpc_timer.h"

#include <functional>
#include <string>
#include <vector>

namespace hft::bench {

// Result for a single benchmark scenario (单个 benchmark 场景的运行结果).
// Latency scenarios fill the latency field; throughput scenarios set is_throughput=true and throughput_ops_sec.
// 延迟场景填 latency 字段; 吞吐场景设置 is_throughput=true 并填 throughput_ops_sec
struct BenchResult {
    std::string name;
    size_t n = 0;
    QPCLatencyStats::Percentiles latency{};
    double throughput_ops_sec = 0.0;
    bool is_throughput = false;
};

// Scenario run configuration (injected from command-line) — 场景运行配置 (从命令行注入).
// runs:    repeats for stable percentiles (重复次数, 用于稳定 percentile)
// warmup:  discarded rounds for JIT/cache warming (丢弃的预热轮数, JIT/缓存预热)
// samples: samples per round for latency scenarios (延迟场景每轮采样数)
struct BenchConfig {
    int runs = 3;
    int warmup = 1;
    size_t samples = 10000;
};

using BenchFn = std::function<BenchResult(const BenchConfig&)>;

struct BenchCase {
    std::string name;
    BenchFn fn;
};

// Global scenario registry. HFT_BENCH macro registers scenarios during static init.
// 全局场景注册表。HFT_BENCH 宏在静态初始化期将场景注册进来。
class BenchRegistry {
public:
    static BenchRegistry& instance();
    void add(std::string name, BenchFn fn);
    const std::vector<BenchCase>& cases() const { return cases_; }

private:
    std::vector<BenchCase> cases_;
};

// Registration convenience macro. Usage (注册便捷宏, 用法):
//   HFT_BENCH(my_scenario) {
//       // ... collect with cfg.samples (用 cfg.samples 采集) ...
//       BenchResult r;
//       r.n = cfg.samples;
//       r.latency = stats.compute();
//       return r;
//   }
#define HFT_BENCH(name)                                                                    \
    static ::hft::bench::BenchResult bench_fn_##name(const ::hft::bench::BenchConfig&);    \
    namespace {                                                                            \
    struct BenchRegistrar_##name {                                                         \
        BenchRegistrar_##name() {                                                          \
            ::hft::bench::BenchRegistry::instance().add(#name, &bench_fn_##name);          \
        }                                                                                  \
    };                                                                                     \
    static BenchRegistrar_##name g_bench_reg_##name;                                       \
    }                                                                                      \
    static ::hft::bench::BenchResult bench_fn_##name(const ::hft::bench::BenchConfig& cfg)

// Output emitters. Markdown for docs/BENCHMARK.md, CSV for CI trending.
// 输出 emitter。Markdown 直接贴 docs/BENCHMARK.md, CSV 给 CI 折线图。
std::string emit_markdown(const std::vector<BenchResult>& results,
                          const std::string& machine_info);
std::string emit_csv(const std::vector<BenchResult>& results);
std::string emit_json(const std::vector<BenchResult>& results,
                      const std::string& machine_info);

// Collect machine specs (CPU brand / OS / compiler / Release flag) — 采集机器规格 (CPU / OS / 编译器 / Release)
std::string collect_machine_info();

} // namespace hft::bench
