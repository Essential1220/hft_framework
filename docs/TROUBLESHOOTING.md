# 故障排查指南

常见问题和解决方案。

---

## 目录

- [编译问题](#编译问题)
- [CTP 连接问题](#ctp-连接问题)
- [运行时问题](#运行时问题)
- [性能问题](#性能问题)
- [Python 策略问题](#python-策略问题)

---

## 编译问题

### `CTP_SDK_DIR is required` — CMake 报错找不到 CTP SDK

**原因：** 没有指定 CTP SDK 路径。

**解决：** 从 [simnow.com.cn](http://www.simnow.com.cn/) 下载 CTP API 包，解压后在 cmake 命令中指定路径：

```powershell
cmake -S . -B build -DCTP_SDK_DIR="C:/你的路径/ctp_api_xxx"
```

### `ThostFtdcMdApi.h not found` — 头文件找不到

**原因：** 指定的 `CTP_SDK_DIR` 路径不对，CMake 找到了目录但没找到 SDK 文件。

**解决：** 确认解压后的目录结构类似：

```
ctp_api_xxx/
├── ThostFtdcMdApi.h
├── ThostFtdcTraderApi.h
├── thostmduserapi_se.dll
├── thosttraderapi_se.dll
└── ...
```

### `Python.h not found` — Python 头文件找不到

**原因：** Python 的开发头文件没有安装。

**解决：**
- Windows：重装 Python，在安装器中勾选 **"Customize installation" → 勾选 "Development files"**
- 或者在 cmake 中手动指定 Python 路径：
  ```powershell
  cmake -S . -B build -DCTP_SDK_DIR=... -DPython3_EXECUTABLE="C:/Python39/python.exe"
  ```

### `pybind11` 找不到

**原因：** pybind11 没有装在当前 Python 环境中。

**解决：**
```powershell
pip install pybind11
# 然后重新运行 cmake
```

### `error C2039: 'string_view': is not a member of 'std'` (MSVC)

**原因：** C++ 标准设置不对。

**解决：** 确保 CMakeLists.txt 中有 `set(CMAKE_CXX_STANDARD 17)`，且使用 VS2022 (v143) 工具集。

### `undefined reference to 'Py_Initialize'` (GCC/Linux)

**原因：** 链接器没找到 Python 库。

**解决：**
```bash
# 确认 Python 开发包已安装
sudo apt install python3-dev   # Debian/Ubuntu
sudo yum install python3-devel # CentOS/RHEL
```

---

## CTP 连接问题

### `行情网关登录超时` / `交易网关登录超时`

**原因（按可能性排序）：**

1. **非交易时段：** SimNow 仿真环境只在交易时段开放登录（工作日 9:00-15:15、21:00-02:30）
2. **防火墙拦截：** 出站 TCP 到 40001 或 40011 端口被防火墙阻止
3. **网络不通：** 公司/学校网络可能屏蔽了非标准端口

**解决：**

- **排查是否为交易时段问题：** 临时修改配置测试：
  ```ini
  [Trading]
  TradingSessions =    ; 留空 = 不做时段检查
  ```
  如果改成空之后可以登录但没行情 → 就是非交易时段的问题。

- **排查防火墙：**
  ```powershell
  # 测试端口是否通
  Test-NetConnection -ComputerName 182.254.243.31 -Port 40001
  ```

### `AppID` / `AuthCode` 校验失败 (CTP 返回 -1)

**原因：** 配置里的 `AppID` 或 `AuthCode` 写错了。

**SimNow 正确值：**
```ini
AppID    = simnow_client_test
AuthCode = 0000000000000000   # 16 个 0
```

### `CTP: 用户不在线` 报错

**原因：** 同一个 UserID 在另一个地方登录了，被踢下线。

**解决：** 确认没有其他的程序（vnpy、快期、文华等）用同一个账号连着 SimNow。

### `CTP: 不合法的报单` (OrderRef 重复)

**原因：** 引擎重启后 `StateFile`（记录 order_ref 计数器）丢失或损坏，导致 order_ref 重号。

**解决：**
- 删除 `dist/runtime_state.dat`，重启引擎会自动生成新的
- 或者在配置中指定一个新的 `StateFile` 路径

---

## 运行时问题

### Engine 启动后没有 tick 输出

**排查步骤：**

1. **确认行情网关已登录成功：** 启动日志里应该有 `行情网关登录成功`
2. **确认订阅的合约代码正确：**
   ```ini
   [Strategy.demo_main]
   Instruments = IF2406    ; 合约代码必须对（含年份月份）
                           ; 错误示例：IF2606（还没上市的月份）
   ```
3. **确认是否在交易时段：** 非交易时段没有行情推送
4. **确认合约状态：** 已摘牌的合约不再推送行情

### `dist\hft_framework.exe` 启动闪退

**原因：** 缺少 CTP DLL。

**解决：**
- 确认 `dist/` 下有 `thostmduserapi_se.dll` 和 `thosttraderapi_se.dll`
- 如果是手动复制 exe，需要用 CMake 安装目标：`cmake --build build --target install`
- Windows 下可以用 Dependency Walker 检查缺少哪些 DLL

### 引擎运行一段时间后内存持续增长

**可能原因：**

1. **日志级别设太高（DEBUG）：** 日志 SPSC 队列积压，内存中排队的日志消息越来越多
   ```ini
   [Log]
   Level = INFO    ; 不要用 DEBUG 长期运行
   ```
2. **Tick 录制开启了但没清理：** 检查是否开启了 tick 录制功能，录制的文件积累在磁盘上
3. **Python 策略有内存泄漏：** 检查策略代码中是否在不停地往 list/dict 里追加数据但不清理

### 引擎在高负载下丢 tick

**可能原因：**

1. **消费者线程没绑核/优先级不够：** 被其他进程抢了 CPU
   ```ini
   [Performance]
   EngineCpuCore = 2          # 钉在专用核心上
   EngineHighPriority = 1     # 提高进程优先级
   ```
2. **Python 策略的 `on_tick` 太慢：** Python 回调每 tick 只能花不到 1μs，重的逻辑应该做异步
3. **MdBatchSize 设小了：** 可以调大
   ```ini
   [Performance]
   MdBatchSize = 1024    # 默认 512，可以调到 1024
   ```

---

## 性能问题

### Benchmark 数据比文档里的差很多

**常见原因：**

1. **用的 Debug 构建：** 必须用 Release 构建 `cmake --build build --config Release`
2. **没绑核：** 确认 `EngineCpuCore` 设置且生效
3. **CPU 频率波动：** 关闭 SpeedStep / Turbo Boost（BIOS 设置），或至少接通电源（笔记本）
4. **后台程序抢 CPU：** 关掉浏览器、杀毒软件、Windows Update 等
5. **热路径上开了 Python：** 考虑设 `DisablePythonHotPath = 1` 做纯 C++ 部署

### Python 策略的延迟比 C++ 策略高很多

**这是预期的：**
- Python 的 `on_tick` 回调要走 pybind11 的 C++ ↔ Python 边界
- 每次调用涉及 Python 对象的创建和销毁
- p99 延迟会比纯 C++ 策略高几倍

**缓解方法：**
- 把重计算逻辑移到 C++ 层
- 使用条件单代替在 Python 中判断触发条件（条件单全程在 C++ 层执行）
- 减少 Python 对象的创建（复用而非新建）

---

## Python 策略问题

### 策略加载失败：`ImportError: No module named 'xxx'`

**原因：** Python 环境中缺少策略依赖的包。

**解决：** 确认引擎使用的 Python 环境中已安装了所需包：
```powershell
# 找到引擎使用的 Python
where python
# 在这个 Python 里安装
pip install <需要的包>
```

### 策略文件改了但没生效（热加载失败）

**可能原因：**

1. **文件保存路径不对：** 确认修改的是 `dist/strategies/` 下的文件，不是仓库源码目录下的
2. **语法错误：** Python 文件有语法错误时热加载会静默失败，启动日志里会有 traceback
3. **文件修改时间检测延迟：** 某些文件系统（如网络驱动器）的文件修改事件可能延迟或丢失

**解决：** 查看 `logs/hft.log`，搜索 `reload` 或 `Traceback`。

### `on_order` 收到 `status = 4` (错误) 的回报

**原因：** 订单被风控拦截。

**解决：** 查看 `status_msg` 字段，它会说明具体拒绝原因：

| status_msg | 含义 | 处理 |
|------------|------|------|
| `单笔数量超限` | 超过 `MaxOrderSize` | 减小下单量 |
| `净持仓超限` | 超过 `MaxNetPosition` | 先平仓再开仓 |
| `报单频率超限` | 超过 `MaxOrdersPerMinute` | 降低报单频率 |
| `撤单率超限` | 超过 `MaxCancelRate` | 减少撤单 |
| `日内亏损超限` | 超过 `MaxDailyLoss` | 暂停策略，等待风控评估 |
| `RMS: 禁开仓` | RMS 模式为 `NoOpen` | 等待 RMS 降级为 Normal |
| `RMS: 暂停` | RMS 模式为 `Halted` | 检查是否触发熔断 |
| `非交易时段` | 当前不在交易时段内 | 等待交易时段 |

### `send_order` 返回的 `order_ref` 该用来干什么

`order_ref` 是你后续撤单的唯一凭据：

```python
order_ref = hft_engine.send_order({...})
# 存下来，后面如果要撤单：
hft_engine.cancel_order(order_ref)
```

同时也是 `on_order` 和 `on_trade` 回调中匹配订单的字段。

### `on_tick` 是锁步执行的，我不能在里面等网络请求怎么办

**做法：** 在 `on_tick` 里只做**轻量的判断和状态更新**，把重的 I/O 操作（发 HTTP 请求、写文件等）丢到独立的线程或队列里：

```python
import threading
import queue

_pending_alerts = queue.Queue()

def _alert_worker():
    while True:
        msg = _pending_alerts.get()
        requests.post("https://hooks.slack.com/xxx", json={"text": msg})

threading.Thread(target=_alert_worker, daemon=True).start()

def on_tick(tick):
    if tick["last_price"] > threshold:
        _pending_alerts.put(f"价格突破 {threshold}")  # 非阻塞
```

---

## 其他常见错误

### `RuntimeError: 配置键 [XXX] 不完整`

**原因：** 某个 section 的必要字段没有填。

**解决：** 参考 `config.example.ini` 或 [CONFIGURATION.md](CONFIGURATION.md)，补齐缺失字段。

### `hft_data.db` 损坏

**原因：** 引擎异常退出（断电/强杀进程）时 SQLite 文件可能损坏。

**解决：**
1. 删除 `dist/hft_data.db`
2. 重新启动引擎：会从 `config.ini` 重新迁移，自动重建数据库
3. 确认重要数据已备份

### 引擎启动打印乱码

**原因：** Windows 终端编码不是 UTF-8。

**解决：**
```powershell
chcp 65001    # 切换到 UTF-8
```
