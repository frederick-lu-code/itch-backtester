"""Loads a run directory written by `backtest` into pandas DataFrames."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import pandas as pd

import schema


@dataclass
class Run:
    """One backtest run: its metadata and its four record files."""

    path: Path
    meta: dict
    orders: pd.DataFrame
    fills: pd.DataFrame
    bbo: pd.DataFrame
    risk: pd.DataFrame

    @property
    def symbols(self) -> dict[int, str]:
        """Stock locate -> symbol, for the symbols the run traded."""
        return {s["locate"]: s["symbol"] for s in self.meta["symbols"]}


def read_records(path: str | Path, name: str) -> np.ndarray:
    """Reads one kind of record from a run directory as a structured array.

    A sharded run keeps each shard's files under shard-<k>/. Their records are concatenated
    and stably sorted by time: every symbol lives in exactly one shard, so each symbol's
    records keep their original order.
    """
    shards = sorted(Path(path).glob("shard-*"), key=lambda p: int(p.name.split("-")[1]))
    if not shards:
        return _read_file(Path(path), name)
    merged = np.concatenate([_read_file(shard, name) for shard in shards])
    return merged[np.argsort(merged["ts"], kind="stable")]


def _read_file(path: Path, name: str) -> np.ndarray:
    """Reads one record file, checking its header first."""
    file_name, type_id, dtype = schema.RECORDS[name]
    path = path / file_name
    size = path.stat().st_size
    header_size = schema.FILE_HEADER.itemsize
    if size < header_size:
        raise ValueError(f"{path}: too short to hold a header")
    header = np.fromfile(path, dtype=schema.FILE_HEADER, count=1)[0]
    if header["magic"] != schema.MAGIC.rstrip(b"\x00"):
        raise ValueError(f"{path}: not a run file (bad magic)")
    if header["version"] != schema.FORMAT_VERSION:
        raise ValueError(f"{path}: format version {header['version']}, "
                         f"this loader reads version {schema.FORMAT_VERSION}")
    if header["record_type"] != type_id or header["record_size"] != dtype.itemsize:
        raise ValueError(f"{path}: header describes record type {header['record_type']} of "
                         f"{header['record_size']} bytes, expected type {type_id} of "
                         f"{dtype.itemsize} bytes")
    if (size - header_size) % dtype.itemsize != 0:
        raise ValueError(f"{path}: ends with a partial record")
    return np.fromfile(path, dtype=dtype, offset=header_size)


def _frame(records: np.ndarray, symbols: dict[int, str]) -> pd.DataFrame:
    names = [n for n in records.dtype.names if n != "pad"]
    frame = pd.DataFrame({n: records[n] for n in names})
    # Timestamps stay integer nanoseconds: float would lose precision late in the day.
    frame["ts"] = frame["ts"].astype("int64")
    frame["symbol"] = frame["locate"].map(symbols).astype("category")
    return frame


def _label(frame: pd.DataFrame, column: str, names: list[str]) -> None:
    frame[column] = pd.Categorical.from_codes(frame[column].to_numpy(), categories=names)


def load_run(path: str | Path) -> Run:
    """Loads run.json and the four record files from a run directory."""
    path = Path(path)
    meta = json.loads((path / "run.json").read_text())
    if meta["format_version"] != schema.FORMAT_VERSION:
        raise ValueError(f"{path}: run.json format version {meta['format_version']}")
    symbols = {s["locate"]: s["symbol"] for s in meta["symbols"]}

    orders = _frame(read_records(path, "orders"), symbols)
    _label(orders, "event", schema.ORDER_EVENT)
    _label(orders, "state", schema.ORDER_STATE)
    _label(orders, "side", schema.SIDE)
    _label(orders, "order_type", schema.ORDER_TYPE)
    _label(orders, "reason", schema.REJECT_REASON)

    fills = _frame(read_records(path, "fills"), symbols)
    _label(fills, "side", schema.SIDE)
    fills["aggressive"] = fills["aggressive"].astype(bool)
    # Signed quantity: positive when the strategy bought.
    fills["signed_qty"] = np.where(fills["side"] == "buy", 1, -1) * fills["qty"].astype("int64")

    bbo = _frame(read_records(path, "bbo"), symbols)
    bbo["has_bid"] = (bbo["flags"] & schema.BBO_HAS_BID) != 0
    bbo["has_ask"] = (bbo["flags"] & schema.BBO_HAS_ASK) != 0
    bbo["tradable"] = (bbo["flags"] & schema.BBO_TRADABLE) != 0
    # Twice the mid, as an integer, where both sides exist. Kept doubled to stay exact.
    bbo["mid2"] = np.where(bbo["has_bid"] & bbo["has_ask"],
                           bbo["bid_price"].astype("int64") + bbo["ask_price"].astype("int64"), 0)

    risk = _frame(read_records(path, "risk"), symbols)
    is_kill = risk["event"] == 1
    risk["reason_name"] = np.where(
        is_kill,
        risk["reason"].map(lambda v: schema.KILL_REASON[v] if v < len(schema.KILL_REASON) else "?"),
        risk["reason"].map(lambda v: schema.REJECT_REASON[v] if v < len(schema.REJECT_REASON) else "?"))
    _label(risk, "event", schema.RISK_EVENT)

    return Run(path=path, meta=meta, orders=orders, fills=fills, bbo=bbo, risk=risk)


def to_parquet(run: Run, out_dir: str | Path) -> list[Path]:
    """Writes each table as a Parquet file and returns the paths."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for name in ("orders", "fills", "bbo", "risk"):
        target = out_dir / f"{name}.parquet"
        getattr(run, name).to_parquet(target, index=False)
        written.append(target)
    return written
