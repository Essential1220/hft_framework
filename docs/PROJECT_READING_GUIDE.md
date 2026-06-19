# hft_framework 项目阅读指南

## 使用方法

1. 按顺序打开文件（从底层到上层，先类型定义再逻辑实现）
2. 每层先**复制 GPT 提示词**丢给 GPT，让它带着你读代码——你边看代码边听它讲
3. 理解后，用"面试追问"自查，看能不能脱稿讲出来
4. GPT 讲完一个模块，立刻用"项目复盘"提示词做总结，把理解固化成你自己的
5. 理解一个模块再进入下一个，不要跳

---

## 怎么用 GPT 学这个项目（总模板）

以下是一个通用提示词模板，每个模块的具体版本见各层。核心是让 GPT 按照固定结构输出：**核心思想 → 代码逐行讲解 → 工程化原则 → 可迁移场景 → 面试话术**。

```
我现在打开着 hft_framework 项目的 <文件路径>，
请你当我的代码导师，按以下结构带我读懂这个模块：

1. 核心思想（2-3 句话）
   - 这个模块是干什么的？解决什么核心问题？
   - 一句话说清楚它的设计哲学

2. 代码逐行讲解
   - 对着我贴给你的代码，逐段解释每个关键函数/结构体在干什么
   - 不要跳，从第一行到最后一行
   - 遇到关键设计决策时，停下来解释"为什么这样写而不那样写"

3. 工程化原则（2-3 条）
   - 这个模块体现了哪些通用的工程化思想？
   - 每条原则：是什么 → 为什么重要 → 不用它会出什么问题

4. 可迁移场景
   - 这个设计思想在非交易系统里能怎么用？
   - 举 1-2 个具体例子（Web 后端、游戏服务器、嵌入式等）

5. 面试话术
   - 如果面试官问"你项目里这个模块怎么设计的"，用 30 秒怎么回答？
   - 给一个可以直接用的回答模板

6. 项目复盘
   - 这个模块如果重做一遍，有哪些可以改进的地方？
   - 有哪些地方过度设计？有哪些地方设计不足？

要求：
- 我是 C++ 初学者，遇到底层概念（内存序、缓存行、原子操作等）要先简单解释再深入
- 解释"为什么"比解释"是什么"更重要
- 中文输出
```

---

## 工程化原则速查（面试前 5 分钟过一遍）

| # | 原则 | 一句话 | 本项目最典型位置 |
|---|------|--------|-----------------|
| 1 | 热路径零分配 | 高频调用的代码路径上，禁止堆分配和系统调用 | SPSC 队列、FixedKey、异步日志 |
| 2 | 单线程拥有状态 | 可变状态只让一个线程写，从根源消灭锁和数据竞争 | 消费线程持有全部引擎状态 |
| 3 | 防御式隔离 | 不信任外部输入——格式、顺序、时机都不假设 | 网关层包围 CTP SDK、成交去重、终态保护 |
| 4 | 优雅降级 | 满的时候丢掉比阻塞好；功能可以关，核心路径不能停 | SPSC 满丢弃、ProductionHftMode |
| 5 | 缓存行感知 | 两个线程各写各的变量，但变量在同一 cache line 上 → 互相拖慢 | `alignas(64)` |
| 6 | 栈优于堆 | 用固定大小栈数组代替动态字符串，编译期确定内存布局 | FixedKey<N>、OrderRefKey |
| 7 | 编译期强制 | 能编译期检查的绝不放到运行时 | `static_assert(is_trivially_copyable)` |
| 8 | 职责边界 | 每个模块只做一件事，模块间通过最小接口通信 | IGateway 接口、OrderManager 不管风控 |
| 9 | 可观测性内建 | 不是出问题了才挂 profiler——运行时一直在记录 | latency.h、延迟跟踪环 |
| 10 | 配置代码分离 | 变参数不重编译，凭证不落明文 | config.ini + ConfigStore(SQLite) |
| 11 | 可测试性是设计目标 | 模块边界画对了，测试才只需要 mock 一两个东西 | FakeMdGateway、3-phase check_tick |
| 12 | 做减法 | 不做的功能和不写的代码同样是设计决策 | 不做 Python 沙箱、不做回测引擎 |

---

## 第 0 层：全局概念 — 先读文档，建立心智模型

这一步不碰代码，先看文档理解整体架构和数据流。

| 顺序 | 文件 | 重点 |
|------|------|------|
| 0.1 | `docs/OVERVIEW.md` | 这是什么项目、不是什么项目、架构全景图、性能数据 |
| 0.2 | `docs/DESIGN.md` | 每个模块为什么这样设计——解决了什么问题、拒绝了什么方案、接受了什么代价 |

### 📋 GPT 提示词

```
现在打开 hft_framework/docs/OVERVIEW.md 和 docs/DESIGN.md。
请你当我的项目导师，按以下结构带我理解这个项目的全局：

1. 这个项目的核心设计哲学是什么？（一句话）
2. 画出架构图：从外部世界（CTP/Python）→ 网关线程 → SPSC 队列 → 消费线程 → 各管理器，每个环节的职责用一句话说明
3. 跟我走一遍数据流：一个 tick 从交易所到策略代码，经过哪些模块？每个模块做什么？
4. 这个项目"刻意不做什么"？为什么不做回测、不做 GUI、不做 Python 沙箱？
5. 性能数据解读：p99=0.70μs 的 tick→策略延迟意味着什么？和 std::mutex、系统调用对比
6. 项目复盘：如果你是面试官，看到这个项目，你最想问哪三个问题？

要求：用中文，先讲"是什么"再讲"为什么"，遇到性能数据时用对比帮我建立直觉。
```

**面试追问：**
- 为什么所有引擎状态都放在一个消费线程？如果拆成多线程会出什么问题？
- 网关线程只做 decode + push，同步操作（下单、查持仓）怎么处理？
- 这个项目"刻意不做什么"？为什么不做回测？为什么不做 GUI？

---

## 第 1 层：类型系统 — 整个项目的数据语言

所有模块之间的通信都靠这些 struct 和 enum。先看懂数据长什么样，后面看模块才能理解"它在处理什么"。

| 顺序 | 文件 | 重点 |
|------|------|------|
| 1.1 | `src/common/types.h` | TickData, OrderInfo, TradeInfo, PositionInfo, AccountInfo, OrderRequest, ConditionalOrder，以及所有的 enum（Direction, Offset, OrderStatus, RiskMode, RiskErrorCode 等） |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/common/types.h，请你当我的 C++ 导师。

这个文件定义了整个交易引擎的数据语言。带我逐段读懂：

1. 核心思想：为什么把数据类型定义集中在 types.h？而不是每个模块各自定义？
2. 逐段讲解：
   - TickData 结构体：每个字段对应现实中的什么？为什么 bid/ask 是 5 档数组而不是 vector？
   - OrderInfo vs OrderRequest 有什么区别？为什么发单和收回报用不同的 struct？
   - Direction、Offset、OrderStatus 这些 enum 为什么用 enum class 而不是 int 常量？
   - RiskMode 的 6 个级别：Normal→NoOpen→ReduceOnly→Liquidating→Halted 为什么这样排序？
   - InstrumentKey（FixedKey）为什么用 char[20] 而不是 std::string？
   - 每个 struct 后面的 static_assert(is_trivially_copyable_v<...>) 是干什么的？
3. 工程化原则：这个文件体现了哪些通用设计思想？至少讲 2 条。
4. 迁移场景：如果你的 Web 后端项目也需要定义一组数据类型在模块间传递，这个文件的设计能借鉴什么？
5. 面试话术：面试官问"你怎么设计项目的数据类型层"，30 秒怎么回答？
6. 项目复盘：这个文件有哪些字段设计可以改进？有没有过度设计的地方？

