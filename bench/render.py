#!/usr/bin/env python3
"""Regenerates the generated tables in BENCHMARKS.md from raw results.

Usage: python3 bench/render.py [bench/results/<dir> ...]

With no arguments it uses every directory under bench/results. Each table lives between
`<!-- BEGIN GENERATED: name -->` and `<!-- END GENERATED: name -->` markers, so numbers in
BENCHMARKS.md are never typed by hand. Standard library only.
"""

from __future__ import annotations

import json
import re
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "bench" / "results"
TARGET = ROOT / "BENCHMARKS.md"


def load(directory: Path) -> tuple[dict, list[dict]]:
    sysinfo = json.loads((directory / "sysinfo.json").read_text())
    results = []
    for path in sorted(directory.glob("*.json")):
        if path.name == "sysinfo.json":
            continue
        data = json.loads(path.read_text())
        data["_file"] = path.name
        results.append(data)
    return sysinfo, results


def rate(result: dict, key: str, scale: float) -> str:
    """Median (min to max) of result[key] per second across runs, divided by scale."""
    runs = result.get("run_seconds") or []
    if not runs or not result.get(key):
        return "n/a"
    per_run = sorted(result[key] / s / scale for s in runs)
    return f"{statistics.median(per_run):.1f} ({per_run[0]:.1f} to {per_run[-1]:.1f})"


def seconds(result: dict) -> str:
    runs = sorted(result.get("run_seconds") or [])
    if not runs:
        return "n/a"
    return f"{statistics.median(runs):.2f} ({runs[0]:.2f} to {runs[-1]:.2f})"


def machine_table(directory: Path, sysinfo: dict) -> str:
    rows = [
        ("Results directory", f"`{directory.relative_to(ROOT)}`"),
        ("Commit", f"`{sysinfo.get('git_commit', '?')[:12]}`"
         + (" (dirty tree)" if sysinfo.get("git_dirty") == "yes" else "")),
        ("CPU", sysinfo.get("cpu_model", "?")),
        ("Cores", cores(sysinfo)),
        ("Memory", f"{int(sysinfo.get('memory_kb', 0)) / 1024 / 1024:.0f} GB"),
        ("OS", f"{sysinfo.get('os', '?')} {sysinfo.get('os_version', sysinfo.get('kernel', ''))}"
         f" ({sysinfo.get('arch', '?')})"),
        ("Compiler", sysinfo.get("compiler", "?")),
        ("CPU governor", sysinfo.get("governor", "?")),
        ("Transparent huge pages", sysinfo.get("transparent_hugepage", "?")),
        ("Power", sysinfo.get("power", "n/a")
         + {"1": ", Low Power Mode ON", "0": ", Low Power Mode off"}.get(
             sysinfo.get("low_power_mode", ""), "")),
        ("Date (UTC)", sysinfo.get("date_utc", "?")),
    ]
    lines = ["| Item | Value |", "| --- | --- |"]
    lines += [f"| {k} | {v} |" for k, v in rows]
    return "\n".join(lines)


def cores(sysinfo: dict) -> str:
    if "performance_cores" in sysinfo:
        return (f"{sysinfo['performance_cores']} performance + "
                f"{sysinfo.get('efficiency_cores', '?')} efficiency")
    return str(sysinfo.get("logical_cpus", "?")) + " logical"


def throughput_table(results: list[dict], benchmark: str) -> str | None:
    rows = [r for r in results if r.get("benchmark") == benchmark]
    if not rows:
        return None
    lines = [
        "| Variant | Messages | Million messages/s, median (min to max) | GB/s, median (min to max) "
        "| Seconds, median (min to max) | Runs |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for r in rows:
        lines.append(
            f"| {r['variant']} | {r['messages']:,} | {rate(r, 'messages', 1e6)} "
            f"| {rate(r, 'bytes', 1e9)} | {seconds(r)} | {len(r['run_seconds'])} |")
    return "\n".join(lines)


def replace_block(text: str, name: str, body: str) -> str:
    pattern = re.compile(
        rf"(<!-- BEGIN GENERATED: {re.escape(name)} -->\n).*?(<!-- END GENERATED: {re.escape(name)} -->)",
        re.S)
    if not pattern.search(text):
        print(f"warning: no '{name}' block in {TARGET.name}", file=sys.stderr)
        return text
    return pattern.sub(lambda m: m.group(1) + body + "\n" + m.group(2), text)


def main(argv: list[str]) -> int:
    directories = [Path(a).resolve() for a in argv] or sorted(
        d for d in RESULTS.iterdir() if d.is_dir())
    if not directories:
        print("no result directories found", file=sys.stderr)
        return 1
    # Later directories win, so the tables always show the most recent run of each benchmark.
    merged: dict[tuple[str, str], dict] = {}
    sysinfo: dict = {}
    latest = directories[-1]
    for directory in directories:
        sysinfo, results = load(directory)
        for r in results:
            r["_dir"] = directory.name
            merged[(r.get("benchmark", "?"), r.get("variant", "?"))] = r
    results = list(merged.values())

    text = TARGET.read_text()
    text = replace_block(text, "machine", machine_table(latest, sysinfo))
    for benchmark in sorted({r.get("benchmark", "?") for r in results}):
        table = throughput_table(results, benchmark)
        if table:
            text = replace_block(text, benchmark, table)
    TARGET.write_text(text)
    print(f"updated {TARGET.relative_to(ROOT)} from {len(directories)} result directories")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
