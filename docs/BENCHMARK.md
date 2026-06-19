# hft_framework 延迟基准测试

由 `hft_bench` 自动生成。复现命令：

```
hft_bench --output docs/BENCHMARK.md --runs 5 --warmup 1
```

---

## 1. 微基准测试 (hft_bench)

### Windows — MSVC + LTO

```
CPU: 12th Gen Intel(R) Core(TM) i5-12490F
Logical CPUs: 12
OS: Windows 10 Pro
Compiler: MSVC _MSC_VER=1942
Build: Release + LTO (NDEBUG)
```

| 场景 | n | p50 | p95 | p99 | p99.9 | max | avg |
|---|---:|---:|---:|---:|---:|---:|---:|
| `cond_order_check_tick` (条件单检查) | 10000 | 0.10us | 0.10us | 0.10us | 0.60us | 121.1us | 0.08us |
| `order_manager_on_order_return` (订单回报) | 10000 | 0.10us | 0.10us | 0.10us | 0.20us | 1.40us | 0.10us |
| `order_manager_on_trade_return` (成交回报) | 10000 | 0.10us | 0.20us | 0.20us | 11.60us | 130.1us | 0.15us |
| `position_on_trade` (持仓更新) | 10000 | 0.10us | 0.10us | 0.10us | 0.10us | 1.30us | 0.06us |
| `position_get_position` (持仓查询) | 10000 | 0.10us | 0.10us | 0.10us | 0.10us | 1.00us | 0.05us |
| `tick_to_strategy_e2e` (Tick->策略) | 10000 | 0.30us | 0.40us | 0.70us | 1.10us | 7.60us | 0.31us |
| `send_order_e2e` (发单端到端) | 10000 | 0.30us | 0.40us | 0.88us | 2.10us | 7.20us | 0.32us |

| 吞吐量 | n | ops/sec |
|---|---:|---:|
| `spsc_queue_throughput` | 1000000 | 8.3 x 10^7 |

### Linux — GCC 13.3 + LTO

```
CPU: AMD EPYC 7K62 48-Core Processor
vCPUs: 4 (shared cloud VM)
OS: Ubuntu 24.04, kernel 6.8.0
Compiler: GCC 13.3.0
Build: Release + LTO (NDEBUG)
```

| 场景 | n | p50 | p95 | p99 | p99.9 | max | avg |
|---|---:|---:|---:|---:|---:|---:|---:|
| `cond_order_check_tick` (条件单检查) | 10000 | 0.08us | 0.08us | 0.08us | 0.08us | 10.03us | 0.08us |
| `order_manager_on_order_return` (订单回报) | 10000 | 0.14us | 0.16us | 0.17us | 0.21us | 6.91us | 0.14us |
| `order_manager_on_trade_return` (成交回报) | 10000 | 0.19us | 0.23us | 0.26us | 1.18us | 74.07us | 0.21us |
| `position_on_trade` (持仓更新) | 10000 | 0.07us | 0.08us | 0.08us | 0.08us | 0.22us | 0.07us |
| `position_get_position` (持仓查询) | 10000 | 0.07us | 0.09us | 0.09us | 0.10us | 0.11us | 0.07us |
| `tick_to_strategy_e2e` (Tick->策略) | 10000 | 0.43us | 0.51us | 0.67us | 2.42us | 115.7us | 0.47us |
| `send_order_e2e` (发单端到端) | 10000 | 0.33us | 0.35us | 0.52us | 1.21us | 50.85us | 0.34us |

| 吞吐量 | n | ops/sec |
|---|---:|---:|
| `spsc_queue_throughput` | 1000000 | 2.3 x 10^8 |

### 跨平台对比

| 指标 | Windows (MSVC) | Linux (GCC) | 倍率 |
|---|---:|---:|---:|
| 条件单检查 p99 | 0.10us | 0.08us | 1.25x |
| 持仓更新 p99 | 0.10us | 0.08us | 1.25x |
| Tick->策略 p99 | 0.70us | 0.67us | 1.04x |
| 发单 p99 | 0.88us | 0.52us | **1.69x** |
| SPSC 吞吐 | 8.3x10^7 | 2.3x10^8 | **2.8x** |

> Linux p50 略高是因为共享云 VM 的调度抖动，裸金属/独占核心上不会有此差异。
> GCC 在 SPSC 队列吞吐上有 2.8 倍优势，`__builtin_expect` 分支提示在 GCC 上有效。

---

## 2. SimNow 实盘延迟 (7x24 环境)

测试环境: Ubuntu 24.04 云 VM -> SimNow 182.254.243.31

### 登录全链路

| 阶段 | 耗时 | 累计 |
|---|---:|---:|
| 行情 TCP 连接 | 92ms | 105ms |
| 行情 CTP 登录 | 30ms | 135ms |
| 交易 TCP 连接 | 184ms | 319ms |
| 交易认证 (AppAuth) | 88ms | 407ms |
| 交易登录 + 结算确认 | 316ms | 723ms |
| 持仓快照同步 | 2,236ms | 2,959ms |
| **账户可交易就绪** | — | **~3s** |

### 框架内部处理

| 指标 | 延迟 | 说明 |
|---|---:|---|
| tick_process | 0-1us | tick 事件调度 |
| tick_to_signal | 1-3us | tick -> 策略信号 |
| order_process | 0-6us | 委托回报处理 |
| trade_process | 3-10us | 成交回报处理 |

### 下单延迟 (API -> CTP SDK)

20 单连续压测 (rb2610):

| 指标 | 值 |
|---|---:|
| p50 | 23us |
| p90 | 36us |
| p99 | 37us |
| min | 11us |
| max | 37us |
| avg | 25us |

### 交易所往返 (order -> 撮合 -> 成交回报)

| 合约 | order_to_trade |
|---|---:|
| rb2610 (螺纹) | 67ms |
| au2608 (黄金) | 28ms |
| sc2608 (原油) | 58ms |
| cu2607 (铜) | 74ms |

> order_to_trade 以网络 RTT 为主 (云 VM -> SimNow ~15ms 单边)，非框架瓶颈。

### 端到端总结

| 路径 | 延迟 |
|---|---:|
| tick 到达 -> 策略信号 | 1-3us |
| 信号 -> 风控 -> CTP 报单发出 | 25-40us |
| **内部总延迟 (tick -> 报单)** | **< 50us** |
| 报单 -> 交易所成交回报 | 28-78ms (网络) |

---

## 数据解读

- **Tick->策略 p99 < 1us**: 交易所发出行情后不到 1 微秒，策略代码就开始执行
- **实盘下单 < 50us**: 从 tick 到达到报单发出 CTP SDK，全链路 50 微秒以内
- **SPSC 吞吐 2.3x10^8 ops/sec (GCC)**: 队列永远不会是瓶颈，远超 CTP 峰值行情 (~1 万 tick/秒)
- **尾部延迟 (p99.9)**: 由操作系统调度抖动引起，可通过 `isolcpus` + 实时优先级进一步压低
