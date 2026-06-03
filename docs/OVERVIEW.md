# 项目概览

## 定位

`hft_framework` 是面向中国期货市场的**生产级 C++17 实盘交易主机**，CTP/QDP 双柜台，
内嵌 Python 策略引擎，热路径单线程无锁，p99 延迟 < 1μs。

**适用场景：** 量化私募/自营机构的实盘交易执行层，不适合回测（请用 vnpy / WonderTrader）。

---

## 整体架构

```
┌──────────────────────────────────────────────────────────────────┐
│                         EXTERNAL                                  │
│  交易所行情 (CTP MD)    交易所交易 (CTP TD)    Python 策略脚本      │
└────────┬─────────────────────┬──────────────────────┬────────────┘
         │                     │                      │
         ▼                     ▼                      ▼
┌──────────────────────────────────────────────────────────────────┐
│                    GATEWAY LAYER (2-4 线程)                        │
│                                                                    │
│  CtpMdGateway                CtpTradeGateway                      │
│  · 解码行情回调              · 解码交易回调                         │
│  · 写入 md_queue_            · 写入 trade_queue_                   │
│  · 线程：CTP SDK 内部线程     · 线程：CTP SDK 内部线程               │
│                                                                    │
│  职责边界：只做解码 + SPSC 入队，绝不触碰引擎状态                     │
└────────┬─────────────────────┬────────────────────────────────────┘
         │                     │
         ▼                     ▼
┌──────────────────────────────────────────────────────────────────┐
│              INTER-THREAD TRANSPORT (无锁 SPSC)                    │
│                                                                    │
│  md_queue_       trade_queue_        cmd_queue_                    │
│  容量: 65,536    容量: 同 md         容量: 4,096                    │
│  · 行情数据      · 订单/成交/持仓     · 控制命令                     │
│                  · 账户信息回调       · RMS 模式切换                 │
└────────────────────────────┬─────────────────────────────────────┘
                             │
                             ▼
┌──────────────────────────────────────────────────────────────────┐
│              CONSUMER THREAD (唯一状态持有者)                       │
│  CPU 绑核 · HIGH_PRIORITY · 热路径零锁                              │
│                                                                    │
│  ┌─────────────────── ENGINE CORE ──────────────────────┐         │
│  │                                                       │         │
│  │  TradingEngine::consumer_loop()                        │         │
│  │    │                                                  │         │
│  │    ├─ process_command()    优先处理控制命令              │         │
│  │    ├─ process_trade_event() 订单/成交状态更新            │         │
│  │    ├─ drain md_queue_ (批量 512)                       │         │
│  │    │   └─ for each tick:                               │         │
│  │    │       ├─ ConditionalOrderMgr::check_tick()        │         │
│  │    │       ├─ Position 浮动盈亏更新                     │         │
│  │    │       ├─ K 线聚合 (可关闭)                         │         │
│  │    │       ├─ Tick 录制   (可关闭)                      │         │
│  │    │       └─ Strategy::on_tick()                      │         │
│  │    │                                                  │         │
│  │    ├─ 定时任务: 条件单超时清理 / 风控报警                 │         │
│  │    └─ idle: CPU pause → yield → sleep (渐进降级)       │         │
│  │                                                       │         │
│  └───────────────────────────────────────────────────────┘         │
│                                                                    │
│  ┌───────────── ENGINE MODULES (全在消费线程上) ────────────┐      │
│  │                                                          │      │
│  │  OrderManager          订单状态机 + 成交去重 + enrich      │      │
│  │  PositionManager       持仓管理 (含今仓/昨仓拆分)          │      │
│  │  RiskManager           7 维风控 + RMS 5 级递进            │      │
│  │  ConditionalOrderMgr   止损/止盈/追踪止损/OCO +           │      │
│  │                        TriggerBounds O(1) 预过滤          │      │
│  │  AlgoOrderMgr          TWAP / Iceberg 算法单              │      │
│  │  StrategyManager       C++ 策略 + Python 策略热加载       │      │
│  │  SessionManager        交易时段门控                        │      │
│  │                                                          │      │
│  └──────────────────────────────────────────────────────────┘      │
│                                                                    │
│  ┌─── ASYNC WORKERS (独立线程，只消费不写回) ──────────────┐       │
│  │                                                         │       │
│  │  AsyncLogger     → log_queue_ → 异步落盘 + fsync         │       │
│  │  StateSave       → 定期持久化 runtime_state.dat          │       │
│  │  TickWriter      → 二进制/JSONL tick 录制                 │       │
│  │                                                         │       │
│  └─────────────────────────────────────────────────────────┘       │
└──────────────────────────────────────────────────────────────────┘
```

**关键设计约束：**
- 消费线程是**唯一**修改引擎状态的线程
- 网关线程只做解码 + SPSC 入队（零状态）
- 异步工作线程只消费数据（不写回引擎）
- **热路径零锁** —— 引擎内部所有数据结构不需要锁，因为单线程独占访问

---

## Tick → 策略 → 下单 → 成交 完整数据流