要求：我是 C++ 初学者，遇到 is_trivially_copyable、POD、enum class 等概念先解释再分析。中文输出。
```

### 工程化原则

#### 原则 1：热路径零分配

**核心思想：** 在每微秒被调用上百次的代码路径上，堆分配（malloc/new）和系统调用是延迟不可预测的根源。设计目标：热路径上的所有数据必须在编译期确定大小、确定位置（栈或对象池）。

**本项目落地：** 所有核心 struct（TickData、OrderInfo、TradeInfo、OrderRequest 等）都是 POD（纯数据，无指针成员）。`static_assert(is_trivially_copyable_v<T>)` 编译期强制约束——如果有人往 TickData 里加了 `std::string`，编译直接报错。

**可迁移场景：** 网络服务器每请求的 context struct 用栈分配或对象池；游戏引擎 Entity 数据放连续数组（ECS 模式）。面试话术："高频路径上所有数据设计成 POD，用 compile-time assert 保证"

#### 原则 2：栈优于堆

**核心思想：** 栈分配 = 移动栈指针（1 条 CPU 指令），堆分配 = 找到空闲块（可能涉及系统调用）。小对象 + 短生命周期 = 栈。

**本项目落地：** `InstrumentKey` 用栈上的 `char[20]` 替代 `std::string`。`FixedKey<N>` 模板把标识符长度固定到编译期。

**可迁移场景：** 高频查找场景把字符串 key 换成定长 key（如 UUID 固定 16 字节），hash map 性能提升 2-5 倍。面试话术："热路径标识符用固定大小栈数组代替 string"

#### 原则 3：防御式设计——异常建模为类型

**核心思想：** 不要用字符串表示错误状态。把每种异常建模为 enum 或类型，编译器帮你检查分支是否穷尽。

**本项目落地：** `OrderRejectReason` 枚举 9 种拒单原因，`RiskMode` 6 级递进，`OrderStatus` 5 个状态。每个模块根据类型做决策，不是根据字符串匹配。

**可迁移场景：** 状态机用 enum class；错误处理返回 `Result<T, ErrorCode>` 而不是 `(T, string)`。面试话术："错误状态定义为 enum，编译器会警告漏掉的分支"

---

## 第 2 层：SPSC 队列 — 架构的血管

整个系统的事件传递全部通过这一层。理解了它，就理解了"单消费者模型"是怎么落地的。

| 顺序 | 文件 | 重点 |
|------|------|------|
| 2.1 | `src/common/spsc_queue.h` | 无锁环形队列，cache line 对齐，head/tail 用 acquire/release 语义 |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/common/spsc_queue.h，请你当我的并发编程导师。

这是一个无锁 SPSC 环形队列的实现，是整个项目"零锁架构"的基石。带我逐行读懂：

1. 核心思想：为什么不用 std::queue + std::mutex？无锁队列解决什么问题？
2. 逐段讲解：
   - 模板参数：为什么 N 必须是 2 的幂？static_assert((N & (N-1)) == 0) 在防什么？
   - 为什么有 static_assert(is_trivially_copyable_v<T>)？如果传了 std::string 会怎样？
   - alignas(64) 在 head_、tail_、buf_、drop_count_ 上——这四个为什么各占独立缓存行？
   - push() 函数逐行讲：relaxed 读 head_ → acquire 读 tail_ → 写 buf_ → release 写 head_。每一步的内存序为什么这样选？
   - pop() 函数逐行讲：同样，relaxed/acquire/release 各在干什么？
   - 队列满了为什么丢弃（drop_count_++）而不是阻塞？如果阻塞会出什么问题？
3. 底层概念解释（我是初学者，请先简单解释再深入）：
   - 什么是缓存行（cache line）？64 字节是怎么来的？
   - 什么是伪共享（false sharing）？不用 alignas(64) 会有什么现象？
   - memory_order_relaxed / acquire / release 分别是什么意思？为什么不能全用 relaxed？
   - 为什么 SPSC 比 MPMC 快？（不需要 CAS）
4. 工程化原则：至少讲 3 条这个队列体现的通用设计思想。
5. 迁移场景：Web 服务器的请求队列、日志队列能不能用这个设计？有什么需要改的？
6. 面试话术：面试官问"你怎么做线程间通信"，30 秒回答模板。
7. 项目复盘：这个 SPSC 实现有什么可以改进的地方？比如异常安全、动态扩容？

注意：我是 C++ 并发初学者，原子操作和内存序的概念请从"为什么要用"开始讲，不要假设我懂。
中文输出。
```

### 工程化原则

#### 原则 4：缓存行感知（alignas）

**核心思想：** CPU 读写内存的最小单位是 64 字节缓存行。两个线程各自访问"不同"变量，如果落在同一个缓存行上，硬件层面会互相无效化对方缓存——**伪共享（false sharing）**。这是最难排查的性能 bug 之一。

**本项目落地：** SPSC 队列的 `head_` 和 `tail_` 各自用 `alignas(64)` 独占缓存行。Producer 只写 head，Consumer 只读 tail——硬件层面零干扰。

**可迁移场景：** 任何多线程共享结构（线程池任务队列、连接池）都需要考虑。用 `perf c2c`（Linux）检查伪共享。面试话术："多线程共享结构体，不同线程读写的字段用 cache line padding 隔开"

#### 原则 5：知道并发模式，用最弱的同步

**核心思想：** 同步机制越强，开销越大。知道只有 1 个写者和 1 个读者 → 用 SPSC + acquire/release；多个写者 → 才需要 CAS 或锁。

**本项目落地：** SPSC——head 只有 producer 写、tail 只有 consumer 写，push/pop 加起来约 2ns。MPMC（如 moodycamel::ConcurrentQueue）需要 CAS 循环，约 20-50ns——10 倍差距。

**可迁移场景：** 日志系统（多业务线程→单日志线程）用 MPSC 不是 MPMC。面试话术："选队列前先分析并发模式，SPSC 比 MPMC 快一个数量级"

#### 原则 6：优雅降级——丢 > 堵

**核心思想：** 实时系统里，丢一个数据包比阻塞整条链路危险得多。丢一个包后续继续处理；阻塞一次整条时间线被推迟，雪崩。

**本项目落地：** 队列满 → push 返回 false → 递增 drop_count_ → 不阻塞。一个 tick 丢了可接受（后续 tick 马上到）；阻塞网关线程 = 整个行情链路停滞。

**可迁移场景：** 实时监控丢采样不可阻塞采集线程；网络路由器包满了丢弃比排队好（Bufferbloat）。面试话术："实时系统丢数据比阻塞好"

#### 原则 7：编译期强制——把 bug 变成编译错误

**核心思想：** 运行时 bug 需要复现、调试、运气。编译期错误只需要看错误信息。

**本项目落地：** `static_assert((N & (N - 1)) == 0)` 强制 2 次幂容量；`static_assert(is_trivially_copyable_v<T>)` 禁止 std::string 混入队列。

**可迁移场景：** 用 constexpr/static_assert 在编译期验证配置合法性。面试话术："能用 static_assert 的约束我不写注释——注释是'期望'，static_assert 是'强制'"

---

## 第 3 层：事件系统 — 数据在模块间怎么传递

| 顺序 | 文件 | 重点 |
|------|------|------|
| 3.1 | `src/common/event.h` | Event（tagged union，承载 tick/order/trade/account/position）和 EngineCommand（控制命令） |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/common/event.h，请你当我的系统设计导师。

这个文件定义了所有线程间传递的事件类型。带我深入理解它的设计：

