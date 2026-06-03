"""
hft_sdk - Python SDK for HFT Framework strategy development.

Provides typed enums, dataclass wrappers, and utility helpers so that
Python strategies get IDE autocompletion and catch typos at import time
instead of at runtime.

Usage in a strategy file:
    from hft_sdk import Direction, Offset, OrderStatus, ConditionType, moving_average
"""

from enum import IntEnum
from typing import List, Optional


# ── Enums mirroring C++ types.h ──────────────────────────────────────

class Direction(IntEnum):
    Long = 0
    Short = 1

class Offset(IntEnum):
    Open = 0
    Close = 1
    CloseToday = 2
    CloseYesterday = 3

class OrderStatus(IntEnum):
    """Maps to C++ OrderStatus after to_legacy_status() mapping.
    Pending covers: Created, CancelPending, Submitted, Pending.
    Rejected covers: RiskRejected, Error.
    """
    Pending = 0
    PartiallyFilled = 1
    Filled = 2
    Cancelled = 3
    Rejected = 4

class ConditionType(IntEnum):
    StopLoss = 0
    TakeProfit = 1
    TrailingStop = 2

class Exchange(IntEnum):
    SHFE = 0
    DCE = 1
    CZCE = 2
    CFFEX = 3
    INE = 4
    GFEX = 5
    Unknown = 99


# ── Helpers ──────────────────────────────────────────────────────────

def safe_float(value, default: float = 0.0) -> float:
    """Safely convert a value to float, returning *default* on failure."""
    try:
        v = float(value)
        return v if v == v else default  # NaN check
    except (TypeError, ValueError):
        return default


def safe_int(value, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def moving_average(prices: List[float], period: int) -> Optional[float]:
    """Simple moving average over the last *period* values."""
    if len(prices) < period or period <= 0:
        return None
    window = prices[-period:]
    return sum(window) / period


def exponential_moving_average(prices: List[float], period: int) -> Optional[float]:
    """Exponential moving average over all values with the given span."""
    if not prices or period <= 0:
        return None
    k = 2.0 / (period + 1)
    ema = float(prices[0])
    for price in prices[1:]:
        ema = price * k + ema * (1.0 - k)
    return ema


def tick_direction_text(direction: int) -> str:
    return "多" if direction == Direction.Long else "空"


def tick_offset_text(offset: int) -> str:
    mapping = {
        Offset.Open: "开仓",
        Offset.Close: "平仓",
        Offset.CloseToday: "平今",
        Offset.CloseYesterday: "平昨",
    }
    return mapping.get(offset, "未知")


def order_status_text(status: int) -> str:
    mapping = {
        OrderStatus.Pending: "待成交",
        OrderStatus.PartiallyFilled: "部分成交",
        OrderStatus.Filled: "全部成交",
        OrderStatus.Cancelled: "已撤单",
        OrderStatus.Rejected: "已拒绝",
    }
    return mapping.get(status, "未知")


# ── Strategy logging helper ──────────────────────────────────────────

import time as _time

def strategy_log(strategy_id: str, level: str, message: str) -> None:
    """Print a structured log line with timestamp and strategy ID."""
    ts = _time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{ts}][{level.upper()}][strategy={strategy_id}] {message}")
