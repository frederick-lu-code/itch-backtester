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


@pytest.fixture
def tool():
    def get(name: str) -> Path:
        path = find_tool(name)
        if path is None:
            pytest.skip(f"{name} is not built; run cmake --build --preset release")
        return path
    return get


@pytest.fixture
def data_file() -> Path:
    path = find_data()
    if path is None:
        pytest.skip("no ITCH data file; run scripts/fetch_data.sh")
    return path
