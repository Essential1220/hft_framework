# HFT Framework 优化建议

> 以"用户视角"审视整个框架，从可靠性、可维护性、性能、安全合规四个维度提出改进点。
> 框架整体质量很高（代码 8.5/10，延迟 10/10），以下是仍可打磨的地方。

---

## 一、可靠性 / 错误处理

### 1.1 `seen_trade_ids_` 无界增长（高优先）

**位置：** `src/order/order_manager.cpp:335`

当前逻辑在集合 >100,000 时一次性 `clear()`，存在两个问题：
- 长时间运行（数周不重启）期间，如果成交频率低，集合会缓慢膨胀却不触发清理。
- `clear()` 瞬间释放大量内存，可能引起一次性 GC / page-fault 抖动。

**建议：** 改为基于时间的过期策略（例如每 2 小时清理一次），或使用 LRU 淘汰。

```cpp
struct TradeDedup {
    std::unordered_set<TradeIdKey> recent;
    steady_clock::time_point last_clear = steady_clock::now();
    
    bool try_insert(const TradeIdKey& id) {
        auto now = steady_clock::now();
        if (now - last_clear > std::chrono::hours(2)) {
            recent.clear();
            last_clear = now;
        }
        return recent.insert(id).second;
    }
};
```

### 1.2 Gateway 查询费率失败后静默继续（高优先）

**位置：** `src/gateway/ctp_trade_gateway.cpp:276-299`

`query_margin_rate` / `query_commission_rate` 失败时只打 WARN 日志，调用方无法感知，策略可能使用过期费率下单。

**建议：** 返回错误码，让 AccountManager 将该账户标记为 "未就绪"，阻止下单直到费率查询成功。

### 1.3 RiskManager::init() 缺少空指针检查（高优先）

**位置：** `src/risk/risk_manager.cpp:62`

`pos_mgr_` / `order_mgr_` 传入 nullptr 时直接解引用崩溃。虽然当前 TradingEngine 保证不会传空，但属于防御性编程缺失。

**建议：**
```cpp
if (!pos_mgr || !order_mgr) 
    throw std::invalid_argument("RiskManager::init: null manager pointer");
```

### 1.4 策略热加载异常安全（高优先）

**位置：** `src/engine/trading_engine.cpp:993-996`

`on_init()` 抛异常后，策略仍被加入列表，后续 `on_tick()` 会派发到未初始化的策略。

**建议：** 异常时回滚——从策略列表移除，并记录错误。

### 1.5 Trade Queue 满时丢弃事件

**位置：** `src/engine/trading_engine.cpp:1149, 1173, 1213`

SPSC 队列满时日志 ERROR 但直接丢弃。成交回报丢失可能导致持仓不一致。

**建议：** 考虑在 trade_queue_ 满时触发 RMS 升级（至少 Warning 级别），并在恢复后主动查询一次持仓做对账。

### 1.6 Webhook 告警无重试

当前 webhook POST 是 fire-and-forget，网络抖动导致告警丢失无感知。

**建议：** 加一个简单的 3 次指数退避重试（间隔 1s/2s/4s），失败后写本地 fallback 日志。

---

## 二、可维护性 / 代码组织

### 2.1 TradingEngine 单文件 4188 行（高优先）

**位置：** `src/engine/trading_engine.cpp`

这是全框架最大的可维护性瓶颈。包含消费者循环、行情处理、委托处理、策略派发、状态机等所有逻辑。

**建议拆分为：**
| 文件 | 职责 |
|------|------|
| `trading_engine_core.cpp` | consumer_loop、启停生命周期 |
| `trading_engine_market.cpp` | process_tick、K线聚合 |
| `trading_engine_order.cpp` | send_order、cancel_order、on_order、on_trade |
| `trading_engine_strategy.cpp` | 策略加载/卸载/热重载/派发 |

保持同一个 class，只拆编译单元，不影响架构。

### 2.2 Config 解析过于宽松

配置缺少必要字段时使用默认值，不打任何警告。新用户漏配关键参数时很难排查。

**建议：** 对关键配置项（BrokerID、UserID、FrontAddr、InstrumentID 列表）缺失时直接报错退出，其余可选项打 WARN。

### 2.3 缺少版本号管理

README 里硬编码版本，CMake 和运行时无版本信息。

**建议：**
```cmake
execute_process(COMMAND git describe --tags --always OUTPUT_VARIABLE GIT_VERSION)
target_compile_definitions(hft_framework PRIVATE HFT_VERSION="${GIT_VERSION}")
```
启动时打印版本号，方便排查线上问题。

---

## 三、性能优化

### 3.1 ConditionalOrderManager Bounds 重建开销（中优先）

**位置：** `src/order/conditional_order_manager.cpp:388-420`

当前采用 lazy rebuild：标记 dirty 后下次检查时 O(N) 重建。如果同一合约有大量条件单且频繁增删，会出现延迟毛刺。

**建议：** 改为增量维护——add 时更新 min/max，remove 时仅在删除的是 min/max 时才重建。

### 3.2 PositionManager 的 mutex 可以放松

**位置：** `src/position/position_manager.cpp:151-154`

`get_position()` 在消费者线程上调用，mutex 永远不会竞争。可以改为 `std::shared_mutex` + `shared_lock`，或加注释说明 lock 纯防御性。

### 3.3 策略快照重建有堆分配

**位置：** `src/engine/trading_engine.cpp:1062`

`std::make_shared<std::vector<...>>` 在策略快照重建时触发堆分配。虽然不在 tick 热路径上，但如果策略频繁热重载，可能产生可测量的延迟。

