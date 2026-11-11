"""Shared pytest helpers: import path, and locating the C++ tools and the data file."""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

ANALYSIS = Path(__file__).resolve().parent
ROOT = ANALYSIS.parent
sys.path.insert(0, str(ANALYSIS))


def find_tool(name: str) -> Path | None:
    """A built C++ tool: $ITCHBT_BUILD_DIR/apps/<name>, else the first preset that has it."""
    override = os.environ.get("ITCHBT_BUILD_DIR")
    candidates = [Path(override)] if override else [
        ROOT / "build" / preset for preset in ("release", "relwithdebinfo", "debug", "asan")]
    for build in candidates:
        tool = build / "apps" / name
        if tool.is_file():
            return tool
    return None


def find_data() -> Path | None:
    """The real ITCH file: $ITCHBT_DATA, else the only file in data/."""
    override = os.environ.get("ITCHBT_DATA")
    if override:
        return Path(override) if Path(override).is_file() else None
    files = sorted((ROOT / "data").glob("*ITCH50")) if (ROOT / "data").is_dir() else []
    return files[0] if files else None


@pytest.fixture(scope="session")
def tool():
    def get(name: str) -> Path:
        path = find_tool(name)
        if path is None:
            pytest.skip(f"{name} is not built; run cmake --build --preset release")
        return path
    return get


@pytest.fixture(scope="session")
def data_file() -> Path:
    path = find_data()
    if path is None:
        pytest.skip("no ITCH data file; run scripts/fetch_data.sh")
    return path


MAKER_CONFIG = """
strategy = "passive_maker"
symbols = ["AAA", "BBB"]
build_all_books = true

[latency]
order_ns = 2000
cancel_ns = 2000
ack_ns = 1000

[risk]
max_position = 600
max_open_orders = 4
rate_limit_per_second = 50000
rate_limit_burst = 5

[passive_maker]
lot = 100
max_inventory = 400
"""


@pytest.fixture(scope="session")
def synthetic_run(tmp_path_factory):
    """A small PassiveMaker run on a random synthetic session, as a run directory path.

    Also returns the input and config paths so tests can run it again.
    """
    import random
    import subprocess

    sys.path.insert(0, str(ANALYSIS / "tests"))
    import itch_fixture as fx

    backtest = find_tool("backtest")
    if backtest is None:
        pytest.skip("backtest is not built; run cmake --build --preset release")
    work = tmp_path_factory.mktemp("synthetic")
    data = work / "session.itch"
    data.write_bytes(fx.random_stream(random.Random(5), ["AAA", "BBB"], events=60_000,
                                      session=True, step_ns=1_000))
    config = work / "maker.toml"
    config.write_text(MAKER_CONFIG)
    out = work / "run"
    result = subprocess.run([str(backtest), "--input", str(data), "--config", str(config),
                             "--out", str(out)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    return {"run": out, "input": data, "config": config, "backtest": backtest, "work": work}