1. 核心思想：为什么用 tagged union（EventType + union）而不是 class 继承 + 虚函数？
2. 逐段讲解：
   - EventType 枚举有哪些值？为什么只有 6 种？
   - Event 结构体：EventType type + union 的设计。union 里各成员的 sizeof 分别是多少？Event 的总大小是多少？
   - 为什么有 static_assert(is_trivially_copyable_v<Event>)？
   - EngineCommand 是什么？为什么控制命令不和行情事件混在一起？
   - 为什么所有事件走一条 SPSC 队列，而不是每种事件各一条？
3. 对比分析：
   - 如果用继承方案（class TickEvent : public Event + virtual dispatch），热路径上多了什么开销？
   - 什么场景下 tagged union 比继承好？什么场景下反过来？
4. 工程化原则：至少讲 2 条这个设计体现的通用思想。
5. 迁移场景：如果你在写一个游戏引擎的消息系统，会不会用 tagged union？为什么？
6. 面试话术：面试官问"你为什么不用面向对象的多态"，你怎么回答？
7. 项目复盘：union 方案最大的代价是什么？如果未来要加第 7 种事件，要改多少地方？

中文输出。对虚函数表、dynamic_cast、union 等概念先简单解释。
```

### 工程化原则

#### 原则 8：做减法——Tagged Union 而不是继承

**核心思想：** 虚函数表查找和堆分配在高频路径上不可接受。Tagged union 把"类型"压缩成 enum 标签，"数据"压缩成 union——大小固定、栈上分配、memcpy 传递。

**本项目落地：** `Event` 用 `EventType type` + `union { TickData; OrderInfo; TradeInfo; ... }`。所有 event 大小 = max(union 成员) + 1 byte。零 new、零 dynamic_cast、零虚表查找。

**代价：** 浪费栈空间（取最大成员大小）。新增事件需改 union（违反开闭原则）。但事件类型有限（6 种）且变更频率低，代价完全值得。

**可迁移场景：** 状态机用 variant/tagged union 表示状态。面试话术："类型变化少、调用频率高 → tagged union 而不是 virtual dispatch"

---

## 第 4 层：基础设施 — 日志、计时、配置、加密

| 顺序 | 文件 | 重点 |
|------|------|------|
| 4.1 | `src/common/logger.h` | 同步日志接口 |
| 4.2 | `src/common/async_logger.h` | 异步日志实现 |
| 4.3 | `src/common/latency.h` | 延迟测量 |
| 4.4 | `src/common/qpc_timer.h` | 高精度定时器 |
| 4.5 | `src/common/config.h` | 配置解析 |
| 4.6 | `src/common/config_store.h` | 加密持久化存储 |
| 4.7 | `src/common/crypto.h` | 加密工具 |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/common/ 目录，重点关注 logger.h、async_logger.h、latency.h、config.h、config_store.h、crypto.h。

请你当我的基础设施设计导师，带我逐一理解：

1. 核心思想：这 7 个文件支撑了系统的可观测性和可运维性——为什么它们被放在 common/ 而不是各自模块里？

2. 逐文件讲解：
   a) logger.h + async_logger.h：
      - 同步日志 vs 异步日志的区别是什么？
      - 异步日志怎么做到热路径零开销？（队列 + 独立线程）
      - 队列满了日志丢了怎么办？为什么选择丢弃？
   b) latency.h：
      - 为什么要把延迟测量嵌在代码里，而不是事后用 profiler？
      - p50/p99/p99.9 怎么算的？环形缓冲区怎么存样本？
   c) config.h + config_store.h：
      - 为什么 config.ini 和 SQLite ConfigStore 要分开？
      - 什么东西存 INI？什么东西存 SQLite？
   d) crypto.h：
      - 凭什么不能把密码明文存 config.ini？
      - DPAPI（Windows）和 AES-256-GCM（Linux）各自怎么保护密钥？
      - 为什么不用环境变量存密码？

3. 工程化原则：异步 I/O、可观测性内建、配置代码分离——每条讲清楚"为什么重要"。
4. 迁移场景：Web 后端的日志系统、配置管理系统能不能用这些设计？
5. 面试话术：面试官问"你怎么设计生产环境的日志和配置"，30 秒回答。
6. 项目复盘：异步日志队列满了丢日志——有没有更好的方案？比如降级到 stderr？

中文输出。对 page fault、fsync、AES-GCM 等概念先简单解释。
```

### 工程化原则

#### 原则 9：异步化 I/O——不可控操作隔离到独立线程

**核心思想：** 磁盘/网络 I/O 的延迟不可控——一次 `fwrite` 可能 1μs 也可能 50ms。热路径上任何 I/O 都应该通过队列扔给独立线程。

**本项目落地：** 异步日志——热路径 push 到 SPSC → 后台线程写磁盘 → 队列满丢弃。Tick 落盘同理。异步状态保存同理。

**可迁移场景：** Web 服务器访问日志用独立线程；移动端数据库写入用后台队列。面试话术："所有 I/O 走异步队列，热路径只做无锁 push"

#### 原则 10：可观测性内建——不是出问题再加监控

**核心思想：** 延迟测量应该像心跳一样一直在跑。每个环节的 p50/p99/p99.9 存在原子变量里，无需重启就能读。

**本项目落地：** `latency.h` 测量宏嵌入各模块关键路径。每个测量点维护固定大小环形缓冲区。

**可迁移场景：** 微服务每个 API handler 内建 histogram（Prometheus 风格）。面试话术："延迟测量放在代码里不是外部工具——因为外部工具需要重编译，内置测量一直在跑"

#### 原则 11：配置代码分离 + 凭据保护

**核心思想：** 改参数不应该重新编译。凭据不应该以明文存在于文件系统。

**本项目落地：** config.ini（启动参数）+ ConfigStore SQLite + DPAPI/AES-GCM 加密。环境变量不用——会被子进程继承、被监控工具记录。

**可迁移场景：** 任何项目配置文件 + 加密凭据存储分离。CI/CD 用 CI secret 机制。面试话术："凭据加密存储即使文件泄露没有机器密钥也解不开"

---

## 第 5 层：网关抽象 — 如何接外部系统

| 顺序 | 文件 | 重点 |
|------|------|------|
| 5.1 | `src/gateway/i_md_gateway.h` | 行情网关抽象接口 |
| 5.2 | `src/gateway/i_trade_gateway.h` | 交易网关抽象接口 |
| 5.3 | `src/gateway/ctp_md_gateway.h` + `.cpp` | CTP 行情接入：回调→解码→push |
| 5.4 | `src/gateway/ctp_trade_gateway.h` + `.cpp` | CTP 交易接入 |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/gateway/ 目录，重点关注 i_md_gateway.h、i_trade_gateway.h、ctp_md_gateway.cpp、ctp_trade_gateway.cpp。

请你当我的系统集成导师，带我理解怎么安全地接外部系统：

1. 核心思想：为什么要有 IMdGateway / ITradeGateway 纯虚接口？没有接口层直接硬编码 CTP API 会出什么问题？

2. 逐段讲解：
   a) i_md_gateway.h / i_trade_gateway.h：
      - 接口定义了几个方法？每个方法的职责是什么？
      - 为什么 connect/login/subscribe 分开而不是一个 init()？
   b) ctp_md_gateway.cpp（重点）：
      - CTP SDK 的回调线程是谁创建的？你能控制它的优先级吗？
      - 回调里做了什么？为什么只做"解码 + push SPSC 队列"？
      - 回调里碰了引擎状态会出什么问题？
   c) ctp_trade_gateway.cpp：
      - 下单流程：策略发 request → 网关做什么 → CTP 返回什么？
      - OrderRef 是怎么生成的？FrontID + SessionID + OrderRef 三个字段分别什么用？

