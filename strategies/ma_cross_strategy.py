# ============================================
# ma_cross_strategy.py - 双均线交叉策略
# ============================================
# 策略逻辑：
#   - MA5 上穿 MA10（金叉）→ 买入开仓
#   - MA5 下穿 MA10（死叉）→ 卖出平仓
#   - 同时设置止损保护
# ============================================

import hft_engine

# 策略参数
instrument = None
order_size = 1
fast_period = 5      # 快线周期
slow_period = 10     # 慢线周期
stop_loss_offset = 20.0  # 止损偏移

# 策略状态
prices = []          # 收集最新价用于计算均线
position = 0         # 当前净仓位
tick_count = 0
last_cross = 0       # 1=金叉 -1=死叉 0=未知
cooldown = 0         # 冷却计数，避免频繁交易


def on_init():
    global instrument, order_size, fast_period, slow_period, stop_loss_offset, position
    ctx = hft_engine.get_strategy_context()

    order_size = hft_engine.get_param_int("OrderSize", order_size)
    fast_period = hft_engine.get_param_int("MomentumTicks", fast_period)
    slow_period = fast_period * 2
    stop_loss_offset = hft_engine.get_param_double("StopLossOffset", stop_loss_offset)

    instruments = ctx.get("instruments", [])
    if instruments:
        instrument = instruments[0]

    if instrument:
        position = hft_engine.get_net_position(instrument)

    print(f"[MA交叉] 初始化完成 合约={instrument} 手数={order_size}")
    print(f"[MA交叉] 快线={fast_period} 慢线={slow_period} 止损={stop_loss_offset}")
    print(f"[MA交叉] 当前净仓={position}")


def calc_ma(data, period):
    """计算简单移动平均"""
    if len(data) < period:
        return None
    return sum(data[-period:]) / period


def on_tick(tick):
    global instrument, tick_count, position, last_cross, cooldown

    if instrument is None:
        instrument = tick["instrument_id"]
        position = hft_engine.get_net_position(instrument)

    if tick["instrument_id"] != instrument:
        return

    price = tick["last_price"]
    if price <= 0:
        return

    tick_count += 1
    prices.append(price)

    # 只保留最近 100 个价格
    if len(prices) > 100:
        prices.pop(0)

    # 冷却期
    if cooldown > 0:
        cooldown -= 1
        return

    # 需要足够数据才能计算均线
    if len(prices) < slow_period:
        if tick_count % 20 == 1:
            print(f"[MA交叉] 收集数据中... {len(prices)}/{slow_period} 最新价={price}")
        return

    fast_ma = calc_ma(prices, fast_period)
    slow_ma = calc_ma(prices, slow_period)

    if fast_ma is None or slow_ma is None:
        return

    # 判断交叉
    cross = 1 if fast_ma > slow_ma else -1

    # 每 20 个 tick 打印一次状态
    if tick_count % 20 == 1:
        print(f"[MA交叉] tick={tick_count} 价格={price:.2f} MA{fast_period}={fast_ma:.2f} MA{slow_period}={slow_ma:.2f} 信号={'金叉' if cross == 1 else '死叉'} 净仓={position}")

    # 金叉：快线上穿慢线 → 买入
    if cross == 1 and last_cross == -1 and position <= 0:
        ask = tick["ask_price1"]
        if ask > 0:
            print(f"[MA交叉] 金叉信号! MA{fast_period}={fast_ma:.2f} > MA{slow_period}={slow_ma:.2f} → 买入开仓 价格={ask}")
            hft_engine.send_order({
                "instrument_id": instrument,
                "direction": "buy",
                "offset": "open",
                "price": ask,
                "volume": order_size,
            })
            cooldown = 10  # 10 个 tick 冷却

    # 死叉：快线下穿慢线 → 卖出平仓
    elif cross == -1 and last_cross == 1 and position > 0:
        bid = tick["bid_price1"]
        if bid > 0:
            print(f"[MA交叉] 死叉信号! MA{fast_period}={fast_ma:.2f} < MA{slow_period}={slow_ma:.2f} → 卖出平仓 价格={bid}")
            hft_engine.send_order({
                "instrument_id": instrument,
                "direction": "sell",
                "offset": "close",
                "price": bid,
                "volume": min(order_size, position),
            })
            cooldown = 10

    last_cross = cross


def on_order(order):
    status_map = {0: "待报", 1: "部分成交", 2: "全部成交", 3: "已撤单", 4: "错误"}
    status_text = status_map.get(order["status"], str(order["status"]))
    print(f"[MA交叉] 订单回报: ref={order['order_ref']} 状态={status_text}")
    if order["status"] == 4:
        print(f"[MA交叉] 订单错误: {order['status_msg']}")


def on_trade(trade):
    global position
    vol = trade["volume"]
    if trade["direction"] == "buy":
        position += vol
    else:
        position -= vol
    print(f"[MA交叉] 成交: {trade['direction']} {trade['offset']} 价格={trade['price']} 手数={vol} 净仓={position}")

    # 开仓后设置止损
    if trade["direction"] == "buy" and trade["offset"] == "open":
        sl_price = trade["price"] - stop_loss_offset
        sl_id = hft_engine.add_conditional_order({
            "instrument_id": instrument,
            "type": "stop_loss",
            "direction": "sell",
            "trigger_price": sl_price,
            "volume": vol,
        })
        print(f"[MA交叉] 设置止损单 id={sl_id} 触发价={sl_price}")


def on_reconnect():
    global position, prices, last_cross, tick_count, cooldown
    if instrument:
        position = hft_engine.get_net_position(instrument)
        prices.clear()
        last_cross = 0
        tick_count = 0
        cooldown = 0
        print(f"[MA交叉] 重连同步净仓={position} 已清空价格序列")
