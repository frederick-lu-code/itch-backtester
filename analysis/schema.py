"""Run file layouts, as NumPy dtypes.

This is the second declaration of each layout; the first is include/itchbt/record/records.hpp.
`analysis/tests/test_schema.py` compares the two field by field (name, offset, size) using
`backtest --print-schema`, so they cannot drift apart unnoticed.

Every file is a 24-byte header followed by fixed-width little-endian records. Prices are
integers in units of 1/10000 USD; timestamps are nanoseconds since midnight.
"""

from __future__ import annotations

import numpy as np

FORMAT_VERSION = 1
MAGIC = b"ITCHBT\x00\x00"

FILE_HEADER = np.dtype([
    ("magic", "S8"),
    ("version", "<u4"),
    ("record_type", "<u4"),
    ("record_size", "<u4"),
    ("reserved", "<u4"),
])

ORDER = np.dtype([
    ("ts", "<u8"),
    ("order_id", "<u8"),
    ("shares_ahead", "<u8"),
    ("price", "<u4"),
    ("qty", "<u4"),
    ("leaves", "<u4"),
    ("locate", "<u2"),
    ("event", "u1"),
    ("state", "u1"),
    ("side", "u1"),
    ("order_type", "u1"),
    ("reason", "u1"),
    ("pad", "V5"),
])

FILL = np.dtype([
    ("ts", "<u8"),
    ("order_id", "<u8"),
    ("shares_ahead_at_arrival", "<u8"),
    ("price", "<u4"),
    ("qty", "<u4"),
    ("leaves", "<u4"),
    ("order_price", "<u4"),
    ("locate", "<u2"),
    ("side", "u1"),
    ("aggressive", "u1"),
    ("pad", "V4"),
])

BBO = np.dtype([
    ("ts", "<u8"),
    ("bid_qty", "<u8"),
    ("ask_qty", "<u8"),
    ("bid_price", "<u4"),
    ("ask_price", "<u4"),
    ("locate", "<u2"),
    ("flags", "u1"),
    ("pad", "V5"),
])

RISK = np.dtype([
    ("ts", "<u8"),
    ("order_id", "<u8"),
    ("value", "<i8"),
    ("locate", "<u2"),
    ("event", "u1"),
    ("reason", "u1"),
    ("pad", "V4"),
])

# name -> (file name, record type id in the header, dtype)
RECORDS = {
    "orders": ("orders.bin", 1, ORDER),
    "fills": ("fills.bin", 2, FILL),
    "bbo": ("bbo.bin", 3, BBO),
    "risk": ("risk.bin", 4, RISK),
}

# Enumerations stored as small integers. Index = stored value.
ORDER_EVENT = ["submitted", "accepted", "fill", "cancel_requested", "cancelled", "rejected",
               "cancel_rejected"]
ORDER_STATE = ["pending_new", "live", "partially_filled", "pending_cancel", "filled",
               "cancelled", "rejected"]
REJECT_REASON = ["none", "kill_switch", "invalid_order", "market_closed", "symbol_halted",
                 "max_order_qty", "max_order_notional", "no_reference_price", "price_collar",
                 "max_position", "max_open_orders", "max_gross_notional", "rate_limit",
                 "event_queue_full", "exchange_closed", "exchange_halted",
                 "exchange_open_order_limit", "too_late_to_cancel"]
SIDE = ["buy", "sell"]
ORDER_TYPE = ["limit", "ioc"]
RISK_EVENT = ["reject", "kill_switch"]
KILL_REASON = ["none", "loss_limit", "reject_rate"]

ENUMS = {
    "order_event": ORDER_EVENT,
    "order_state": ORDER_STATE,
    "reject_reason": REJECT_REASON,
    "side": SIDE,
    "order_type": ORDER_TYPE,
    "risk_event": RISK_EVENT,
    "kill_reason": KILL_REASON,
}

PRICE_SCALE = 10_000          # price units per US dollar
BBO_HAS_BID = 1
BBO_HAS_ASK = 2
BBO_TRADABLE = 4            # session open and symbol not halted: the mid is a fair mark