3. 工程化原则：
   - 防御式隔离：外部 SDK 不可控 → 围在薄层里，污染范围限死
   - 面向接口编程：换柜台（CTP→XTP）只加类不改引擎；测试用 Fake 实现
   - 每条原则讲清楚"不这样做会怎样"

4. 迁移场景：支付回调、消息队列消费、第三方 API 集成——这些场景能不能用同样的"薄适配层+队列隔离"模式？

5. 面试话术：面试官问"你怎么设计第三方 SDK 的集成"，30 秒回答。

6. 项目复盘：网关层除了接口抽象，还需要什么？比如重连策略、心跳、超时处理——目前够吗？

中文输出。对纯虚接口、回调线程、SPSC push 等概念结合代码上下文解释。
```

### 工程化原则

#### 原则 12：防御式隔离外部依赖

**核心思想：** 无法控制的代码（第三方 SDK、外部服务）隔离在薄层。薄层职责：外部格式→内部格式→队列→传给核心。外部行为再奇怪，污染范围限死在这一层。

**本项目落地：** CTP SDK 回调线程你控制不了。网关只做：①解码 ②push SPSC ③return。绝不碰引擎状态。

**可迁移场景：** 支付回调→验签→入队→业务处理。消息队列消费→反序列化→入队→业务逻辑。面试话术："第三方 SDK 回调我控制不了，所以隔离在薄层，只做解码和入队"

#### 原则 13：面向接口编程——对扩展开放，对修改封闭

**核心思想：** 核心逻辑依赖抽象接口不依赖具体实现。换外部系统只需新增一个实现同一接口的类，核心代码零改动。更重要的是——测试时用假实现。

**本项目落地：** `IMdGateway` / `ITradeGateway` 纯虚接口。FakeMdGateway 在内存中生成合成 tick——测试不需要 CTP 环境。

**可迁移场景：** 数据库 Repository 接口→真实现 PostgreSQL，测试用 SQLite。文件存储接口→真实现 S3，测试用本地文件。面试话术："外部依赖一定走接口，不是为了未来可能换，是为了今天能写测试"

---

## 第 6 层：交易引擎 — 系统的大脑

| 顺序 | 文件 | 重点 |
|------|------|------|
| 6.1 | `src/engine/trading_engine.h` | 引擎接口全貌，三大 SPSC 队列和所有管理器的关系 |
| 6.2 | `src/engine/trading_engine.cpp` | consumer_loop() 实现——核心消费循环 |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/engine/trading_engine.h 和 trading_engine.cpp。
请你当我的实时系统架构导师，带我读懂这个系统的"大脑"：

1. 核心思想：为什么所有引擎状态都在一个 consumer_loop() 里串行执行？这是整个项目最重要的设计决策——讲清楚"单线程拥有状态"为什么比"多线程加锁"好。

2. 逐段讲解（重点在 trading_engine.cpp 的 consumer_loop）：
   - 主循环的结构：md_queue_ drain → trade_queue_ drain → cmd_queue_ drain → 定时任务
   - 三个队列的 drain 顺序有没有讲究？为什么先 drain md_queue_ 再 trade_queue_？
   - md_batch_size_ = 512：为什么一次取 512 条而不是一条一条取？批量处理的好处是什么？
   - strategies_snapshot_ptr_（atomic<shared_ptr<const vector>>）：为什么策略列表用原子 RCU？
     添加策略和 tick 分发怎么做到无锁并发？
   - order_latency_ring_：为什么用固定 512 的环形数组而不是 map<string, timestamp>？
     环形数组比 map 好在哪？
   - ProductionHftMode：这个开关能级联关闭哪些功能？为什么要在设计时就留好开关？

3. 工程化原则：
   - 单线程拥有状态（原则 14）
   - 批量处理均摊开销（原则 15）
   - 原子 RCU 读写分离（原则 16）
   - 固定大小环形数组替代动态容器（原则 17）
   每条讲清楚：是什么 → 为什么 → 不用会怎样。

4. 迁移场景：
   - 游戏服务器的主循环（每个房间一个逻辑线程）能不能用这个模式？
   - GUI 应用的主线程消息循环和 consumer_loop 有什么异同？

5. 面试话术："为什么你的引擎是单线程的？"——30 秒回答模板。

6. 项目复盘：consumer_loop 里 drain 顺序是否合理？如果 trade_queue 积压严重会不会饿死 cmd_queue？
   如果让你重做，你会怎么设计优先级调度？

中文输出。对原子 RCU、shared_ptr 的线程安全性、环形缓冲区等概念结合代码解释。
```

### 工程化原则

#### 原则 14：单线程拥有状态——用架构消除并发问题

**核心思想：** 更好的方案不是"用更快的锁"，而是从架构层面让并发问题不存在。所有可变状态只被一个线程拥有 = 不需要锁。

**本项目落地：** `consumer_loop()` 是系统唯一"写入者"。所有订单、持仓、风控、策略状态变更在同一线程串行执行。引擎内部零 mutex、零 atomic（除跨线程通知标志位）。

**可迁移场景：** 游戏服务器每房间一个逻辑线程；GUI 所有 UI 状态在主线程。面试话术："首选不是更好的锁，而是设计成不需要锁"

#### 原则 15：批量处理——均摊开销

**核心思想：** 一次取 512 条处理，512 次处理均摊一次原子操作开销。连续处理相似操作让 CPU 分支预测器和指令预取器保持热身。

**本项目落地：** `md_batch_size_` = 512，在 i5-12490F 上实测的甜点位。

**可迁移场景：** 网络服务器批量 recv/send；数据库批量 INSERT。面试话术："批量处理均摊固定开销，具体数量需要实测"

#### 原则 16：原子 RCU——读写分离零锁

**核心思想：** 读远多于写时用 RCU：写者建新副本→修改→原子替换；读者拿不可变快照——零锁。

**本项目落地：** `strategies_snapshot_ptr_`（策略列表）、PositionManager（持仓数据）都用 `atomic<shared_ptr<const T>>`。

**可迁移场景：** 配置热更新、路由表。面试话术："读多写少的数据用原子 RCU，写者 copy 后原子替换，读者零锁读"

#### 原则 17：固定环形数组替代动态容器

**核心思想：** 固定大小环形数组，内存开销在编译期确定，全部在 L1 缓存内。O(n) 扫描 n=512 且全在缓存里，比 hash map 的 O(1) + 可能的 cache miss 更快。

**本项目落地：** `order_latency_ring_` 跟踪下单→成交延迟，512 × 24B ≈ 12KB，全在 L1 缓存。

**可迁移场景：** HTTP 服务器的 request tracing ring buffer。面试话术："高频追踪用固定环形数组——虽然查找是 O(n)，但 n 小且全在缓存"

---

## 第 7 层：订单管理 — 状态机的核心

| 顺序 | 文件 | 重点 |
|------|------|------|
| 7.1 | `src/order/order_manager.h` | 订单状态机接口、enrich 逻辑、成交去重 |
| 7.2 | `src/order/order_manager.cpp` | 状态变迁实现，Terminal State 保护 |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/order/order_manager.h 和 order_manager.cpp。
请你当我的状态机设计导师，带我理解订单从创建到成交的完整生命周期：

1. 核心思想：订单管理的本质是一个状态机——Pending→PartTraded→AllTraded/Cancelled/Error。
   为什么需要终态保护？为什么需要成交去重？

