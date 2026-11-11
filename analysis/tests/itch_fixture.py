"""Builds small ITCH 5.0 byte streams for Python tests.

Written against the official specification independently of the C++ fixture writer, so the two
test suites do not share a layout mistake.
"""

from __future__ import annotations

import struct


def _header(kind: bytes, locate: int, ts: int) -> bytes:
    return kind + struct.pack(">HH", locate, 0) + ts.to_bytes(6, "big")


def _record(message: bytes) -> bytes:
    return struct.pack(">H", len(message)) + message


def _symbol(symbol: str) -> bytes:
    return symbol.encode("ascii").ljust(8, b" ")


def system_event(ts: int, code: str) -> bytes:
    return _record(_header(b"S", 0, ts) + code.encode())


def stock_directory(ts: int, locate: int, symbol: str) -> bytes:
    body = (_symbol(symbol) + b"QN" + struct.pack(">I", 100) + b"N" + b"C" + b"Z " + b"P" + b"N"
            + b"N" + b"1" + b"N" + struct.pack(">I", 0) + b"N")
    return _record(_header(b"R", locate, ts) + body)


def add(ts: int, locate: int, ref: int, side: str, shares: int, symbol: str, price: int,
        mpid: str | None = None) -> bytes:
    body = struct.pack(">QcI", ref, side.encode(), shares) + _symbol(symbol) + struct.pack(">I", price)
    if mpid is None:
        return _record(_header(b"A", locate, ts) + body)
    return _record(_header(b"F", locate, ts) + body + mpid.encode().ljust(4, b" "))


def execute(ts: int, locate: int, ref: int, shares: int, match: int = 0) -> bytes:
    return _record(_header(b"E", locate, ts) + struct.pack(">QIQ", ref, shares, match))


def execute_with_price(ts: int, locate: int, ref: int, shares: int, price: int,
                       printable: bool = True, match: int = 0) -> bytes:
    return _record(_header(b"C", locate, ts) + struct.pack(">QIQ", ref, shares, match)
                   + (b"Y" if printable else b"N") + struct.pack(">I", price))


def cancel(ts: int, locate: int, ref: int, shares: int) -> bytes:
    return _record(_header(b"X", locate, ts) + struct.pack(">QI", ref, shares))


def delete(ts: int, locate: int, ref: int) -> bytes:
    return _record(_header(b"D", locate, ts) + struct.pack(">Q", ref))


def replace(ts: int, locate: int, old_ref: int, new_ref: int, shares: int, price: int) -> bytes:
    return _record(_header(b"U", locate, ts) + struct.pack(">QQII", old_ref, new_ref, shares, price))


def trade(ts: int, locate: int, shares: int, symbol: str, price: int, match: int = 0) -> bytes:
    return _record(_header(b"P", locate, ts) + struct.pack(">QcI", 0, b"B", shares)
                   + _symbol(symbol) + struct.pack(">IQ", price, match))


def random_stream(rng, symbols: list[str], events: int, extra_symbol: str = "OTHER") -> bytes:
    """A valid random order flow over `symbols`, plus one symbol the tests do not ask for."""
    all_symbols = symbols + [extra_symbol]
    out = [system_event(1, "O")]
    for i, symbol in enumerate(all_symbols):
        out.append(stock_directory(2, 10 + i, symbol))
    live: dict[int, tuple[int, int]] = {}  # ref -> (locate, shares)
    next_ref = 1
    for ts in range(100, 100 + events):
        action = rng.random()
        if not live or action < 0.45:
            index = rng.randrange(len(all_symbols))
            shares = rng.randrange(1, 20) * 100
            # A narrow price range, so levels fill up and empty out repeatedly.
            price = 1_000_000 + rng.randrange(-15, 16) * 100
            out.append(add(ts, 10 + index, next_ref, rng.choice("BS"), shares,
                           all_symbols[index], price, "GSCO" if rng.random() < 0.1 else None))
            live[next_ref] = (10 + index, shares)
            next_ref += 1
            continue
        ref = rng.choice(list(live))
        locate, shares = live[ref]
        if action < 0.60:
            out.append(delete(ts, locate, ref))
            del live[ref]
        elif action < 0.72:
            take = rng.randrange(1, shares + 1)
            out.append(execute(ts, locate, ref, take))
            live[ref] = (locate, shares - take)
        elif action < 0.78:
            take = rng.randrange(1, shares + 1)
            out.append(execute_with_price(ts, locate, ref, take, 999_900, rng.random() < 0.5))
            live[ref] = (locate, shares - take)
        elif action < 0.88:
            take = rng.randrange(1, shares + 1)
            out.append(cancel(ts, locate, ref, take))
            live[ref] = (locate, shares - take)
        else:
            new_shares = rng.randrange(1, 20) * 100
            price = 1_000_000 + rng.randrange(-15, 16) * 100
            out.append(replace(ts, locate, ref, next_ref, new_shares, price))
            del live[ref]
            live[next_ref] = (locate, new_shares)
            next_ref += 1
            continue
        if ref in live and live[ref][1] == 0:
            del live[ref]
    out.append(system_event(10**12, "C"))
    return b"".join(out)
