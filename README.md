# hft_framework

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Standard: C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform: Windows | Linux](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)](#build)

> C++17 live-trading framework for Chinese futures (CTP / QDP gateways),
> with embedded Python strategies and reproducible latency benchmarks.

[English](#english) · [中文](#中文)

---

## English

### What is this?

`hft_framework` is a **low-latency live-trading framework** for Chinese
futures. It connects to the CTP and QDP (SHFE FTD) gateways, supports
embedded Python strategies via pybind11, and keeps the hot path on a
single lock-free consumer thread.

**This is not** a backtest engine, a GUI application, or a turnkey trading
system. It focuses on the live execution layer — market data ingestion,
order routing, pre-trade risk, and strategy callbacks — and tries to do
that part well.

If you need a full-featured backtest + GUI platform, mature projects like
[vnpy](https://github.com/vnpy/vnpy) or
[WonderTrader](https://github.com/wondertrader/wondertrader)
are great choices. This project complements them by providing a
lightweight, benchmark-driven execution layer for latency-sensitive
experiments.

### Architecture

```
                        ┌─────────────────────────────────────────────┐
                        │              Consumer Thread                │
                        │  ┌─────────┐ ┌──────┐ ┌────────┐ ┌──────┐ │
  ┌──────────┐  SPSC    │  │ Risk    │ │Order │ │Position│ │Cond  │ │
  │CTP/QDP   │─────────►│  │ Manager │ │Mgr   │ │Manager │ │Orders│ │
  │MD Gateway│  Queue    │  └────┬────┘ └──┬───┘ └────────┘ └──────┘ │
  └──────────┘ (lock-   │       │         │                          │
               free)    │  ┌────▼─────────▼───┐    ┌──────────────┐  │
  ┌──────────┐  SPSC    │  │  Trading Engine   │───►│  Strategies  │  │
  │CTP/QDP   │─────────►│  │  (event loop)     │    │ C++ / Python │  │
  │TD Gateway│  Queue    │  └──────────────────┘    └──────────────┘  │
  └──────────┘          └──────────┬──────────────────────────────────┘
                                   │
                    ┌──────────────┼──────────────┐
                    ▼              ▼              ▼
              ┌──────────┐ ┌────────────┐ ┌────────────┐
              │Async Log │ │Tick Record │ │ WebUI/API  │
              │(SPSC)    │ │(WAL)       │ │ (HTTP)     │
              └──────────┘ └────────────┘ └────────────┘
```

> One consumer thread owns all engine state. Gateway threads only decode
> and enqueue via lock-free SPSC queues. Strategies never need locks.

### Key features

| Category | Details |
|---|---|
| **Gateways** | CTP (market data + trade), QDP (SHFE FTD, optional), FIX (stub), UDP multicast relay, shared-memory IPC, dual-gateway hot failover |
| **Strategies** | Native C++, embedded Python (pybind11, zero-copy), hot-reload without engine restart |
| **Order types** | Limit, conditional (stop-loss / take-profit / trailing-stop / entry-trigger), OCO groups, algorithmic (TWAP / Iceberg) |
| **Risk** | Pre-trade checks (size / position / order-rate / cancel-rate / daily-loss), 5 RMS modes (Normal → Halted) |
| **Infrastructure** | Lock-free SPSC queue, async logger, CPU pinning, VirtualLock, WAL (io_uring on Linux), standalone watchdog |
| **Observability** | HTTP REST API, Prometheus `/metrics`, embedded WebUI, webhook alerts (Slack / DingTalk) |
| **Security** | Encrypted credential storage (DPAPI on Windows, AES-GCM on Linux) |
| **Data** | Tick recording (binary / JSONL), K-line aggregation, CSV import, paper-trading engine |

### Benchmark

All numbers are from the built-in `hft_bench` tool and can be reproduced
on your own hardware — see **[docs/BENCHMARK.md](docs/BENCHMARK.md)**
for full methodology and machine specs.

| Metric | Windows (MSVC, i5-12490F) | Linux (GCC 13, EPYC 7K62) |
|---|---|---|
| Tick → strategy callback p99 | 0.70 μs | 0.67 μs |
| Send-order e2e p99 | 0.88 μs | 0.52 μs |
| SPSC queue throughput | 8.3 × 10⁷ ops/s | 2.3 × 10⁸ ops/s |

Real-world test on SimNow (7×24, cloud VM → SimNow server):

| Path | Latency |
|---|---|
| Tick arrival → strategy signal | 1–3 μs |
| Signal → risk check → CTP order out | 25–40 μs |
| **Total internal (tick → order sent)** | **< 50 μs** |
| Order → exchange fill (network RTT) | 28–78 ms |

> The exchange round-trip is dominated by network latency, not framework
> overhead. On a co-located setup, internal latency would be the same
> while network RTT would drop significantly.

### Quickstart

```bash
# 1. Clone
git clone https://github.com/Essential1220/hft_framework.git && cd hft_framework

# 2. Download CTP SDK from http://www.simnow.com.cn/ (free registration)
#    Extract and note the path, e.g. C:/ctp_api/20250617_traderapi64_se_windows

# 3a. Build — Windows (MSVC 2022, x64)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
      -DCTP_SDK_DIR=C:/ctp_api/20250617_traderapi64_se_windows
cmake --build build --config Release

# 3b. Build — Linux (GCC 13+)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCTP_SDK_DIR=/path/to/ctp_sdk_linux -DENABLE_LTO=ON
cmake --build build -j$(nproc)

# 4. Configure and run
cp config.example.ini dist/config.ini
# Edit dist/config.ini: fill in UserID + Password
cd dist && ./hft_framework --config config.ini
```

Full guide: **[docs/QUICKSTART.md](docs/QUICKSTART.md)**.

### Documentation

| Doc | What's inside |
|---|---|
| [docs/QUICKSTART.md](docs/QUICKSTART.md) | Clone-to-SimNow in 5 minutes |
| [docs/OVERVIEW.md](docs/OVERVIEW.md) | Component map, threading model, data flow |
| [docs/DESIGN.md](docs/DESIGN.md) | Engineering decisions and trade-offs |
| [docs/API_REFERENCE.md](docs/API_REFERENCE.md) | C++ / Python / REST API reference |
| [docs/STRATEGY.md](docs/STRATEGY.md) | Python strategy authoring, `hft_engine` API, conditional orders |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Every `config.ini` key explained |
| [docs/SECURITY.md](docs/SECURITY.md) | Credential encryption (DPAPI / AES-GCM) |
| [docs/BENCHMARK.md](docs/BENCHMARK.md) | Reproducible latency numbers |

### CTP SDK

The CTP C++ SDK is **not bundled**. Download it from
<http://www.simnow.com.cn/> (free registration) and pass
`-DCTP_SDK_DIR=<path>` to CMake. The SDK is property of SFIT and
governed by its own license — see `LICENSE`.

### Contributing

See **[CONTRIBUTING.md](CONTRIBUTING.md)**.

### License

[MIT](LICENSE).

---

## 中文

### 这是什么？

`hft_framework` 是一套面向**中国期货市场**的 C++17 **低延迟实盘交易框架**。
支持 CTP 和 QDP（上期所 FTD）网关，内嵌 Python 写策略（pybind11），
热路径单线程无锁。

项目**不包含**回测引擎、GUI 或 Web 控制台。专注于实盘执行层：
行情接入、报单路由、盘前风控、策略回调，把这一层做好。

如果需要完整的回测 + GUI 平台，推荐
[vnpy](https://github.com/vnpy/vnpy) 或
[WonderTrader](https://github.com/wondertrader/wondertrader)
等成熟项目。本项目可以作为它们的补充，提供一个轻量、可测量的
低延迟执行层，用于延迟敏感的实验和学习。

### 架构概览

```
                        ┌─────────────────────────────────────────────┐
                        │              消费者线程 (唯一状态持有者)       │
                        │  ┌─────────┐ ┌──────┐ ┌────────┐ ┌──────┐ │
  ┌──────────┐  SPSC    │  │ 风控    │ │订单  │ │ 持仓   │ │条件单│ │
  │CTP/QDP   │─────────►│  │ 管理器  │ │管理器│ │ 管理器 │ │管理器│ │
  │行情网关  │  队列     │  └────┬────┘ └──┬───┘ └────────┘ └──────┘ │
  └──────────┘ (无锁)   │       │         │                          │
                        │  ┌────▼─────────▼───┐    ┌──────────────┐  │
  ┌──────────┐  SPSC    │  │  交易引擎         │───►│  策略        │  │
  │CTP/QDP   │─────────►│  │  (事件循环)       │    │ C++ / Python │  │
  │交易网关  │  队列     │  └──────────────────┘    └──────────────┘  │
  └──────────┘          └──────────┬──────────────────────────────────┘
                                   │
                    ┌──────────────┼──────────────┐
                    ▼              ▼              ▼
              ┌──────────┐ ┌────────────┐ ┌────────────┐
              │异步日志  │ │Tick 录制   │ │ WebUI/API  │
              │(SPSC)    │ │(WAL)       │ │ (HTTP)     │
              └──────────┘ └────────────┘ └────────────┘
```

> 单消费者线程持有全部引擎状态，网关线程只解码 + SPSC 入队，策略不需要任何锁。

### 主要功能

| 类别 | 说明 |
|---|---|
| **网关** | CTP（行情 + 交易）、QDP（上期所 FTD，可选）、FIX（stub）、UDP 组播转发、共享内存 IPC、双活热切换 |
| **策略** | C++ 原生、内嵌 Python（pybind11，零拷贝）、热加载无需重启 |
| **委托类型** | 限价、条件单（止损/止盈/追踪止损/触发开仓）、OCO 互斥组、算法单（TWAP/冰山） |
| **风控** | 盘前检查（单笔/净持仓/报单率/撤单率/日内亏损）、5 档 RMS 模式（正常→熔断） |
| **基础设施** | 无锁 SPSC 队列、异步日志、CPU 绑核、VirtualLock、WAL（Linux io_uring）、独立看门狗 |
| **可观测性** | HTTP REST API、Prometheus `/metrics`、内嵌 WebUI、Webhook 报警（Slack/钉钉） |
| **安全** | 凭据加密（Windows DPAPI / Linux AES-GCM） |
| **数据** | Tick 录制（二进制/JSONL）、K 线聚合、CSV 导入、模拟撮合引擎 |

### 性能数据

所有数据由内置 `hft_bench` 工具生成，可在你自己的机器上复现 ——
详见 **[docs/BENCHMARK.md](docs/BENCHMARK.md)**。

| 指标 | Windows (MSVC, i5-12490F) | Linux (GCC 13, EPYC 7K62) |
|---|---|---|
| Tick → 策略 p99 | 0.70 μs | 0.67 μs |
| 发单端到端 p99 | 0.88 μs | 0.52 μs |
| SPSC 队列吞吐 | 8.3 × 10⁷ ops/s | 2.3 × 10⁸ ops/s |

SimNow 实盘测试（7×24，云 VM → SimNow）:

| 路径 | 延迟 |
|---|---|
| Tick 到达 → 策略信号 | 1–3 μs |
| 信号 → 风控 → CTP 报单发出 | 25–40 μs |
| **框架内部总延迟（tick → 报单）** | **< 50 μs** |
| 报单 → 交易所成交回报（网络） | 28–78 ms |

> 交易所往返主要是网络延迟，不是框架瓶颈。托管机房环境下，
> 框架内部延迟不变，网络 RTT 会大幅降低。

### 快速上手

```bash
# 1. Clone
git clone https://github.com/Essential1220/hft_framework.git && cd hft_framework

# 2. 从 http://www.simnow.com.cn/ 下载 CTP SDK（免费注册）

# 3a. 构建 — Windows (MSVC 2022, x64)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
      -DCTP_SDK_DIR=C:/ctp_api/20250617_traderapi64_se_windows
cmake --build build --config Release

# 3b. 构建 — Linux (GCC 13+)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCTP_SDK_DIR=/path/to/ctp_sdk_linux -DENABLE_LTO=ON
cmake --build build -j$(nproc)

# 4. 配置并运行
cp config.example.ini dist/config.ini
# 编辑 dist/config.ini: 填 UserID + Password
cd dist && ./hft_framework --config config.ini
```

完整指南: **[docs/QUICKSTART.md](docs/QUICKSTART.md)**。

### 文档索引

| 文档 | 内容 |
|---|---|
| [docs/QUICKSTART.md](docs/QUICKSTART.md) | 5 分钟从 clone 到 SimNow 连通 |
| [docs/OVERVIEW.md](docs/OVERVIEW.md) | 组件映射、线程模型、数据流图 |
| [docs/DESIGN.md](docs/DESIGN.md) | 工程设计决策与取舍 |
| [docs/API_REFERENCE.md](docs/API_REFERENCE.md) | C++ / Python / REST API 参考 |
| [docs/STRATEGY.md](docs/STRATEGY.md) | Python 策略开发、`hft_engine` API、条件单 |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | `config.ini` 每个 key 详解 |
| [docs/SECURITY.md](docs/SECURITY.md) | 凭据加密（DPAPI / AES-GCM） |
| [docs/BENCHMARK.md](docs/BENCHMARK.md) | 可复现的延迟数据 |

### 设计文档

详细的工程设计决策（每个模块做了什么、为什么、放弃了什么）见
**[docs/DESIGN.md](docs/DESIGN.md)**。

### CTP SDK

仓库**不附带** CTP SDK。从 <http://www.simnow.com.cn/> 免费注册下载，
通过 `-DCTP_SDK_DIR=<path>` 传给 CMake。SDK 版权归 SFIT，
不适用本仓库的 MIT 协议。

### 免责声明

本项目仅供学习与研究。**实盘交易风险自担**。
使用真实资金前，请在 SimNow 等模拟环境充分测试，并自行评估合规风险。

### 协议

[MIT](LICENSE)。
