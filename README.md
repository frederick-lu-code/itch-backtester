# itch-backtester

A deterministic, event-driven backtester in C++20. It replays a full day of Nasdaq
TotalView-ITCH 5.0 data, rebuilds the limit order book for every symbol, runs a strategy
against it, simulates order entry and fills with latency, enforces risk controls, and writes a
dataset that a Python package turns into a post-trade report.

<!-- BEGIN GENERATED: headline -->
**Headline result:** the order book for all 8,906 symbols is rebuilt from a full day of 269 million messages at **6.5 million messages per second** on one thread (Apple M5, on battery in Low Power Mode).
<!-- END GENERATED: headline -->

Those figures come from an Apple Silicon laptop, not the bare-metal Linux x86-64 machine the
project was specified for. [BENCHMARKS.md](BENCHMARKS.md) opens with exactly what that changes.

## Quick start

```sh
scripts/fetch_data.sh                                    # one Nasdaq sample day, 8 GB, into data/
cmake --preset release && cmake --build --preset release
build/release/apps/backtest --input data/12302019.NASDAQ_ITCH50 \
    --config configs/passive_maker.toml --out runs/demo
ITCHBT_RUN=runs/demo jupyter lab analysis/report.ipynb   # the post-trade report
```

Requirements: GCC 13+ or Clang 17+, CMake 3.25+, Ninja; Python 3.11+ with
`analysis/requirements.txt` (and `requirements-notebook.txt` to open the notebook). About 5 GB
of free memory for a run that builds every symbol's book; `build_all_books = false` in the
config, or `--symbols` and `--max-messages` on the command line, make it far lighter.

The project targets Linux, and CI on Ubuntu 24.04 is the authoritative build. It also builds
and passes on macOS with Apple Clang, except the `fuzz` preset. If linking on macOS fails with
`tapi error: malformed file`, see the first macOS entry in [DEBUGGING.md](DEBUGGING.md).

### Tools

| Tool | What it does |
| --- | --- |
| `backtest` | Runs a strategy over a file and writes a run directory. `--shards N` splits symbols across N engines |
| `itch-stats` | Message counts by type, first and last timestamp, symbol count |
| `book-dump` | Prints a symbol's book as of a time of day: `--symbol AAPL --at 10:30:00 --depth 10` |
| `book-check` | Replays a whole file and checks the book invariants; exits non-zero if any fails |

### Presets

| Preset | Purpose |
| --- | --- |
| `debug` | Unoptimised, assertions on |
| `release` | `-O2`, tuned for the build machine's CPU; the benchmark build |
| `relwithdebinfo` | `release` plus symbols and frame pointers, for profiling |
| `timing` | `release` plus per-message latency timing in the benchmarks |
| `asan` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer |
| `fuzz` | libFuzzer target with ASan and UBSan; needs upstream Clang |

`scripts/check.sh [presets...]` formats, builds and tests in one step.

## Architecture

The core is one thread running one loop over two time-ordered streams: the historical ITCH
messages and the strategy's own delayed actions. Keeping it single-threaded is what makes a run
reproducible.

```
 ITCH file ──mmap──> Reader ──> Parser ──────────────> Order book ──change──> Simulated exchange
                                   │                    (per symbol)            (queue position,
                                   │                         │                   fills)
                                   │                  top of book, trades            │
                                   │                         v                 order updates
                                   │                     Strategy <──────────────────┤
                                   │                         │ submit / cancel       │
                                   │                         v                       │
                                   │                     Risk gate ──reject──────────┤
                                   │                         │ accept                │
                                   │                         v                       v
                                   └──release when due── Latency queue <──────── (ack latency)

 every order event, fill, top-of-book change and risk event ──> SPSC ring ──> writer thread ──> run directory
```

| Component | Where | In one sentence |
| --- | --- | --- |
| Reader | `include/itchbt/itch/reader.hpp` | Maps the file read-only; a `read()` version exists as the benchmark baseline |
| Parser | `include/itchbt/itch/parser.hpp` | Validates each record's type and length, decodes it with `memcpy` and a byte swap, and dispatches to a handler bound at compile time |
| Order book | `include/itchbt/book/` | One book per stock locate and one order store for all, each operation returning a record of what changed |
| Simulated exchange | `include/itchbt/sim/exchange.hpp` | Holds the strategy's orders and decides fills by watching the historical book go by, never changing it |
| Strategy | `strategies/` | Any type satisfying the `Strategy` concept; three samples are included |
| Risk gate | `include/itchbt/risk/risk.hpp` | Eleven pre-trade checks, integer position and PnL tracking, and a kill switch |
| Latency queue | `include/itchbt/sim/event_queue.hpp` | A fixed-size heap of everything that happens later, ordered by release time then by queue order |
| Engine | `include/itchbt/sim/engine.hpp` | The loop: release what is due, apply the message, check fills, call the strategy |
| Recorder | `include/itchbt/record/` | Fixed-width little-endian records pushed through a lock-free ring to a writer thread |
| Analysis | `analysis/` | Loads a run into pandas and produces the report; includes an independent Python order book for testing |
| Sharded runner | `include/itchbt/sim/sharded.hpp` | Optional: a router thread feeding one engine per shard of symbols |

