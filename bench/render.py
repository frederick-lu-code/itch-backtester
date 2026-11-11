#!/usr/bin/env python3
"""Regenerates the generated tables in BENCHMARKS.md and README.md from raw results.

Usage: python3 bench/render.py [bench/results/<dir> ...]

With no arguments it uses the directories listed in bench/results/CURRENT. Each table lives
between `<!-- BEGIN GENERATED: name -->` and `<!-- END GENERATED: name -->` markers, so numbers
in BENCHMARKS.md are never typed by hand. Standard library only.
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
LOG = ROOT / "bench" / "optimisation_log.json"

MESSAGE_NAMES = {
    "all": "all messages", "A": "A add", "F": "F add (MPID)", "E": "E executed",
    "C": "C executed with price", "X": "X cancel", "D": "D delete", "U": "U replace",
    "P": "P trade", "Q": "Q cross", "I": "I NOII", "L": "L participant position",
    "H": "H trading action", "R": "R directory", "Y": "Y Reg SHO", "S": "S system event",
}


def load(directory: Path) -> tuple[dict, list[dict], dict | None]:
    sysinfo = json.loads((directory / "sysinfo.json").read_text())
    results, micro = [], None
    for path in sorted(directory.glob("*.json")):
        if path.name == "sysinfo.json":
            continue
        data = json.loads(path.read_text())
        if "benchmarks" in data:  # Google Benchmark's own format
            micro = data
        else:
            results.append(data)
    return sysinfo, results, micro


def med(values: list[float]) -> float:
    return statistics.median(values)


def per_second(result: dict, key: str = "messages") -> list[float]:
    return sorted(result[key] / s for s in result["run_seconds"])


def spread(values: list[float], scale: float, digits: int = 1) -> str:
    values = sorted(v / scale for v in values)
    return f"{med(values):.{digits}f} ({values[0]:.{digits}f} to {values[-1]:.{digits}f})"


def table(header: list[str], rows: list[list]) -> str:
    lines = ["| " + " | ".join(header) + " |", "| " + " | ".join("---" for _ in header) + " |"]
    lines += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return "\n".join(lines)


def cores(sysinfo: dict) -> str:
    if "performance_cores" in sysinfo:
        return (f"{sysinfo['performance_cores']} performance + "
                f"{sysinfo.get('efficiency_cores', '?')} efficiency")
    return str(sysinfo.get("logical_cpus", "?")) + " logical"


def machine_table(directory: list[Path], sysinfo: dict) -> str:
    low_power = {"1": ", **Low Power Mode on**", "0": ", Low Power Mode off"}.get(
        sysinfo.get("low_power_mode", ""), "")
    return table(["Item", "Value"], [
        ["Results", ", ".join(f"`{d.relative_to(ROOT)}`" for d in directory)],
        ["Commit", f"`{sysinfo.get('git_commit', '?')[:12]}`"
         + (" (dirty tree)" if sysinfo.get("git_dirty") == "yes" else "")],
        ["CPU", sysinfo.get("cpu_model", "?")],
        ["Cores", cores(sysinfo)],
        ["Memory", f"{int(sysinfo.get('memory_kb', 0)) / 1024 / 1024:.0f} GB"],
        ["OS", f"{sysinfo.get('os', '?')} {sysinfo.get('os_version', sysinfo.get('kernel', ''))}"
         f" ({sysinfo.get('arch', '?')})"],
        ["Compiler", sysinfo.get("compiler", "?")],
        ["CPU governor", sysinfo.get("governor", "?")],
        ["Transparent huge pages", sysinfo.get("transparent_hugepage", "?")],
        ["Power", sysinfo.get("power", "n/a") + low_power],
        ["Date (UTC)", sysinfo.get("date_utc", "?")],
    ])


def throughput_table(rows: list[dict]) -> str:
    return table(
        ["Variant", "Million messages/s, median (min to max)", "GB/s, median (min to max)",
         "Seconds, median (min to max)", "Runs"],
        [[r["variant"], spread(per_second(r), 1e6), spread(per_second(r, "bytes"), 1e9, 2),
          spread(r["run_seconds"], 1, 2), len(r["run_seconds"])] for r in rows])


def book_table(rows: list[dict]) -> str:
    base = next((r for r in rows if r["variant"] == "baseline/reserve"), rows[0])
    base_rate = med(per_second(base))
    return table(
        ["Variant", "Million messages/s, median (min to max)", "Versus baseline/reserve",
         "Seconds, median (min to max)", "Runs", "Book fingerprint"],
        [[r["variant"], spread(per_second(r), 1e6, 2), f"{med(per_second(r)) / base_rate:.2f}x",
          spread(r["run_seconds"], 1, 1), len(r["run_seconds"]),
          (f"`{r['book_fingerprint_low32']:08x}` ({r['fingerprint_checkpoints']} checkpoints)"
           if r.get("fingerprint_checkpoints") else "not recorded")] for r in rows])


def memory_table(rows: list[dict]) -> str:
    return table(
        ["Variant", "Peak RSS (MiB)", "Peak live orders", "Orders added",
         "Minor page faults per run", "Major page faults per run", "Order-store rehashes"],
        [[r["variant"], f"{r.get('peak_rss_bytes', 0) / 2**20:,.0f}",
          f"{r.get('peak_live_orders', 0):,}", f"{r.get('orders_added', 0):,}",
          f"{r.get('minor_page_faults_per_run', 0):,}", f"{r.get('major_page_faults_per_run', 0):,}",
          r.get("order_store_rehashes", "n/a")] for r in rows])


def parse_fault_table(rows: list[dict]) -> str:
    out = []
    for r in rows:
        runs = r.get("runs_including_warmup") or 1
        out.append([r["variant"], f"{r.get('minor_page_faults_total', 0) // runs:,}",
                    f"{r.get('major_page_faults_total', 0) // runs:,}",
                    f"{r.get('peak_rss_bytes', 0) / 2**20:,.0f}"])
    return table(["Variant", "Minor page faults per run", "Major page faults per run",
                  "Peak RSS (MiB)"], out)


def latency_table(rows: list[dict]) -> str:
    out = []
    for r in rows:
        for entry in r["latency"]:
            if entry["count"] < 1000 and entry["type"] != "all":
                continue  # too few samples for a 99.9th percentile to mean anything
            out.append([r["variant"], MESSAGE_NAMES.get(entry["type"], entry["type"]),
                        f"{entry['count'] // len(r['run_seconds']):,}",
                        f"{entry['p50_ns']:.0f}", f"{entry['p90_ns']:.0f}",
                        f"{entry['p99_ns']:.0f}", f"{entry['p999_ns']:.0f}",
                        f"{entry['max_ns'] / 1e6:.2f}"])
    return table(["Variant", "Message", "Count per run", "p50 (ns)", "p90 (ns)", "p99 (ns)",
                  "p99.9 (ns)", "max (ms)"], out)


def timing_overhead_table(latency: list[dict], untimed: list[dict]) -> str:
    out = []
    for r in latency:
        plain = next((u for u in untimed if u["variant"] == r["variant"]), None)
        if plain is None:
            continue
        timed_rate, plain_rate = med(per_second(r)), med(per_second(plain))
        out.append([r["variant"], f"{plain_rate / 1e6:.2f}", f"{timed_rate / 1e6:.2f}",
                    f"{(1e9 / timed_rate) - (1e9 / plain_rate):.0f}",
                    f"{r.get('tick_ns', 0):.3f}"])
    return table(["Variant", "Untimed (M msg/s)", "Timed (M msg/s)",
                  "Timing overhead per message (ns)", "Counter tick (ns)"], out)


def e2e_table(rows: list[dict]) -> str:
    base = next((r for r in rows if r["variant"].endswith("/none")), None)
    out = []
    for r in rows:
        slowdown = (f"{(med(r['run_seconds']) / med(base['run_seconds']) - 1) * 100:+.1f}%"
                    if base else "n/a")
        out.append([r["variant"], spread(r["run_seconds"], 1, 2), spread(per_second(r), 1e6, 2),
                    f"{r['records']:,}", slowdown,
                    f"{med(r['recorder_blocked_records']):,.0f}",
                    f"{med(r['recorder_blocked_ms']):.2f}", len(r["run_seconds"])])
    return table(["Variant", "Wall seconds, median (min to max)",
                  "Million messages/s, median (min to max)", "Records written",
                  "Slowdown versus no recording", "Records that waited on a full ring (median)",
                  "Time waited, ms (median)", "Runs"], out)


def sharding_table(rows: list[dict]) -> str:
    rows = sorted(rows, key=lambda r: r["shards"])
    base = med(per_second(rows[0]))
    return table(
        ["Engines", "Million messages/s, median (min to max)", "Speed-up over 1",
         "Wall seconds, median (min to max)", "Router waits on a full queue (median)", "Runs"],
        [[("1 (single-threaded, no router)" if r["shards"] == 1
           else f"{r['shards']} shards + router thread"),
          spread(per_second(r), 1e6, 2), f"{med(per_second(r)) / base:.2f}x",
          spread(r["run_seconds"], 1, 1), f"{med(r['router_waits']):,.0f}",
          len(r["run_seconds"])] for r in rows])


def micro_rows(micro: dict) -> dict[str, dict]:
    return {b["name"]: b for b in micro["benchmarks"] if b.get("run_type", "iteration") == "iteration"}


def micro_table(micro: dict) -> str:
    out = []
    for name, b in micro_rows(micro).items():
        items = b.get("items_per_second")
        per_item = f"{1e9 / items:.1f}" if items else "n/a"
        label = b.get("label", "")
        if "callbacks_per_run" in b:
            label = (f"{b['messages_per_run']:,.0f} messages, {b['callbacks_per_run']:,.0f} "
                     f"callbacks per run")
        out.append([f"`{name.replace('book::', '').replace('/real_time', '')}`", per_item,
                    f"{items / 1e6:.1f}" if items else "n/a", label])
    return table(["Benchmark", "ns per item", "Million items/s", "Notes"], out)


def dispatch_sentence(micro: dict) -> str:
    rows = micro_rows(micro)
    concept, virtual = rows.get("BM_EngineDispatchConcept"), rows.get("BM_EngineDispatchVirtual")
    if not concept or not virtual:
        return ""
    scale = {"ms": 1e6, "us": 1e3, "ns": 1.0}[concept["time_unit"]]
    delta_ns = (virtual["real_time"] - concept["real_time"]) * scale
    per_callback = delta_ns / concept["callbacks_per_run"]
    return (f"Per replay of the same stream the virtual adapter took "
            f"{delta_ns / 1e6:+.2f} ms relative to the concept-bound strategy, which is "
            f"{per_callback:+.1f} ns per callback over {concept['callbacks_per_run']:,.0f} "
            f"callbacks.")


def log_table(results: list[dict], micro: dict | None, latency: list[dict]) -> str:
    by_key = {(r["benchmark"], r["variant"]): r for r in results}
    p99 = {r["variant"]: next(e["p99_ns"] for e in r["latency"] if e["type"] == "all")
           for r in latency}
    out = []
    for entry in json.loads(LOG.read_text()):
        if entry["benchmark"] == "micro":
            if not micro:
                continue
            rows = micro_rows(micro)
            before, after = rows.get(entry["before"]), rows.get(entry["after"])
            if not before or not after:
                continue
            ratio = before["real_time"] / after["real_time"]
            numbers = (f"{before['real_time']:.2f} to {after['real_time']:.2f} "
                       f"{before['time_unit']} per replay ({ratio:.2f}x). "
                       + dispatch_sentence(micro))
        else:
            before = by_key.get((entry["benchmark"], entry["before"]))
            after = by_key.get((entry["benchmark"], entry["after"]))
            if not before or not after:
                continue
            b, a = med(per_second(before)), med(per_second(after))
            ratio = a / b
            numbers = f"{b / 1e6:.2f} to {a / 1e6:.2f} M msg/s ({ratio:.2f}x)."
            if entry["before"] in p99 and entry["after"] in p99:
                numbers += f" p99 {p99[entry['before']]:.0f} to {p99[entry['after']]:.0f} ns."
            elif entry["after"] in p99:
                numbers += f" p99 after: {p99[entry['after']]:.0f} ns."
        verdict = (entry["verdict_faster"] if ratio > 1.02
                   else entry["verdict_slower"] if ratio < 0.98 else entry["verdict_same"])
        out.append([entry["change"], entry["hypothesis"], numbers, verdict])
    return table(["Change", "Hypothesis", "Before and after", "Verdict"], out)


def targets_table(results: list[dict], latency: list[dict]) -> str:
    by_key = {(r["benchmark"], r["variant"]): r for r in results}

    def rate(benchmark: str, variant: str) -> float | None:
        r = by_key.get((benchmark, variant))
        return med(per_second(r)) if r else None

    parse = rate("parse_only", "mmap/decode")
    parse_read = rate("parse_only", "read/decode")
    book = rate("book_build", "optimised/reserve/prefetch") or rate("book_build", "optimised/reserve")
    base = rate("book_build", "baseline/reserve")
    best = next((r for r in latency if r["variant"].endswith("prefetch")),
                latency[-1] if latency else None)
    p99 = next(e["p99_ns"] for e in best["latency"] if e["type"] == "all") if best else None

    def verdict(value, target, higher=True) -> str:
        if value is None:
            return "not measured"
        return "met" if (value >= target if higher else value <= target) else "**not met**"

    rows = [
        ["Parse only", "50 M msg/s or better",
         (f"{parse / 1e6:.1f} M msg/s mapped, {parse_read / 1e6:.1f} with read()"
          if parse and parse_read else "not measured"),
         verdict(max(parse or 0, parse_read or 0) or None, 50e6)],
        ["Book build, all symbols", "10 M msg/s or better",
         f"{book / 1e6:.2f} M msg/s" if book else "not measured", verdict(book, 10e6)],
        ["Book build, p99 per message", "under 1 microsecond",
         f"{p99:.0f} ns" if p99 else "not measured", verdict(p99, 1000, higher=False)],
        ["Optimised versus baseline book", "at least 3 times the throughput",
         f"{book / base:.2f}x" if book and base else "not measured",
         verdict(book / base if book and base else None, 3.0)],
    ]
    return table(["Benchmark", "Aim", "Measured on this machine", "Result"], rows)


def best_book(results: list[dict]) -> dict | None:
    by_variant = {r["variant"]: r for r in results if r["benchmark"] == "book_build"}
    return by_variant.get("optimised/reserve/prefetch") or by_variant.get("optimised/reserve")


def conditions(sysinfo: dict) -> str:
    power = "on battery" if "Battery" in sysinfo.get("power", "") else "on mains power"
    if sysinfo.get("low_power_mode") == "1":
        power += " in Low Power Mode"
    return f"{sysinfo.get('cpu_model', 'unknown CPU')}, {power}"


def headline(results: list[dict], latency: list[dict], sysinfo: dict) -> str | None:
    book = best_book(results)
    if not book:
        return None
    timed = next((r for r in latency if r["variant"] == book["variant"]), None)
    text = (f"**Headline result:** the order book for all 8,906 symbols is rebuilt from a full "
            f"day of {book['messages'] / 1e6:.0f} million messages at "
            f"**{med(per_second(book)) / 1e6:.1f} million messages per second** on one thread")
    if timed:
        entry = next(e for e in timed["latency"] if e["type"] == "all")
        text += (f", with a median of {entry['p50_ns']:.0f} ns and a 99th percentile of "
                 f"**{entry['p99_ns']:.0f} ns** per message")
    return text + f" ({conditions(sysinfo)})."


def bench_summary(results: list[dict], latency: list[dict], sysinfo: dict) -> str | None:
    by_key = {(r["benchmark"], r["variant"]): r for r in results}

    def rate(benchmark: str, variant: str) -> str:
        r = by_key.get((benchmark, variant))
        return f"{med(per_second(r)) / 1e6:.1f}" if r else "not measured"

    book = best_book(results)
    if not book:
        return None
    base = by_key.get(("book_build", "baseline/reserve"))
    rows = [
        ["Parse only, every field decoded", f"{rate('parse_only', 'mmap/decode')} M msg/s mapped, "
         f"{rate('parse_only', 'read/decode')} with `read()`"],
        ["Book build, all symbols, baseline book", f"{rate('book_build', 'baseline/reserve')} M msg/s"],
        ["Book build, all symbols, optimised book", f"{med(per_second(book)) / 1e6:.1f} M msg/s"
         + (f" ({med(per_second(book)) / med(per_second(base)):.2f}x the baseline)" if base else "")],
    ]
    timed = next((r for r in latency if r["variant"] == book["variant"]), None)
    if timed:
        e = next(x for x in timed["latency"] if x["type"] == "all")
        rows.append(["Book build latency per message",
                     f"p50 {e['p50_ns']:.0f} ns, p99 {e['p99_ns']:.0f} ns, "
                     f"p99.9 {e['p999_ns']:.0f} ns"])
    e2e = by_key.get(("end_to_end", "passive_maker/async"))
    if e2e:
        rows.append(["End-to-end backtest, PassiveMaker on 10 symbols, all books, recording on",
                     f"{med(e2e['run_seconds']):.1f} s wall, "
                     f"{med(per_second(e2e)) / 1e6:.1f} M msg/s"])
    shards = sorted((r for r in results if r["benchmark"] == "sharding"), key=lambda r: r["shards"])
    if len(shards) > 1:
        best = max(shards, key=lambda r: med(per_second(r)))
        rows.append(["Sharded replay, all books, no strategy",
                     f"{med(per_second(best)) / 1e6:.1f} M msg/s with {best['shards']} shards "
                     f"({med(per_second(best)) / med(per_second(shards[0])):.2f}x one engine)"])
    rows.append(["Hot-path allocations after start-up", "0 (counted by a replaced `operator new`)"])
    return (f"Measured on: {conditions(sysinfo)}. Medians; ranges and run counts are in "
            f"BENCHMARKS.md.\n\n" + table(["Benchmark", "Result"], rows))


NOT_MEASURED = {
    "end_to_end": "scripts/run_bench.sh --only e2e",
    "sharding": "scripts/run_bench.sh --only shards",
    "micro": "scripts/run_bench.sh --only micro",
    "dispatch": "scripts/run_bench.sh --only micro",
    "latency": "scripts/run_bench.sh --only latency",
    "timing_overhead": "scripts/run_bench.sh --only latency",
}


def replace_block(text: str, name: str, body: str | None) -> str:
    if body is None:
        if name not in NOT_MEASURED:
            return text
        body = (f"**Not measured** in the published result set: the machine would not stay "
                f"awake long enough for a clean run (see `NOTES.md` in the results "
                f"directories). To measure: `{NOT_MEASURED[name]}`, then `python3 bench/render.py`.")
    pattern = re.compile(
        rf"(<!-- BEGIN GENERATED: {re.escape(name)} -->\n).*?(<!-- END GENERATED: {re.escape(name)} -->)",
        re.S)
    if not pattern.search(text):
        print(f"warning: no '{name}' block in {TARGET.name}", file=sys.stderr)
        return text
    return pattern.sub(lambda m: m.group(1) + body + "\n" + m.group(2), text)


def main(argv: list[str]) -> int:
    # With no arguments, publish the directories named in bench/results/CURRENT (one per line,
    # later ones overriding earlier ones benchmark by benchmark), or failing that the newest
    # by the time recorded inside it. CURRENT exists so that which runs the page shows is a
    # reviewed, committed choice, not an accident of directory names.
    current = RESULTS / "CURRENT"
    if argv:
        directories = [Path(a).resolve() for a in argv]
    elif current.is_file():
        directories = [RESULTS / line.strip() for line in current.read_text().splitlines()
                       if line.strip() and not line.startswith("#")]
    else:
        available = sorted((d for d in RESULTS.iterdir() if (d / "sysinfo.json").is_file()),
                           key=lambda d: json.loads((d / "sysinfo.json").read_text())["date_utc"])
        directories = available[-1:]
    if not directories:
        print("no result directories found", file=sys.stderr)
        return 1
    merged: dict[tuple[str, str], dict] = {}
    micro = None
    sysinfo: dict = {}
    for directory in directories:  # later directories win
        sysinfo, results, found_micro = load(directory)
        micro = found_micro or micro
        for r in results:
            merged[(r.get("benchmark", "?"), r.get("variant", "?"))] = r
    results = list(merged.values())

    def of(benchmark: str) -> list[dict]:
        return [r for r in results if r["benchmark"] == benchmark]

    order = ["baseline", "baseline/reserve", "flat_orders/reserve", "binary_levels/reserve",
             "optimised/reserve", "optimised/reserve/prefetch"]
    books = sorted(of("book_build"),
                   key=lambda r: order.index(r["variant"]) if r["variant"] in order else 99)
    latency = sorted(of("book_latency"),
                     key=lambda r: order.index(r["variant"]) if r["variant"] in order else 99)

    text = TARGET.read_text()
    text = replace_block(text, "machine", machine_table(directories, sysinfo))
    text = replace_block(text, "parse_only", throughput_table(of("parse_only")) if of("parse_only") else None)
    text = replace_block(text, "parse_faults", parse_fault_table(of("parse_only")) if of("parse_only") else None)
    text = replace_block(text, "book_build", book_table(books) if books else None)
    text = replace_block(text, "memory", memory_table(books) if books else None)
    text = replace_block(text, "latency", latency_table(latency) if latency else None)
    text = replace_block(text, "timing_overhead",
                         timing_overhead_table(latency, books) if latency else None)
    text = replace_block(text, "end_to_end", e2e_table(of("end_to_end")) if of("end_to_end") else None)
    text = replace_block(text, "sharding", sharding_table(of("sharding")) if of("sharding") else None)
    text = replace_block(text, "micro", micro_table(micro) if micro else None)
    text = replace_block(text, "dispatch", dispatch_sentence(micro) if micro else None)
    text = replace_block(text, "optimisation_log", log_table(results, micro, latency))
    text = replace_block(text, "targets", targets_table(results, latency))
    TARGET.write_text(text)

    readme_path = ROOT / "README.md"
    readme = readme_path.read_text()
    for name, body in (("headline", headline(results, latency, sysinfo)),
                       ("bench_summary", bench_summary(results, latency, sysinfo))):
        if body is not None and f"BEGIN GENERATED: {name}" in readme:
            readme = replace_block(readme, name, body)
    readme_path.write_text(readme)
    print(f"updated {TARGET.relative_to(ROOT)} from {', '.join(d.name for d in directories)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
