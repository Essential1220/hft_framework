# ============================================
# instant_trigger_strategy.py - 首tick立即触发策略
# 用途：验证交易链路，收到第一个tick即以对手价开仓1手
# ============================================

import hft_engine

instrument = None
order_size = 1
triggered = False
position = 0


def on_init():
    global instrument, order_size, position
    ctx = hft_engine.get_strategy_context()
    order_size = hft_engine.get_param_int("OrderSize", 1)
    instruments = ctx.get("instruments", [])
    if instruments:
        instrument = instruments[0]
    if instrument:
        position = hft_engine.get_net_position(instrument)
    print(f"[即时触发] 初始化完成 合约={instrument} 手数={order_size} 当前仓位={position}")


def on_tick(tick):
    global instrument, triggered, position

    if instrument is None:
        instrument = tick["instrument_id"]
        position = hft_engine.get_net_position(instrument)

    if tick["instrument_id"] != instrument:
        return

    if triggered:
        return

    price = tick.get("last_price", 0)
    ask = tick.get("ask_price1", 0)
    bid = tick.get("bid_price1", 0)

    if price <= 0:
        return

    triggered = True
    order_price = ask if ask > 0 else price
    print(f"[即时触发] 首tick触发! price={price} ask={ask} bid={bid} -> 买入开仓 {order_size}手 @ {order_price}")

    hft_engine.send_signal({
        "type": "entry",
        "message": f"首tick触发买入 {instrument} @ {order_price}",
    })

    hft_engine.send_order({
        "instrument_id": instrument,
        "direction": "buy",
        "offset": "open",
        "price": order_price,
        "volume": order_size,
        "order_type": "limit",
    })


def on_order(order):
    status = order.get("status", -1)
    ref = order.get("order_ref", "")
    status_names = {0: "待报", 1: "部分成交", 2: "全部成交", 3: "已撤单", 4: "错误"}
    status_text = status_names.get(status, str(status))
    print(f"[即时触发] 委托回报 ref={ref} status={status_text}")
    if status == 4:
        print(f"[即时触发] 委托被拒: {order.get('status_msg', '')}")


def on_trade(trade):
    global position
    vol = trade.get("volume", 0)
    price = trade.get("price", 0)
    position = hft_engine.get_net_position(instrument)
    print(f"[即时触发] 成交! price={price} vol={vol} 当前仓位={position}")
    hft_engine.send_signal({
        "type": "fill",
        "message": f"成交 {instrument} {vol}手 @ {price} 仓位={position}",
    })


def on_stop():
    print(f"[即时触发] 策略停止 最终仓位={position}")


def on_reconnect():
    global position, triggered
    if instrument:
        position = hft_engine.get_net_position(instrument)
        print(f"[即时触发] 重连后同步仓位={position}")
        if position > 0:
            triggered = True
