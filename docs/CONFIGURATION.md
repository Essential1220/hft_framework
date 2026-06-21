# 配置参考

`config.ini` 与可执行文件同目录(默认 `dist/config.ini`)。
除非启用了某项功能，所有 section 都是可选的。
仓库内的 `config.example.ini` 就是模板。

### `[Accounts]`

多账户路由。engine 同时只能挂**一个 MD 网关**(由 `MarketDataAccount`
决定)，但可以并行跑 **N 个交易网关**。

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `List` | csv | `""` | 逗号分隔的 `UserID`。每个 ID 都需要对应 `[CTP.<UserID>]`(或 `[QDP.<UserID>]`)段 |
| `MarketDataAccount` | string | List 第一个 | 用哪个账户的行情网关 |
| `<UserID>.Gateway` | enum | `CTP` | 网关类型：`CTP` 或 `QDP`(上期所 FTD 低延迟通道，仅期货，需 `HFT_ENABLE_QDP=ON` 编译) |

### `[CTP.<UserID>]` / `[QDP.<UserID>]`

| Key | 类型 | 必填 | 说明 |
|---|---|---|---|
| `BrokerID` | string | 是 | SimNow 是 `9999` |
| `UserID` | string | 是 | 重复 section 名里的 UserID |
| `Password` | string | 是 | 首次明文，启动后**就地加密**改写为 `ENC:<base64>` |
| `AppID` | string | 仅 CTP | 经纪商颁发的应用 ID(SimNow 用 `simnow_client_test`) |
| `AuthCode` | string | 仅 CTP | 经纪商授权码，同样自动加密 |
| `TradeFront` | string | 是 | 经纪商交易前置 `tcp://host:port` |
| `MarketFront` | string | 是(或经 MD) | 经纪商行情前置 `tcp://host:port` |

QDP 没有 `AppID` / `AuthCode`，因为 FTD 协议跳过了 `ReqAuthenticate`
这一步。凭据加密细节见 [SECURITY.md](SECURITY.md)。

### `[Strategies]`

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `List` | csv | `""` | 逗号分隔的策略 ID。每个 ID 需要对应 `[Strategy.<id>]` 段 |

### `[Strategy.<id>]`

| Key | 类型 | 必填 | 说明 |
|---|---|---|---|
| `Type` | enum | 是 | `python` 或 `cpp` |
| `ScriptPath` | path | python 必填 | `.py` 文件路径，相对工作目录 |
| `ClassName` | string | cpp 必填 | C++ 内置策略类名(如 `SimpleStrategy`) |
| `AccountID` | string | 可选 | 把策略下的单路由到指定账户 |
| `Instruments` | csv | 推荐 | 订阅的合约 ID 列表 |
| `OrderSize` | int | 可选 | 默认手数，策略里通过 `hft_engine.get_param_int("OrderSize", ...)` 读 |
| 任意自定义 key | string | 否 | 通过 `get_param_*` 暴露给策略 |

### `[Risk]`

每账户盘前风控上限。

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `MaxOrderSize` | int | 5 | 单笔最大手数 |
| `MaxNetPosition` | int | 10 | 单合约 abs(多 − 空) 上限 |
| `MaxOrdersPerMinute` | int | 30 | 滑窗报单率上限 |
| `MaxCancelRate` | float | 0.5 | (撤单 / 报单) 比率上限 |
| `MaxDailyLoss` | float | 5000 | 当日实现 + 浮动 PnL ≤ −`MaxDailyLoss` 时禁开仓 |
| `CancelRateWindowMinutes` | int | 60 | 撤单率统计窗口 |

### `[Trading]`

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `TradingSessions` | string | `""` | 逗号分隔的 `HH:MM-HH:MM` 时段。空 = 始终开放(仅 SimNow 测试用)。支持跨午夜区间(如 `21:00-02:30`) |

### `[Performance]`

热路径调优。默认值都安全 —— 每个 Disable 开关都意味着关掉一项功能。
生产 HFT 部署建议：

```ini
[Performance]
EngineCpuCore = 2
LoggerCpuCore = 3
EngineHighPriority = 1
ProductionHftMode = 1   ; 打开下面几项
MdBatchSize = 512
```

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `EngineCpuCore` | int | -1 | 把消费者线程绑定到 CPU N(0-based)。-1 不绑 |
| `LoggerCpuCore` | int | -1 | 异步日志线程绑核 |
| `CtpThreadCpuCore` | int | -1 | CTP SDK 内部线程绑核 |
| `EngineHighPriority` | bool | 1 | 提升进程优先级类 |
| `RealtimePriority` | bool | 0 | 用 `HIGH_PRIORITY_CLASS` —— 需要环境变量 `HFT_REALTIME_PRIORITY=1` |
| `ProductionHftMode` | bool | 0 | 总开关 —— 同时打开下面几项 |
| `MdBatchSize` | int | 512 | 消费者一次从 MD SPSC 拉的最大 tick 数 |
| `DisablePythonHotPath` | bool | 0 | 跳过 Python on_tick 派发(C++ 策略不受影响) |
| `DisableTickRecordingHotPath` | bool | 0 | 不录 tick(省每笔 memcpy + SPSC push) |
| `DisableKlineHotPath` | bool | 0 | 跳过 K 线聚合 |
| `StrategyHotInstrumentsOnly` | bool | 0 | 只派发所有策略 `Instruments` 并集对应的 tick |