2. 逐段讲解：
   a) allocate_order_ref()：
      - order_ref 是怎么生成的？为什么拼 FrontID + SessionID + 递增序号？
      - 引擎重启后 order_ref 计数器从哪恢复？
   b) create_order()：
      - 创建订单时做了什么？状态初始是什么？
      - 为什么创建后立即加入 orders_ 字典？
   c) on_order_return()（重点）：
      - CTP 的回调可能乱序——trade 先到、order 后到。怎么处理？
      - 终态保护：为什么已经 AllTraded 的订单不能再改成 Pending？
      - enrich_order_info() 做了什么？为什么回报里没有策略 ID？
   d) on_trade_return()：
      - 成交去重：seen_trade_ids_ 是什么？为什么用 unordered_set<FixedKey<24>>？
      - 一笔成交到达后，持仓、盈亏、订单状态分别怎么更新？
   e) get_pending_open_volume()：风控为什么需要这个数字？

3. 工程化原则：防御式隔离（乱序/重放/字段缺失都处理）。至少讲 2 条。

4. 迁移场景：
   - 支付系统的订单状态机（待支付→已支付→已退款）能不能借鉴终态保护？
   - 消息队列的消费者怎么去重？和成交去重的思路一样吗？

5. 面试话术：面试官问"你怎么处理外部回调的乱序和重放"，30 秒回答。

6. 项目复盘：seen_trade_ids_ 无限增长怎么办？是否需要 LRU 淘汰？
   enrich 逻辑在字典中查不到订单时应该怎么处理？

中文输出。对状态机、幂等去重等概念结合代码解释。
```

### 工程化原则

#### 原则 18：不信任外部输入——乱序、重放、缺失都要处理

**核心思想：** 外部回调不受你控制。可能乱序（trade 先于 order）、可能重放（同一笔成交推两次）、可能缺失字段（回报里无策略 ID）。不能假设"回调总是按理想顺序、携带完整信息"。

**本项目落地：** 成交去重 `seen_trade_ids_`；终态保护——进入终态后不再接受状态变更；enrich 逻辑——从本地字典回填缺失字段。

**可迁移场景：** 支付回调幂等设计；消息队列去重 + 乱序处理。面试话术："外部回调按最坏情况设计——去重、终态保护、字段回填"

---

## 第 8 层：条件单 + 算法单 — 策略的延伸

| 顺序 | 文件 | 重点 |
|------|------|------|
| 8.1 | `src/order/conditional_order_manager.h` + `.cpp` | 止损/止盈/追踪止损/触发开仓，OCO 互斥组，TriggerBounds 优化 |
| 8.2 | `src/order/algo_order_manager.h` + `.cpp` | Iceberg 冰山单，TWAP 算法单 |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/order/conditional_order_manager.h 和 conditional_order_manager.cpp。
请你当我的算法设计导师，带我理解条件单系统：

1. 核心思想：条件单的本质是"价格触发→自动发单"。为什么在 C++ 引擎层做触发判断，
   而不是在 Python 策略里自己写 if 判断？

2. 逐段讲解（重点）：
   a) TriggerBounds 预计算：
      - 每个合约的 trigger_bounds_（min/max 区间）是什么意思？
      - may_trigger_locked(price) 为什么能在 O(1) 过滤掉 99% 的 tick？
      - bounds 什么时候重新计算？dirty flag 怎么用？
   b) check_tick() 的三阶段（这是核心精华）：
      - Phase 1（locked filter）：持锁扫描触发单，构建触发列表
      - Phase 2（unlocked callback）：释放锁，调用 send_order
      - Phase 3（locked commit）：重新持锁，更新状态
      - 为什么中间要解锁？如果一直持锁调用 send_order 会怎样？
   c) OCO 互斥组：
      - allocate_group_id() 怎么分配 group？
      - cancel_group() 在一个触发后怎么取消同组其他单？
      - OCO 的竞态条件怎么处理？
   d) 失败重试：
      - 条件单触发后发单被拒，什么时候 RetryLater？什么时候直接 Cancelled？
      - retry_failures_ 累加到多少停止重试？

3. 工程化原则：
   - 用预计算换遍历（TriggerBounds）
   - 3-phase lock 防止 callback 内死锁
   每条讲清楚"为什么这样做"

4. 迁移场景：
   - 电商的"价格监控+自动下单"能不能用 TriggerBounds 的思路？
   - 事件系统的 dispatch 能不能用 3-phase lock？

5. 面试话术：面试官问"条件单的触发延迟为什么能做到 0.1μs"，30 秒回答。

6. 项目复盘：TriggerBounds 在极端行情（开盘跳空）下是否可能漏触发？
   3-phase lock 在 Phase 2 期间如果条件单被另一个线程删除，会怎么处理？

中文输出。对 OCO、3-phase lock 等概念结合代码上下文解释。
```

### 工程化原则

#### 原则 19：用预计算换遍历——在变更时计算，不在查询时遍历

**核心思想：** 查询远比变更频繁时，维护一个缓存。每次变更更新缓存，每次查询 O(1)。

**本项目落地：** `TriggerBounds`——每合约维护 min/max 触发区间。99% tick 在 O(1) 直接返回 false。条件单增删/触发后 dirty flag 触发重算。

**可迁移场景：** 订单簿维护 min_ask/max_bid；规则引擎维护触发热点索引。面试话术："高频查询的过滤条件预计算成 interval，查询 O(1)，只在变更时 O(n) 重算"

#### 原则 20：3-phase lock 模式——防止 callback 内死锁

**核心思想：** 持锁状态调用外部 callback，callback 可能重入你的模块 → 必须分阶段加锁。

**本项目落地：** locked filter → unlocked callback（send_order）→ locked commit。中间解锁防止 send_order 回调进入 ConditionalOrderManager 导致死锁。

**可迁移场景：** 事件系统 dispatch 时放锁；Observer 模式通知在锁外。面试话术："callback 可能重入我的模块，callback 前释放锁、callback 后重新获取"

---

## 第 9 层：风控 RMS — 7 维防线

| 顺序 | 文件 | 重点 |
|------|------|------|
| 9.1 | `src/risk/risk_manager.h` | 检查维度、RMS 模式、滑动窗口、is_risk_reduction 豁免机制 |
| 9.2 | `src/risk/risk_manager.cpp` | 实现 |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/risk/risk_manager.h 和 risk_manager.cpp。
请你当我的风控系统设计导师，带我理解 7 维风控的设计：

1. 核心思想：风控是引擎的最后一道防线。7 个维度的检查顺序不是随意的——
   按成本从低到高排列，前 3 个检查拦截 90%+ 的拒单。为什么这样设计？

2. 逐段讲解：
   a) check_order() 的 7 维瀑布流：
      1. RMS 模式（1 次比较，最便宜）
      2. 单笔数量上限（1 次比较）
      3. 交易时段（1 次查找）
      4. 报单频率（滑动窗口 prune + count，中等成本）
      5. 撤单率（类似）
      6. 净持仓（查持仓 + 投影计算，较贵）
      7. 日内亏损（查账户 + 浮动盈亏，最贵）
      每一维被拦截后是什么效果？为什么这个顺序不能换？
   b) is_risk_reduction 豁免机制：
      - 什么操作算"降低风险"？
      - 为什么平仓止损单可以绕过部分限制？
      - 如果风控拦住了止损单，后果是什么？
   c) RMS 递进模式：
      - Normal→NoOpen→ReduceOnly→Liquidating→Halted 的触发条件分别是什么？
      - 为什么升级是自动的、降级是手动的？
      - 如果自动降级，在闪崩场景下会发生什么？
   d) 滑动窗口流控：
      - 为什么用 deque<TimePoint> 而不是一个计数器？
      - prune_order_rate_window_locked() 怎么清理过期时间戳？
   e) 撤单率限制：
      - 为什么需要 cancel_rate_min_sample？5 单撤 4 单该不该拦？
   f) shared_mutex：
      - check_order（写）拿独占锁，get_snapshot（SSE 推送）拿共享锁
      - 如果换成普通 mutex，前端每 500ms 查询一次会有什么影响？

