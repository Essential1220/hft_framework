# 工程设计决策

> 每个模块：**做了什么决策 → 为什么这样做 → 拒绝了什么替代方案 → 项目里代码怎么写的。**

---

## 目录

1. [核心原则：单消费者线程模型](#1-核心原则单消费者线程模型)
2. [SPSC 队列](#2-spsc-队列)
3. [FixedKey 栈分配标识符](#3-fixedkey-栈分配标识符)
4. [Event 事件系统 (Tagged Union)](#4-event-事件系统-tagged-union)
5. [网关抽象层](#5-网关抽象层)
6. [消费者主循环](#6-消费者主循环)
7. [订单管理：去重 + 终态保护 + enrich](#7-订单管理去重--终态保护--enrich)
8. [条件单管理器：TriggerBounds + 3 阶段检查](#8-条件单管理器triggerbounds--3-阶段检查)
9. [风控管理器：7 维检查 + RMS 递进](#9-风控管理器7-维检查--rms-递进)
10. [持仓管理器](#10-持仓管理器)
11. [策略热加载 + pybind11](#11-策略热加载--pybind11)
12. [凭据加密存储](#12-凭据加密存储)

---

## 1. 核心原则：单消费者线程模型

### 决策

所有引擎状态（订单、持仓、风控、策略）由**唯一线程**——消费线程——持有。
网关线程只做解码 + SPSC 入队。异步线程（日志、状态保存、tick 落盘）只消费数据，
绝不回写引擎状态。

### 解决什么问题

| 多线程加锁的问题 | 本设计如何消除 |
|---|---|
| 锁竞争 → 尾延迟不可控 | 热路径零锁 → 延迟确定 |
| 死锁风险 | 单一持有者 → 没有锁顺序 |
| 数据竞争 | 无可变共享状态 |
| 多线程 bug 难调试 | 单线程逻辑 → 线性执行，一目了然 |

### 放弃了什么

- 不能跨核心并行执行策略。但在 <1μs/tick 量级，网络 I/O 和交易所延迟才是瓶颈。
- `on_tick()` 不能跑阻塞操作（设计约束：需要阻塞 I/O 丢给异步线程）。

### 为什么不全用无锁数据结构

无锁编程（CAS 循环、hazard pointer）比单线程持有更难写对。本设计只在网关→引擎
边界用了**唯一一个**无锁结构（SPSC 队列）。引擎内部全是普通单线程代码。

---

## 2. SPSC 队列

**文件：** `src/common/spsc_queue.h`

### 决策

固定大小环形缓冲区，`alignas(64)` 缓存行隔离，acquire/release 内存序，
零 mutex、零条件变量、零系统调用。

### 为什么不用 std::queue + std::mutex

- `mutex::lock()` 有竞争时进入内核 → 一次几百纳秒
- 竞争时线程可能被挂起 → 调度器唤醒延迟不可预测 → p99 爆炸
- SPSC push/pop 只需 ~2ns，走的是纯用户态原子操作

### 为什么不用 MPMC 无锁队列

MPMC 在 push 和 pop 两端都用 CAS 循环，可能多次重试。SPSC 知道只有一个 producer
和一个 consumer，只需要 store + load——不需要 CAS，不需要重试。

### 代码

```cpp
template <typename T, size_t N>
class SPSCQueue {
    // 两个编译期硬约束
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    bool push(const T& item) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & kMask;           // 位运算取模，1 cycle
        if (next == tail_.load(std::memory_order_acquire)) {
            drop_count_.fetch_add(1, std::memory_order_relaxed);
            return false;                               // 满 → 丢弃，不阻塞
        }
        buf_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        item = buf_[t];
        tail_.store((t + 1) & kMask, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t kMask = N - 1;
    alignas(64) std::atomic<size_t> head_;        // 每个变量独占缓存行
    alignas(64) std::atomic<size_t> tail_;        // 消除伪共享
    alignas(64) T buf_[N];
    alignas(64) std::atomic<size_t> drop_count_;
};
```

### 三条队列三种容量

| 队列 | 容量 | 理由 |
|------|------|------|
| `md_queue_` | 65,536 | 行情每秒数千条，需吸收微突发 |
| `trade_queue_` | 同 md | 重连同步时可能大量回调涌入 |
| `cmd_queue_` | 4,096 | 命令很少，但丢失命令比丢行情更危险 |

---

## 3. FixedKey 栈分配标识符

**文件：** `src/common/types.h`

### 决策

合约 ID、订单引用等标识符用 `char[N]` 数组在栈上存储，不用 `std::string`。

### 为什么不用 std::string

`std::string` 在热路径上每次构造可能 `malloc` → 内核调用 → 不可预测延迟。
运行几小时后 `malloc` 碎片化，性能持续下降。

```cpp
// std::string 版本（被拒绝）：
std::unordered_map<std::string, OrderInfo> orders_;
// 每次 insert: "IF2406" → malloc(6) → free
// 10,000 次插入 = 10,000 次堆分配

// FixedKey 版本（本设计）：
std::unordered_map<OrderRefKey, OrderInfo, FixedKeyHash<16>> orders_;
// 10,000 次插入 = 零次堆分配
```

### 代码

```cpp
template <size_t N>
struct FixedKey {
    char data[N]{};                          // 栈上数组，零堆分配

    explicit FixedKey(const char* s) {
        safe_copy(data, s, N);              // strncpy
    }

    bool operator==(const FixedKey& o) const {
        return std::strcmp(data, o.data) == 0;
    }
};

using OrderRefKey = FixedKey<16>;   // 报单引用号
using TradeIdKey  = FixedKey<24>;   // 成交编号

// FNV-1a 哈希，编译期内联，无间接调用
struct InstrumentKeyHash {
    size_t operator()(const InstrumentKey& k) const {
        size_t h = 14695981039346656037ULL;
        for (const char* p = k.data; *p; ++p) {
            h ^= static_cast<size_t>(static_cast<unsigned char>(*p));
            h *= 1099511628211ULL;
        }
        return h;
    }
};
```

### trivially_copyable 编译期强制检查

```cpp
// 所有走 SPSC 队列的类型必须在编译期通过此检查
static_assert(std::is_trivially_copyable_v<Event>,
    "Event must be trivially copyable for SPSC queue");
```

如果有人往 TickData 里加了 `std::string` 字段，编译直接挂，给出明确错误信息，
不会静默产生运行时的未定义行为。

---

## 4. Event 事件系统 (Tagged Union)

**文件：** `src/common/event.h`

### 决策

用 union + type tag 传递事件，不用继承 + 虚函数。

### 对比

```cpp
// 本设计 (tagged union)：固定大小、memcpy 安全、零虚表
struct Event {
    EventType type;          // 1 字节标签
    union {
        TickData tick;
        OrderInfo order;
        TradeInfo trade;
        AccountInfo account;
        PositionInfo position;
        CancelRejectInfo cancel_reject;
    };
};
// sizeof(Event) = max(sizeof(各成员)) + 1 字节
// 从网关传到引擎：一次 memcpy（SPSC push → pop）

// 被拒绝的继承方案：
class Event { virtual void dispatch() = 0; };  // 虚表指针 8 字节
class TickEvent : public Event { TickData tick; }; // 需要 new → 堆分配
```

### 为什么单队列而不是多队列

六种队列 = 六个 cache line 在核心间弹跳 + **丧失了事件间的 FIFO 顺序保证**。
如果一个 tick 和一笔 trade 按这个顺序从交易所到达，单队列保证消费顺序也是
tick → trade。多队列无法保证。

---

## 5. 网关抽象层

**文件：** `src/gateway/i_md_gateway.h`

### 决策

纯虚接口 `IMdGateway` / `ITradeGateway` 隐藏 CTP/QDP SDK 差异。

### 解决三个问题

1. **无 CTP 也能测试：** `FakeMdGateway` 在内存中生成合成 tick，整个引擎管线
   不需要 CTP 账号就能跑集成测试。
2. **多柜台零改动引擎：** 加一个新柜台（如 XTP）只需实现接口，引擎代码不动。
3. **防御式隔离：** CTP SDK 内部线程你控制不了。网关把它围起来——回调线程
   只做解码+入队，绝不碰引擎状态。CTP 线程和引擎线程通过 SPSC 队列彻底解耦。

### 放弃了什么

每个 tick 多一次虚函数调用（~5ns），跟网关实际做的解码 + memcpy 相比可忽略。

---

## 6. 消费者主循环

**文件：** `src/engine/trading_engine.cpp`

### 决策

单线程事件循环，按优先级 drain：cmd queue → trade queue → md queue。

### 代码

```cpp
// 消费者主循环核心结构
while (running_) {
    bool got_event = false;
    EngineCommand cmd;

    // 1. 优先处理命令队列（紧急命令不能等）
    while (cmd_queue_.pop(cmd)) {
        got_event = true;
        process_command(cmd);
    }

    // 2. 处理交易回报队列（订单状态、成交、账户信息）
    // ... drain trade_queue_ ...

    // 3. 批量处理行情队列
    //    每 64 tick 穿插检查命令队列，防止紧急命令被大批量行情阻塞
    size_t md_processed = 0;
    while (md_processed < md_batch_size_ && md_queue_.pop(evt)) {
        ++md_processed;
        if (evt.type == EventType::Tick) {
            process_tick(evt.tick);
        }
        if ((md_processed & 63) == 0) {
            while (cmd_queue_.pop(cmd)) process_command(cmd);
        }
    }

    // 4. 无事件时：渐进降级休眠
    if (!got_event) {
        ++idle_spins;
        if (in_trading_session) {
            if (idle_spins < 100)
                HFT_CPU_PAUSE();                    // ~40 cycles
            else if (idle_spins < 1000)
                std::this_thread::yield();          // 让出 CPU
            else
                std::this_thread::sleep_for(10us);  // 真正休眠
        }
    } else {
        idle_spins = 0;
    }
}
```

### 批量大小的选择

- 生产模式 (`ProductionHftMode=1`)：默认 512
- 非生产模式：默认 1（逐条处理，方便调试）
- 上限 2048：防止极端行情下无限循环

---

## 7. 订单管理：去重 + 终态保护 + enrich

**文件：** `src/order/order_manager.cpp`

### 问题 1：CTP 可能重放成交回报

CTP 的 `THOST_TERT_QUICK` 模式可能将同一个成交回放多次。没有去重 →
持仓翻倍、PnL 翻倍。

### 代码：成交去重

```cpp
void OrderManager::on_trade_return(const TradeInfo& trade) {
    std::lock_guard<std::mutex> lock(mtx_);

    // 用 FixedKey 做 O(1) 哈希去重
    if (trade.trade_id[0] != '\0') {
        TradeIdKey tid(trade.trade_id);

        // 防止 seen_trade_ids_ 无限增长
        if (seen_trade_ids_.size() > 100000) {
            seen_trade_ids_.clear();
        }

        if (!seen_trade_ids_.insert(tid).second) {
            return;  // 重复成交，直接忽略
        }
    }
    // 正常处理...
}
```

### 问题 2：CTP 可能乱序投递回调

CTP 在高流量期间已知会乱序投递回调——成交回报在订单状态更新之前到达，或者
已成交的订单后又收到 "Pending" 状态。

### 问题 3：enrich —— 回填策略 ID

CTP 回调不包含策略 ID——只有引擎生成的 `OrderRef`。`enrich_order_info()` 从本地
订单字典（按 OrderRef 索引）回填 strategy_id：

```cpp
bool OrderManager::enrich_order_info(OrderInfo& order) const {
    std::lock_guard<std::mutex> lock(mtx_);
    OrderRefKey key(order.order_ref);
    auto it = orders_.find(key);
    if (it == orders_.end()) return false;

    // 回填策略关心的字段
    safe_copy(order.strategy_id, it->second.strategy_id, ...);
    safe_copy(order.instrument_id, it->second.instrument_id, ...);
    return true;
}
```

---

## 8. 条件单管理器：TriggerBounds + 3 阶段检查

**文件：** `src/order/conditional_order_manager.cpp`

### 问题

100 个活跃条件单分布在 20 个合约上。每 tick 遍历全部 100 个条件单？
99% 的 tick 价格根本不在触发价附近——纯浪费。

### 决策：TriggerBounds O(1) 预过滤

为每个合约维护触发价格的 min/max 边界，每 tick 先 O(1) 检查当前价格是否落在
任何触发范围内，不落在范围则直接跳过该合约。

### 代码

```cpp
// 预计算的触发边界
struct TriggerBounds {
    bool dirty = false;      // 是否需要重建（条件单增删后置脏）
    bool has_trailing = false;
    double min_stop_buy  = +inf;   // 多头止损最低触发价
    double max_stop_sell = -inf;   // 空头止损最高触发价
    double max_take_buy  = -inf;   // 多头止盈最高触发价
    double min_take_sell = +inf;   // 空头止盈最低触发价
};

// O(1) 快速检查：该合约是否有条件单可能在此价格触发？
bool ConditionalOrderManager::may_trigger_locked(
    const InstrumentKey& key, double price) {

    auto bounds_it = trigger_bounds_.find(key);
    if (bounds_it == trigger_bounds_.end()) return false;

    // 懒重建：边界标记为脏时从活跃订单重建
    if (bounds_it->second.dirty) {
        rebuild_bounds_locked(key);
        bounds_it = trigger_bounds_.find(key);
        if (bounds_it == trigger_bounds_.end()) return false;
    }

    const auto& b = bounds_it->second;
    return b.has_trailing ||
           price >= b.min_stop_buy  - eps ||
           price <= b.max_stop_sell + eps ||
           price <= b.max_take_buy  + eps ||
           price >= b.min_take_sell - eps;
}
```

### 3 阶段 check_tick：防止死锁

```cpp
ConditionalCheckResult check_tick(const TickData& tick, callback) {
    // Phase 1 (持锁过滤)：评估哪些条件单触发了 → 构建快照
    {
        std::lock_guard lock(mtx_);
        if (!may_trigger_locked(instr, tick.last_price)) return;
        // 遍历该合约的条件单，判断触发 → 收集到 trigger_list
    }  // 释放锁

    // Phase 2 (解锁回调)：对触发单逐一调用 send_order
    //    为什么解锁？send_order 可能回调到 ConditionalOrderManager
    //    （如 OCO 替补单）→ 持锁回调 = 死锁
    for (const auto& info : trigger_list) {
        callback(info.req, reason);
    }

    // Phase 3 (持锁提交)：更新状态，移除已触发单
    {
        std::lock_guard lock(mtx_);
        for (auto& cr : callback_results) {
            if (cr.result == Sent)
                order.status = Triggered;
                orders_.erase(order_id);
                mark_bounds_dirty_locked(instr);  // 触发边界需要重建
        }
    }
}
```

---

## 9. 风控管理器：7 维检查 + RMS 递进

**文件：** `src/risk/risk_manager.cpp`

### 决策

检查维度按**成本从低到高**排列，最便宜的先执行。`is_risk_reduction` 操作
绕过非必要检查——**风控绝不能阻止你降低风险**。

### 检查顺序（瀑布流）

| # | 检查项 | 成本 | 说明 |
|---|--------|------|------|
| 1 | volume > 0 | ~1 次比较 | 基本合法性 |
| 2 | volume ≤ MaxOrderSize | ~1 次比较 | 胖手指检查 |
| 3 | 在交易时段内 | ~1 次查表 | 非交易时段直接拒绝 |
| 4 | 日内亏损 < MaxDailyLoss | 查 AccountInfo + PnL | 保护本金 |
| 5 | 净持仓 ≤ MaxNetPosition（含挂单投影） | 查持仓 + 条件单待开仓量 | 控制风险敞口 |
| 6 | 可平仓位 ≥ 平仓量 | 查持仓（今仓/昨仓分别验证） | 防止超卖废单 |
| 7 | 报单频率 < MaxOrdersPerMinute | O(滑动窗口) | 防止流控熔断 |
| 8 | 撤单率 < MaxCancelRate | O(滑动窗口) | 防止交易所警告或封号 |

### is_risk_reduction 豁免机制

当操作本质上是降低风险时（如平掉亏损仓位），跳过非必要检查：

```
场景：日内亏损已超过 5000 限额
操作 A：BUY  OPEN  1 手 → 检查 1-8 全走 → 第 4 步被拒 ✓
操作 B：SELL CLOSE 1 手 → is_risk_reduction=true
       → 跳过检查 2/3/4/7/8 → 放行 ✓
```

### RMS 模式递进

```
Normal ──→ NoOpen ──→ ReduceOnly ──→ Liquidating ──→ Halted
  正常       禁开仓       仅减仓          强制平仓        熔断
              ↑                            │
              └── 手动降级 ─────────────────┘
```

**升级自动，降级必须手动。** 防止振荡——如果系统自动恢复，可能在闪崩期间
反复开仓→平仓→平仓→开仓。手动降级强制人工确认情况已解除。

### 撤单率豁免（开发/测试策略）

某些开发策略（反复 send→cancel 测试）会打满撤单率窗口。
通过 glob pattern 在配置中声明豁免：

```cpp
// glob_match 支持 * 通配符，如 "*_test", "dev_*"
bool RiskManager::is_cancel_rate_exempt(const char* strategy_id) const {
    for (const auto& pat : cancel_rate_exempt_patterns_) {
        if (glob_match(pat, strategy_id)) return true;
    }
    return false;
}
```

---

## 10. 持仓管理器

**文件：** `src/position/position_manager.h`

### 决策

用 `std::mutex` 保护持仓表。持仓更新只在 `on_trade()` 时发生（每秒几次），
不在每个 tick 上都调用，mutex 竞争几乎为零。

### 关键设计点

```cpp
class PositionManager {
public:
    void on_trade(const TradeInfo& trade);  // 只在成交时调用，非热路径
    PositionInfo get_position(const char* instrument, Direction dir) const;

    // 对账标记：当平仓量超过持仓量时触发，通知引擎发起与柜台的持仓查询
    bool needs_reconciliation() const;
    void clear_reconciliation_flag();

private:
    // InstrumentKey 用 FixedKey，避免 map key 堆分配
    using Map = std::unordered_map<InstrumentKey, PositionInfo, InstrumentKeyHash>;
    mutable std::mutex mtx_;
    Map positions_;
    std::atomic<bool> reconciliation_needed_{false};
};
```

**`get_position()` 虽然加了锁，但实际竞争为零**——读写都在消费线程上，
锁从不阻塞。锁的存在只是为了形式上的线程安全保证（防止未来代码误用）。

---

## 11. 策略热加载 + pybind11

**文件：** `src/strategy/strategy_manager.*`, `src/strategy/py_strategy.*`

### 决策

Python 策略通过 pybind11 在进程内运行，`on_tick` 从消费线程调用。
热加载支持不停机替换 `.py` 文件——监听文件变化 → `on_destroy()` 旧实例
→ 重新 import 模块 → `on_init()` 新实例。

### 为什么 pybind11（不用 ctypes、不用 subprocess）

| 方案 | TickData 传递 | GIL | 启动成本 |
|------|-------------|-----|---------|
| pybind11 | **零拷贝**：直接访问 C++ 字段 | 显式管理 | ~10ms |
| ctypes | 逐字段复制到 Python | 每次调用开销 | ~100ms |
| subprocess+IPC | 序列化+管道+反序列化 | 独立解释器 | ~1s |

### GIL 批量优化

```cpp
// 所有 Python 策略共享一次 GIL 获取/释放
// 而不是每个策略各自获取一次
if (strategy->is_interpreted()) {
    auto gil = strategy->acquire_interpreter_lock();  // 获取 GIL 一次
    for (auto& s : python_strategies) {
        s->on_tick(tick);   // 所有策略在同一次 GIL 持有下运行
    }
}  // GIL 在 gil 离开作用域时自动释放
```

### ProductionHftMode 禁用 Python 热路径

```cpp
// 生产 HFT 部署可以完全跳过 Python 解释器
production_hft_mode_ = config.get_int("Performance", "ProductionHftMode", 0) > 0;
if (production_hft_mode_) {
    hft_disable_python_hot_path_ = true;   // 关闭 Python on_tick
    disable_tick_recording_hot_path_ = true; // 关闭 tick 录制
    disable_kline_hot_path_ = true;         // 关闭 K 线聚合
}
```

纯 C++ 策略在 ProductionHftMode 下每个 tick 省去 ~100-200ns 的 Python 解释器开销。

---

## 12. 凭据加密存储

**文件：** `src/common/crypto.h`, `src/common/crypto.cpp`

### 决策

凭据存 SQLite，密码用 DPAPI (Windows) 加密，首次明文写入后自动改写为
`ENC:<base64>` 不可恢复。

### 为什么不能 config.ini 明文

- INI 文件可能误提交 git → 凭据泄露
- INI 没有访问控制 → 任何进程可读
- INI 不能安全热加载

### 为什么不用环境变量

环境变量被子进程继承、在 Linux `/proc` 中可见、可能被监控工具记录。

### DPAPI 原理

加密密钥从 Windows 用户登录凭据派生。只有以该用户身份运行的进程才能解密。
加密后的数据在其他机器或用户下完全无用。

### 代码

```cpp
namespace hft::crypto {

// 加密明文，返回 "ENC:" + Base64 密文
std::string encrypt_config_value(const std::string& plaintext);

// 解密：如果以 "ENC:" 开头则解，否则原样返回（兼容旧明文配置）
std::string decrypt_config_value(const std::string& value);

} // namespace hft::crypto
```
