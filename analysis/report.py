"""Post-trade analysis of one run. Each function returns a DataFrame; plotting is separate.

Conventions: timestamps are integer nanoseconds since midnight; prices are integers in units of
1/10000 USD until the last step, where results are converted to dollars or basis points.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

import schema
from load import Run

NS_PER_SECOND = 1_000_000_000


def session_mids(run: Run, symbol: str) -> pd.DataFrame:
    """Two-sided top-of-book changes for one symbol while it could trade: ts, mid2.

    Outside the session, or while halted, the book is thin and its mid is not a fair mark. The
    engine marks positions on exactly these records, so analysis done on them agrees with it.
    """
    bbo = run.bbo
    rows = bbo[(bbo["symbol"] == symbol) & bbo["has_bid"] & bbo["has_ask"] & bbo["tradable"]]
    return rows[["ts", "mid2"]].reset_index(drop=True)


def pnl_timeline(run: Run, symbol: str, step_seconds: float = 1.0) -> pd.DataFrame:
    """Position and PnL for one symbol on a regular time grid.

    PnL is cash plus position marked at the mid, in dollars. The last value matches the
    engine's own figure in run.json (checked by the tests), which is what makes this timeline
    trustworthy rather than merely plausible.
    """
    fills = run.fills[run.fills["symbol"] == symbol].sort_values("ts", kind="stable")
    mids = session_mids(run, symbol)
    if mids.empty:
        return pd.DataFrame(columns=["ts", "position", "pnl_usd", "mid_usd"])
    start = int(mids["ts"].iloc[0])
    end = int(max(mids["ts"].iloc[-1], fills["ts"].max() if len(fills) else 0))
    step = int(step_seconds * NS_PER_SECOND)
    grid = pd.DataFrame({"ts": np.arange(start, end + step, step, dtype="int64")})

    state = pd.DataFrame({
        "ts": fills["ts"].to_numpy(),
        "position": fills["signed_qty"].cumsum().to_numpy(),
        "cash": (-(fills["signed_qty"] * fills["price"].astype("int64"))).cumsum().to_numpy(),
    })
    out = pd.merge_asof(grid, state, on="ts")
    out[["position", "cash"]] = out[["position", "cash"]].fillna(0).astype("int64")
    out = pd.merge_asof(out, mids, on="ts")
    out["mid2"] = out["mid2"].ffill().bfill().astype("int64")
    # 2 x PnL = 2 x cash + position x (bid + ask); halve and scale once, at the end.
    out["pnl_usd"] = (2 * out["cash"] + out["position"] * out["mid2"]) / (2 * schema.PRICE_SCALE)
    out["mid_usd"] = out["mid2"] / (2 * schema.PRICE_SCALE)
    return out[["ts", "position", "pnl_usd", "mid_usd"]]


def pnl_timelines(run: Run, step_seconds: float = 1.0) -> pd.DataFrame:
    """PnL and position for every symbol on one shared time grid, plus the total.

    Columns: ts, total_pnl_usd, and <SYMBOL>_pnl_usd / <SYMBOL>_position for each symbol.
    """
    per_symbol = {symbol: pnl_timeline(run, symbol, step_seconds)
                  for symbol in run.symbols.values()}
    per_symbol = {s: t for s, t in per_symbol.items() if len(t)}
    if not per_symbol:
        return pd.DataFrame(columns=["ts", "total_pnl_usd"])
    step = int(step_seconds * NS_PER_SECOND)
    start = min(int(t["ts"].iloc[0]) for t in per_symbol.values())
    end = max(int(t["ts"].iloc[-1]) for t in per_symbol.values())
    out = pd.DataFrame({"ts": np.arange(start, end + step, step, dtype="int64")})
    out["total_pnl_usd"] = 0.0
    for symbol, timeline in per_symbol.items():
        merged = pd.merge_asof(out[["ts"]], timeline, on="ts")
        # Before a symbol's first mark there is no position and no PnL; after its last, both
        # hold their final values.
        out[f"{symbol}_pnl_usd"] = merged["pnl_usd"].fillna(0.0).to_numpy()
        out[f"{symbol}_position"] = merged["position"].fillna(0).astype("int64").to_numpy()
        out["total_pnl_usd"] += out[f"{symbol}_pnl_usd"]
    return out


def final_pnl(run: Run) -> pd.DataFrame:
    """Per-symbol end-of-run position and PnL recomputed from fills and marks alone.

    Returns integers in the engine's own units (price units x shares, PnL doubled) so they can
    be compared exactly with run.json.
    """
    rows = []
    for symbol in run.symbols.values():
        fills = run.fills[run.fills["symbol"] == symbol].sort_values("ts", kind="stable")
        position = 0
        cost = 0  # signed cost of the open position
        cash = 0
        for qty, price in zip(fills["signed_qty"].tolist(), fills["price"].tolist()):
            if position == 0 or (position > 0) == (qty > 0):
                cost += qty * price
            elif abs(qty) <= abs(position):
                # Truncating division, as C++ does it.
                removed = abs(cost) * abs(qty) // abs(position)
                cost -= removed if cost >= 0 else -removed
            else:
                cost = (position + qty) * price
            position += qty
            cash -= qty * price
            if position == 0:
                cost = 0
        mids = session_mids(run, symbol)
        # The engine marks at the last tradable two-sided top of book, or at the first fill
        # price if it never saw one.
        mark2 = int(mids["mid2"].iloc[-1]) if len(mids) else (
            2 * int(fills["price"].iloc[0]) if len(fills) else 0)
        rows.append({
            "symbol": symbol,
            "position": position,
            "realised": cash + cost,
            "unrealised_x2": position * mark2 - 2 * cost,
            "fills": len(fills),
        })
    return pd.DataFrame(rows)


def order_summary(run: Run) -> pd.DataFrame:
    """One row per order: what was sent and how it ended."""
    orders = run.orders
    submitted = orders[orders["event"] == "submitted"].set_index("order_id")
    summary = submitted[["ts", "symbol", "side", "order_type", "price", "qty"]].rename(
        columns={"ts": "submit_ts"})

    accepted = orders[orders["event"] == "accepted"].set_index("order_id")
    summary["accept_ts"] = accepted["ts"]
    summary["shares_ahead"] = accepted["shares_ahead"]

    fills = orders[orders["event"] == "fill"].groupby("order_id")
    summary["filled_qty"] = fills["qty"].sum()
    summary["first_fill_ts"] = fills["ts"].min()
    summary["filled_qty"] = summary["filled_qty"].fillna(0).astype("int64")

    last = orders.groupby("order_id")["state"].last()
    summary["final_state"] = last
    rejected = orders[orders["event"] == "rejected"].set_index("order_id")
    summary["reject_reason"] = rejected["reason"]
    return summary.reset_index()


def fill_stats(run: Run) -> dict:
    """Fill rate and time from live to first fill, for limit orders that went live."""
    summary = order_summary(run)
    live = summary[summary["accept_ts"].notna()]
    filled = live[live["filled_qty"] > 0]
    wait_ns = (filled["first_fill_ts"] - filled["accept_ts"]).astype("float64")
    return {
        "orders_submitted": int(len(summary)),
        "orders_live": int(len(live)),
        "orders_with_a_fill": int(len(filled)),
        "fill_rate": float(len(filled) / len(live)) if len(live) else float("nan"),
        "share_fill_rate": float(live["filled_qty"].sum() / live["qty"].sum())
        if len(live) else float("nan"),
        "time_to_first_fill_ms": (wait_ns / 1e6).describe(
            percentiles=[0.5, 0.9, 0.99]).to_dict() if len(filled) else {},
    }


def time_to_first_fill(run: Run) -> pd.Series:
    """Seconds from an order going live to its first fill, one value per filled order."""
    summary = order_summary(run)
    filled = summary[summary["accept_ts"].notna() & (summary["filled_qty"] > 0)]
    return ((filled["first_fill_ts"] - filled["accept_ts"]) / NS_PER_SECOND).rename("seconds")


def markouts(run: Run, horizons_s: tuple[float, ...] = (1, 10, 60)) -> pd.DataFrame:
    """Mid-price move after each fill, signed so that positive is good for the strategy.

    For a buy: mid at fill time + h minus the fill price. For a sell: the reverse. In basis
    points of the fill price. Fills too close to the end of the session to have a mid at
    + h are left as NaN for that horizon rather than marked at a stale price.
    """
    frames = []
    for symbol in run.symbols.values():
        fills = run.fills[run.fills["symbol"] == symbol].sort_values("ts", kind="stable")
        mids = session_mids(run, symbol)
        if fills.empty or mids.empty:
            continue
        out = fills[["ts", "symbol", "side", "price", "qty", "aggressive"]].copy()
        last_mid_ts = int(mids["ts"].iloc[-1])
        for h in horizons_s:
            at = pd.DataFrame({"ts": out["ts"].to_numpy() + int(h * NS_PER_SECOND)})
            later = pd.merge_asof(at, mids, on="ts")
            move2 = later["mid2"].to_numpy() - 2 * out["price"].to_numpy().astype("int64")
            sign = np.where(out["side"].to_numpy() == "buy", 1, -1)
            bps = sign * move2 / (2 * out["price"].to_numpy()) * 1e4
            bps[at["ts"].to_numpy() > last_mid_ts] = np.nan
            out[f"markout_{h:g}s_bps"] = bps
        frames.append(out)
    if not frames:
        return pd.DataFrame()
    return pd.concat(frames, ignore_index=True)


def markout_summary(run: Run, horizons_s: tuple[float, ...] = (1, 10, 60)) -> pd.DataFrame:
    """Share-weighted mean markout per horizon, split by passive and aggressive fills."""
    table = markouts(run, horizons_s)
    if table.empty:
        return table
    rows = []
    for aggressive, group in table.groupby("aggressive"):
        row = {"fills": "aggressive" if aggressive else "passive", "count": len(group)}
        for h in horizons_s:
            column = f"markout_{h:g}s_bps"
            valid = group[group[column].notna()]
            row[column] = (np.average(valid[column], weights=valid["qty"])
                           if len(valid) else float("nan"))
        rows.append(row)
    return pd.DataFrame(rows)


def queue_vs_outcome(run: Run, buckets: int = 5) -> pd.DataFrame:
    """Fill probability of limit orders by how many shares were ahead of them on arrival."""
    summary = order_summary(run)
    live = summary[summary["accept_ts"].notna()].copy()
    if live.empty:
        return pd.DataFrame()
    live["filled"] = live["filled_qty"] > 0
    try:
        live["queue_bucket"] = pd.qcut(live["shares_ahead"], q=buckets, duplicates="drop")
    except ValueError:  # every order saw the same queue: one bucket
        live["queue_bucket"] = "all"
    grouped = live.groupby("queue_bucket", observed=True)
    return grouped.agg(orders=("filled", "size"), fill_probability=("filled", "mean"),
                       median_shares_ahead=("shares_ahead", "median")).reset_index()


def rejects_by_reason(run: Run) -> pd.DataFrame:
    """Risk rejections counted by reason code."""
    rejects = run.risk[run.risk["event"] == "reject"]
    counts = rejects["reason_name"].value_counts().rename_axis("reason").reset_index(name="count")
    return counts


def kill_switch_events(run: Run) -> pd.DataFrame:
    """Kill-switch trips: when, why, and total PnL at that moment in dollars."""
    trips = run.risk[run.risk["event"] == "kill_switch"]
    return pd.DataFrame({
        "ts": trips["ts"],
        "reason": trips["reason_name"],
        "total_pnl_usd": trips["value"] / (2 * schema.PRICE_SCALE),
    })


def format_time(ts: int) -> str:
    """HH:MM:SS.mmm for a timestamp in nanoseconds since midnight."""
    seconds, nanos = divmod(int(ts), NS_PER_SECOND)
    return f"{seconds // 3600:02d}:{seconds // 60 % 60:02d}:{seconds % 60:02d}.{nanos // 1_000_000:03d}"
