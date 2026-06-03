# Strategies Directory

[English](#english) · [中文](#中文)

---

## English

This directory holds Python strategy scripts. The engine loads any
strategy listed in `[Strategies] List` of `config.ini`, pointing at a
`ScriptPath` here.

| File | One-liner |
|---|---|
| `example_strategy.py` | Conditional-order verification — opens, then attaches stop-loss / take-profit / trailing-stop with an OCO group |
| `ma_cross_strategy.py` | Moving-average crossover using the helpers in `hft_sdk.py` |
| `instant_trigger_strategy.py` | Minimal sanity check — fires one order on the first tick |
| `hft_sdk.py` | **Library, not a strategy** — reusable indicator and order helpers; `import hft_sdk` from your strategy file |

See **[../docs/STRATEGY.md](../docs/STRATEGY.md)** for the full
authoring guide: lifecycle callbacks, dict shapes, the `hft_engine`
API, conditional orders, risk gates, and hot reload semantics.

---

## 中文

本目录存放 Python 策略脚本。`config.ini` 的 `[Strategies] List` 里挂
的策略,会根据各自 `ScriptPath` 字段从这里加载。

| 文件 | 一句话 |
|---|---|
| `example_strategy.py` | 条件单验证 —— 开仓后挂止损 / 止盈 / 追踪止损,带 OCO 互斥分组 |
| `ma_cross_strategy.py` | 均线穿越,用 `hft_sdk.py` 的辅助函数 |
| `instant_trigger_strategy.py` | 极简冒烟测试 —— 首个 tick 直接触发一笔单 |
| `hft_sdk.py` | **库,不是策略** —— 通用指标 / 下单辅助函数,在策略里 `import hft_sdk` 使用 |

完整开发指南见 **[../docs/STRATEGY.md](../docs/STRATEGY.md)**:
生命周期回调、字典结构、`hft_engine` API、条件单、风控闸口、热加载。
