#!/usr/bin/env python3
"""Compares closing cross prices from the ITCH file with published closing prices.

Usage:
    build/release/apps/book-check --input data/<file> --crosses-out crosses.csv
    python3 analysis/reconcile_close.py crosses.csv 2019-12-30 AAPL MSFT AMZN GOOGL TSLA

The published side comes from Yahoo Finance's chart API, which returns split-adjusted prices
and the list of splits. Each split since the file date is undone before comparing, and the
adjustment is shown. Needs network access; standard library only. Local use, never CI.
"""

from __future__ import annotations

import csv
import datetime as dt
import json
import sys
import urllib.request

CHART = "https://query1.finance.yahoo.com/v8/finance/chart/{symbol}?period1={start}&period2={end}&interval={interval}&events=split"


def fetch(symbol: str, start: int, end: int, interval: str) -> dict:
    url = CHART.format(symbol=symbol, start=start, end=end, interval=interval)
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)["chart"]["result"][0]


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    crosses_path, date_text, symbols = argv[0], argv[1], argv[2:]
    day = dt.datetime.strptime(date_text, "%Y-%m-%d").replace(tzinfo=dt.timezone.utc)
    start = int(day.timestamp())
    end = start + 86400
    now = int(dt.datetime.now(dt.timezone.utc).timestamp())

    closing = {}
    with open(crosses_path, newline="") as f:
        for row in csv.DictReader(f):
            if row["cross_type"] == "C":
                closing[row["symbol"]] = (int(row["price"]), int(row["shares"]))

    print("| Symbol | ITCH closing cross | Cross shares | Published close (split-adjusted) "
          "| Splits since | Published close, unadjusted | Match |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    all_match = True
    for symbol in symbols:
        cross_price, shares = closing[symbol]
        adjusted = fetch(symbol, start, end, "1d")["indicators"]["quote"][0]["close"][0]
        events = fetch(symbol, end, now, "3mo").get("events", {}).get("splits", {})
        factor = 1.0
        labels = []
        for split in sorted(events.values(), key=lambda s: s["date"]):
            factor *= split["numerator"] / split["denominator"]
            when = dt.datetime.fromtimestamp(split["date"], dt.timezone.utc).date()
            labels.append(f"{split['splitRatio']} on {when}")
        unadjusted = adjusted * factor
        # Published prices are floats rounded to a cent or less; compare at half a cent.
        match = abs(unadjusted - cross_price / 10000) < 0.005
        all_match &= match
        print(f"| {symbol} | {cross_price / 10000:.4f} | {shares:,} | {adjusted:.4f} "
              f"| {', '.join(labels) or 'none'} | {unadjusted:.4f} | {'yes' if match else 'NO'} |")
    return 0 if all_match else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