```
CTP 交易所行情
  │
  ▼
CTP SDK 回调线程 ──► CtpMdGateway::OnRtnDepthMarketData()
  │                    · 解码 CTP 结构体 → TickData (POD)
  │                    · md_queue_.push(tick)  ← release store
  ▼
消费线程 consumer_loop():
  │
  ├─ 1. drain cmd_queue_ (优先处理紧急命令)
  ├─ 2. drain trade_queue_ (最多 512 条)
  │      ├─ OrderManager::on_order_return() → enrich → dispatch on_order()
  │      ├─ OrderManager::on_trade_return() → trade dedup → update position → dispatch on_trade()
  │      └─ RiskManager 更新风控计数器
  │
  ├─ 3. drain md_queue_ (批量 512，每 64 tick 穿插检查 cmd_queue_)
  │      for each tick:
  │        ├─ ConditionalOrderMgr::check_tick()
  │        │    ├─ may_trigger_locked(instrument, price) → O(1) 边界检查
  │        │    ├─ 若可能触发 → 遍历该合约条件单，判断触发条件
  │        │    └─ 触发 → 构建 OrderRequest → send_order()
  │        ├─ 更新浮动盈亏 (PositionManager)
  │        ├─ K 线聚合 (可选，可关闭)
  │        ├─ Tick 录制 (可选，可关闭)
  │        └─ StrategyManager::dispatch_tick()
  │             ├─ C++ 策略: strategy->on_tick(tick)
  │             └─ Python 策略 (若未禁用):
  │                   acquire GIL → batch dispatch → release GIL
  │
  ├─ 4. 策略内部调用 send_order(req):
  │      ├─ RiskManager::check_order() ← 7 维瀑布流检查
  │      │    拒绝 → on_order(status=Rejected, status_msg=原因)
  │      │    通过 → OrderManager 创建订单
  │      └─ CtpTradeGateway 发出 → CTP OnRtnOrder 回调
  │
  ├─ 5. 定时任务 (每秒):
  │      ├─ 条件单超时清理 (TTL 过期)
  │      ├─ 风控报警检查 (无 tick / 持仓不一致)
  │      └─ AlgoOrderMgr 心跳
  │
  └─ 6. 无事件时: CPU pause → yield → sleep 渐进降级休眠
```

---

## 模块职责矩阵

| 模块 | 源文件 | 核心职责 | 线程 |
|------|--------|---------|------|
| **SPSC Queue** | `common/spsc_queue.h` | 无锁环形缓冲区，线程间唯一通信通道 | 跨线程 |
| **FixedKey** | `common/types.h` | 栈分配标识符，替代 `std::string` 避免热路径堆分配 | 消费线程 |
| **Event System** | `common/event.h` | Tagged union 事件定义，memcpy 安全 | 跨线程 |
| **CtpMdGateway** | `gateway/ctp_md_gateway.*` | CTP 行情前置连接、解码 | CTP SDK 线程 |
| **CtpTradeGateway** | `gateway/ctp_trade_gateway.*` | CTP 交易前置连接、解码 | CTP SDK 线程 |
| **TradingEngine** | `engine/trading_engine.*` | 消费主循环、模块编排、生命周期 | 消费线程 |
| **OrderManager** | `order/order_manager.*` | 订单状态机、成交去重、enrich | 消费线程 |
| **ConditionalOrderMgr** | `order/conditional_order_manager.*` | 条件单触发、TriggerBounds、OCO | 消费线程 |
| **AlgoOrderMgr** | `order/algo_order_manager.*` | TWAP/冰山算法单 | 消费线程 |
| **RiskManager** | `risk/risk_manager.*` | 7 维风控、RMS 模式递进 | 消费线程 |
| **PositionManager** | `position/position_manager.*` | 多账户持仓、今仓/昨仓拆分 | 消费线程 |
| **StrategyManager** | `strategy/strategy_manager.*` | 策略生命周期、热加载、Python 桥接 | 消费线程 |
| **Crypto** | `common/crypto.*` | DPAPI/AES-GCM 凭据加密 | 启动时 |

---

## 性能数据

测试环境：i5-12490F, Windows, MSVC 2022 Release, 单消费线程。

| 场景 | p50 | p99 | p99.9 | 说明 |
|------|-----|-----|-------|------|
| Tick→策略 端到端 | 0.30 μs | **0.70 μs** | 34.30 μs | 含 SPSC pop + 条件单检查 + 策略派发 |
| 发单 端到端 | 0.30 μs | **1.10 μs** | 9.90 μs | 含 7 维风控 + 订单创建 |
| 条件单检查 | 0.10 μs | 0.10 μs | 0.60 μs | 含 TriggerBounds 预过滤 |
| 订单回报处理 | 0.10 μs | 0.10 μs | 0.20 μs | 含 enrich + 去重 |
| 持仓更新 | 0.10 μs | 0.10 μs | 0.10 μs | 纯内存操作 |
| SPSC 队列吞吐 | — | **5.72×10⁸ ops/s** | — | push+pop 配对 |

完整报告: [BENCHMARK.md](BENCHMARK.md)

---

## 项目规模

| 指标 | 数值 |
|------|------|
| 语言 | C++17 |
| 源文件 | 73 |
| 总代码行数 | ~89,600 |
| 测试用例 | 89 (全通过) |
| 柜台 | CTP + QDP (可选) |
| 平台 | Windows (MSVC) + Linux (GCC) |