3. 工程化原则：至少讲 4 条。
4. 迁移场景：API 网关限流；服务熔断。能不能借鉴这个风控设计？
5. 面试话术：面试官问"你怎么设计风控/限流系统"，30 秒回答。
6. 项目复盘：deque 滑动窗口在高频下有什么问题？有没有更高效的数据结构？

中文输出。对滑动窗口、deque、shared_mutex 等概念结合代码解释。
```

### 工程化原则

#### 原则 21：按成本排序检查——瀑布流拦截

**核心思想：** 多个校验规则按成本从低到高排列。便宜的（整数比较）放前面，贵的（查数据库/算浮动盈亏）放后面。前面拦截了后面不用算。

**本项目落地：** 风控 7 维按计算成本严格排序。前 3 个拦截 90%+ 拒单但只消耗几次整数比较。

**可迁移场景：** API 网关先 rate limit→再 auth→再参数检查→最后查数据库。表单验证先非空→再格式→再业务规则。面试话术："校验按成本排序，被前面拦截了后面就不用算"

#### 原则 22：不能阻止自救——is_risk_reduction 豁免

**核心思想：** 风控目的是控制风险。如果阻止了降低风险的操作，风控本身就是风险源。

**本项目落地：** `is_risk_reduction = true` 绕过频率/时段/亏损限制。亏损超标拒绝开仓（正确），但必须允许平仓。

**可迁移场景：** 熔断后拒绝"增加负载"但允许"降低负载"操作（取消排队任务）。限流后拒绝新请求但允许 health check。面试话术："风控必须区分增加风险和降低风险的操作"

#### 原则 23：升级自动，降级手动——防止振荡

**核心思想：** 升级自动（延迟造成更大损失），降级手动（防止自动恢复→再触发→恢复→再触发的振荡）。

**本项目落地：** RMS 递进自动，降级必须运维手动执行命令。

**可迁移场景：** 服务熔断自动打开断路器手动关闭。自动扩容手动缩容。面试话术："升级自动降级手动——防止临界条件反复振荡"

#### 原则 24：读写锁用于可观测性

**核心思想：** 业务写和监控读不应该互相阻塞。shared_mutex 允许多读者并发。

**本项目落地：** 风控 check_order（独占锁），get_snapshot（共享锁）。普通 mutex 会导致前端查询阻塞发单。

**可迁移场景：** 缓存读共享锁更新独占锁；配置管理读共享热更新独占。面试话术："监控查询不能阻塞业务流程——读写锁分离"

---

## 第 10 层：持仓管理 — 状态的一致性

| 顺序 | 文件 | 重点 |
|------|------|------|
| 10.1 | `src/position/position_manager.h` | 原子 RCU 快照、多账户、今昨仓区分 |
| 10.2 | `src/position/position_manager.cpp` | 更新逻辑 |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/position/position_manager.h 和 position_manager.cpp。
请你当我的数据一致性设计导师，带我理解持仓管理系统：

1. 核心思想：持仓数据"读频繁、写稀少"（每 tick 读、偶尔成交才写），
   所以用原子 RCU（atomic<shared_ptr<const PositionSet>>）实现读写分离零锁。
   这和前面策略列表的 RCU 是同一个模式——讲清楚"为什么不用 mutex"。

2. 逐段讲解：
   a) PositionSet 结构：为什么是 immutable？写者怎么"修改"一个不可变对象？
   b) get_position() 读端：
      - atomic_load 拿到 shared_ptr<const PositionSet>
      - 为什么这个读操作是零锁的？
      - 多个读者同时读会不会互相阻塞？
   c) update_position() 写端：
      - 写者怎么构建新 PositionSet？是 copy 还是 move？
      - atomic_store 替换旧指针后，旧快照什么时候被回收？
      - 为什么写操作只在消费线程（on_trade）里发生？
   d) 今昨仓区分：
      - 为什么区分 today_volume 和 yesterday_volume？
      - need_close_today_flag() 里的交易所白名单（SHFE/INE）为什么精确到交易所？
      - 大商所/郑商所平今免费、上期所/能源中心平今加收——这些业务规则怎么映射到代码里？

3. 工程化原则：原子 RCU 零锁读；面向业务规则设计。
4. 迁移场景：交易所的手续费差异化规则→电商的不同国家税率规则，设计模式一样吗？
5. 面试话术：面试官问"持仓系统怎么保证并发安全"，30 秒回答。
6. 项目复盘：RCU 模式每次写都要 copy 整个 PositionSet——如果持仓合约数非常大（比如 1000 个），这个 copy 开销还能接受吗？有什么优化思路？

中文输出。对 RCU、shared_ptr 原子操作、copy-on-write 等概念结合代码解释。
```

### 工程化原则

#### 原则 25：面向业务规则设计——代码直接映射现实约束

**核心思想：** 领域模型中的字段和逻辑应该直接反映业务规则，不是"通用设计"或"为了方便"。简化建模 = 边缘 case 出错。

**本项目落地：** 今昨仓区分映射中国期货手续费差异化规则。`need_close_today_flag()` 精确到交易所级别白名单。

**可迁移场景：** 电商不同国家税率映射订单模型；金融不同交易所结算规则映射清算系统。面试话术："领域模型字段直接回答业务问题"

---

## 第 11 层：策略引擎 — 用户接口

| 顺序 | 文件 | 重点 |
|------|------|------|
| 11.1 | `src/strategy/strategy_base.h` | 策略基类接口 |
| 11.2 | `src/strategy/simple_strategy.h` + `.cpp` | 原生 C++ 策略示例 |
| 11.3 | `src/strategy/py_strategy.h` + `.cpp` | Python 策略桥接（pybind11），热加载 |
| 11.4 | `strategies/example_strategy.py` | Python 策略示例 |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/strategy/ 目录，重点关注 strategy_base.h、py_strategy.h、py_strategy.cpp。
请你当我的 API 设计和嵌入式脚本导师：

1. 核心思想：策略层是"用户接口"——引擎是系统代码，策略是用户代码。
   接口设计的原则是"暴露越少，误用越少"。另一方面，pybind11 让 Python 能零拷贝访问 C++ struct。

2. 逐段讲解：
   a) strategy_base.h：
      - on_tick 是纯虚（必须实现），on_order/on_trade 有默认空实现——为什么？
      - 为什么所有回调跑在消费线程？用户能不能在里面 sleep？
      - set_engine() 注入 ITradingContext 指针——这是什么模式？（依赖注入）
      - send_order/cancel_order/add_conditional_order 为什么是 protected？
      - handles_event/handles_instrument/handles_account 三个路由函数怎么协作？
   b) py_strategy.cpp（重点）：
      - pybind11 怎么把 C++ TickData 传给 Python？有拷贝吗？
      - tick.last_price 在 Python 里直接读 C++ 的 double 字段——怎么做到的？
      - GIL（全局解释器锁）怎么处理的？多个 Python 策略同时 on_tick 怎么不互相阻塞？
      - is_interpreted() → GIL batch：为什么所有 Python 策略共享一次 GIL 获取？
   c) 热加载：
      - 检测 .py 文件变化 → on_destroy() → 重新 import → on_init()
      - 引擎其他功能（其他策略、行情接收）会受影响吗？
      - 旧策略有挂单时，卸载会发生什么？

