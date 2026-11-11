# Validation

What has been checked, on what data, and the results. Everything here was run on the reference
day, `12302019.NASDAQ_ITCH50` (30 December 2019, 268,744,780 messages, SHA-256
`5d81c2e14a0f748b29c674b6a342796932702034b4dd341e39e9a9ec5bac610f`).

## Parser

`itch-stats` reads the whole file with no errors. Every record's length matches the
specification's length for its type, and there are no unknown types.

## Book invariants

`build/release/apps/book-check --input data/12302019.NASDAQ_ITCH50 --recount-every 20000000`

| Check | Result |
| --- | --- |
| E, C, X, D, U referring to an order that is not live | 0 of 144,610,481 |
| Order reference re-used while live | 0 of 140,270,523 orders added |
| Order reduced by more than it holds | 0 |
| Level total differs from the sum of its orders (14 full recounts) | 0 |
| Live orders at end of day | 0 (peak 1,924,078) |
| Book crossed or locked in regular hours while trading | 0 violations in 255,041,094 checks |
| Crossed states tolerated as reopening transients | 642, longest 1.214 ms after release |

The last row is the one place the specification's invariant had to be refined; the
investigation is written up in [DEBUGGING.md](../DEBUGGING.md).

## Differential test against the Python reference book

`analysis/reference_book.py` shares no code with the C++ book. `book-dump` and the reference
are compared level for level (price, shares, order count) at ten checkpoints.

| Run | Symbols | Messages | Result |
| --- | --- | --- | --- |
| Specified test (runs whenever the data file is present) | AAPL, MSFT, SPY | first 5,000,000 | identical |
| Extended run (`ITCHBT_DIFF_MESSAGES=268744780`) | AAPL, MSFT, SPY | whole day | identical |
| Synthetic random streams, three seeds (runs in CI) | 3 of 4 symbols | 20,000 events each, 20 checkpoints | identical |

## Reconciliation against published closing prices

`python3 analysis/reconcile_close.py crosses.csv 2019-12-30 AAPL MSFT AMZN GOOGL TSLA`, where
`crosses.csv` comes from `book-check --crosses-out`. The ITCH side is the price of the closing
cross (`Q` message, cross type `C`). The published side is Yahoo Finance's daily close, which
is split-adjusted; each split since the file date is undone.

| Symbol | ITCH closing cross | Cross shares | Published close (split-adjusted) | Splits since | Published close, unadjusted | Match |
| --- | --- | --- | --- | --- | --- | --- |
| AAPL | 291.5200 | 1,335,177 | 72.8800 | 4:1 on 2020-08-31 | 291.5200 | yes |
| MSFT | 157.5900 | 2,279,904 | 157.5900 | none | 157.5900 | yes |
| AMZN | 1846.8900 | 136,244 | 92.3445 | 20:1 on 2022-06-06 | 1846.8900 | yes |
| GOOGL | 1339.7100 | 91,482 | 66.9855 | 20:1 on 2022-07-18 | 1339.7099 | yes |
| TSLA | 414.7000 | 112,548 | 27.6467 | 5:1 on 2020-08-31, 3:1 on 2022-08-25 | 414.7000 | yes |

All five match to the cent. Four of the five have split since 2019, so comparing against
today's price history without undoing the splits would have shown four apparent mismatches.
The GOOGL figure differs in the fourth decimal only because the published value is a 32-bit
float.
