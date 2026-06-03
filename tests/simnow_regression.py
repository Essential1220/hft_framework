#!/usr/bin/env python3
"""
SimNow real-counter regression test.

The counter response is the source of truth. A submitted HTTP request is not
enough: the order must become active after insert and then reach cancelled
state after cancel. Rejected contracts fail the test.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time
from contextlib import contextmanager
from typing import Any
from urllib.parse import urlencode

import requests


BASE_URL = "http://127.0.0.1:8080"
ACCOUNT_ID = os.environ.get("HFT_SIMNOW_ACCOUNT_ID", "")
TIMEOUT = 10


class SimNowTestError(Exception):
    pass


def api_get(path: str) -> Any:
    try:
        resp = requests.get(f"{BASE_URL}{path}", timeout=TIMEOUT)
        resp.raise_for_status()
        return resp.json()
    except Exception as exc:
        raise SimNowTestError(f"GET {path} failed: {exc}") from exc


def api_post(path: str, params: dict[str, Any] | None = None, json_body: dict[str, Any] | None = None) -> Any:
    try:
        url = f"{BASE_URL}{path}"
        if params:
            url += "?" + urlencode(params)
        resp = requests.post(url, json=json_body, timeout=TIMEOUT)
        resp.raise_for_status()
        return resp.json()
    except Exception as exc:
        raise SimNowTestError(f"POST {path} failed: {exc}") from exc


def list_payload(payload: Any) -> list[Any]:
    if isinstance(payload, dict) and isinstance(payload.get("value"), list):
        return payload["value"]
    if isinstance(payload, list):
        return payload
    return []


def wait_for_engine_ready(timeout: float = 60.0) -> dict[str, Any]:
    deadline = time.time() + timeout
    last_status: dict[str, Any] = {}
    while time.time() < deadline:
        status = api_get("/api/status")
        last_status = status
        if (
            status.get("md_gateway_logged_in")
            and status.get("td_gateway_logged_in")
            and status.get("trading_ready")
        ):
            return status
        time.sleep(0.5)
    raise SimNowTestError(f"engine is not trading-ready before timeout: {last_status}")


def ensure_engine_started() -> None:
    status = api_get("/api/status")
    if status.get("trading_ready"):
        return
    result = api_post("/api/engine/start", {"confirm": "true"})
    if not result.get("ok"):
        raise SimNowTestError(f"engine start rejected: {result}")
    wait_for_engine_ready()


def cancel_all_orders() -> None:
    try:
        api_post("/api/cancel_all_orders", {"confirm": "true", "account_id": ACCOUNT_ID})
    except Exception as exc:
        print(f"[WARN] cancel_all_orders failed during cleanup: {exc}")
    time.sleep(1.0)


@contextmanager
def safety_guard():
    try:
        yield
    finally:
        cancel_all_orders()
        active = api_get(f"/api/active_orders?account_id={ACCOUNT_ID}")
        if len(list_payload(active)) != 0:
            raise SimNowTestError(f"cleanup left active orders: {active}")


def assert_contract_is_known(instrument: str, price: float) -> None:
    result = api_post(
        "/api/order/estimate",
        {
            "account_id": ACCOUNT_ID,
            "instrument": instrument,
            "direction": "Buy",
            "offset": "Open",
            "price": price,
            "volume": 1,
        },
    )
    if not result.get("ok"):
        raise SimNowTestError(f"instrument estimate failed: {result}")
    contract = result.get("contract", {})
    if contract.get("instrument_id") != instrument:
        raise SimNowTestError(f"unexpected contract estimate result: {result}")


def get_last_tick(instrument: str) -> dict[str, Any]:
    payload = api_get(f"/api/ticks?instruments={instrument}&limit=1")
    ticks = list_payload(payload)
    if not ticks:
        raise SimNowTestError(f"no tick available for {instrument}")
    return ticks[0]


def choose_far_limit_price(instrument: str, fallback: float) -> float:
    tick = get_last_tick(instrument)
    lower = float(tick.get("lower_limit") or 0)
    upper = float(tick.get("upper_limit") or 0)
    last = float(tick.get("last_price") or fallback)
    if lower > 0:
        return lower
    return max(1.0, min(last, fallback))


def wait_for_active_order(order_ref: str, timeout: float = 8.0) -> dict[str, Any]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        active = list_payload(api_get(f"/api/active_orders?account_id={ACCOUNT_ID}"))
        for order in active:
            if str(order.get("order_ref")) == order_ref:
                return order
        time.sleep(0.1)
    raise SimNowTestError(f"order ref={order_ref} did not become active")


def wait_for_cancelled_order(order_ref: str, instrument: str, timeout: float = 10.0) -> dict[str, Any]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        orders = list_payload(api_get(f"/api/orders/history?account_id={ACCOUNT_ID}&limit=80"))
        for order in orders:
            if str(order.get("order_ref")) != order_ref:
                continue
            if order.get("instrument") != instrument:
                continue
            status = int(order.get("status", -1))
            traded = int(order.get("traded_volume", -1))
            msg = str(order.get("status_msg", ""))
            if status == 4 or "找不到合约" in msg or "not found" in msg.lower():
                raise SimNowTestError(f"counter rejected order ref={order_ref}: {order}")
            if status == 3 and traded == 0:
                return order
        time.sleep(0.1)
    raise SimNowTestError(f"order ref={order_ref} did not reach cancelled state")


def send_order(
    instrument: str,
    price: float,
    direction: str = "Buy",
    offset: str = "Open",
    volume: int = 1,
    price_type: str = "Limit",
) -> str:
    order_result = api_post(
        "/api/order",
        {
            "confirm": "true",
            "account_id": ACCOUNT_ID,
            "instrument": instrument,
            "direction": direction,
            "offset": offset,
            "price": price,
            "volume": volume,
            "price_type": price_type,
        },
    )
    if not order_result.get("ok"):
        raise SimNowTestError(f"order API rejected: {order_result}")
    order_ref = str(order_result.get("order_ref", ""))
    if not order_ref:
        raise SimNowTestError(f"missing order_ref: {order_result}")
    return order_ref


def test_connection() -> None:
    status = wait_for_engine_ready()
    print(
        "[PASS] connection "
        f"engine={status.get('engine_state')} gateway={status.get('gateway_state')} "
        f"td={status.get('td_gateway_logged_in')} md={status.get('md_gateway_logged_in')}"
    )


def test_market_snapshot(instrument: str) -> None:
    tick = get_last_tick(instrument)
    if tick.get("instrument") != instrument and tick.get("instrument_id") != instrument:
        raise SimNowTestError(f"unexpected tick instrument: {tick}")
    print(
        "[PASS] market_snapshot "
        f"instrument={instrument} last={tick.get('last_price')} bid1={tick.get('bid1')} ask1={tick.get('ask1')}"
    )


def test_order_cancel_cycle(instrument: str, price: float, cycles: int) -> list[float]:
    assert_contract_is_known(instrument, price)

    latencies_ms: list[float] = []
    for seq in range(1, cycles + 1):
        send_started = time.perf_counter()
        order_ref = send_order(instrument, price, "Buy", "Open", 1, "Limit")
        active_order = wait_for_active_order(order_ref)
        cancel_result = api_post(
            "/api/cancel_order",
            {"confirm": "true", "account_id": ACCOUNT_ID, "order_ref": order_ref},
        )
        if not cancel_result.get("ok"):
            raise SimNowTestError(f"cancel API rejected ref={order_ref}: {cancel_result}")

        final_order = wait_for_cancelled_order(order_ref, instrument)
        elapsed_ms = (time.perf_counter() - send_started) * 1000.0
        latencies_ms.append(elapsed_ms)
        print(
            f"[PASS] cycle={seq} ref={order_ref} active_status={active_order.get('status')} "
            f"final_status={final_order.get('status')} elapsed_ms={elapsed_ms:.2f}"
        )
        time.sleep(0.3)
    return latencies_ms


def test_order_type_acceptance(instrument: str, price: float, include_market: bool) -> None:
    estimate = api_post(
        "/api/order/estimate",
        {
            "account_id": ACCOUNT_ID,
            "instrument": instrument,
            "direction": "Buy",
            "offset": "Open",
            "price": price,
            "volume": 1,
        },
    )
    if not estimate.get("ok"):
        raise SimNowTestError(f"estimate failed before order type tests: {estimate}")
    print("[PASS] order_estimate")

    fak_ref = send_order(instrument, price, "Buy", "Open", 1, "Fak")
    print(f"[PASS] fak_submitted ref={fak_ref}")
    time.sleep(1.0)

    if include_market:
        market_ref = send_order(instrument, 0, "Buy", "Open", 1, "Market")
        print(f"[PASS] market_submitted ref={market_ref}")
        time.sleep(1.0)


def test_conditional_order(instrument: str, price: float) -> None:
    key = f"simnow-regression-{int(time.time() * 1000)}"
    result = api_post(
        "/api/conditional_orders/create",
        {
            "confirm": "true",
            "account_id": ACCOUNT_ID,
            "instrument": instrument,
            "direction": "Buy",
            "offset": "Open",
            "trigger_price": price,
            "order_price": price,
            "volume": 1,
            "trigger_type": "take_profit",
            "idempotency_key": key,
        },
    )
    if not result.get("ok"):
        raise SimNowTestError(f"conditional create failed: {result}")
    cond_id = result.get("id")
    active = list_payload(api_get(f"/api/conditional_orders?account_id={ACCOUNT_ID}"))
    if not any(str(o.get("id")) == str(cond_id) for o in active):
        raise SimNowTestError(f"conditional order not visible after create id={cond_id}: {active}")
    cancel = api_post("/api/conditional_orders/cancel", json_body={"id": cond_id})
    if not cancel.get("ok"):
        raise SimNowTestError(f"conditional cancel failed id={cond_id}: {cancel}")
    print(f"[PASS] conditional_create_cancel id={cond_id}")


def test_strategy_api() -> None:
    strategies = api_get("/api/strategies")
    items = strategies if isinstance(strategies, list) else strategies.get("strategies") or strategies.get("items") or []
    if not isinstance(items, list):
        raise SimNowTestError(f"unexpected strategies payload: {strategies}")
    strategy_id = "demo_main"
    if items:
        strategy_id = str(items[0].get("id") or items[0].get("strategy_id") or strategy_id)

    perf = api_get(f"/api/strategy/performance?strategy_id={strategy_id}")
    if not isinstance(perf, list):
        raise SimNowTestError(f"unexpected strategy performance payload: {perf}")

    start = api_post("/api/strategy/start", json_body={"strategy_id": strategy_id})
    if not start.get("ok"):
        raise SimNowTestError(f"strategy start failed: {start}")
    pause = api_post("/api/strategy/pause", json_body={"strategy_id": strategy_id})
    if not pause.get("ok"):
        raise SimNowTestError(f"strategy pause failed: {pause}")
    stop = api_post("/api/strategy/stop", json_body={"strategy_id": strategy_id})
    if not stop.get("ok"):
        raise SimNowTestError(f"strategy stop failed: {stop}")

    status = api_get("/api/status")
    python_supported = bool(status.get("python_supported"))
    print(f"[PASS] strategy_lifecycle strategy_id={strategy_id} python_supported={python_supported}")
    if not python_supported:
        print("[WARN] python strategy runtime is disabled in this binary; Python strategies are not fully usable")


def test_flat_reverse_if_position(instrument: str, price: float) -> None:
    positions = list_payload(api_get(f"/api/positions?account_id={ACCOUNT_ID}"))
    matched = [p for p in positions if p.get("instrument") == instrument and int(p.get("total", 0)) > 0]
    if not matched:
        print(f"[SKIP] flat_reverse no existing position for {instrument}")
        return
    pos = matched[0]
    close_direction = "Sell" if str(pos.get("direction")).lower() in ("buy", "long", "多") else "Buy"
    ref = send_order(instrument, price, close_direction, "Close", 1, "Limit")
    print(f"[PASS] flat_order_submitted ref={ref}")
    time.sleep(1.0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--instrument", default="IF2605")
    parser.add_argument("--price", type=float, default=0.0)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--include-market", action="store_true")
    parser.add_argument("--include-flat-reverse", action="store_true")
    parser.add_argument("--skip-order-types", action="store_true")
    parser.add_argument("--skip-strategy", action="store_true")
    args = parser.parse_args()

    try:
        ensure_engine_started()
        price = args.price if args.price > 0 else choose_far_limit_price(args.instrument, 100.0)
        with safety_guard():
            test_connection()
            test_market_snapshot(args.instrument)
            test_conditional_order(args.instrument, price)
            if not args.skip_strategy:
                test_strategy_api()
            if not args.skip_order_types:
                test_order_type_acceptance(args.instrument, price, args.include_market)
            if args.include_flat_reverse:
                test_flat_reverse_if_position(args.instrument, price)
            latencies = test_order_cancel_cycle(args.instrument, price, args.cycles)
        if latencies:
            avg = sum(latencies) / len(latencies)
            p50 = statistics.median(latencies)
            worst = max(latencies)
            print(f"[PASS] all cycles passed avg_elapsed_ms={avg:.2f} p50_ms={p50:.2f} max_ms={worst:.2f}")
        return 0
    except Exception as exc:
        print(f"[FAIL] {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
