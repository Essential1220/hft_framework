# 快速上手

从 `git clone` 到 SimNow 连通的 engine，大约 5 分钟。

### 前置依赖

| 项目 | 最低版本 | 备注 |
|---|---|---|
| CMake | 3.20 | `cmake --version` 查看 |
| 编译器(Windows) | MSVC 2022 (v143) + C++ 桌面开发负载 | 在 VS Installer 中勾选 "使用 C++ 的桌面开发" |
| 编译器(Linux) | g++ 9+ 或 clang 10+ | `g++ --version` |
| Python | 3.9.x 或 3.10.x | 需要头文件(`Python.h`);官方 Windows 安装器默认勾选 |
| Git LFS | 不需要 | CTP SDK 需自行下载，不在仓库内 |
| 硬件 | x86_64 | 热路径优化(CPU 绑核等)默认 x86_64 |
| SimNow 账号 | 需要 | 在 <http://www.simnow.com.cn/> 免费注册 |

> **注：** SimNow 只在交易时段开放登录。要 7x24 测试，请在
> `config.ini` 设 `[Trading] TradingSessions =`(留空)。

### 第 1 步：克隆

```powershell
git clone https://github.com/Essential1220/hft_framework.git
cd hft_framework
```

### 第 2 步：下载 CTP SDK

在 <http://www.simnow.com.cn/> 免费注册后下载 CTP API 包，解压并记下路径。

典型解压路径：`C:/ctp_api/20250617_traderapi64_se_windows`

### 第 3 步：CMake 配置

Windows (PowerShell)：

```powershell
cmake -S . -B build_vs2022 -G "Visual Studio 17 2022" -A x64 `
      -DCTP_SDK_DIR="C:/ctp_api/20250617_traderapi64_se_windows"
```

Linux：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCTP_SDK_DIR=/path/to/ctp_api/linux64
```

如果找不到 Python，显式指定解释器：

```powershell
cmake -S . -B build_vs2022 -G "Visual Studio 17 2022" -A x64 `
      -DCTP_SDK_DIR="C:/ctp_api/20250617_traderapi64_se_windows" `
      -DPython3_EXECUTABLE="C:/Users/<你>/AppData/Local/Programs/Python/Python39/python.exe"
```

### 第 4 步：编译

```powershell
cmake --build build_vs2022 --config Release --target hft_framework
```

Linux：

```bash
cmake --build build -j --target hft_framework
```

编译成功后，`dist/` 下会出现：

- `hft_framework.exe`(Linux 为 `hft_framework`)
- `thostmduserapi_se.dll` / `thosttraderapi_se.dll`(Windows 运行时)
- `config.example.ini`
- `md_flow/`、`td_flow/`、`strategies/` 三个目录

### 第 5 步：配置账户

```powershell
copy config.example.ini dist\config.ini
notepad dist\config.ini
```

最小可用 SimNow 配置 —— 只填 UserID 和密码，其它默认：

```ini
[Accounts]
List = 123456                 ; <-- 你的 SimNow UserID
MarketDataAccount = 123456

[CTP.123456]
BrokerID    = 9999
UserID      = 123456           ; <-- 重复一次
Password    = yourpassword     ; <-- 首次明文，框架会自动加密改写
AppID       = simnow_client_test
AuthCode    = 0000000000000000
TradeFront  = tcp://182.254.243.31:40001
MarketFront = tcp://182.254.243.31:40011
```

> **安全提示：** 首次启动后，明文密码会被改写成
> `Password = ENC:<base64>`，**原文不可恢复**。详见
> [SECURITY.md](SECURITY.md)。

### 第 6 步：首次运行

```powershell
dist\hft_framework.exe --interactive --config dist\config.ini
```

启动成功后你会看到类似输出：

```
[2026-06-20 14:30:01.123] [INFO] Logger initialized (queue=8192)
[2026-06-20 14:30:01.125] [INFO] ConfigStore: loaded 1 account(s)
[2026-06-20 14:30:01.130] [INFO] WebServer listening on port 9090
[2026-06-20 14:30:01.132] [INFO] 行情网关初始化: CTP.123456 -> tcp://182.254.243.31:40011
[2026-06-20 14:30:01.224] [INFO] 行情TCP连接成功 (92ms)
[2026-06-20 14:30:01.254] [INFO] 行情CTP登录成功 (30ms)
[2026-06-20 14:30:01.438] [INFO] 交易TCP连接成功 (184ms)
[2026-06-20 14:30:01.526] [INFO] 交易AppAuth认证成功 (88ms)
[2026-06-20 14:30:01.842] [INFO] 交易登录+结算确认成功 (316ms)
[2026-06-20 14:30:04.078] [INFO] 持仓快照同步完成 (2236ms)
[2026-06-20 14:30:04.079] [INFO] ✓ 账户就绪: 123456 (balance=100000.00, available=100000.00)
[2026-06-20 14:30:04.080] [INFO] Strategy loaded: demo_main (python, instruments=rb2610)
[2026-06-20 14:30:04.081] [INFO] Engine started. Type 'help' for commands, 'q' to quit.
[2026-06-20 14:30:04.500] [INFO] 首个tick到达: rb2610 last=3256.0 vol=12345
>
```

看到 `✓ 账户就绪` 和 `首个tick到达` 就说明一切正常了。
打开浏览器访问 `http://localhost:9090/` 可以看到 WebUI 仪表盘。
输入 `q` 或 `quit` 优雅关闭。

### 第 7 步：挂一个策略

编辑 `dist/config.ini`：

```ini
[Strategies]
List = demo_main

[Strategy.demo_main]
Type        = python
ScriptPath  = strategies/example_strategy.py
AccountID   = 123456
Instruments = rb2610            ; <-- 选一个当前活跃合约
OrderSize   = 1
```

重启 `hft_framework.exe`，示例策略的 tick 事件会写入 `logs/hft.log`。
自己写策略请看 [STRATEGY.md](STRATEGY.md)。

### 故障排查

| 现象 | 可能原因 | 解决 |
|---|---|---|
| `行情网关登录超时` | SimNow 非交易时段，或防火墙挡 `tcp:40001/40011` 出站 | 改在交易时段，或放行防火墙 |
| `AppID` / `AuthCode` 拒绝 | `[CTP.<UserID>]` 拼错 | 检查 SimNow 默认：`simnow_client_test` / 16 个 0 |
| CMake 阶段提示 `Python.h not found` | Python 头文件未安装 | 重装 Python，勾选 "Customize installation" → "Development files" |
| `pybind11` 找不到 | venv 错位 | `pip install pybind11`，然后用 `-DPython3_EXECUTABLE=...` 指向同一个 Python 重跑 cmake |
| 运行时 `thostmduserapi_se.dll` 缺失 | dist 未生成 / 当前路径错 | 重跑 `cmake --build`，在 `dist/` 下启动 |
| Engine 起来了但没 tick | 合约 ID 错(比如已经摘牌) | 换当月主力合约 |