### `[Log]`

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `Level` | enum | `INFO` | `DEBUG / INFO / WARN / ERROR` |
| `Directory` | path | `logs` | 日志目录，不存在自动创建 |
| `FilePrefix` | string | `hft` | 文件名前缀(`hft.YYYYMMDD.log`) |
| `QueueCapacity` | int | 8192 | 异步日志 SPSC 容量(建议 2 的幂) |
| `RecentBufferSize` | int | 400 | 内存里保留的最近日志行数，供运行时 `recent_logs` 查询 |
| `FlushIntervalMs` | int | 1000 | fsync 节奏 |
| `RetentionDays` | int | 7 | 自动删除超过 N 天的日志 |

### `[Runtime]`

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `RunMode` | enum | `service` | `service`(收信号停)或 `interactive`(stdin 读命令) |
| `StateFile` | path | `runtime_state.dat` | 持久化 order_ref 计数器等 |
| `NoTickWarnSeconds` | int | 10 | 交易时段内 N 秒没收到 tick 触发告警 |
| `PythonHome` | path | `python` | `Py_SetPythonHome` 路径(若 `python/` 不在默认位置) |
| `EnablePython` | bool | 1 | 设 0 完全跳过嵌入 Python 初始化(纯 C++ 部署) |

### `[Alerts]`

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `WebhookEnabled` | bool | 0 | 启用出站 webhook(Slack / 钉钉等) |
| `WebhookUrl` | url | `""` | POST 目标 |
| `WebhookSecret` | string | `""` | 共享密钥(可选)，首次保存自动加密 |

### `[Web]`

> **内部功能，不建议对外暴露。** 若存在，会在 `<Host>:<Port>` 启
> 一个状态查询 HTTP 端点；`AuthToken` 必须保密，
> **千万不要**把这个端口暴露到公网。

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `Port` | int | `9090` | HTTP 监听端口 |
| `Host` | string | `0.0.0.0` | 监听地址。生产环境建议改为 `127.0.0.1` |
| `AuthToken` | string | `""` | Bearer token 鉴权。非空时每个请求需带 `Authorization: Bearer <token>` |
| `EnableControl` | bool | `0` | 启用 POST 操作端点(撤单 / 切换 RMS 模式)。**0 = 只读** |
| `AllowedOrigins` | string | `*` | CORS 允许来源。生产环境建议改为具体域名 |
| `AllowInsecureNoAuth` | bool | `0` | 允许无 token 访问(仅本地调试用) |
| `AutoOpenBrowser` | bool | `0` | 启动时自动打开浏览器访问 WebUI |

### `[Watchdog]`

独立看门狗进程通过共享内存监控引擎心跳。
看门狗是独立二进制(`hft_watchdog`)，不在引擎进程内。

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `Enable` | bool | `0` | 引擎侧：是否创建共享内存并写入心跳 |
| `ShmName` | string | `hft_watchdog` | 共享内存名称(引擎和 watchdog 须一致) |
| `StaleThresholdMs` | int | `3000` | 心跳超时阈值(毫秒)。watchdog 连续 3 次超时则报 CRITICAL |

看门狗 CLI：
```bash
hft_watchdog --shm-name hft_watchdog --threshold 3000 --poll-interval 500
```

### `[Features]`

内置技术指标管道。按合约独立维护滑动窗口。

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `Enable` | bool | `0` | 启用 Feature Pipeline(SMA/EMA/RSI/ATR/VWAP) |
| `HistorySize` | int | `100` | 每合约保留的 tick 历史窗口大小 |
| `SmaPeriod` | int | `20` | 简单移动平均周期 |
| `EmaPeriod` | int | `12` | 指数移动平均周期 |
| `RsiPeriod` | int | `14` | RSI 指标周期 |
| `AtrPeriod` | int | `14` | ATR 真实波幅周期 |

### `[UDP]`

UDP 组播行情分发。用于同一网段内多进程共享行情。

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `MulticastGroup` | string | `239.1.1.1` | 组播 IP 地址 |
| `Port` | int | `5555` | 组播端口 |
| `PublishEnabled` | bool | `0` | 引擎侧：是否将收到的 tick 通过 UDP 组播转发 |
