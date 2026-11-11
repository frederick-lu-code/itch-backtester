"""End-to-end checks of the C++ command-line tools on small synthetic files."""

from __future__ import annotations

import random
import subprocess

import itch_fixture as fx


def run(tool_path, *args):
    return subprocess.run([str(tool_path), *map(str, args)], capture_output=True, text=True,
                          check=False)


def test_itch_stats_counts_messages(tmp_path, tool):
    path = tmp_path / "s.itch"
    path.write_bytes(b"".join([
        fx.system_event(1, "O"),
        fx.stock_directory(2, 7, "AAPL"),
        fx.add(3, 7, 1, "B", 100, "AAPL", 1000),
        fx.add(4, 7, 2, "S", 100, "AAPL", 1010, mpid="GSCO"),
        fx.delete(5, 7, 1),
    ]))
    result = run(tool("itch-stats"), "--input", path)
    assert result.returncode == 0, result.stderr
    assert "messages:    5" in result.stdout
    assert "symbols:     1" in result.stdout
    for kind in "ADFRS":
        assert f"\n{kind}    " in result.stdout


def test_itch_stats_reports_the_offset_of_a_bad_record(tmp_path, tool):
    good = fx.delete(1, 1, 1)
    path = tmp_path / "bad.itch"
    path.write_bytes(good + b"\x00\x03Zab")
    result = run(tool("itch-stats"), "--input", path)
    assert result.returncode == 1
    assert f"unknown message type at file offset {len(good)}" in result.stderr


def test_book_check_passes_a_valid_stream(tmp_path, tool):
    path = tmp_path / "ok.itch"
    path.write_bytes(fx.random_stream(random.Random(7), ["AAA", "BBB"], events=5_000))
    result = run(tool("book-check"), "--input", path, "--recount-every", 500)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASS" in result.stdout
    assert "unknown order references:  0" in result.stdout


def trading_action(ts: int, state: str) -> bytes:
    return fx._record(fx._header(b"H", 7, ts) + b"AAPL    " + state.encode() + b" " + b"    ")


def test_book_check_flags_a_crossed_book_in_regular_hours(tmp_path, tool):
    path = tmp_path / "crossed.itch"
    path.write_bytes(b"".join([
        fx.stock_directory(1, 7, "AAPL"),
        trading_action(2, "T"),
        fx.system_event(3, "Q"),
        fx.add(10, 7, 1, "B", 100, "AAPL", 1010),
        fx.add(11, 7, 2, "S", 100, "AAPL", 1020),  # uncrossed: the release is over
        fx.add(12, 7, 3, "S", 100, "AAPL", 1000),  # crosses the bid
    ]))
    result = run(tool("book-check"), "--input", path)
    assert result.returncode == 1
    assert "CROSSED AAPL" in result.stdout
    assert "crossed/locked violations: 1" in result.stdout


def halt_and_release(release_ts: int) -> list[bytes]:
    """Orders build up crossed during a halt, as in an auction, then the symbol is released."""
    return [
        fx.stock_directory(1, 7, "AAPL"),
        fx.system_event(2, "Q"),
        trading_action(3, "H"),
        fx.add(10, 7, 1, "B", 100, "AAPL", 1010),
        fx.add(11, 7, 2, "S", 100, "AAPL", 1000),
        trading_action(release_ts, "T"),
    ]


def test_book_check_tolerates_a_cross_still_printing_after_a_release(tmp_path, tool):
    path = tmp_path / "reopen.itch"
    path.write_bytes(b"".join(halt_and_release(1_000) + [
        # The cross's own executions arrive just after the release, as in the real feed.
        fx.execute_with_price(1_100, 7, 1, 60, 1005, printable=False),
        fx.execute_with_price(1_200, 7, 1, 40, 1005, printable=False),
        fx.execute_with_price(1_300, 7, 2, 100, 1005, printable=False),
    ]))
    result = run(tool("book-check"), "--input", path)
    assert result.returncode == 0, result.stdout
    assert "reopening transients:      1" in result.stdout
    assert "crossed/locked violations: 0" in result.stdout


def test_book_check_does_not_tolerate_a_cross_that_outlives_the_release(tmp_path, tool):
    path = tmp_path / "stuck.itch"
    one_second = 10**9
    path.write_bytes(b"".join(halt_and_release(1_000) + [
        fx.execute_with_price(1_000 + one_second, 7, 1, 60, 1005, printable=False),
    ]))
    result = run(tool("book-check"), "--input", path)
    assert result.returncode == 1
    assert "crossed/locked violations: 1" in result.stdout


def test_book_check_flags_references_to_dead_orders(tmp_path, tool):
    path = tmp_path / "dead.itch"
    path.write_bytes(b"".join([
        fx.stock_directory(1, 7, "AAPL"),
        fx.add(2, 7, 1, "B", 100, "AAPL", 1000),
        fx.delete(3, 7, 1),
        fx.execute(4, 7, 1, 50),
    ]))
    result = run(tool("book-check"), "--input", path)
    assert result.returncode == 1
    assert "unknown order references:  1" in result.stdout


def test_book_dump_text_shows_the_top_of_book(tmp_path, tool):
    path = tmp_path / "top.itch"
    path.write_bytes(b"".join([
        fx.stock_directory(1, 7, "AAPL"),
        fx.add(34_200 * 10**9, 7, 1, "B", 100, "AAPL", 2_915_100),
        fx.add(34_201 * 10**9, 7, 2, "S", 300, "AAPL", 2_915_300),
        fx.add(34_300 * 10**9, 7, 3, "B", 100, "AAPL", 2_915_200),  # after the cut-off below
    ]))
    result = run(tool("book-dump"), "--input", path, "--symbol", "AAPL", "--at", "09:30:02")
    assert result.returncode == 0, result.stderr
    assert "291.5100 | 291.5300" in result.stdout
    assert "after 3 messages" in result.stdout
    assert "291.5200" not in result.stdout


def test_book_dump_rejects_an_unknown_symbol(tmp_path, tool):
    path = tmp_path / "one.itch"
    path.write_bytes(fx.stock_directory(1, 7, "AAPL"))
    result = run(tool("book-dump"), "--input", path, "--symbol", "NOPE")
    assert result.returncode == 1
    assert "not found" in result.stderr
