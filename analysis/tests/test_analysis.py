"""Loader and report functions against a real (synthetic-input) run from the C++ engine."""

from __future__ import annotations

import filecmp
import json
import math
import subprocess

import numpy as np
import pandas as pd
import pytest

import load
import report
import schema


@pytest.fixture(scope="module")
def run(synthetic_run):
    return load.load_run(synthetic_run["run"])


def test_run_actually_traded(run):
    # The checks below mean little on an empty run.
    assert run.meta["stats"]["fills"] > 50
    assert run.meta["stats"]["cancels_requested"] > 50
    assert run.meta["stats"]["orders_rejected_by_risk"] > 0
    assert run.meta["stats"]["events_dropped"] == 0
    assert run.symbols == {10: "AAA", 11: "BBB"}


def test_record_counts_match_run_json(run):
    for name in ("orders", "fills", "bbo", "risk"):
        assert len(getattr(run, name)) == run.meta["records"][name], name
    assert len(run.fills) == run.meta["stats"]["fills"]
    assert int(run.fills["qty"].sum()) == run.meta["stats"]["shares_filled"]


def test_records_are_in_time_order(run):
    for name in ("orders", "fills", "bbo", "risk"):
        ts = getattr(run, name)["ts"].to_numpy()
        assert np.all(np.diff(ts) >= 0), name


def test_every_order_has_a_coherent_life(run):
    summary = report.order_summary(run)
    assert len(summary) == run.meta["stats"]["orders_submitted"]
    assert summary["order_id"].is_unique
    terminal = {"filled", "cancelled", "rejected"}
    assert set(summary["final_state"].astype(str)) <= terminal  # day orders all end
    filled = summary[summary["final_state"] == "filled"]
    assert (filled["filled_qty"] == filled["qty"]).all()
    assert (summary["filled_qty"] <= summary["qty"]).all()
    rejected = summary[summary["final_state"] == "rejected"]
    assert (rejected["filled_qty"] == 0).all()
    assert rejected["reject_reason"].notna().all()
    # Latency: nothing is accepted before it was sent plus the 2000 ns order latency.
    live = summary[summary["accept_ts"].notna()]
    assert ((live["accept_ts"] - live["submit_ts"]) == 2000).all()


