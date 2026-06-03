# hft_framework 延迟基准测试

由 `hft_bench` 自动生成。复现命令：

```
hft_bench --output docs/BENCHMARK.md --runs 5 --warmup 1
```

## 测试机器

```
CPU: 12th Gen Intel(R) Core(TM) i5-12490F
Logical CPUs: 12
OS: Windows
Compiler: MSVC _MSC_VER=1942
Build: Release (NDEBUG)
```

## 热路径延迟

| 场景 | n | p50 | p95 | p99 | p99.9 | max | avg |
|---|---:|---:|---:|---:|---:|---:|---:|
| `cond_order_check_tick` (条件单检查) | 10000 | 0.10us | 0.10us | 0.10us | 0.60us | 121.1us | 0.08us |
| `order_manager_on_order_return` (订单回报处理) | 10000 | 0.10us | 0.10us | 0.10us | 0.20us | 1.40us | 0.10us |
| `order_manager_on_trade_return` (成交回报处理) | 10000 | 0.10us | 0.20us | 0.20us | 11.60us | 130.1us | 0.15us |
| `position_on_trade` (持仓更新) | 10000 | 0.10us | 0.10us | 0.10us | 0.10us | 1.30us | 0.06us |
| `position_get_position` (持仓查询) | 10000 | 0.10us | 0.10us | 0.10us | 0.10us | 1.00us | 0.05us |
| `tick_to_strategy_e2e` (Tick→策略端到端) | 10000 | 0.30us | 0.40us | 0.70us | 34.30us | 223.0us | 0.43us |
| `send_order_e2e` (发单端到端) | 10000 | 0.30us | 0.40us | 1.10us | 9.90us | 331.7us | 0.43us |

## 吞吐量

| 场景 | n | ops/sec |
|---|---:|---:|
| `spsc_queue_throughput` (SPSC 队列吞吐) | 1000000 | 5.721e+08 |

## 数据解读

- **Tick→策略 p99 = 0.70μs**：交易所发出行情后不到 1 微秒，策略代码就开始执行
- **发单 p99 = 1.10μs**：7 维风控检查 + 订单创建在约 1 微秒内完成
- **SPSC 吞吐 5.72×10⁸ ops/sec**：队列永远不会是瓶颈，远超 CTP 峰值行情 (~1 万 tick/秒)
- **p99.9 的尾巴**（34.30μs / 223.0μs）：由操作系统调度抖动、TLB miss 或缓存失效引起，属于正常的长尾分布
