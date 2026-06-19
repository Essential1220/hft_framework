# API 参考

本文档覆盖策略开发者需要用到的所有 API：C++ 原生接口和 Python (`hft_engine`) 接口。

---

## 目录

- [C++ 策略 API](#c-策略-api)
  - [StrategyBase 基类](#strategybase-基类)
  - [交易 API (protected)](#交易-api-protected)
  - [数据结构](#c-数据结构)
- [Python 策略 API](#python-策略-api)
  - [hft_engine 模块](#hft_engine-模块)
  - [数据结构 (Python dict)](#python-数据结构)
- [配置参数读取](#配置参数读取)
- [错误码参考](#错误码参考)

---

## C++ 策略 API

### StrategyBase 基类

**头文件：** `src/strategy/strategy_base.h`

所有策略（无论是 C++ 原生还是 Python 桥接）都继承自 `StrategyBase`。

#### 生命周期回调

| 方法 | 调用时机 | 说明 |
|------|---------|------|
| `void on_init()` | 策略加载后、首个 tick 前 | **必须实现。** 在此初始化策略状态 |
| `void on_tick(const TickData& tick)` | 每收到一笔行情数据 | **必须实现。** 热路径回调 |
| `void on_order(const OrderInfo& order)` | 每收到一条订单状态回报 | **必须实现。** |
| `void on_trade(const TradeInfo& trade)` | 每收到一笔成交回报 | **必须实现。** |
| `void on_reconnect()` | 交易网关重连成功后 | 可选。用于重新同步状态 |
| `void on_stop()` | 策略被卸载前 | 可选。清理资源 |

#### 元数据/路由

| 方法 | 返回 | 说明 |
|------|------|------|
| `strategy_id()` | `const std::string&` | 策略唯一标识 |
| `default_account_id()` | `const std::string&` | 默认资金账号 |
| `watched_instruments()` | `const std::vector<std::string>&` | 关注的合约列表 |
| `strategy_type()` | `const std::string&` | `"cpp"` 或 `"python"` |
| `is_python()` | `bool` | 是否为 Python 策略（O(1) 热路径判断） |
| `script_path()` | `const std::string&` | 脚本文件路径 |

#### 事件分发判断

| 方法 | 说明 |
|------|------|
| `handles_strategy(strategy_id)` | 事件是否属于本策略 |
| `handles_account(account_id)` | 事件是否属于本策略的默认账号 |
| `handles_instrument(instrument_id)` | 合约是否在本策略关注列表中 |
| `handles_event(account_id, instrument_id)` | 综合判断：是否应该将事件派发给本策略 |

### 交易 API (protected)

以下方法只能在策略的 `on_init` / `on_tick` / `on_order` / `on_trade` 回调中调用（因为回调跑在消费者线程上，不需要锁）。

#### 普通委托

```cpp
// 发送限价单，返回报单引用号
std::string send_order(const OrderRequest& req);

// 撤单
void cancel_order(const std::string& order_ref);
```

#### 条件单

```cpp
// 添加条件单（止损/止盈/追踪止损/触发开仓），返回条件单 ID
uint32_t add_conditional_order(const ConditionalOrder& order);

// 取消条件单
void cancel_conditional_order(uint32_t id);

// 分配 OCO 互斥分组 ID
uint32_t allocate_cond_group_id();
```

#### 持仓查询

```cpp
// 查询全账户聚合某方向的持仓
PositionInfo get_position(const char* instrument, Direction dir);

// 查询指定账户某方向的持仓
PositionInfo get_position(const char* instrument, Direction dir, const char* account_id);

// 查询全账户净持仓（多正空负）
int get_net_position(const char* instrument);

// 查询指定账户净持仓
int get_net_position(const char* instrument, const char* account_id);
```

### C++ 数据结构

#### TickData

```cpp
struct TickData {
    std::string instrument_id;   // 合约代码，如 "IF2406"
    std::string exchange_id;      // 交易所，如 "CFFEX"
    double last_price;            // 最新价
    double pre_close_price;       // 昨收
    double open_price;            // 开盘价
    double highest_price;         // 最高价
    double lowest_price;          // 最低价
    double volume;                // 成交量
    double turnover;              // 成交额
    double open_interest;         // 持仓量
    double bid_price[5];          // 买一 ~ 买五
    double bid_volume[5];         // 买一量 ~ 买五量
    double ask_price[5];          // 卖一 ~ 卖五
    double ask_volume[5];         // 卖一量 ~ 卖五量
    double upper_limit;           // 涨停价
    double lower_limit;           // 跌停价
    std::string update_time;      // 更新时间 "HH:MM:SS"
    int update_millisec;          // 毫秒
    std::string trading_day;      // 交易日
    std::string action_day;       // 业务日期
};
```

#### OrderRequest

```cpp
struct OrderRequest {
    std::string instrument_id;    // 合约代码
    Direction direction;           // Direction::Buy 或 Direction::Sell
    OffsetFlag offset;            // OffsetFlag::Open / Close / CloseToday / CloseYesterday
    double price;                  // 限价
    int volume;                    // 手数
    std::string strategy_id;      // 策略 ID（引擎自动填充）
    std::string account_id;       // 资金账号（引擎按策略配置自动填充）
};
```

#### OrderInfo（错误码参考：[错误码参考](#错误码参考)）

```cpp
struct OrderInfo {
    std::string order_ref;        // 本地报单引用号
    std::string order_sys_id;     // 交易所系统编号
    std::string instrument_id;    // 合约代码
    std::string exchange_id;      // 交易所
    Direction direction;           // 买卖方向
    OffsetFlag offset;            // 开平
    double price;                  // 委托价格
    int total_volume;              // 总数量
    int traded_volume;             // 已成交数量
    OrderStatus status;            // 订单状态
    std::string status_msg;       // 状态消息（风控拒绝原因等）
    int front_id;                  // 前置 ID
    int session_id;                // 会话 ID
};
```

#### TradeInfo

```cpp
struct TradeInfo {
    std::string instrument_id;    // 合约代码
    Direction direction;           // 买卖方向
    OffsetFlag offset;            // 开平
    double price;                  // 成交价
    int volume;                    // 成交数量
    std::string trade_id;         // 成交编号
    std::string order_ref;        // 对应报单引用号
    std::string trade_time;       // 成交时间
};
```

#### ConditionalOrder

```cpp
struct ConditionalOrder {
    enum class Type { StopLoss, TakeProfit, TrailingStop, EntryTrigger };

    std::string instrument_id;    // 合约代码
    Type type;                     // 条件单类型
    Direction direction;           // 触发后的买卖方向
    double trigger_price;          // 触发价格（StopLoss/TakeProfit/EntryTrigger）
    double trail_offset;           // 追踪价差（TrailingStop，从最高点回撤多少触发）
    int volume;                    // 触发后的报单手数
    uint32_t group_id;             // OCO 互斥分组 ID（0 = 不分组）
};
```

#### 枚举类型

```cpp
enum class Direction { Buy, Sell };
enum class OffsetFlag { Open, Close, CloseToday, CloseYesterday };
enum class OrderStatus {
    PendingSubmit = 0,   // 待报（已发往交易所，等待确认）
    PartiallyFilled = 1, // 部成（部分成交）
    Filled = 2,          // 全成（全部成交）
    Canceled = 3,        // 已撤
    Rejected = 4,        // 错误（被风控拦截或交易所拒绝）
};
enum class RiskMode { Normal, NoOpen, ReduceOnly, Liquidating, Halted };
```

#### PositionInfo

```cpp
struct PositionInfo {
    int volume;                    // 持仓量
    int today_volume;              // 今仓量
    int yesterday_volume;          // 昨仓量
    double avg_price;              // 开仓均价
};
```

---

## Python 策略 API

### hft_engine 模块

`hft_engine` 由 C++ 引擎在运行时注入到 Python 环境，在策略文件顶部直接 `import` 即可：

```python
import hft_engine
```

#### 普通委托

```python
# 发限价单，返回 str 类型的报单引用号
order_ref = hft_engine.send_order({
    "instrument_id": "IF2406",   # str, 必填
    "direction":     "buy",      # "buy" | "sell", 必填
    "offset":        "open",     # "open" | "close" | "close_today" | "close_yesterday", 必填
    "price":         3850.0,     # float, 必填
    "volume":        1,          # int, 必填
})

# 撤单，返回 bool
success = hft_engine.cancel_order(order_ref)
```

#### 条件单

```python
# 止损单
hft_engine.add_conditional_order({
    "instrument_id": "IF2406",
    "type":          "stop_loss",
    "direction":     "sell",
    "trigger_price": 3840.0,
    "volume":        1,
    "group_id":      group_id,    # 可选，OCO 互斥分组
})

# 止盈单
hft_engine.add_conditional_order({
    "instrument_id": "IF2406",
    "type":          "take_profit",
    "direction":     "sell",
    "trigger_price": 3860.0,
    "volume":        1,
})

# 追踪止损单
hft_engine.add_conditional_order({
    "instrument_id": "IF2406",
    "type":          "trailing_stop",
    "direction":     "sell",
    "trail_offset":  8.0,    # 从最高点回撤 8 个价位即触发
    "volume":        1,
})

# 触发开仓单
hft_engine.add_conditional_order({
    "instrument_id": "IF2406",
    "type":          "entry_trigger",
    "direction":     "buy",
    "trigger_price": 3860.0,
    "volume":        1,
})

# 取消条件单，返回 bool
hft_engine.cancel_conditional_order(cond_order_id)

# 分配 OCO 互斥分组 ID，返回 int
group_id = hft_engine.allocate_group_id()
```

**条件单类型说明：**

| type | 触发逻辑 |
|------|---------|
| `stop_loss` | 价格 ≤ trigger_price 时触发卖出（止损） |
| `take_profit` | 价格 ≥ trigger_price 时触发卖出（止盈） |
| `trailing_stop` | 价格从最高点回撤 ≥ trail_offset 时触发卖出 |
| `entry_trigger` | 价格突破 trigger_price 时触发开仓 |

**注意：** 条件单的触发判断完全在 C++ 引擎层执行，不进 Python 回调。触发到发单的延迟在 p99 ≈ 0.1μs。

#### 持仓查询

```python
# 全账户净持仓（多正空负），返回 int
net_pos = hft_engine.get_net_position("IF2406")

# 指定方向的持仓明细，返回 dict
pos = hft_engine.get_position("IF2406", "buy")
# pos = {"volume": 2, "avg_price": 3845.0, "today_volume": 1, "yesterday_volume": 1}
```

#### 策略上下文

```python
# 获取策略配置上下文，返回 dict
ctx = hft_engine.get_strategy_context()
# ctx = {
#     "strategy_id":    "demo_main",
#     "account_id":     "123456",
#     "instruments":    ["IF2406", "IC2406"],
#     "order_size":     1,
#     "params":         {"key1": "val1", ...},
# }

# 读取配置参数（类型安全，带默认值）
value_int    = hft_engine.get_param_int("OrderSize", 1)
value_float  = hft_engine.get_param_double("Threshold", 0.01)
value_str    = hft_engine.get_param_string("Mode", "default")
```

### Python 数据结构

策略通过回调函数的参数接收数据，全部为 `dict` 类型。

#### on_tick(tick)

```python
{
    "instrument_id":   "IF2406",
    "exchange_id":     "CFFEX",
    "last_price":      3850.2,
    "pre_close_price": 3848.0,
    "open_price":      3851.0,
    "highest_price":   3855.6,
    "lowest_price":    3847.4,
    "volume":          12345,
    "turnover":        4_752_300.0,
    "open_interest":   23456,

    "bid_price1": 3850.0, "bid_volume1": 12,
    "bid_price2": 3849.8, "bid_volume2": 8,
    "bid_price3": 3849.6, "bid_volume3": 15,
    "bid_price4": 3849.4, "bid_volume4": 6,
    "bid_price5": 3849.2, "bid_volume5": 20,

    "ask_price1": 3850.4, "ask_volume1": 9,
    "ask_price2": 3850.6, "ask_volume2": 11,
    "ask_price3": 3850.8, "ask_volume3": 5,
    "ask_price4": 3851.0, "ask_volume4": 13,
    "ask_price5": 3851.2, "ask_volume5": 7,

    "upper_limit":  4200.0,
    "lower_limit":  3500.0,
    "update_time":  "21:30:01",
    "update_millisec": 250,
    "trading_day":  "20260527",
    "action_day":   "20260527",
}
```

#### on_order(order)

```python
{
    "order_ref":      "0000123",
    "order_sys_id":   "       123",  # 交易所系统编号，未报时为空
    "instrument_id":  "IF2406",
    "exchange_id":    "CFFEX",
    "direction":      "buy",           # "buy" | "sell"
    "offset":         "open",          # "open" | "close" | "close_today" | "close_yesterday"
    "price":          3850.2,
    "total_volume":   1,
    "traded_volume":  0,
    "status":         0,               # 0=待报 1=部成 2=全成 3=已撤 4=错误
    "status_msg":     "",              # 被拒绝时这里是原因
    "front_id":       1,
    "session_id":     100,
}
```

#### on_trade(trade)

```python
{
    "instrument_id": "IF2406",
    "direction":     "buy",
    "offset":        "open",
    "price":         3850.4,
    "volume":        1,
    "trade_id":      "abc123",        # 交易所成交编号
    "order_ref":     "0000123",       # 对应的报单引用号
    "trade_time":    "21:30:01",
}
```

---

## 配置参数读取

策略通过 `config.ini` 中的 `[Strategy.<id>]` 段接收参数：

```ini
[Strategy.demo_main]
Type        = python
ScriptPath  = strategies/demo.py
AccountID   = 123456
Instruments = IF2406, IC2406
OrderSize   = 1
Threshold   = 0.005             # 自定义参数
MaxHold     = 3                 # 自定义参数
```

**C++ 读取：**
```cpp
int order_size = get_parameter_int("OrderSize", 1);
double threshold = get_parameter_double("Threshold", 0.01);
std::string mode = get_parameter("MaxHold", "1");
```

**Python 读取：**
```python
order_size = hft_engine.get_param_int("OrderSize", 1)
threshold  = hft_engine.get_param_double("Threshold", 0.01)
max_hold   = hft_engine.get_param_string("MaxHold", "1")
```

---

## 错误码参考

### 订单状态码 (OrderStatus)

| 值 | 枚举 | 含义 |
|----|------|------|
| 0 | `PendingSubmit` | 待报：订单已发往交易所，等待确认 |
| 1 | `PartiallyFilled` | 部成：部分成交，剩余继续挂单 |
| 2 | `Filled` | 全成：全部成交 |
| 3 | `Canceled` | 已撤：已撤销 |
| 4 | `Rejected` | 错误：被风控拦截或交易所拒绝 |

### 风控拒绝原因 (status_msg)

| status_msg | 触发条件 |
|------------|---------|
| `单笔数量超限` | 报单手数超过 `[Risk] MaxOrderSize` |
| `净持仓超限` | 开仓后净持仓（含挂单）超过 `MaxNetPosition` |
| `报单频率超限` | 滑动窗口内报单次数超过 `MaxOrdersPerMinute` |
| `撤单率超限` | 滑动窗口内 (撤单数/报单数) 超过 `MaxCancelRate` |
| `日内亏损超限` | 当日实现+浮动亏损超过 `MaxDailyLoss` |
| `RMS: 禁开仓` | 当前 RMS 模式为 `NoOpen` |
| `RMS: 暂停` | 当前 RMS 模式为 `Halted` |
| `非交易时段` | 当前时间不在 `[Trading] TradingSessions` 范围内 |
| `可平仓位不足` | 平仓量超过当前持仓量 |

### RMS 风控模式 (RiskMode)

| 模式 | 含义 | 允许的操作 |
|------|------|-----------|
| `Normal` | 正常 | 所有操作 |
| `NoOpen` | 禁开仓 | 只能平仓，禁止开仓 |
| `ReduceOnly` | 仅减仓 | 只放行 `is_risk_reduction = true` 的订单 |
| `Liquidating` | 平仓中 | 系统正在主动平仓，用户不能发单 |
| `Halted` | 熔断 | 全部订单被拦截 |

**RMS 模式升级是自动的，降级是手动的。** 例如：连续亏损触发 `Normal → NoOpen`，但恢复需要运维手动执行降级命令。

---

## REST API

启用 `[Web] Port=9090` 后，引擎会在指定端口启动 HTTP 服务，提供以下端点。
所有响应均为 JSON 格式，支持 CORS。

### 查询端点 (GET)

| 端点 | 说明 | 参数 |
|------|------|------|
| `GET /api/status` | 引擎运行状态、风控模式、运行时间、队列丢弃数 | — |
| `GET /api/accounts` | 所有账户快照（余额、可用、保证金、持仓盈亏） | — |
| `GET /api/strategies` | 所有策略状态（成交数、胜率、盈亏、最近信号） | — |
| `GET /api/latency` | 实时延迟快照（tick_to_signal、signal_to_order 等，单位 μs） | — |
| `GET /api/ticks` | 当前订阅合约的最新 tick | — |
| `GET /api/orders` | 最近委托列表 | `?limit=50` |
| `GET /api/trades` | 最近成交列表 | `?limit=50` |
| `GET /api/pnl` | 资金曲线时间序列 | `?limit=240` |
| `GET /api/risk` | 风控实时状态（报单频率、撤单率、日内亏损等） | `?account_id=xxx` |
| `GET /api/alerts` | 最近报警消息 | `?limit=50` |

### 操作端点 (POST)

需要在 `config.ini` 中设置 `[Web] EnableControl=1` 才能使用操作类端点。

| 端点 | 说明 | 参数 (form body) |
|------|------|------------------|
| `POST /api/test_order` | 发送测试单并返回下单延迟 | `instrument`, `direction`, `price`, `volume`, `offset` |
| `POST /api/cancel_order` | 撤销指定委托 | `order_ref`, `account_id` |
| `POST /api/cancel_all` | 撤销全部挂单 | `account_id`（可选） |
| `POST /api/risk/mode` | 手动切换 RMS 模式 | `mode` (0-5), `reason` |

### 监控端点

| 端点 | 说明 |
|------|------|
| `GET /metrics` | Prometheus 文本格式指标（可直接接入 Grafana） |
| `GET /health` | 健康检查，返回 `ok` |
| `GET /` | 内嵌 WebUI 仪表盘（HTML SPA） |

### 示例

```bash
# 查看引擎状态
curl http://localhost:9090/api/status

# 查看实时延迟
curl http://localhost:9090/api/latency

# 发送测试单并测量延迟
curl -X POST http://localhost:9090/api/test_order \
     -d 'instrument=rb2610&direction=buy&price=3000&volume=1&offset=open'

# 撤销全部挂单
curl -X POST http://localhost:9090/api/cancel_all
```

---

## C API (FFI)

**头文件：** `src/api/hft_api.h`

提供 `extern "C"` 接口，支持从 Go、Rust、C#、Java JNI、Python ctypes 等语言调用。

### 生命周期

```c
HftEngineHandle hft_engine_create(void);
int  hft_engine_init(HftEngineHandle h, const char* config_path);
int  hft_engine_start(HftEngineHandle h);
void hft_engine_stop(HftEngineHandle h);
void hft_engine_destroy(HftEngineHandle h);
```

### 交易操作

```c
int  hft_send_order(HftEngineHandle h, const char* instrument,
                    int direction, int offset, double price,
                    int volume, char* out_ref, int ref_len);
int  hft_cancel_order(HftEngineHandle h, const char* order_ref);
int  hft_cancel_all(HftEngineHandle h, const char* account_id);
```

### 查询（返回堆分配 JSON，调用方需 `hft_free_string` 释放）

```c
char* hft_get_account(HftEngineHandle h, const char* account_id);
char* hft_get_positions(HftEngineHandle h, const char* account_id);
char* hft_get_active_orders(HftEngineHandle h, const char* account_id);
char* hft_get_last_tick(HftEngineHandle h, const char* instrument);
char* hft_get_latency(HftEngineHandle h);
// ... 更多查询见 hft_api.h
void  hft_free_string(char* s);  // 释放上述返回的 JSON 字符串
```