3. 工程化原则：最小接口（原则 27）；做减法（原则 26）；热加载不重启（原则 28）。
4. 迁移场景：插件系统、游戏 mod 系统、微服务蓝绿部署能不能借鉴热加载思路？
5. 面试话术：面试官问"Python 策略和 C++ 引擎怎么通信"，30 秒回答。
6. 项目复盘：不做 Python 沙箱是故意的——什么场景下这个决定是错误的？
   GIL batch 在策略数量非常多时还有效吗？

中文输出。对 pybind11、GIL、热加载等概念先简单解释再深入。
```

### 工程化原则

#### 原则 26：做减法——有意识的不做

**核心思想：** 每增加一个功能，bug 表面积和测试复杂度在增长。设计决策包括"刻意没做什么"。

**本项目落地：** 不做 Python 沙箱（信任策略作者）；不做回测引擎（和实盘是不同的复杂度维度）；不做 GUI（CLI+日志让部署变成一个文件）。

**可迁移场景：** 任何项目 README 或设计文档明确"不做"清单防止范围蔓延。面试话术："不做比做更需要判断力——加沙箱只增加代码没有安全收益"

#### 原则 27：最小接口——暴露越少，误用越少

**核心思想：** 给用户的 API 只包含必要方法。每个多出的方法都是潜在的误用场景。

**本项目落地：** `StrategyBase` 只有 5 个回调方法，on_tick 纯虚、其余有默认空实现。策略开发者不需要理解 SPSC 队列、不知道引擎有几个线程。

**可迁移场景：** 框架给插件暴露 5 个 hook 而不是 20 个。SDK 公开 API 方法数控制在个位数。面试话术："暴露越少，误解越少，bug 越少"

#### 原则 28：热加载不重启——用户代码独立部署

**核心思想：** 用户代码和系统代码生命周期解耦。用户改策略不重启引擎、不断其他策略。

**本项目落地：** 文件变化检测→on_destroy()→重新加载→on_init()。期间引擎其他部分不受影响。

**可迁移场景：** 插件系统热加载；微服务蓝绿部署。面试话术："策略挂了不影响引擎，改策略不需要停系统"

---

## 第 12 层：辅助模块 + 应用入口

| 顺序 | 文件 | 重点 |
|------|------|------|
| 12.1 | `src/engine/account_manager.h` + `.cpp` | 多账户管理 |
| 12.2 | `src/engine/session_manager.h` + `.cpp` | 交易时段管理 |
| 12.3 | `src/engine/kline_manager.h` + `.cpp` | K 线聚合 |
| 12.4 | `src/engine/tick_recorder.h` + `.cpp` | Tick 落盘 |
| 12.5 | `src/engine/paper_trading.h` + `.cpp` | 模拟交易 |
| 12.6 | `src/app/app_runtime.h` + `.cpp` | 应用生命周期管理 |
| 12.7 | `src/main.cpp` | 入口点 |
| 12.8 | `CMakeLists.txt` | 构建配置 |

### 📋 GPT 提示词

```
现在打开 hft_framework/src/app/app_runtime.cpp、src/main.cpp、CMakeLists.txt。
请你当我的应用架构导师，带我理解怎么把 12 个模块组装成一个可运行的系统：

1. 核心思想：AppRuntime 是组装层——读配置→创建引擎→根据配置决定加载哪些模块→启动。
   main.cpp 只管解析命令行参数。如果未来要把引擎嵌到 GUI 或 Docker，只需改 AppRuntime。

2. 逐段讲解：
   a) app_runtime.cpp：
      - 初始化顺序有没有讲究？为什么先日志、再配置、再引擎、最后策略？
      - 多账户怎么处理的？一个 MD 网关 + N 个交易网关的模型？
      - 启动后怎么进入主循环？interactive 模式和 service 模式的区别？
   b) main.cpp：
      - 命令行参数有哪些？--config、--interactive 各自做什么？
      - CPU 绑核和优先级设置在这里做——为什么不在引擎内部做？
   c) CMakeLists.txt：
      - 三个 target（hft_framework, hft_tests, hft_bench）的依赖关系？
      - CTP_SDK_DIR 为什么是必传参数？不放在仓库里？
      - HFT_ENABLE_QDP 是可选的编译开关——怎么实现的？

3. 工程化原则：热路径可选关闭（ProductionHftMode 级联开关）。
4. 迁移场景：微服务的启动组装层；Docker 容器化改造怎么改 AppRuntime？
5. 面试话术：面试官问"这个项目怎么部署"，30 秒回答。

中文输出。
```

### 工程化原则

#### 原则 29：热路径可选关闭——开发时全开，生产时裁剪

**核心思想：** 开发效率和生产性能往往矛盾。设计时留好开关，同一二进制在开发和生产间切换。

**本项目落地：** `ProductionHftMode` 级联禁用 Python 热路径、tick 录制、K 线聚合。默认全开，生产关掉→免费性能提升。

**可迁移场景：** 任何系统的 debug/release 模式不仅是编译级别还包括功能开关。面试话术："功能开关不是事后优化——设计时就考虑什么场景下是纯开销"

---

## 第 13 层：测试 — 理解预期的行为

| 顺序 | 文件 | 验证什么 |
|------|------|----------|
| 13.1 | `tests/test_spsc_queue.cpp` | SPSC 队列正确性和并发行为 |
| 13.2 | `tests/test_order_manager.cpp` | 订单状态机 |
| 13.3 | `tests/test_position_manager.cpp` | 持仓更新 |
| 13.4 | `tests/test_conditional_order.cpp` | 条件单触发、OCO |
| 13.5 | `tests/test_risk_manager.cpp` | 风控拦截 |
| 13.6 | `tests/test_rms.cpp` | RMS 模式递进 |
| 13.7 | `tests/test_integration.cpp` | e2e 集成 |
| 13.8 | `tests/test_stress.cpp` | 压力测试 |
| 13.9 | `tests/fakes/fake_md_gateway.h` | Mock 行情网关 |
| 13.10 | `tests/fakes/fake_trade_gateway.h` | Mock 交易网关 |

### 📋 GPT 提示词

```
现在打开 hft_framework/tests/ 目录。请你当我的测试设计导师，重点看两点：
1）fakes/fake_md_gateway.h 和 fake_trade_gateway.h——Mock 是怎么设计的？
2）test_integration.cpp——集成测试怎么把各模块串起来的？

1. 核心思想：可测试性是设计目标。因为有了网关接口抽象，Fake 实现才能存在——
   不需要 CTP SDK、不需要网络、不需要交易日，在内存里跑完所有测试。

2. 逐段讲解：
   a) fake_md_gateway.h：
      - FakeMdGateway 怎么实现 IMdGateway 接口？
      - 怎么在测试里模拟"推送一个 tick"？
      - 怎么模拟"行情乱序到达"？
   b) fake_trade_gateway.h：
      - 怎么模拟"下单成功/失败"？
      - 怎么模拟"成交回报"？
      - 怎么模拟"回调重放"（同一笔成交推两次）？
   c) test_integration.cpp：
      - 集成测试覆盖了哪些场景？
      - tick→策略→发单→成交→持仓更新的完整链路怎么测？
      - 不连 simnow 怎么验证整个流程是对的？

3. 工程化原则：可测试性是设计目标（原则 30）。
4. 迁移场景：你的 Web 后端单元测试，数据库和外部 API 怎么 mock？
5. 面试话术：面试官问"这个项目怎么测试"，30 秒回答。

