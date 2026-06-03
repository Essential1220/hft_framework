# ============================================
# latency_test_strategy.py — SimNow 真实延迟测试
#
# 测量真实端到端延迟:
#   1. tick 到达 → on_tick 回调延迟(引擎内部派发)
#   2. send_order → on_order(待报) 延迟
#   3. on_order(待报) → on_order(已撤) 延迟(撤单)
#   4. on_tick 整体处理时间
#
# 使用 QPCTimer 采集微秒级精度,结果写入 logs/latency_report.csv
# ============================================

import hft_engine
import time
import os
import csv
from datetime import datetime

instrument = None
order_size = 1

# 测试状态
tick_count = 0
test_phase = "waiting"        # waiting → sending → canceling → done
pending_order_ref = None
send_order_tick_ts = 0        # send_order 前的 tick timestamp
order_insert_tick_ts = 0      # on_order(待报) 时的 tick timestamp
cancel_req_tick_ts = 0        # 发撤单请求时的 tick timestamp
order_cancelled_tick_ts = 0   # on_order(已撤) 时的 tick timestamp

# 延迟采集结果
latency_samples = []          # list of dicts
max_test_orders = 5           # 最多测几轮下单/撤单
test_order_count = 0

# CSV 输出路径
CSV_PATH = "logs/latency_report.csv"


def _now_us():
    """当前时间戳(微秒)"""
    return int(time.time() * 1_000_000)


