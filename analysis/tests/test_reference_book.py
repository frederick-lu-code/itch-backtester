"""The Python reference book on its own, then against the C++ book."""

from __future__ import annotations

import os
import random
import subprocess

import pytest

import itch_fixture as fx
from reference_book import ReferenceBook, parse_book_dump_csv, read_messages, replay


def book_from(stream: bytes, symbols: list[str], tmp_path) -> ReferenceBook:
    path = tmp_path / "stream.itch"
    path.write_bytes(stream)
    book = ReferenceBook(symbols)
    for message in read_messages(path):
        book.apply(message)
    return book


def levels(book: ReferenceBook) -> list[tuple]:
    return [row[1:] for row in book.snapshot()]


def test_levels_aggregate_and_sort_best_first(tmp_path):
    stream = b"".join([
        fx.stock_directory(1, 7, "AAPL"),
        fx.add(2, 7, 1, "B", 100, "AAPL", 1000),
        fx.add(3, 7, 2, "B", 200, "AAPL", 1010),
        fx.add(4, 7, 3, "B", 300, "AAPL", 1010),
        fx.add(5, 7, 4, "S", 400, "AAPL", 1030),
        fx.add(6, 7, 5, "S", 500, "AAPL", 1020, mpid="GSCO"),
    ])
    assert levels(book_from(stream, ["AAPL"], tmp_path)) == [
        ("AAPL", "B", 1010, 500, 2),
        ("AAPL", "B", 1000, 100, 1),
        ("AAPL", "S", 1020, 500, 1),
        ("AAPL", "S", 1030, 400, 1),
    ]


def test_execute_cancel_delete_and_replace(tmp_path):
    stream = b"".join([
        fx.stock_directory(1, 7, "AAPL"),
        fx.add(2, 7, 1, "B", 500, "AAPL", 1000),
        fx.add(3, 7, 2, "B", 100, "AAPL", 1000),
        fx.execute(4, 7, 1, 100),                    # 400 left
        fx.execute_with_price(5, 7, 1, 100, 999),    # 300 left; level price unchanged
        fx.cancel(6, 7, 1, 100),                     # 200 left
        fx.delete(7, 7, 2),                          # second order gone
        fx.replace(8, 7, 1, 3, 50, 1005),            # moves to 1005, still a buy
        fx.add(9, 7, 4, "S", 10, "AAPL", 1010),
        fx.execute(10, 7, 4, 10),                    # executed to zero: level disappears
    ])
    book = book_from(stream, ["AAPL"], tmp_path)
    assert levels(book) == [("AAPL", "B", 1005, 50, 1)]
    assert list(book.orders) == [3]


def test_untracked_symbols_are_ignored(tmp_path):
    stream = b"".join([
        fx.stock_directory(1, 7, "AAPL"),
        fx.stock_directory(1, 8, "MSFT"),
        fx.add(2, 8, 1, "B", 100, "MSFT", 1000),
        fx.add(3, 7, 2, "S", 100, "AAPL", 2000),
        fx.delete(4, 8, 1),
    ])
    book = book_from(stream, ["AAPL"], tmp_path)
    assert levels(book) == [("AAPL", "S", 2000, 100, 1)]
    assert book.messages == 5  # every message is counted, tracked or not


def test_reader_rejects_bad_framing(tmp_path):
    path = tmp_path / "bad.itch"
    path.write_bytes(fx.delete(1, 1, 1)[:-3])
    with pytest.raises(ValueError, match="truncated"):
        list(read_messages(path))
    path.write_bytes(b"\x00\x03Zab")
    with pytest.raises(ValueError, match="bad type"):
        list(read_messages(path))


def run_book_dump(tool_path, data, symbols, max_messages, checkpoint_every):
    command = [str(tool_path), "--input", str(data), "--symbols", ",".join(symbols),
               "--depth", "0", "--format", "csv", "--checkpoint-every", str(checkpoint_every)]
    if max_messages is not None:
        command += ["--max-messages", str(max_messages)]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
    return parse_book_dump_csv(result.stdout)


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_cpp_book_matches_reference_on_random_streams(seed, tmp_path, tool):
    symbols = ["AAA", "BBB", "CCC"]
    path = tmp_path / "random.itch"
    path.write_bytes(fx.random_stream(random.Random(seed), symbols, events=20_000))

    expected = replay(path, symbols, checkpoint_every=1_000)
    actual = run_book_dump(tool("book-dump"), path, symbols, None, 1_000)
    assert len(expected) > 100  # the stream really does build books
    assert actual == expected


def test_cpp_book_matches_reference_on_real_data(tool, data_file):
    """Three symbols over the first 5 million messages of the real file (local only).

    Set ITCHBT_DIFF_MESSAGES to go further; 100 million reaches well into regular hours.
    """
    symbols = ["AAPL", "MSFT", "SPY"]
    max_messages = int(os.environ.get("ITCHBT_DIFF_MESSAGES", "5000000"))
    checkpoint_every = max(max_messages // 10, 1)

    expected = replay(data_file, symbols, max_messages, checkpoint_every)
    actual = run_book_dump(tool("book-dump"), data_file, symbols, max_messages, checkpoint_every)
    assert {row[1] for row in expected} == set(symbols)
    assert actual == expected