中文输出。
```

### 工程化原则

#### 原则 30：可测试性是设计目标，不是事后补救

**核心思想：** 模块很难测试→问题不在测试代码→模块边界画错了。好设计让每个模块只需 mock 1-2 个外部依赖。

**本项目落地：** Fake 系列不依赖 CTP SDK。所有测试场景在内存中完成，单次 <100ms。测试验证的不止逻辑正确性，还有"模块边界对不对"。

**可迁移场景：** 先写核心模块的测试，写起来痛苦→说明模块需要重构。面试话术："可测试性是我评价设计好坏的第一标准——设计时就问这个模块怎么测"

---

## 第 14 层：Benchmark — 性能可度量

| 顺序 | 文件 | 重点 |
|------|------|------|
| 14.1 | `bench/main.cpp` | benchmark 入口 |
| 14.2 | `bench/bench_scenarios.cpp` | 各场景的测试用例 |
| 14.3 | `bench/bench_runner.h` + `.cpp` | benchmark 框架 |

### 📋 GPT 提示词

```
现在打开 hft_framework/bench/ 目录。请你当我的性能工程导师：

1. 核心思想：Benchmark 和单元测试的区别——单元测试验证"对不对"，benchmark 验证"多快"。
   为什么只测微观操作不测端到端？

2. 逐段讲解：
   a) bench_runner.h/.cpp：
      - 测试框架怎么设计的？warmup→runs→统计分位数
      - 为什么先 warmup？
      - p50/p95/p99/p99.9 怎么算？为什么 p99 比 avg 重要？
   b) bench_scenarios.cpp：
      - 每个 scenario 测什么？怎么保证不被编译器优化掉？（benchmark 的 DoNotOptimize）
      - cond_order_check_tick 怎么测的？构造了多少条件单？
      - spsc_queue_throughput 怎么测的？5.72×10⁸ ops/sec 怎么得出来的？

3. 工程化原则：微观 benchmark 优于端到端（可归因）。
4. 迁移场景：Web 后端的 handler benchmark。
5. 面试话术："这些数字怎么测出来的？"——描述测试环境和流程。

中文输出。对 warmup、分位数、DoNotOptimize 等概念解释。
```

### 工程化原则

#### 原则 31：微观 benchmark 优于端到端——可归因

**核心思想：** 端到端受外部因素影响太大（网络抖动、OS 调度），测出来没可比性。微观 benchmark 测纯代码路径——能精确对应到某几行代码，优化时能归因。

**本项目落地：** Benchmark 只测微观操作，每个独立跑 10000 次，输出完整分布。

**可迁移场景：** 性能优化先 benchmark 微观操作找瓶颈再优化——别猜。面试话术："微观能精确归因到代码行，端到端能验证整体效果。不能只看端到端"

---

## 项目全局复盘（GPT 提示词）

读完所有模块后，用这个提示词做一个完整的项目复盘：

```
我已经读完了 hft_framework 的整个代码库（14 层，73 个源文件，约 89,000 行代码）。

请你帮我做一次全面的项目复盘：

1. 架构回顾：
   - 单消费者线程模型是这个项目最核心的设计决策。站在现在的视角，这个决策是否仍然正确？
   - 如果引擎状态需要跨多个 CPU 核心并行处理（比如策略数量增长到几百个），架构需要怎么演进？

2. 模块评价：
   - 哪些模块设计得最好？为什么？
   - 哪些模块设计过度？哪些设计不足？
   - 如果要砍掉一个模块，砍哪个？

3. 性能评估：
   - 99% 的热路径延迟已经 <1μs。下一步性能瓶颈在哪？（网络？CTP SDK？Python GIL？）
   - 如果要做到 p99.9 < 1μs，成本是什么？

4. 面试准备：
   - 作为面试官，看到这个项目简历，你最想深入追问的三个问题是什么？
   - 分别给出参考答案。

5. 成长路径：
   - 如果我要在交易系统方向继续深入，下一步应该学什么？
   - 这个项目的哪些设计思想可以直接复用到下一个项目？

中文输出。诚实评价，不要因为这是我的项目就说好话。
```

---

## 阅读进度跟踪

```
[ ] 第 0 层：全局概念（2 files）
[ ] 第 1 层：类型系统（1 file）
[ ] 第 2 层：SPSC 队列（1 file）
[ ] 第 3 层：事件系统（1 file）
[ ] 第 4 层：基础设施（7 files）
[ ] 第 5 层：网关抽象（4 files）
[ ] 第 6 层：交易引擎（2 files）
[ ] 第 7 层：订单管理（2 files）
[ ] 第 8 层：条件单 + 算法单（2 files）
[ ] 第 9 层：风控 RMS（2 files）
[ ] 第 10 层：持仓管理（2 files）
[ ] 第 11 层：策略引擎（4 files）
[ ] 第 12 层：辅助模块 + 应用入口（8 files）
[ ] 第 13 层：测试（10 files）
[ ] 第 14 层：Benchmark（3 files）
```

---

## 附录：工程化思维工具箱（面试速查）

以下 12 条原则可脱离本项目，在任何系统设计面试中直接使用。每条记住三要素：**是什么 → 为什么 → 举个通用例子**。

### 1. 热路径零分配

高频调用的代码路径上，禁止堆分配和系统调用。因为 `malloc` 延迟不可预测（20ns-2000ns），碎片化随时间恶化。

通用做法：POD struct + 栈分配 + 对象池 + compile-time assert。

### 2. 单线程拥有状态

可变状态只让一个线程写，从根源消灭锁和数据竞争。比"用更快的锁"好一个数量级。

通用做法：主线程持有状态 + 工作线程只做异步 I/O + 线程间通过队列通信。

### 3. 防御式隔离

不信任外部输入。外部回调可能乱序、重放、缺失字段——对每种异常情况都有显式处理。

通用做法：薄适配层 + 去重 + 终态保护 + 缺失字段回填。

### 4. 优雅降级

满的时候丢弃比阻塞好。实时系统里丢一个数据包可恢复，阻塞一次整个时间线错乱。

通用做法：队列满→返回 false→递增 drop_count→不阻塞。

### 5. 缓存行感知

两个线程各自写"不同"变量，若同一条 cache line→互相无效化缓存（伪共享）。`alignas(64)` 隔离。

通用做法：多线程共享结构体，不同线程读写的字段用 padding 隔到不同 cache line。

### 6. 栈优于堆

栈分配 = 移动栈指针（1 条 CPU 指令）。堆分配 = 找空闲块（可能涉及系统调用）。小对象+短生命周期=栈。

通用做法：FixedKey<N> 替代 std::string，固定大小数组替代 vector（当大小确定且已知小时）。

### 7. 编译期强制

能编译期检查的绝不放到运行时。`static_assert` > 注释 > 文档。

通用做法：static_assert 约束类型、大小、容量。enum class 穷尽 switch-case。

### 8. 职责边界

每个模块只做一件事，通过最小接口通信。边界画对，测试只需 mock 1-2 个依赖。

通用做法：接口抽象 + 依赖注入 + 单一职责。

### 9. 可观测性内建

不是出问题才挂 profiler——运行时一直在记录。每个关键环节的延迟分布随时可查。

通用做法：内嵌 histogram + atomic 暴露 + 固定大小环形缓冲区存储样本。

### 10. 配置代码分离 + 凭据保护

变参数不重编译，凭证不落明文。加密存储即使文件泄露，没有机器密钥也解不开。

通用做法：INI/YAML 管启动参数 + SQLite+加密管运行时数据和凭据。凭据不用环境变量。

### 11. 做减法

不做的功能和做的功能同样是设计决策。每增加一个功能，bug 表面积和测试复杂度都在增长。

通用做法：设计文档明确"不做"清单及理由。防止范围蔓延。

### 12. 可测试性是设计目标

设计模块时先问：这个模块怎么测？如果很难测，说明边界不对——重构模块，不是补充测试。

通用做法：外部依赖走接口→Mock 替代真实现→不依赖网络/数据库/第三方 SDK 就能跑测试。