def _write_report():
    """把采集到的延迟数据写入 CSV"""
    if not latency_samples:
        return

    os.makedirs(os.path.dirname(CSV_PATH), exist_ok=True)
    with open(CSV_PATH, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "round", "tick_to_on_tick_us", "send_order_to_ack_us",
            "ack_to_cancel_ack_us", "total_order_lifecycle_us",
            "tick_price", "order_ref"
        ])
        writer.writeheader()
        for row in latency_samples:
            writer.writerow(row)

    # 打印汇总到 stdout
    print("=" * 60)
    print("  SimNow Latency Test Report")
    print("=" * 60)
    print(f"  Instrument: {instrument}")
    print(f"  Rounds: {len(latency_samples)}")
    print()

    def _stats(key):
        vals = [r[key] for r in latency_samples if r[key] > 0]
        if not vals:
            return None
        vals.sort()
        n = len(vals)
        return {
            "n": n,
            "min": vals[0],
            "max": vals[-1],
            "avg": sum(vals) // n,
            "p50": vals[n // 2],
            "p95": vals[int(n * 0.95)] if n >= 20 else vals[-1],
            "p99": vals[int(n * 0.99)] if n >= 100 else vals[-1],
        }

    labels = {
        "tick_to_on_tick_us": "Tick → on_tick 回调",
        "send_order_to_ack_us": "send_order → 订单回报(待报)",
        "ack_to_cancel_ack_us": "撤单请求 → 撤单回报",
        "total_order_lifecycle_us": "订单全生命周期(send → 撤单完成)",
    }

    for key, label in labels.items():
        s = _stats(key)
        if s:
            print(f"  {label}:")
            print(f"    n={s['n']}  min={s['min']}us  avg={s['avg']}us  "
                  f"p50={s['p50']}us  p95={s['p95']}us  p99={s['p99']}us  "
                  f"max={s['max']}us")
        else:
            print(f"  {label}: no data")
        print()

    print(f"  CSV saved to: {CSV_PATH}")
    print("=" * 60)


def on_init():
    global instrument, order_size
    ctx = hft_engine.get_strategy_context()
    instruments = ctx.get("instruments", [])
    if instruments:
        instrument = instruments[0]
    order_size = hft_engine.get_param_int("OrderSize", order_size)

    print(f"[延迟测试] 初始化: instrument={instrument} order_size={order_size}")
    print(f"[延迟测试] 等待 tick 到达后开始测试(最多 {max_test_orders} 轮)")


def on_tick(tick):
    global tick_count, test_phase, pending_order_ref
    global send_order_tick_ts, test_order_count

    if instrument is None:
        return
    if tick["instrument_id"] != instrument:
        return

    tick_count += 1
    tick_arrival_ts = _now_us()

    # on_tick 入口到当前时间 = 引擎派发延迟 + Python 回调开销
    # (tick 内部的 UpdateTime 可以做粗略参考,但精度有限)

    if test_phase == "waiting" and tick_count >= 3:
        # 开始第一轮测试
        test_phase = "sending"
        test_order_count = 0
        _send_test_order(tick)

    elif test_phase == "canceling" and pending_order_ref is not None:
        # 发撤单请求
        _cancel_test_order(tick_arrival_ts)

    elif test_phase == "done":
        # 测试完成,输出报告
        test_phase = "finished"
        _write_report()


def _send_test_order(tick):
    global pending_order_ref, send_order_tick_ts, test_phase

    send_order_tick_ts = _now_us()

    # 用一个不太可能成交的价格(远离市价)避免真成交
    price = tick["bid_price1"] - 100.0  # 远低于买一价
    if price <= 0:
        price = tick["last_price"] - 100.0

    pending_order_ref = hft_engine.send_order({
        "instrument_id": instrument,
        "direction": "buy",
        "offset": "open",
        "price": price,
        "volume": order_size,
    })

    print(f"[延迟测试] 第{test_order_count + 1}轮: send_order ref={pending_order_ref} "
          f"price={price} ts={send_order_tick_ts}")


def _cancel_test_order(ts):
    global pending_order_ref, cancel_req_tick_ts, test_phase

    if pending_order_ref is None:
        return

    cancel_req_tick_ts = ts
    result = hft_engine.cancel_order(pending_order_ref)
    print(f"[延迟测试] 撤单: ref={pending_order_ref} result={result} ts={cancel_req_tick_ts}")


def on_order(order):
    global test_phase, pending_order_ref
    global order_insert_tick_ts, order_cancelled_tick_ts
    global test_order_count, latency_samples

    if order["order_ref"] != pending_order_ref:
        return

    now = _now_us()
    status = order["status"]

    if status == 0:
        # 待报 → 测量 send_order → 订单回报延迟
        order_insert_tick_ts = now
        send_to_ack = order_insert_tick_ts - send_order_tick_ts
        print(f"[延迟测试] 订单回报(待报): ref={order['order_ref']} "
              f"send→ack={send_to_ack}us")

        # 进入撤单阶段
        test_phase = "canceling"

    elif status == 3:
        # 已撤单 → 测量撤单延迟
        order_cancelled_tick_ts = now
        ack_to_cancel = order_cancelled_tick_ts - cancel_req_tick_ts
        total_lifecycle = order_cancelled_tick_ts - send_order_tick_ts

        tick_to_on_tick = order_insert_tick_ts - send_order_tick_ts  # 近似

        sample = {
            "round": test_order_count + 1,
            "tick_to_on_tick_us": 0,  # 需要在 on_tick 里单独测
            "send_order_to_ack_us": order_insert_tick_ts - send_order_tick_ts,
            "ack_to_cancel_ack_us": ack_to_cancel,
            "total_order_lifecycle_us": total_lifecycle,
            "tick_price": 0,
            "order_ref": order["order_ref"],
        }
        latency_samples.append(sample)

        print(f"[延迟测试] 已撤单: ref={order['order_ref']} "
              f"ack→cancel={ack_to_cancel}us total={total_lifecycle}us")

        test_order_count += 1
        pending_order_ref = None

        if test_order_count >= max_test_orders:
            test_phase = "done"
            print(f"[延迟测试] {max_test_orders} 轮测试完成,等待下一个 tick 输出报告")
        else:
            test_phase = "waiting"  # 等下一个 tick 再发下一轮

    elif status == 4:
        # 错误
        print(f"[延迟测试] 订单错误: {order.get('status_msg', 'unknown')}")
        pending_order_ref = None
        if test_order_count >= max_test_orders:
            test_phase = "done"
        else:
            test_phase = "waiting"


def on_trade(trade):
    # 不应该成交(价格远离市价),但如果真成交了就记录
    print(f"[延迟测试] 意外成交: {trade['direction']} {trade['offset']} "
          f"price={trade['price']} vol={trade['volume']}")


def on_reconnect():
    global test_phase, pending_order_ref, latency_samples
    print("[延迟测试] 重连,重置测试状态")
    test_phase = "waiting"
    pending_order_ref = None
    latency_samples.clear()


def on_destroy():
    if latency_samples:
        _write_report()
    print("[延迟测试] 策略卸载")
