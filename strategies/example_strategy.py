# ============================================
# example_strategy.py - 条件单验证策略
# ============================================

import hft_engine

instrument = None
order_size = 1

stop_loss_offset = 10.0
take_profit_offset = 15.0
trail_offset = 8.0

tick_count = 0
position = 0
has_opened = False
cond_order_ids = []


def _safe_float(value, default_value):
    try:
        return float(value)
    except Exception:
        return default_value


def on_init():
    global instrument, order_size, stop_loss_offset, take_profit_offset, trail_offset, position
    ctx = hft_engine.get_strategy_context()
    params = ctx.get("params", {})

    order_size = hft_engine.get_param_int("OrderSize", order_size)
    stop_loss_offset = hft_engine.get_param_double(
        "StopLossOffset",
        _safe_float(params.get("stop_loss_offset"), stop_loss_offset),
    )
    take_profit_offset = hft_engine.get_param_double(
        "TakeProfitOffset",
        _safe_float(params.get("take_profit_offset"), take_profit_offset),
    )
    trail_offset = hft_engine.get_param_double(
        "TrailOffset",
        _safe_float(params.get("trail_offset"), trail_offset),
    )

    instruments = ctx.get("instruments", [])
    if instruments:
        instrument = instruments[0]

    print("[验证策略] 初始化完成")
    print(
        f"[验证策略] 参数: 手数={order_size} 止损偏移={stop_loss_offset} "
        f"止盈偏移={take_profit_offset} 追踪回撤={trail_offset}"
    )
    if instrument:
        print(f"[验证策略] 配置关注合约={instrument}")
    # 从引擎同步真实持仓，防止重启后仓位归零
    if instrument is not None:
        position = hft_engine.get_net_position(instrument)
        print(f"[验证策略] 同步引擎净仓={position}")
    print("[验证策略] 等待收到有效 tick 后自动开仓")


def on_tick(tick):
    global instrument, tick_count, has_opened, position

    if instrument is None:
        instrument = tick["instrument_id"]
        # 首次绑定合约后同步持仓
        position = hft_engine.get_net_position(instrument)
        print(f"[验证策略] 绑定合约={instrument} 当前净仓={position}")

    if tick["instrument_id"] != instrument:
        return

    tick_count += 1

    if tick_count % 10 == 1:
        print(
            f"[验证策略] 第{tick_count}笔行情 最新={tick['last_price']} "
            f"买一={tick['bid_price1']} 卖一={tick['ask_price1']}"
        )

    if tick_count == 3 and not has_opened:
        has_opened = True
        price = tick["ask_price1"]
        print(f"[验证策略] 发出买入开仓 价格={price} 手数={order_size}")
        hft_engine.send_order(
            {
                "instrument_id": instrument,
                "direction": "buy",
                "offset": "open",
                "price": price,
                "volume": order_size,
            }
        )


def on_order(order):
    status_map = {
        0: "待报",
        1: "部分成交",
        2: "全部成交",
        3: "已撤单",
        4: "错误",
    }
    status_text = status_map.get(order["status"], str(order["status"]))
    print(
        f"[验证策略] 订单回报: ref={order['order_ref']} 状态={status_text} "
        f"已成={order['traded_volume']}/{order['total_volume']}"
    )

    if order["status"] == 4:
        print(f"[验证策略] 订单错误: {order['status_msg']}")


def on_trade(trade):
    global position, cond_order_ids

    vol = trade["volume"]
    fill_price = trade["price"]

    if trade["direction"] == "buy":
        position += vol
    else:
        position -= vol

    print(
        f"[验证策略] 成交: {trade['direction']} {trade['offset']} "
        f"价格={fill_price} 手数={vol} 净仓={position}"
    )

    if trade["direction"] == "buy" and trade["offset"] == "open":
        # 分配互斥分组编号：同组条件单中一个触发后自动取消其他
        group_id = hft_engine.allocate_group_id()
        print(f"[验证策略] 分配互斥分组编号={group_id}")

        sl_price = fill_price - stop_loss_offset
        sl_id = hft_engine.add_conditional_order(
            {
                "instrument_id": instrument,
                "type": "stop_loss",
                "direction": "sell",
                "trigger_price": sl_price,
                "volume": vol,
                "group_id": group_id,
            }
        )
        cond_order_ids.append(sl_id)
        print(f"[验证策略] 添加止损单 编号={sl_id} 触发价={sl_price} 分组={group_id}")

        tp_price = fill_price + take_profit_offset
        tp_id = hft_engine.add_conditional_order(
            {
                "instrument_id": instrument,
                "type": "take_profit",
                "direction": "sell",
                "trigger_price": tp_price,
                "volume": vol,
                "group_id": group_id,
            }
        )
        cond_order_ids.append(tp_id)
        print(f"[验证策略] 添加止盈单 编号={tp_id} 触发价={tp_price} 分组={group_id}")

        ts_id = hft_engine.add_conditional_order(
            {
                "instrument_id": instrument,
                "type": "trailing_stop",
                "direction": "sell",
                "trail_offset": trail_offset,
                "volume": vol,
                "group_id": group_id,
            }
        )
        cond_order_ids.append(ts_id)
        print(f"[验证策略] 添加追踪止损 编号={ts_id} 回撤={trail_offset} 分组={group_id}")
        print(f"[验证策略] 条件单编号列表: {cond_order_ids}")

    if trade["direction"] == "sell" and trade["offset"] == "close":
        print(f"[验证策略] 条件单触发平仓成交 价格={fill_price}")
        if position == 0:
            cond_order_ids.clear()
            print("[验证策略] 持仓已清零, 清空条件单列表")


def on_reconnect():
    global position, cond_order_ids
    if instrument is not None:
        position = hft_engine.get_net_position(instrument)
        cond_order_ids.clear()
        print(f"[验证策略] 重连后同步净仓={position} 已清空条件单列表")
