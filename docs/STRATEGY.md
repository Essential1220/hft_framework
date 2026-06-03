# 策略开发指南

本指南讲怎么用 Python 写 `hft_framework` 的策略。
C++ 策略入口在 `src/strategy/strategy_base.h`，参考 `SimpleStrategy`。

### 生命周期回调

Python 策略本质就是一个模块，实现以下任意子集即可：

| 回调 | 时机 | 参数 |
|---|---|---|
| `on_init()` | 脚本加载后、首个 tick 前 | 无 |
| `on_tick(tick)` | 每笔行情 | `dict` |
| `on_order(order)` | 每条订单回报 | `dict` |
| `on_trade(trade)` | 每笔成交回报 | `dict` |
| `on_reconnect()` | 交易网关重连成功后，首个新 tick 前 | 无 |
| `on_destroy()` | 卸载策略前(热加载或停机) | 无 |

所有回调都跑在**消费者线程**上，**不需要任何锁**保护策略状态。
绝对**不要**在回调里做阻塞 I/O(`time.sleep`、`requests.get` 等)，
否则整个 engine 就停了。

### Tick 字典结构 (on_tick)

```python
{
    "instrument_id": "IF2406",
    "exchange_id":   "CFFEX",
    "last_price":    3850.2,
    "pre_close_price": 3848.0,
    "open_price":    3851.0,
    "highest_price": 3855.6,
    "lowest_price":  3847.4,
    "volume":        12345,
    "turnover":      4_752_300.0,
    "open_interest": 23456,

    "bid_price1": 3850.0, "bid_volume1": 12,
    "ask_price1": 3850.4, "ask_volume1": 9,
    # ... bid_price2..5 / ask_price2..5 / bid_volume2..5 / ask_volume2..5

    "upper_limit": 4200.0,
    "lower_limit": 3500.0,
    "update_time": "21:30:01",
    "update_millisec": 250,
    "trading_day": "20260527",
    "action_day":  "20260527",
}
```

### 订单 / 成交字典结构

订单 (`on_order`)：

```python
{
    "order_ref":      "0000123",
    "instrument_id":  "IF2406",
    "exchange_id":    "CFFEX",
    "direction":      "buy",          # "buy" | "sell"
    "offset":         "open",         # "open" | "close" | "close_today" | "close_yesterday"
    "price":          3850.2,
    "total_volume":   1,
    "traded_volume":  0,
    "status":         0,              # 0 待报 / 1 部分成交 / 2 全部成交 / 3 已撤单 / 4 错误
    "status_msg":     "",
    "front_id":       1,
    "session_id":     100,
}
```

成交 (`on_trade`)：

```python
{
    "instrument_id": "IF2406",
    "direction":     "buy",
    "offset":        "open",
    "price":         3850.4,
    "volume":        1,
    "trade_id":      "abc123",
    "order_ref":     "0000123",
    "trade_time":    "21:30:01",
}
```

### Engine API (`hft_engine` 模块)

`hft_engine` 模块由 C++ 宿主在运行时注入，在策略文件顶部 import 即可。
常用 API：

| API | 返回 | 说明 |
|---|---|---|
| `send_order(req: dict)` | `str`(order_ref) | 发限价单。req 字段：`instrument_id, direction, offset, price, volume` |
| `cancel_order(order_ref: str)` | `bool` | 撤未成交单 |
| `add_conditional_order(req: dict)` | `int`(cond_order_id) | 服务端条件单(止损 / 止盈 / 追踪止损) |
| `cancel_conditional_order(cond_order_id: int)` | `bool` | 撤排队中的条件单 |
| `allocate_group_id()` | `int` | 申请 OCO 互斥分组编号(一个触发自动撤其他) |
| `get_net_position(instrument: str)` | `int` | 净仓(多正空负) |
| `get_position(instrument: str, direction: str)` | `dict` | 单方向持仓：`volume`、`avg_price`、`today_volume`、`yesterday_volume` |
| `get_strategy_context()` | `dict` | 策略启动上下文：`params`、`instruments`、`account_id`、`order_size` |
| `get_param_int(name, default)` | `int` | 读 `[Strategy.<id>]` 参数为 int |
| `get_param_double(name, default)` | `float` | 同上，float |
| `get_param_string(name, default)` | `str` | 同上，string |

