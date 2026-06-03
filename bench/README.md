# hft_bench

独立的延迟 benchmark 二进制,采集 hft_framework 热路径的 p50/p95/p99/p999/max/avg 数据,以及 SPSC 队列的饱和吞吐(ops/sec)。

测量基础设施复用 `src/common/qpc_timer.h` 的 `QPCTimer` 与 `QPCLatencyStats`,跨平台亚微秒精度(Windows = QueryPerformanceCounter,Linux = `clock_gettime(CLOCK_MONOTONIC)`)。

## Build

```powershell
cmake --build build_vs2022 --config Release --target hft_bench
```

产物:`dist/hft_bench.exe`(Windows)/ `dist/hft_bench`(Linux)。

## Run

```powershell
# 默认:跑全部场景,Markdown 写到 stdout
hft_bench

# 写到 docs/BENCHMARK.md,5 轮取最稳定一轮
hft_bench --output docs/BENCHMARK.md --runs 5 --warmup 1

# 列出所有已注册场景
hft_bench --list

# 只跑名字含 "order_manager" 的场景
hft_bench --filter order_manager

# 输出 CSV(给 CI 折线图)/ JSON(给监控系统)
hft_bench --format csv --output bench.csv
hft_bench --format json --output bench.json
```

## CLI 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--output <path>` | stdout | 报告写到指定文件 |
| `--format <md\|csv\|json>` | md | 输出格式 |
| `--runs <N>` | 3 | 每个场景重复 N 轮,取 p99 最稳一轮作为最终结果 |
| `--warmup <N>` | 1 | 丢弃的预热轮数(JIT / 缓存预热) |
| `--samples <N>` | 10000 | 延迟场景每轮采样数 |
| `--filter <regex>` | (none) | 仅运行匹配该正则的场景 |
| `--list` | - | 列出场景后退出 |

## 已注册场景

延迟(latency,单位 μs):

| Scenario | 说明 |
|---|---|
| `cond_order_check_tick` | `ConditionalOrderManager::check_tick`,1000 条挂单的扫描 |
| `order_manager_on_order_return` | `OrderManager::on_order_return`,FixedKey<16> 哈希查找 + 终态保护 |
| `order_manager_on_trade_return` | `OrderManager::on_trade_return`,含 trade_id 去重 |
| `position_on_trade` | `PositionManager::on_trade`,持仓累计 + 均价 |
| `position_get_position` | `PositionManager::get_position`,查询热路径 |

吞吐(throughput,ops/sec):

| Scenario | 说明 |
|---|---|
| `spsc_queue_throughput` | `SPSCQueue<size_t,65536>` 单生产者-单消费者饱和吞吐 |

## 注册新场景

在 `bench/bench_scenarios.cpp` 用 `HFT_BENCH(name)` 宏即可,宏会在静态初始化期把场景塞进全局注册表:

```cpp
HFT_BENCH(my_new_scenario) {
    QPCLatencyStats stats(cfg.samples);
    for (size_t i = 0; i < cfg.samples; ++i) {
        QPCTimer t;
        t.start();
        // ... 待测路径 ...
        t.stop();
        stats.record_elapsed(t);
    }
    BenchResult r;
    r.n = cfg.samples;
    r.latency = stats.compute();
    return r;
}
```

吞吐场景:设 `r.is_throughput = true; r.throughput_ops_sec = ...`。

## 结果落地

最新一次正式跑分见 [`docs/BENCHMARK.md`](../docs/BENCHMARK.md)。

CI 建议每次 PR 跑 `--filter ` 选定核心场景生成 CSV,跟基线比对,p99 退化超过阈值则告警。