Walkthroughs of each milestone's code, with the questions a reviewer is likely to ask, are in
[docs/milestones/](docs/milestones/).

## Design decisions

The full log, about sixty entries each with the alternative considered and the trade-off, is
[docs/DECISIONS.md](docs/DECISIONS.md). The ones that shape the project:

- **Integers everywhere in the engine.** Prices are 1/10000 USD, quantities are shares, and PnL
  is held doubled so a half-unit mid never needs rounding. No floating point touches a result.
- **Determinism by construction.** Simulation time is the ITCH timestamp; the engine never
  reads a clock. One event queue with one tie-break rule orders everything delayed. The same
  input, config and binary give byte-identical output files, and tests hold it to that.
- **The strategy is bound at compile time.** A C++20 concept, so callbacks inline into the
  loop. A virtual adapter exists for strategies chosen at run time; its measured cost is in the
  [optimisation log](BENCHMARKS.md#optimisation-log).
- **The book was built twice on purpose.** First from `std::unordered_map` and `std::map`,
  made fully correct and benchmarked; then with an open-addressing order map and sorted-vector
  levels. Each step is in the optimisation log with its measurement, and every variant must
  produce the same books as the baseline.
- **Decoding never casts a pointer to a struct.** Every field is a `memcpy` and a byte swap,
  which is free once inlined and has no alignment or aliasing undefined behaviour.
- **No allocation, locks or system calls on the hot path after start-up.** Storage is sized at
  start-up; a test replaces `operator new` to prove the count is zero, and states the
  condition it depends on.
- **Latency in both directions.** Orders and cancels take effect later; the strategy and the
  risk gate learn of fills later. Neither ever knows what the exchange has not yet told it.
- **Order ids are per symbol.** That one choice is what lets a sharded run reproduce a
  single-threaded one exactly.

## Validation

Details and commands are in [docs/VALIDATION.md](docs/VALIDATION.md). On the reference day,
30 December 2019 (268,744,780 messages, 8,906 symbols):

| What was checked | Result |
| --- | --- |
| Every record parses, with the length the specification gives for its type | Yes |
| ITCH layouts in the project specification against the official Nasdaq document | All 23 message types and lengths agree |
| Executions, cancels, deletes and replaces referring to an order that is not live | 0 of 144,610,481 |
| Level totals against a full recount from the order store | Equal at every recount |
| Book crossed during regular hours while trading | 0, after one refinement of the rule (below) |
| C++ book against an independent pure-Python book, level by level | Identical for AAPL, MSFT and SPY at ten checkpoints across the whole day |
| Optimised book against baseline book | Identical after every message on random streams; identical fingerprints at 16 checkpoints of the real day |
| Closing cross prices against published closes, five symbols, splits undone | All five match to the cent |
| Two runs of the same backtest | Byte-identical output files |
| Sharded run against single-threaded, per symbol | Byte-identical records, on synthetic streams and on the real day |
| PnL recomputed in Python from the fill records | Equal to the engine's figure to the last price unit |

Closing-price reconciliation:

| Symbol | ITCH closing cross | Published close today (split-adjusted) | Splits since | Published close, splits undone | Match |
| --- | --- | --- | --- | --- | --- |
| AAPL | 291.5200 | 72.8800 | 4:1 on 2020-08-31 | 291.5200 | yes |
| MSFT | 157.5900 | 157.5900 | none | 157.5900 | yes |
| AMZN | 1846.8900 | 92.3445 | 20:1 on 2022-06-06 | 1846.8900 | yes |
| GOOGL | 1339.7100 | 66.9855 | 20:1 on 2022-07-18 | 1339.7099 | yes |
| TSLA | 414.7000 | 27.6467 | 5:1 on 2020-08-31, 3:1 on 2022-08-25 | 414.7000 | yes |

Four of the five have split since 2019; without undoing the splits there would have been four
apparent mismatches. GOOGL differs in the fourth decimal because the published value is a
32-bit float.

The one invariant that did not hold as written: the book was crossed 642 times during regular
hours. Every case was a symbol reopening after a halt or IPO, where the feed announces
"trading" before it has finished publishing the reopening cross's executions. The longest
lasted 1.2 ms. The check now tolerates exactly that and nothing else; the investigation is in
[DEBUGGING.md](DEBUGGING.md).

Testing in CI on every push: unit, property and integration tests under GCC 13 and Clang 18 in
debug, release, ASan+UBSan and TSan builds; 60 seconds of libFuzzer on the parser; clang-format
and clang-tidy; and the Python tests, which build the C++ tools and drive them. CI never
downloads Nasdaq data and never runs benchmarks.

## Simulation limits

The fill model is a model. Its assumptions, and which way each one leans:

| Assumption | Effect |
| --- | --- |
| **No market impact.** Simulated orders never enter the historical book and historical order flow never reacts to them | Optimistic. Every marketable order sees the full displayed size, however many came before it |
| **Fills after the queue is exhausted are double-counted.** Once nothing is ahead of a simulated order, each historical execution at its price fills it by up to the executed size, and the historical order was filled too | Optimistic |
| **Simulated orders do not queue behind each other.** Two at one price are each served in full | Optimistic |
| **No hidden liquidity.** Only displayed size can be hit; a hidden trade exactly at a simulated order's price does not advance its queue | Mixed: pessimistic for passive fills, and misses hidden size a marketable order would really have met |
| **A resting order the historical book later crosses is not filled until a trade prints through it** | Pessimistic |
| **Auctions are not simulated.** Cross executions move queue positions but never fill a simulated order; day orders expire at the end of market hours | Neither: out of scope |
| **Market data is not delayed.** The strategy sees each book change at its exchange timestamp; only its own orders have latency | Optimistic |
| **One venue.** Nasdaq's book only | Out of scope |

Taken together the model flatters a strategy. A strategy that loses money here would lose more
in the market; one that makes money here has shown nothing yet. The included PassiveMaker loses
about 42,000 dollars on the reference day with markouts that are negative at every horizon,
which is the expected fate of a naive quoter at the touch and a reasonable sign that the fill
model is not handing out free money.

## Benchmarks

Full tables, method and the optimisation log: [BENCHMARKS.md](BENCHMARKS.md).

<!-- BEGIN GENERATED: bench_summary -->
Measured on: Apple M5, on battery in Low Power Mode. Medians; ranges and run counts are in BENCHMARKS.md.

| Benchmark | Result |
| --- | --- |
| Parse only, every field decoded | 29.0 M msg/s mapped, 43.8 with `read()` |
| Book build, all symbols, baseline book | 2.2 M msg/s |
| Book build, all symbols, optimised book | 6.5 M msg/s (2.98x the baseline) |
| Hot-path allocations after start-up | 0 (counted by a replaced `operator new`) |
<!-- END GENERATED: bench_summary -->

What could not be measured on this machine, and is said so rather than estimated: hardware
counters, flame graphs, a cold-cache I/O comparison, and anything with Low Power Mode off.

Bugs found while building the project, each with the real output that revealed it, are in
[DEBUGGING.md](DEBUGGING.md).

## Development notes

This project was implemented by Claude Code (Anthropic's coding agent) working from a written
specification, one milestone at a time, with the repository owner approving the plan for the
first two milestones and then asking for the rest to be completed without further sign-off.
The owner did not write or review the code line by line. Everything that follows from that is
on the page: the decisions log records each choice the agent made on its own, and the debugging
log records each time it was wrong.

Cases where its output was wrong, and what caught each:

1. **A risk check that could overflow.** The first notional check multiplied a 32-bit price by
   a 32-bit quantity in a signed 64-bit integer, with a comment saying this could not overflow.
   It can, for the largest possible order. UndefinedBehaviorSanitizer caught it on a test that
   deliberately sent absurd orders.
2. **A wrong assumption about a library.** The config loader used toml++'s `value<bool>()`,
   which converts integers to booleans, in a loader whose purpose was to be strict. A test
   written alongside it, listing inputs that must be rejected, failed on `build_all_books = 1`.
3. **A check that checked nothing.** The book benchmark's fingerprint, meant to prove that
   every book implementation builds the same books, was computed at the end of the day, when
   every book is empty. It was zero for all of them and would have been for a wrong one too.
   It was noticed only because the rendered table showed a column of zeros.
4. **A commit that did not compile.** A scripted edit silently failed to apply, the build
   error scrolled past, and the tests "passed" by running the previous binaries. Commits are
   now gated on a script that stops at the first failure.
5. **Wrong expectations in its own tests**, twice: a book-check test that placed orders on the
   wrong side of a halt release, and a kill-switch test that miscounted a rolling window. In
   both the code was right and the test was wrong, found by working the case by hand.
6. **A benchmark that measured itself.** A first micro-benchmark reported virtual calls as
   twice as fast as inlined ones. The loop's own dependency chain was being timed.

The pattern in what caught them: sanitizers, tests that state expected values independently of
the code, an independent second implementation, and looking at the output before believing it.

## Data notice

Nasdaq sample data is not included in this repository. `scripts/fetch_data.sh` downloads it
from Nasdaq's public sample directory to `data/`, which is git-ignored. The reference day for
this project is `12302019.NASDAQ_ITCH50` (30 December 2019). Tests and CI use synthetic data
only.

## Licence

[MIT](LICENSE)
