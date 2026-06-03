# hft_framework

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Standard: C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform: Windows | Linux](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)](#build)
[![Latency: p99 < 1μs](https://img.shields.io/badge/hot--path%20p99-%3C1%20%CE%BCs-brightgreen)](docs/BENCHMARK.md)

> **Production-ready CTP/QDP live-trading host with embedded Python
> strategies, p99 hot-path latency < 1μs.**

[English](#english) · [中文](#中文)

---

## English

### What is this?

`hft_framework` is a **live-only** trading host for the Chinese futures
market. It runs CTP and QDP (上期所 FTD 低延迟通道) gateways,
embeds Python for strategy code, and keeps the hot path single-threaded
and lock-free.

It deliberately does **not** include a backtest engine, a GUI, or a web
control panel — keeping the binary one process, one thread on the hot
path, and one file to deploy.

### Why this over alternatives?

| | `hft_framework` | [vnpy](https://github.com/vnpy/vnpy) | [WonderTrader](https://github.com/wondertrader/wondertrader) |
|---|---|---|---|
| **Language** | C++17 + embedded Python | Python (CTP via ctypes) | C++ / C# |
| **Hot-path p99** | **< 1 μs** (see [benchmark](docs/BENCHMARK.md)) | ~500 μs (estimated) | No public benchmark |
| **Gateway support** | CTP ✓ QDP ✓ | CTP ✓ | CTP ✓ XTP ✓ others |
| **Embedded Python** | Yes (pybind11, zero-copy) | Native Python | No (C++ only) |
| **Hot-reload strategies** | Yes (no engine restart) | Yes | No |
| **Public benchmark** | Yes (reproducible `hft_bench`) | No | No |
| **RMS modes** | 5 modes (Normal → Halted) | Basic | Basic |
| **Backtest** | No (use a dedicated tool) | Yes | Yes |
| **GUI** | No (stdout + log) | Yes (VNStudio) | Yes (WtStudio) |

If you need a backtest engine or GUI, use vnpy or WonderTrader. If you
need **production latency** with a clean single-binary deployment and
Python strategy authoring, this is the one.

### Performance highlights

Desktop i5-12490F, MSVC Release, single-threaded consumer:

| Metric | Value |
|---|---|
| Conditional order check (止损/止盈/追踪触发) | **p99 = 0.10 μs** |
| Order return processing | **p99 = 0.10 μs** |
| Position update on trade | **p99 = 0.10 μs** |
| SPSC queue throughput | **5.07 × 10⁸ ops/sec** |

Full report with machine spec and reproduction steps:
**[docs/BENCHMARK.md](docs/BENCHMARK.md)**.

### Feature matrix

| | Status |
|---|---|
| CTP market-data gateway (`thostmduserapi_se`) | ✅ |
| CTP trade gateway (`thosttraderapi_se`) | ✅ |
| QDP market-data gateway (上期所 FTD, optional) | ✅ |
| QDP trade gateway (上期所 FTD, optional) | ✅ |
| Multi-account, single-MD-account routing | ✅ |
| Native C++ strategies | ✅ |
| Embedded Python strategies (pybind11) | ✅ |
| Hot-reload strategies (no engine restart) | ✅ |
| Pre-trade risk (size/position/order-rate/cancel-rate/daily-loss) | ✅ |
| RMS modes (Normal / NoOpen / ReduceOnly / Liquidating / Halted) | ✅ |
| Encrypted credential storage (DPAPI / AES-GCM) | ✅ |
| Tick recording (binary or JSONL) | ✅ |
| Algorithmic orders (TWAP / Iceberg) | ✅ |
| Paper-trading engine | ✅ |
| K-line aggregation + CSV import | ✅ |
| Conditional orders (stop-loss / take-profit / trailing-stop / entry-trigger) | ✅ |
| OCO groups (one-cancels-other) | ✅ |
| Async logger, lock-free SPSC queue, CPU pinning, `VirtualLock` | ✅ |
| Trading-session gating | ✅ |
| Webhook alerts (Slack/DingTalk compatible) | ✅ |
| Latency benchmark (`hft_bench`) | ✅ |
| Backtest | ❌ (out of scope — use a dedicated tool) |
| GUI | ❌ (use the binary's stdout/log) |
| HTTP API | ❌ |

### Quickstart

```powershell
# 1. Clone
git clone https://github.com/<you>/hft_framework.git && cd hft_framework

# 2. Download CTP SDK from http://www.simnow.com.cn/ (requires free registration)
#    Extract and note the path, e.g. C:/ctp_api/20250617_traderapi64_se_windows

# 3. Build (MSVC 2022, x64)
cmake -S . -B build_vs2022 -G "Visual Studio 17 2022" -A x64 -DCTP_SDK_DIR=C:/ctp_api/20250617_traderapi64_se_windows
cmake --build build_vs2022 --config Release --target hft_framework

# 4. Configure and run
copy config.example.ini dist\config.ini
# Edit dist/config.ini: fill in UserID + Password, leave the rest default
dist\hft_framework.exe --interactive --config dist\config.ini
```

Full guide with prerequisites, troubleshooting, and first-strategy steps:
**[docs/QUICKSTART.md](docs/QUICKSTART.md)**.

### Documentation

| Doc | What's inside |
|---|---|
| [docs/QUICKSTART.md](docs/QUICKSTART.md) | 5-minute clone-to-SimNow guide |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Component map, threading model, data flow diagrams |
| [docs/STRATEGY.md](docs/STRATEGY.md) | Python strategy authoring: callbacks, `hft_engine` API, conditional orders, risk gates |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Every `config.ini` section and key explained |
| [docs/SECURITY.md](docs/SECURITY.md) | Credential encryption (DPAPI/AES-GCM), what to protect |
| [docs/BENCHMARK.md](docs/BENCHMARK.md) | Reproducible latency numbers |
| [CONTRIBUTING.md](CONTRIBUTING.md) | How to contribute |

### Architecture

See **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** for full diagrams.

TL;DR — One consumer thread owns all engine state. Gateway threads only
decode and enqueue via SPSC. Strategies run on the consumer thread, so
they never need to think about locking.

### CTP SDK

The CTP C++ SDK is **not bundled** in this repository. You need to
download it separately from <http://www.simnow.com.cn/> (free registration
required) and pass `-DCTP_SDK_DIR=<path>` to CMake.

The SDK is the property of Shanghai Futures Information Technology Co.
Ltd (SFIT) and is governed by SFIT's own license terms, not by this
project's MIT license — see `LICENSE` for details.

### Contributing

See **[CONTRIBUTING.md](CONTRIBUTING.md)**.

### License

[MIT](LICENSE).

---

## 中文

### 这是什么？

`hft_framework` 是一套面向**中国期货市场**的**纯实盘**交易框架。
同时支持 CTP 和 QDP(上期所 FTD 低延迟通道)网关,内嵌 Python 写策略,
热路径单线程 + 无锁。

刻意**不做**回测引擎、GUI、Web 控制台 —— 进程只剩一个,热路径单线程,
部署只丢一个可执行文件。

### 为什么选它而不是别的？

| | `hft_framework` | [vnpy](https://github.com/vnpy/vnpy) | [WonderTrader](https://github.com/wondertrader/wondertrader) |
|---|---|---|---|
| **语言** | C++17 + 嵌入 Python | Python (CTP 走 ctypes) | C++ / C# |
| **热路径 p99** | **< 1 μs**(见 [benchmark](docs/BENCHMARK.md)) | ~500 μs(估) | 无公开数据 |
| **柜台支持** | CTP ✓ QDP ✓ | CTP ✓ | CTP ✓ XTP ✓ 其它 |
| **嵌入 Python** | 有(pybind11, 零拷贝) | 原生 Python | 无(C++ only) |
| **策略热加载** | 有(不停机) | 有 | 无 |
| **公开 benchmark** | 有(可复现 `hft_bench`) | 无 | 无 |
| **RMS 风控模式** | 5 档(Normal → Halted) | 基础 | 基础 |
| **回测** | 无(用专门工具) | 有 | 有 |
| **GUI** | 无(stdout + 日志) | 有(VNStudio) | 有(WtStudio) |

如果你需要回测或 GUI,用 vnpy 或 WonderTrader。如果你需要
**生产级延迟** + 干净的单二进制部署 + Python 策略开发体验,选这个。

### 性能亮点

桌面 i5-12490F, MSVC Release, 单线程消费者:

| 指标 | 数值 |
|---|---|
| 条件单检查(止损/止盈/追踪触发) | **p99 = 0.10 μs** |
| 订单回报处理 | **p99 = 0.10 μs** |
| 成交后持仓更新 | **p99 = 0.10 μs** |
| SPSC 队列吞吐 | **5.07 × 10⁸ ops/sec** |

完整报告(含机器规格和复现步骤):
**[docs/BENCHMARK.md](docs/BENCHMARK.md)**。

### 功能矩阵

| | 状态 |
|---|---|
| CTP 行情网关 (`thostmduserapi_se`) | ✅ |
| CTP 交易网关 (`thosttraderapi_se`) | ✅ |
| QDP 行情网关(上期所 FTD,可选) | ✅ |
| QDP 交易网关(上期所 FTD,可选) | ✅ |
| 多账户,单行情账户路由 | ✅ |
| C++ 原生策略 | ✅ |
| 内嵌 Python 策略 (pybind11) | ✅ |
| 策略热加载,无需重启引擎 | ✅ |
| 盘前风控(单笔/净持仓/分钟报单率/撤单率/日内亏损) | ✅ |
| RMS 风控模式(正常/禁开仓/仅减仓/平仓中/熔断) | ✅ |
| 凭据加密存储(Windows DPAPI / Linux AES-GCM) | ✅ |
| Tick 录制(二进制 / JSONL) | ✅ |
| 算法单(TWAP / 冰山) | ✅ |
| 模拟撮合引擎 | ✅ |
| K 线聚合 + CSV 导入 | ✅ |
| 条件单(止损/止盈/追踪止损/触发开仓) | ✅ |
| OCO 互斥分组(一个触发自动撤其他) | ✅ |
| 异步日志、无锁 SPSC 队列、CPU 绑核、`VirtualLock` | ✅ |
| 交易时段控制 | ✅ |
| Webhook 报警(Slack / 钉钉兼容) | ✅ |
| 延迟 benchmark (`hft_bench`) | ✅ |
| 回测 | ❌ (不在范围内 —— 用专门工具) |
| GUI | ❌ |
| HTTP API | ❌ |

### 快速上手

```powershell
# 1. Clone
git clone https://github.com/<you>/hft_framework.git && cd hft_framework

# 2. 从 http://www.simnow.com.cn/ 下载 CTP SDK(免费注册)
#    解压并记下路径, 例如 C:/ctp_api/20250617_traderapi64_se_windows

# 3. 构建 (MSVC 2022, x64)
cmake -S . -B build_vs2022 -G "Visual Studio 17 2022" -A x64 -DCTP_SDK_DIR=C:/ctp_api/20250617_traderapi64_se_windows
cmake --build build_vs2022 --config Release --target hft_framework

# 4. 配置并运行
copy config.example.ini dist\config.ini
# 编辑 dist/config.ini: 填 UserID + Password,其它默认
dist\hft_framework.exe --interactive --config dist\config.ini
```

完整指南(含前置依赖、故障排查、首个策略上手):
**[docs/QUICKSTART.md](docs/QUICKSTART.md)**。

### 文档索引

| 文档 | 内容 |
|---|---|
| [docs/QUICKSTART.md](docs/QUICKSTART.md) | 5 分钟从 clone 到 SimNow 连通 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 组件映射、线程模型、数据流图 |
| [docs/STRATEGY.md](docs/STRATEGY.md) | Python 策略开发:回调、`hft_engine` API、条件单、风控 |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | `config.ini` 每个 section 和 key 详解 |
| [docs/SECURITY.md](docs/SECURITY.md) | 凭据加密原理(DPAPI/AES-GCM)、文件保护 |
| [docs/BENCHMARK.md](docs/BENCHMARK.md) | 可复现的延迟数据 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 贡献指南 |

### 架构

详见 **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** 完整图表。

一句话:单消费者线程持有引擎全部状态;网关线程只解码 + SPSC 入队;
策略跑在消费者线程上,**不需要任何锁**。

### CTP SDK 说明

仓库**不附带** CTP C++ SDK。请从 <http://www.simnow.com.cn/> 免费注册后
下载,解压后通过 `-DCTP_SDK_DIR=<path>` 传给 CMake。

该 SDK 版权归上海期货信息技术有限公司(SFIT),以 SFIT 官方授权为准,
**不适用本仓库的 MIT 协议**。SimNow 测试账号(BrokerID=9999)可在同一
网站免费注册。

### 免责声明

本项目仅供学习与个人研究使用。**实盘亏损概不负责**。在用于真实资金
前,请在 SimNow 等模拟环境完整跑通,并自行评估法律合规风险。

### 协议

[MIT](LICENSE)。