def test_order_ids_carry_the_locate(run):
    assert ((run.orders["order_id"] // 2**40) == run.orders["locate"]).all()


def test_fill_records_agree_with_order_events(run):
    events = run.orders[run.orders["event"] == "fill"]
    assert len(events) == len(run.fills)
    assert (events["qty"].to_numpy() == run.fills["qty"].to_numpy()).all()
    assert (events["order_id"].to_numpy() == run.fills["order_id"].to_numpy()).all()
    passive = run.fills[~run.fills["aggressive"]]
    assert (passive["price"] == passive["order_price"]).all()  # resting orders fill at their limit


def test_pnl_recomputed_from_fills_matches_the_engine_exactly(run):
    recomputed = report.final_pnl(run).set_index("symbol")
    engine = {s["symbol"]: s for s in run.meta["risk"]["symbols"]}
    assert set(recomputed.index) == set(engine)
    total_x2 = 0
    for symbol, row in recomputed.iterrows():
        assert row["position"] == engine[symbol]["position"], symbol
        assert row["realised"] == engine[symbol]["realised"], symbol
        assert row["unrealised_x2"] == engine[symbol]["unrealised_x2"], symbol
        total_x2 += 2 * row["realised"] + row["unrealised_x2"]
    assert total_x2 == run.meta["risk"]["total_pnl_x2"]


def test_pnl_timeline_ends_at_the_engines_figure(run):
    for entry in run.meta["risk"]["symbols"]:
        timeline = report.pnl_timeline(run, entry["symbol"], step_seconds=0.001)
        assert len(timeline) > 10
        assert timeline["position"].iloc[-1] == entry["position"]
        engine_pnl = (2 * entry["realised"] + entry["unrealised_x2"]) / (2 * schema.PRICE_SCALE)
        assert timeline["pnl_usd"].iloc[-1] == pytest.approx(engine_pnl, abs=1e-6)
        assert np.all(np.diff(timeline["ts"]) > 0)


def test_fill_stats_are_consistent(run):
    stats = report.fill_stats(run)
    assert stats["orders_submitted"] == run.meta["stats"]["orders_submitted"]
    assert 0 < stats["fill_rate"] <= 1
    assert 0 < stats["share_fill_rate"] <= 1
    assert stats["orders_with_a_fill"] <= stats["orders_live"] <= stats["orders_submitted"]
    waits = report.time_to_first_fill(run)
    assert len(waits) == stats["orders_with_a_fill"]
    assert (waits >= 0).all()


def test_markouts_are_signed_by_side(run):
    table = report.markouts(run, horizons_s=(0.00001, 0.0001))
    assert len(table) == len(run.fills)
    column = "markout_1e-05s_bps"
    assert column in table.columns
    # Check one buy and one sell by hand against the mids.
    for side, sign in (("buy", 1), ("sell", -1)):
        row = table[(table["side"] == side) & table[column].notna()].iloc[0]
        mids = report.session_mids(run, row["symbol"])
        later = mids[mids["ts"] <= row["ts"] + 10_000]["mid2"].iloc[-1]
        expected = sign * (later - 2 * row["price"]) / (2 * row["price"]) * 1e4
        assert row[column] == pytest.approx(expected)
    summary = report.markout_summary(run, horizons_s=(0.00001, 0.0001))
    assert set(summary["fills"]) <= {"passive", "aggressive"}
    assert summary["count"].sum() == len(run.fills)


def test_queue_position_table(run):
    table = report.queue_vs_outcome(run)
    assert table["orders"].sum() == report.fill_stats(run)["orders_live"]
    assert ((table["fill_probability"] >= 0) & (table["fill_probability"] <= 1)).all()


def test_rejects_by_reason_matches_risk_records(run):
    table = report.rejects_by_reason(run)
    assert table["count"].sum() == run.meta["stats"]["orders_rejected_by_risk"]
    assert set(table["reason"]) <= set(schema.REJECT_REASON)
    assert "rate_limit" in set(table["reason"])
    assert report.kill_switch_events(run).empty


def test_parquet_export_round_trips(run, tmp_path):
    paths = load.to_parquet(run, tmp_path / "parquet")
    assert [p.name for p in paths] == ["orders.parquet", "fills.parquet", "bbo.parquet",
                                       "risk.parquet"]
    fills = pd.read_parquet(tmp_path / "parquet" / "fills.parquet")
    assert len(fills) == len(run.fills)
    assert fills["price"].tolist() == run.fills["price"].tolist()
    assert fills["ts"].dtype == "int64"


def test_loader_rejects_damaged_files(synthetic_run, tmp_path):
    source = synthetic_run["run"] / "fills.bin"
    data = source.read_bytes()

    def load_with(contents: bytes):
        (tmp_path / "fills.bin").write_bytes(contents)
        return load.read_records(tmp_path, "fills")

    with pytest.raises(ValueError, match="magic"):
        load_with(b"NOTARUN\x00" + data[8:])
    with pytest.raises(ValueError, match="version"):
        load_with(data[:8] + (99).to_bytes(4, "little") + data[12:])
    with pytest.raises(ValueError, match="record type"):
        load_with(data[:12] + (1).to_bytes(4, "little") + data[16:])  # claims to be orders
    with pytest.raises(ValueError, match="partial record"):
        load_with(data[:-5])
    with pytest.raises(ValueError, match="too short"):
        load_with(data[:10])
    assert len(load_with(data)) == len(data[24:]) // schema.FILL.itemsize


def test_two_runs_are_byte_identical(synthetic_run):
    """Determinism at the tool level, including the writer thread: only timing may differ."""
    second = synthetic_run["work"] / "run_again"
    sync = synthetic_run["work"] / "run_sync"
    for out, extra in ((second, []), (sync, ["--recorder", "sync"])):
        result = subprocess.run(
            [str(synthetic_run["backtest"]), "--input", str(synthetic_run["input"]), "--config",
             str(synthetic_run["config"]), "--out", str(out), *extra],
            capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stderr
        for name in ("orders.bin", "fills.bin", "bbo.bin", "risk.bin"):
            assert filecmp.cmp(synthetic_run["run"] / name, out / name, shallow=False), name
        first_meta = json.loads((synthetic_run["run"] / "run.json").read_text())
        other_meta = json.loads((out / "run.json").read_text())
        assert set(first_meta) == set(other_meta)
        for key in first_meta:
            if key != "timing":
                assert first_meta[key] == other_meta[key], key


def test_run_json_records_the_input_digest(synthetic_run):
    import hashlib

    meta = json.loads((synthetic_run["run"] / "run.json").read_text())
    data = synthetic_run["input"].read_bytes()
    assert meta["input"]["size"] == len(data)
    assert meta["input"]["sha256"] == hashlib.sha256(data).hexdigest()
    assert meta["input"]["file"] == "session.itch"
    assert meta["config"]["strategy"] == "passive_maker"
    assert not math.isnan(meta["timing"]["wall_time_s"])


SHARDABLE_CONFIG = """
strategy = "passive_maker"
symbols = ["AAA", "BBB"]

[latency]
order_ns = 2000
cancel_ns = 2000
ack_ns = 1000

[risk]
max_position = 600
max_open_orders = 4

[passive_maker]
max_inventory = 400
"""


def test_sharded_run_matches_single_threaded_per_symbol(synthetic_run, tmp_path):
    config = tmp_path / "shardable.toml"
    config.write_text(SHARDABLE_CONFIG)
    runs = {}
    for name, extra in (("single", []), ("sharded", ["--shards", "2"])):
        out = tmp_path / name
        result = subprocess.run(
            [str(synthetic_run["backtest"]), "--input", str(synthetic_run["input"]), "--config",
             str(config), "--out", str(out), *extra], capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stderr
        runs[name] = load.load_run(out)
    single, sharded = runs["single"], runs["sharded"]
    assert sharded.meta["shards"] == 2 and single.meta["shards"] == 1
    assert (tmp_path / "sharded" / "shard-1" / "fills.bin").is_file()
    assert single.meta["stats"]["fills"] > 50

    # Engine totals and per-symbol risk agree exactly.
    for key in ("messages", "orders_submitted", "fills", "shares_filled", "cancels_requested",
                "book_updates", "trades"):
        assert single.meta["stats"][key] == sharded.meta["stats"][key], key
    assert single.meta["risk"] == sharded.meta["risk"]
    assert single.meta["symbols"] == sharded.meta["symbols"]
    assert single.meta["records"] == sharded.meta["records"]

    # And so does every record, symbol by symbol.
    for name in ("orders", "fills", "bbo", "risk"):
        for symbol in ("AAA", "BBB"):
            a = getattr(single, name)
            b = getattr(sharded, name)
            a = a[a["symbol"] == symbol].reset_index(drop=True)
            b = b[b["symbol"] == symbol].reset_index(drop=True)
            pd.testing.assert_frame_equal(a, b, check_categorical=False)

    # The analysis sees no difference either.
    pd.testing.assert_frame_equal(report.final_pnl(single), report.final_pnl(sharded))


def test_sharding_refuses_cross_symbol_limits(synthetic_run, tmp_path):
    result = subprocess.run(
        [str(synthetic_run["backtest"]), "--input", str(synthetic_run["input"]), "--config",
         str(synthetic_run["config"]), "--out", str(tmp_path / "x"), "--shards", "2"],
        capture_output=True, text=True, check=False)
    assert result.returncode == 1
    assert "cannot be used with --shards" in result.stderr
    assert "risk.rate_limit_per_second" in result.stderr