### 条件单类型

`add_conditional_order(req)` 的 `type` 支持 `stop_loss`(止损)、
`take_profit`(止盈)、`trailing_stop`(追踪止损)、
`entry_trigger`(触发开仓)：

```python
hft_engine.add_conditional_order({
    "instrument_id": "IF2406",
    "type":          "stop_loss",
    "direction":     "sell",
    "trigger_price": 3840.0,         # stop_loss / take_profit / entry_trigger 用
    "volume":        1,
    "group_id":      group_id,        # 可选，OCO 互斥分组
})

hft_engine.add_conditional_order({
    "instrument_id": "IF2406",
    "type":          "trailing_stop",
    "direction":     "sell",
    "trail_offset":  8.0,            # 跟踪最高点，回撤超过 8 即触发
    "volume":        1,
})
```

条件单触发判断**完全在 engine 的消费者线程上**做，不进 Python 回调。
触发延迟见 benchmark 的 `cond_order_check_tick` 行(桌面 i5 上 p99 ≈ 0.1μs)。

### 盘前风控 + RMS

每次 `send_order()` 会先过两道闸：

1. **盘前风控**(`[Risk]` 段)：单笔手数上限、净仓上限、分钟报单数上限、
   撤单率上限、日内亏损上限。
2. **RMS 模式**：`Normal`(默认)放行所有；`NoOpen` 禁开仓；
   `ReduceOnly` 仅放行减仓单；`Liquidating` 主动平仓中；`Halted` 全禁。

被拒的单子会在 `on_order` 回调里以 `status = 4`(错误)出现，
`status_msg` 含拒绝原因。**不要**盲目重报，先看消息。

### 热加载

engine 运行时保存 `.py` 文件会自动触发热加载。
生命周期顺序：旧实例 `on_destroy()`，再新实例 `on_init()`。
**模块级变量**(`tick_count`、`position`、`cond_order_ids` 等)会丢失，
请在 `on_init()` 里用 `hft_engine.get_net_position()` 重新同步。

### 示例策略

| 文件 | 用途 |
|---|---|
| `strategies/example_strategy.py` | 条件单验证(止损 / 止盈 / 追踪) |
| `strategies/ma_cross_strategy.py` | 均线穿越(用 `hft_sdk` 辅助函数) |
| `strategies/instant_trigger_strategy.py` | 极简：首个 tick 触发一笔单 |
| `strategies/hft_sdk.py` | 可复用辅助函数(**不注入**，策略里 `import hft_sdk`) |

### 策略开发注意事项

**热路径约束：**

`on_tick` 跑在消费者线程上，占用了 tick→策略→发单 整条链路的时间。
策略逻辑应该尽可能轻量。不要在回调里做：

- 网络请求（HTTP、数据库连接）
- 文件 I/O
- `time.sleep()` 或任何阻塞调用
- 重计算（复杂的数值优化、大循环）

如果需要这些操作，把它们丢到独立线程或队列里异步处理。

**状态管理：**

```python
# 策略级别的状态用模块变量或类变量
position = 0
last_signal = None

def on_tick(tick):
    global position, last_signal
    # 轻量判断...
    if should_buy(tick):
        hft_engine.send_order({...})
        position += 1
```

**错误处理：**

不要在 `on_tick` 里用 bare except——未捕获的异常会终止策略但不会影响其他策略
或 engine 本身。不过被终止的策略就收不到后续 tick 了。用明确的异常类型：

```python
def on_tick(tick):
    try:
        price = tick["last_price"]
    except KeyError:
        return  # 字段缺失，跳过
```

**多策略隔离：**

每个策略是独立的模块实例。策略 A 的异常不影响策略 B。
但**它们共享同一个消费者线程**——策略 A 的慢 `on_tick` 会延迟策略 B 的 `on_tick`。
因此，对延迟敏感的多策略部署建议用 `ProductionHftMode` + 纯 C++ 策略。
