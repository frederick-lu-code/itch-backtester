# itch-backtester

A deterministic, event-driven backtester in C++20. It replays a full day of Nasdaq
TotalView-ITCH 5.0 data, rebuilds the limit order book, runs a strategy against it, simulates
order entry and fills, enforces risk controls, and writes a dataset for post-trade analysis in
Python.

**Status: M0 (scaffold) complete.** The build, test, sanitizer, lint and CI plumbing exist, along
with the synthetic ITCH fixture writer that later tests are built on. There is no parser, book
or engine yet, so there is no headline throughput or latency figure. One will appear here only
once it has been measured.

| Milestone | Deliverable | Status |
| --- | --- | --- |
| M0 Scaffold | CMake presets, CI, formatting, sanitizer builds, `itch_writer` fixture | Done |
| M1 Parser | Reader, parser, `itch-stats`, parser fuzz target | Not started |
| M2 Order book | Baseline book, `book-dump`, invariants, reference book, reconciliation | Not started |
| M3 Backtester | Strategy API, simulated exchange, order state machine, latency queue | Not started |
| M4 Risk | Pre-trade checks, position and PnL tracking, kill switch | Not started |
| M5 Recorder and analysis | Binary recorder, Python loader, report notebook | Not started |
| M6 Optimisation | Optimised book, timing build, benchmark suite, optimisation log | Not started |
| M7 Sharding (stretch) | Router thread, SPSC queues, shard workers | Not started |
| M8 Release | README complete, results regenerated from a clean checkout | Not started |

## Quick start

```sh
scripts/fetch_data.sh                                   # one sample day into data/ (large)
cmake --preset release && cmake --build --preset release
ctest --preset release
```

Running a backtest and opening the report arrive with M3 and M5.

Requirements: GCC 13+ or Clang 17+, CMake 3.25+, Ninja, and Python 3.11+ for the analysis.

### Presets

| Preset | Purpose |
| --- | --- |
| `debug` | Unoptimised, assertions on |
| `release` | `-O2`, tuned for the build machine's CPU; the benchmark build |
| `relwithdebinfo` | `release` plus symbols and frame pointers, for profiling |
| `asan` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer |
| `fuzz` | libFuzzer targets with ASan and UBSan; needs upstream Clang, not Apple Clang |

Each has matching build and test presets: `cmake --build --preset asan && ctest --preset asan`.

### Building on macOS

The project targets Linux, and CI on Ubuntu 24.04 is the authoritative build. Everything except
the `fuzz` preset also builds and passes with Apple Clang. If linking fails with
`tapi error: malformed file` naming an SDK newer than your OS, point the build at the matching
SDK, for example `export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`.

## Architecture

To be written as the components land. The planned pipeline is: reader, parser, order book,
simulated exchange, strategy, risk gate, latency queue, recorder, driven by a single-threaded
event loop so that runs are deterministic.

## Design decisions

Recorded here as they are made, each with the alternatives considered and the trade-off.

- **Decoding never casts to a packed struct.** Every field is read with `std::memcpy` plus a
  byte swap (the fixture writer does the reverse with shifts), which avoids alignment and
  aliasing undefined behaviour at no measurable cost once inlined. To be confirmed by benchmark
  in M1.
- **Test data is synthetic.** `tests/support/itch_writer` builds valid ITCH byte streams from a
  short script of events, so tests and CI never need Nasdaq data. Its layouts were checked field
  by field against the official specification, and its tests state expected offsets
  independently of the writer.

## Validation

Nothing to report yet. Planned: book invariants on the full file, a differential test against a
pure-Python reference book, and reconciliation of closing cross prices against published closes.

The ITCH message table in the project specification was checked against the official Nasdaq
specification (revision of April 28, 2023): all 23 message types and lengths agree.

## Simulation limits

To be written with the fill model in M3. Known in advance: no market impact, optimistic fills
after queue exhaustion, and no hidden liquidity.

## Benchmarks

None recorded yet. See [BENCHMARKS.md](BENCHMARKS.md). Bugs found along the way are logged in
[DEBUGGING.md](DEBUGGING.md).

## Development notes

This project is built with Claude Code (Anthropic) as the implementer, working one milestone at
a time against a written specification, with interfaces proposed and approved before each
milestone. Cases where its output was wrong, and how each was caught, will be listed here as
they occur.

## Data notice

Nasdaq sample data is not included in this repository. `scripts/fetch_data.sh` downloads it
from Nasdaq's public sample directory to `data/`, which is git-ignored. The reference day for
this project is `12302019.NASDAQ_ITCH50` (30 December 2019).

## Licence

[MIT](LICENSE)
