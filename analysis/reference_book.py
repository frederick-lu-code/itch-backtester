"""A deliberately simple, slow order book in pure Python.

It exists only for differential testing: it shares no code and no design with the C++ book, so
when the two agree message for message on real data, a mistake would have to be made twice in
two languages to go unnoticed. Clarity beats speed everywhere in this file.
"""

from __future__ import annotations

import mmap
import struct
from pathlib import Path
from typing import Iterator

# Message lengths from the official ITCH 5.0 specification, excluding the 2-byte length prefix.
MESSAGE_LENGTHS = {
    b"S": 12, b"R": 39, b"H": 25, b"Y": 20, b"L": 26, b"V": 35, b"W": 12, b"K": 28,
    b"J": 35, b"h": 21, b"A": 36, b"F": 40, b"E": 31, b"C": 36, b"X": 23, b"D": 19,
    b"U": 35, b"P": 44, b"Q": 40, b"B": 19, b"I": 50, b"N": 20, b"O": 48,
}

Row = tuple[int, str, str, int, int, int]  # messages, symbol, side, price, shares, orders


def read_messages(path: str | Path, max_messages: int | None = None) -> Iterator[bytes]:
    """Yields each message (without its length prefix), validating the framing."""
    with open(path, "rb") as f:
        if f.seek(0, 2) == 0:
            return
        f.seek(0)
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
            pos = 0
            count = 0
            size = len(data)
            while pos < size and (max_messages is None or count < max_messages):
                if pos + 2 > size:
                    raise ValueError(f"truncated length prefix at offset {pos}")
                (length,) = struct.unpack_from(">H", data, pos)
                message = data[pos + 2:pos + 2 + length]
                if len(message) != length:
                    raise ValueError(f"truncated message at offset {pos}")
                if MESSAGE_LENGTHS.get(message[:1]) != length:
                    raise ValueError(f"bad type or length at offset {pos}")
                yield message
                pos += 2 + length
                count += 1


class ReferenceBook:
    """Order books for a chosen set of symbols, keyed by stock locate."""

    def __init__(self, symbols: list[str]):
        self.wanted = set(symbols)
        self.symbol_of: dict[int, str] = {}  # locate -> symbol, in directory order
        # ref -> [locate, side, price, shares]
        self.orders: dict[int, list] = {}
        # (locate, side) -> {price: [shares, orders]}
        self.levels: dict[tuple[int, str], dict[int, list[int]]] = {}
        self.messages = 0

    # --- message handling -----------------------------------------------------------------

    def apply(self, m: bytes) -> None:
        kind = m[:1]
        (locate,) = struct.unpack_from(">H", m, 1)
        if kind == b"R":
            symbol = m[11:19].decode("ascii").rstrip(" ")
            if symbol in self.wanted:
                self.symbol_of[locate] = symbol
        elif locate in self.symbol_of:
            if kind in (b"A", b"F"):
                ref, side, shares = struct.unpack_from(">QcI", m, 11)
                (price,) = struct.unpack_from(">I", m, 32)
                self._add(ref, locate, side.decode(), price, shares)
            elif kind in (b"E", b"C", b"X"):
                # Executed, executed with price and cancel all take shares off an order.
                ref, shares = struct.unpack_from(">QI", m, 11)
                self._reduce(ref, shares)
            elif kind == b"D":
                (ref,) = struct.unpack_from(">Q", m, 11)
                self._reduce(ref, self.orders[ref][3])
            elif kind == b"U":
                old_ref, new_ref, shares, price = struct.unpack_from(">QQII", m, 11)
                _, side, _, old_shares = self.orders[old_ref]
                self._reduce(old_ref, old_shares)
                # The replacement inherits side and symbol from the order it replaces.
                self._add(new_ref, locate, side, price, shares)
        self.messages += 1

    def _add(self, ref: int, locate: int, side: str, price: int, shares: int) -> None:
        assert ref not in self.orders, f"order reference {ref} re-used while live"
        self.orders[ref] = [locate, side, price, shares]
        level = self.levels.setdefault((locate, side), {}).setdefault(price, [0, 0])
        level[0] += shares
        level[1] += 1

    def _reduce(self, ref: int, shares: int) -> None:
        order = self.orders[ref]  # KeyError here means the feed referred to a dead order
        locate, side, price, remaining = order
        assert shares <= remaining, f"order {ref} reduced by more than it holds"
        level = self.levels[(locate, side)][price]
        level[0] -= shares
        order[3] -= shares
        if order[3] == 0:
            del self.orders[ref]
            level[1] -= 1
            if level[1] == 0:
                assert level[0] == 0
                del self.levels[(locate, side)][price]

    # --- output ---------------------------------------------------------------------------

    def snapshot(self) -> list[Row]:
        """Every level of every tracked symbol, in the row order book-dump prints."""
        rows: list[Row] = []
        for locate, symbol in self.symbol_of.items():
            for side, best_first in (("B", True), ("S", False)):
                levels = self.levels.get((locate, side), {})
                for price in sorted(levels, reverse=best_first):
                    shares, orders = levels[price]
                    rows.append((self.messages, symbol, side, price, shares, orders))
        return rows


def replay(path: str | Path, symbols: list[str], max_messages: int | None = None,
           checkpoint_every: int = 0) -> list[Row]:
    """Replays a file and returns book snapshots at each checkpoint and at the end."""
    book = ReferenceBook(symbols)
    rows: list[Row] = []
    for message in read_messages(path, max_messages):
        book.apply(message)
        if checkpoint_every and book.messages % checkpoint_every == 0:
            rows.extend(book.snapshot())
    if not checkpoint_every or book.messages % checkpoint_every != 0:
        rows.extend(book.snapshot())
    return rows


def parse_book_dump_csv(text: str) -> list[Row]:
    """Parses the output of `book-dump --format csv` into the same row tuples."""
    rows: list[Row] = []
    for line in text.splitlines():
        messages, symbol, side, price, shares, orders = line.split(",")
        rows.append((int(messages), symbol, side, int(price), int(shares), int(orders)))
    return rows