**建议：** 预分配 buffer 或用 `small_vector` 替代。

---

## 四、测试覆盖

### 4.1 缺少网关断线重连测试

当前 89 个测试用例全部通过，但没有覆盖 CTP/QDP 网络断线→重连→查询恢复的场景。

**建议：** 用 MockGateway 模拟断线，验证：
- 断线期间委托是否正确排队/拒绝
- 重连后持仓对账是否一致
- 费率重新查询是否成功

### 4.2 缺少并发测试

当前无多线程竞争测试。虽然架构是单消费者线程，但条件单管理器的 3-phase 检查、RiskManager 的 shared_mutex 读写、策略热加载与委托并发都应有专门测试。

**建议：** 加入 TSan (ThreadSanitizer) 构建选项：
```cmake
option(HFT_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(HFT_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()
```

### 4.3 缺少磁盘故障场景测试

Tick 录制和日志在磁盘满时静默失败，无测试覆盖。

### 4.4 缺少回测一致性验证

没有测试验证"同一行情序列，回测与实盘是否产生相同信号"。对策略研发至关重要。

---

## 五、安全与合规

### 5.1 委托价格未校验（高优先）

RiskManager 检查了手数 >0，但**没有检查价格**。发送价格 ≤0 或离谱价格的委托可能通过风控。

**建议：**
```cpp
if (req.price <= 0) return "price must be positive";
if (req.price > last_price * 1.1 || req.price < last_price * 0.9)
    return "price deviates >10% from last tick";
```

### 5.2 Python 策略无沙箱

策略通过文件路径加载，可以执行任意 Python 代码（文件读写、网络请求、系统调用）。

**建议：**
- 短期：策略目录白名单 + 启动时 hash 校验
- 长期：考虑 `RestrictedPython` 或 `seccomp` 隔离

### 5.3 缺少不可篡改的审计日志

当前日志可被修改或删除。对于期货交易，监管可能要求不可篡改的委托/成交记录。

**建议：** 增加 append-only 的审计日志（单独文件），记录每笔委托和成交的完整字段 + 时间戳，文件权限设为只追加。

### 5.4 下单频率兜底

如果 RiskManager 出 bug 或被配置绕过，`send_order()` 本身没有硬限流。

**建议：** 在 Gateway 层加一个简单的令牌桶限流（例如 50 笔/秒），作为最后防线。

---

## 六、运维与部署

### 6.1 缺少 CMake install target

无法 `cmake --install`，部署依赖手动拷贝。

**建议：**
```cmake
install(TARGETS hft_framework RUNTIME DESTINATION bin)
install(DIRECTORY strategies/ DESTINATION strategies)
```

### 6.2 日志级别不可动态调整

运行时无法从 INFO 切换到 DEBUG，排查线上问题时只能重启。

**建议：** 通过 cmd_queue_ 支持动态日志级别切换命令。

### 6.3 SQLite WAL 同步级别

**位置：** `CMakeLists.txt:91`

当前 `SQLITE_DEFAULT_WAL_SYNCHRONOUS=1`（NORMAL），断电时可能丢失最近约 1 秒的配置变更。

**建议：** 生产环境考虑设为 `2`（FULL），或至少在文档中说明这个 trade-off。

### 6.4 ProductionHftMode 缺少运行时校验

`ProductionHftMode=1` 时禁用了 Python/Tick录制/K线，但如果配置了 Python 策略，不会报错或警告，策略的 `on_tick()` 被静默跳过。

**建议：** 启动时检测冲突配置，直接报错退出。

---

## 七、优先级排序

| 优先级 | 编号 | 改动 | 预估工作量 |
|--------|------|------|-----------|
| P0 | 5.1 | 委托价格校验 | 0.5h |
| P0 | 1.3 | RiskManager 空指针检查 | 0.5h |
| P0 | 1.4 | 策略热加载异常回滚 | 1h |
| P1 | 1.1 | seen_trade_ids 改为时间过期 | 1h |
| P1 | 1.2 | Gateway 费率查询返回错误码 | 2h |
| P1 | 5.4 | Gateway 层令牌桶限流 | 2h |
| P1 | 2.1 | TradingEngine 拆文件 | 3h |
| P2 | 3.1 | 条件单 bounds 增量维护 | 2h |
| P2 | 4.1 | 网关断线重连测试 | 3h |
| P2 | 4.2 | TSan 并发测试 | 2h |
| P2 | 5.3 | 审计日志 | 3h |
| P3 | 1.5 | Trade queue 满时 RMS 升级 | 1h |
| P3 | 1.6 | Webhook 重试 | 1h |
| P3 | 2.2 | Config 必填项校验 | 1h |
| P3 | 2.3 | 版本号管理 | 0.5h |
| P3 | 3.2 | PositionManager shared_mutex | 0.5h |
| P3 | 6.1 | CMake install target | 0.5h |
| P3 | 6.2 | 动态日志级别 | 1h |
| P3 | 6.3 | SQLite WAL FULL | 0.5h |
| P3 | 6.4 | ProductionHftMode 冲突检测 | 0.5h |

---

## 总评

框架整体**非常优秀**：
- 延迟表现顶级（p99 < 1μs tick-to-strategy）
- 单消费者线程架构消除了一整类并发 bug
- 零堆分配热路径、SPSC 无锁队列、FixedKey 等设计高度专业
- 文档齐全，测试覆盖基本到位（89 cases all pass）

主要短板集中在**防御性编程**（空指针/价格校验/异常回滚）和**可维护性**（TradingEngine 单文件过大）。P0 项建议立即修复，总计约 2 小时工作量。
