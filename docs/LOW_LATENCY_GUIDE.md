# 低延迟开发指南

从具体代码出发，理解 `hft_framework` 中每一项低延迟技术：**为什么用它 → 它解决什么问题 → 项目里怎么写的。**

---

## 目录

1. [CPU 缓存与伪共享 —— 为什么 `alignas(64)`](#1-cpu-缓存与伪共享--为什么-alignas64)
2. [无锁 SPSC 队列 —— 为什么不用 mutex](#2-无锁-spsc-队列--为什么不用-mutex)
3. [内存序 —— acquire/release 在干什么](#3-内存序--acquirerelease-在干什么)
4. [FixedKey —— 为什么不用 std::string](#4-fixedkey--为什么不用-stdstring)
5. [Tagged Union —— 为什么不用继承+虚函数](#5-tagged-union--为什么不用继承虚函数)
6. [CPU 绑核 —— SetThreadAffinityMask](#6-cpu-绑核--setthreadaffinitymask)
7. [VirtualLock —— 防止页面换出](#7-virtuallock--防止页面换出)
8. [批量处理 —— 一次 drain 512 条](#8-批量处理--一次-drain-512-条)
9. [渐进降级休眠 —— CPU pause → yield → sleep](#9-渐进降级休眠--cpu-pause--yield--sleep)
10. [延伸阅读](#10-延伸阅读)

---

## 1. CPU 缓存与伪共享 —— 为什么 `alignas(64)`

### 背景

CPU 访问内存不是按字节读的，而是一次读 64 字节，这块叫**缓存行（cache line）**。
如果两个线程各改各的变量，但这俩变量落在同一个 64 字节缓存行里，就会互相
无效对方的缓存——这叫**伪共享（false sharing）**。

### 后果

Producer 写 `head_` → 该缓存行在 Producer 的 core 上变脏 → Consumer 所在 core
的同一缓存行被无效 → Consumer 读 `tail_` 时 cache miss → 重新从内存加载 → 白花 ~100ns。
**Consumer 读的 `tail_` 根本没人动过，但它还是 cache miss 了。**

### 项目代码：`src/common/spsc_queue.h`

```cpp
template <typename T, size_t N>
class SPSCQueue {
private:
    // 每个关键变量独占一个 64 字节缓存行
    // Producer 写 head_ 不会影响 Consumer 读 tail_
    alignas(64) std::atomic<size_t> head_;       // Producer 写入
    alignas(64) std::atomic<size_t> tail_;       // Consumer 读取
    alignas(64) T buf_[N];                       // 数据缓冲区
    alignas(64) std::atomic<size_t> drop_count_; // 丢弃计数
};
```

`alignas(64)` 强制变量从 64 字节对齐的地址开始，确保每个变量落在独立的缓存行上。
Producer 和 Consumer 操纵不同的缓存行，互不干扰。

---

## 2. 无锁 SPSC 队列 —— 为什么不用 mutex

### 背景

线程间传递数据，最直接的想法是 `std::queue` + `std::mutex`。但在 HFT 场景下
这不可接受：

- `std::mutex::lock()` 有竞争时进入内核态 → 一次几百纳秒到几微秒
- 竞争激烈时线程被 OS 挂起 → 可能几毫秒后才被唤醒 → p99 直接爆炸

### 为什么 SPSC

SPSC = Single Producer Single Consumer。因为只有**一个人写、一个人读**，
head 和 tail 各归各的线程管，不需要 CAS（Compare-And-Swap）循环重试，
直接 store/load 就够——这是最弱的同步，也是最快的同步。

对比：

| 方案 | 同步机制 | 延迟 |
|------|---------|------|
| `std::mutex` + `std::queue` | 内核互斥锁 | ~500ns（有竞争时） |
| 无锁 MPMC 队列 | CAS 循环 | ~20-50ns |
| **无锁 SPSC 队列** | **store + load** | **~2ns** |

### 项目代码：`src/common/spsc_queue.h`

```cpp
// Producer 调用：写入一个元素
bool push(const T& item) {
    const size_t h = head_.load(std::memory_order_relaxed);
    const size_t next = (h + 1) & kMask;             // 位运算取模，1 个 CPU 周期
    if (next == tail_.load(std::memory_order_acquire)) {
        drop_count_.fetch_add(1, std::memory_order_relaxed);
        return false;                                 // 队列满，丢弃而非阻塞
    }
    buf_[h] = item;
    head_.store(next, std::memory_order_release);     // "数据写好了"
    return true;
}

// Consumer 调用：读出一个元素
bool pop(T& item) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) {
        return false;                                 // 队列空
    }
    item = buf_[t];
    tail_.store((t + 1) & kMask, std::memory_order_release);
    return true;
}
```

**两个编译期硬约束：**

```cpp
static_assert((N & (N - 1)) == 0, "N must be a power of 2");
// 容量必须是 2 的幂 → 取模用 & 而非 %，1 cycle vs 20-80 cycles

static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
// 元素必须是平凡可复制 → memcpy 安全，防止 std::string 混进来
```

**为什么满了丢弃而不是阻塞？** 丢一个旧 tick 可以接受（价格过期）；阻塞网关
线程 = 后续所有 tick 全丢 = 灾难。

---

## 3. 内存序 —— acquire/release 在干什么

### 背景

CPU 和编译器会**乱序执行**你的代码。在单线程内不影响结果，但在多线程间：

```cpp
// 线程 A
data = 42;
ready = true;    // CPU 可能先执行这行，再执行 data = 42

// 线程 B
while (!ready) {}
int x = data;    // 可能读到旧值！因为线程 A 的写入被重排了
```

### memory_order_release / acquire 的作用

| 内存序 | 通俗理解 |
|--------|---------|
| `release` | "我前面所有的内存操作都完成了，后面的线程可以安全读了" |
| `acquire` | "我看到这个信号了，我保证读到的是对方写完后的最新数据" |

两者配对使用，形成一个 **happens-before** 关系。

### 项目里怎么用的

```cpp
// Producer (网关线程):
buf_[h] = item;                                     // ① 数据先放好
head_.store(next, std::memory_order_release);       // ② "开门"——保证①在②之前

// Consumer (引擎线程):
if (t == head_.load(std::memory_order_acquire))     // ③ "等门开"——保证后续读在③之后
    return false;
item = buf_[t];                                     // ④ 安全读取——一定读到①写入的数据
```

**为什么先 `relaxed` 读再 `acquire` 读？** `relaxed` 最便宜（无同步开销），
先用它读一次，只有不空/不满时才需要 acquire 语义。这是一种微优化。

---

## 4. FixedKey —— 为什么不用 std::string

### 背景

`std::string` 在热路径上是延迟杀手：
- 每次构造可能触发 `malloc`（堆分配）→ 内核调用 → 不可预测延迟
- 运行几小时后 malloc 碎片化 → 性能持续下降
- 即使是 SSO（小字符串优化，<16 字符不分配），一旦字符串变长就越过阈值

### 项目代码：`src/common/types.h`

```cpp
// 栈上固定大小标识符，零堆分配
template <size_t N>
struct FixedKey {
    char data[N]{};                          // 栈上数组，不分配堆内存

    FixedKey() = default;
    explicit FixedKey(const char* s) {
        safe_copy(data, s, N);              // strncpy，无 malloc
    }

    bool operator==(const FixedKey& o) const {
        return std::strcmp(data, o.data) == 0;  // memcmp 级别，一条 SSE 指令
    }
};

// 使用方式：用 FixedKey 替代 std::string 做 map key
using OrderRefKey = FixedKey<16>;
using TradeIdKey  = FixedKey<24>;

// 在 OrderManager 里：
std::unordered_map<OrderRefKey, OrderInfo, FixedKeyHash<16>> orders_;
// 每次插入/查找：零堆分配，hash 内联计算
```

**效果：** 10,000 次 `orders_` 插入 = 零次 malloc + 零次 free。如果用 `std::string`，
就是 10,000 次 malloc + 10,000 次 free，全在热路径上。

---

## 5. Tagged Union —— 为什么不用继承+虚函数

### 背景

不同事件类型（Tick、Order、Trade）需要走 SPSC 队列传递。如果用继承方案：

```cpp
// 被拒绝的方案
class Event { virtual void dispatch() = 0; };   // 有虚表指针，8 字节浪费
class TickEvent : public Event { TickData tick; }; // 需要 new，堆分配
// dynamic_cast 查找虚表 → 10-20 cycles
```

### 项目代码：`src/common/event.h`

```cpp
enum class EventType : uint8_t {
    Tick, Order, Trade, Account, Position, CancelRejected
};

struct Event {
    EventType type;          // 1 字节标签
    union {
        TickData tick;       // 所有事件类型共享同一块内存
        OrderInfo order;
        TradeInfo trade;
        AccountInfo account;
        PositionInfo position;
        CancelRejectInfo cancel_reject;
    };
    // 固定大小 = max(sizeof(各成员)) + 1 字节
    // 零虚表、零堆分配、memcpy 安全
};

// 编译期保证 SPSC 安全
static_assert(std::is_trivially_copyable_v<Event>,
    "Event must be trivially copyable for SPSC queue");
```

**好处：**
- 一个 Event 从网关传到引擎只需要一次 memcpy（SPSC push/pop），没有虚函数派发
- 消费侧用 switch-case 分发（CPU 分支预测器可以学习模式）
- 单队列保证 FIFO 顺序（多队列会失去事件间的时序关系）

---

## 6. CPU 绑核 —— SetThreadAffinityMask

### 背景

操作系统随时可能把线程调度到不同的 CPU 核心上。每切换一次：
- 原来的 L1/L2 缓存全部作废
- 新的核心要从头加载数据 → 全是 cache miss
- p99 延迟出现不可预测的毛刺

### 项目代码：`src/main.cpp`

```cpp
// 把指定线程绑定到指定 CPU 核心
void set_thread_affinity(std::thread& thread, int cpu_core) {
    if (cpu_core < 0 || !thread.joinable()) return;
#ifdef _WIN32
    const DWORD_PTR mask = DWORD_PTR{1} << static_cast<DWORD_PTR>(cpu_core);
    SetThreadAffinityMask(static_cast<HANDLE>(thread.native_handle()), mask);
#endif
}

// 设置线程为最高优先级——减少被其他线程抢走 CPU 的概率
void set_thread_high_priority(std::thread& thread) {
#ifdef _WIN32
    SetThreadPriority(static_cast<HANDLE>(thread.native_handle()),
                      THREAD_PRIORITY_HIGHEST);
#endif
}

// CTP SDK 内部线程不在我们控制范围内，通过枚举进程所有线程强制绑核
void pin_process_threads_to_core(int exclude_tid_main, int target_core) {
#ifdef _WIN32
    const DWORD_PTR mask = DWORD_PTR{1} << static_cast<DWORD_PTR>(target_core);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    // 遍历当前进程所有线程，跳过自己，其余全绑到 target_core
    // ...
    while (Thread32Next(snap, &te)) {
        HANDLE hThread = OpenThread(..., te.th32ThreadID);
        SetThreadAffinityMask(hThread, mask);
    }
#endif
}
```

### 生产配置

```ini
[Performance]
EngineCpuCore = 2      # 消费线程绑到 CPU 2
LoggerCpuCore = 3      # 日志线程绑到 CPU 3（跟消费线程不同核心）
CtpThreadCpuCore = 4   # CTP SDK 线程绑到 CPU 4（避免跟消费线程抢缓存）
```

---

## 7. VirtualLock —— 防止页面换出

### 背景

操作系统按 4KB 页管理内存。不常用的内存页可能被换出到磁盘（swap）。
热路径上某次访问恰好触发 page fault → 几百纳秒到几毫秒的延迟尖刺。

### 项目代码：`src/main.cpp`

```cpp
void lock_current_thread_memory() {
#ifdef _WIN32
    // 仅当环境变量 HFT_LOCK_STACK=1 时启用（需要管理员权限）
    const char* enabled = std::getenv("HFT_LOCK_STACK");
    if (!enabled || std::string(enabled) != "1") return;

    // 先提升进程权限以允许 VirtualLock
    HANDLE hToken = nullptr;
    OpenProcessToken(GetCurrentProcess(),
                     TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);
    // ... 启用 SE_LOCK_MEMORY_NAME 权限 ...

    // 锁定当前线程栈内存——这块内存永不换出
    volatile char stack_probe[64 * 1024]{};
    if (!VirtualLock((LPVOID)stack_probe, sizeof(stack_probe))) {
        LOG_WARN("VirtualLock failed");
    }
#endif
}
```

**注意：** VirtualLock 需要 `SeLockMemoryPrivilege` 权限，且锁定太多内存会影响
系统整体性能。本项目默认关闭（需手动 `HFT_LOCK_STACK=1`），仅在需要极限低延迟
的生产环境开启。

---

## 8. 批量处理 —— 一次 drain 512 条

### 背景

逐条 pop→process→pop→process 的循环有几个问题：
- 每次循环跳转都有分支预测开销
- CPU 流水线频繁刷新
- 指令缓存利用率低

### 项目代码：`src/engine/trading_engine.cpp`

```cpp
// 配置读取：生产模式默认 512，非生产模式默认 1
md_batch_size_ = config_.get_int("Performance", "MdBatchSize",
    production_hft_mode_ ? 512 : 1);
md_batch_size_ = std::min(md_batch_size_, kMaxMdBatchSize);  // 上限 2048

// 消费者主循环：
size_t md_processed = 0;
while (md_processed < md_batch_size_ && md_queue_.pop(evt)) {
    got_event = true;
    ++md_processed;
    if (evt.type == EventType::Tick) {
        process_tick(evt.tick);
    }

    // 每处理 64 个 tick，穿插检查一次命令队列
    // 防止大批量行情阻塞紧急命令（如 EmergencyClose）
    if ((md_processed & 63) == 0) {
        while (cmd_queue_.pop(cmd)) {
            process_command(cmd);
        }
    }
}
```

**设计要点：**
- 每 64 tick 穿插检查 cmd queue → 确保紧急命令不会因为大 batch 而延迟
- `md_processed & 63` 是位运算，比 `md_processed % 64` 快一个数量级
- batch size 上限 2048 → 极端行情下不会无限循环阻塞命令和定时任务

---

## 9. 渐进降级休眠 —— CPU pause → yield → sleep

### 背景

消费线程在没有事件时如果什么都不做（忙等 spin-wait），会 100% 占满一个 CPU 核心。
但如果直接 `sleep`，唤醒延迟可能到毫秒级——下一个 tick 来了不能及时响应。

### 项目代码：`src/engine/trading_engine.cpp`

```cpp
if (!got_event) {
    try_refresh_active_orders();
    ++idle_spins;

    if (session_mgr_.last_in_trading_session()) {
        // 交易时段内：渐进降级，平衡延迟和 CPU 占用
        if (idle_spins < 100) {
            HFT_CPU_PAUSE();                         // ~40 cycles，极快唤醒
        } else if (idle_spins < 1000) {
            std::this_thread::yield();               // 让出 CPU，但保持调度就绪
        } else {
            std::this_thread::sleep_for(
                std::chrono::microseconds(10));      // 真正休眠
        }
    } else {
        // 非交易时段：直接长休眠
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
} else {
    idle_spins = 0;  // 有事件处理时重置计数器
}
```

**策略：** 空闲次数越少 → 越激进地等（CPU pause，延迟最低）；空闲次数越多 →
越温和地让出 CPU（sleep，省资源）。交易时段和非交易时段两套策略。

`HFT_CPU_PAUSE()` 的定义：
```cpp
#if defined(__x86_64__) || defined(_M_X64)
    #define HFT_CPU_PAUSE() _mm_pause()   // x86 PAUSE 指令，~40 cycles
#elif defined(__aarch64__)
    #define HFT_CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#endif
```

---

## 10. 延伸阅读

- [BENCHMARK.md](BENCHMARK.md) —— 本项目可复现的延迟数据
- [DESIGN.md](DESIGN.md) —— 每个模块为什么这样设计的详细解释
- [What Every Programmer Should Know About Memory](https://people.freebsd.org/~lstewart/articles/cpumemory.pdf) —— Ulrich Drepper 经典长文
- [C++ Concurrency in Action](https://www.manning.com/books/c-plus-plus-concurrency-in-action-second-edition) —— Anthony Williams
