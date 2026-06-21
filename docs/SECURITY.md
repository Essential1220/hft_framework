# 安全模型

`hft_framework` 会在 `config.ini` 中自动加密敏感凭据(密码、授权码、
Webhook 密钥)。本文档说明原理以及**你需要自行保护**的部分。

### 凭据加密机制

首次启动时，engine 识别到的凭据字段(`Password`、`AuthCode`、
`WebhookSecret`)如果是明文，会**就地加密改写**为：

```
Password = ENC:<base64 密文>
```

改写后，明文**不可恢复**。如果忘记密码，只能到经纪商那边重置。

| 平台 | 算法 | 密钥存储位置 |
|---|---|---|
| Windows | DPAPI (`CryptProtectData`) | 绑定当前 Windows 登录会话 + 硬件，无需独立密钥文件 |
| Linux | XOR 混淆 + Base64 | `~/.hft_framework_key`(权限 0600)。**仅限开发/测试环境；生产部署应替换为 AES-256-GCM** |

Windows 上，密钥与当前 Windows 登录会话绑定。把 `config.ini` 复制到
另一台机器或另一个用户账户，**解密会失败** —— 必须在那里重新输入明文。

Linux 上，密钥文件 `~/.hft_framework_key` 自动创建且 `chmod 600`。
如果该文件丢失或重装系统，必须重新输入凭据。

### 需要保护的文件

| 文件 | 原因 |
|---|---|
| `config.ini`(含真实凭据) | 里的 `ENC:...` 密文只有在原机器上才能解密。**绝不**提交到 Git |
| `dist/config.ini.bak` | 首次运行时自动生成的加密前备份。同 `config.ini` |
| `~/.hft_framework_key`(Linux) | XOR 混淆密钥文件。**绝不**分享或提交 |
| `dist/hft_data.db` | SQLite 数据库，含迁移后的配置(含加密值)。不在 Git 中(已 gitignore) |
| `[Web] AuthToken` | 内部状态 HTTP 服务的令牌(若启用)。**绝不**提交 |

### 哪些可以进 Git

- `config.example.ini` —— **模板**，凭据字段留空
- 所有 `.cpp` / `.h` / `.py` / `.md` 文件
- `CMakeLists.txt`
- `LICENSE`

`config.ini`、`config.ini.bak`、`hft_data.db` 和 Linux 密钥文件都已
在 `.gitignore` 中。如果误提交了凭据，**立即**到经纪商端轮换，
然后用 `git filter-branch` 或 `git filter-repo` 清除历史。

### 运行时安全

- 当 `EngineHighPriority=1` 且 `HFT_REALTIME_PRIORITY=1` 时，消费者
  线程以提升的优先级运行 —— 这是 HFT 的预期行为。
- 默认不开放任何入站网络端口。可选的 `[Web]` 端点**只绑定**
  `127.0.0.1` 且需要 `AuthToken`。
- Webhook(出站 HTTP POST)在 `WebhookUrl` 以 `https://` 开头时使用
  TLS。`WebhookSecret` 以 `Authorization` 头发送。

### 安全性说明

**本项目设计上不保证对抗以下威胁：**

- **root/Administrator 权限攻击者：** 拥有管理员权限的进程可以读取 DPAPI
  密钥、dump 进程内存、或直接注入代码到 engine 进程。
- **内核级恶意软件：** 任何可以读取物理内存的攻击都可以提取凭据。
- **同一用户下的未隔离进程：** DPAPI 基于用户上下文。同一用户运行的
  任何进程理论上可以尝试解密你的凭据。

**你需要额外做的：**

- 生产环境以专用低权限用户运行 engine
- 确保 `AuthToken`(若启用 Web 端点)是强随机字符串
- 定期轮换 CTP 密码和 AuthCode
- 生产机器启用全盘加密(BitLocker/LUKS)
