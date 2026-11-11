# Decisions and trade-offs

A running log of choices made while building the project, written at the time each was made.
Where a choice departs from the project specification, the entry says so. Measured choices
link to the optimisation log in [BENCHMARKS.md](../BENCHMARKS.md).

## Environment

**E1. Develop on macOS, treat Linux CI as authoritative.** The specification assumes Linux
x86-64. The only machine available is an Apple Silicon Mac (M5, 16 GB) with no Docker, so code
is written and tested locally with Apple Clang and every push is built and tested on Ubuntu
24.04 with GCC 13 and Clang 18, including ASan, UBSan, TSan, the fuzzer and clang-tidy.
Trade-off: Linux-only behaviour is only ever exercised in CI, never interactively.

**E2. Benchmarks run on the Mac, with the method adapted and labelled.** The specification says
not to report numbers from anything but bare-metal Linux. The owner chose to use the machine
available. Every result records the machine; `BENCHMARKS.md` opens with a table of what could
not be done (core pinning, governor, `perf stat`, TSC timing). Trade-off: absolute numbers are
not comparable with the specification's x86 targets; ratios between variants are still valid.

**E3. The `fuzz` preset only builds with upstream Clang.** Apple Clang ships no libFuzzer, so
fuzzing happens in CI (60 s per push). Trade-off: no long local fuzzing sessions.

**E4. Commits use GitHub's no-reply address.** The repository is public; the owner's work email
is kept out of the history.

## Build

**B1. Sanitizer flags are global, warnings are per target.** GoogleTest must be compiled with
the same sanitizer instrumentation as the code under test, but must not be held to this
project's `-Werror` warning set. Dependencies are also included as `SYSTEM`.

**B2. Dependencies are pinned in one file but fetched only when first used.** Keeps CI fast in
early milestones without scattering version pins.

**B3. `std::byteswap` replaced by compiler builtins.** The specification suggests
`std::byteswap`, which is C++23. The project builds as strict C++20 (`CMAKE_CXX_EXTENSIONS
OFF`), so `__builtin_bswap16/32/64` are used, as the specification allows.

## Input data

**I1. Reference day is 30 December 2019** (`12302019.NASDAQ_ITCH50`, 8,251,407,909 bytes,
268,744,780 messages). It is the smallest full day Nasdaq publishes in the classic file naming,
which matters on a 16 GB machine.

**I2. Downloads are verified by gzip CRC, not MD5.** Nasdaq lists `.md5sum` files but its server
returns 404 for them. The script prints a SHA-256 of the decompressed file instead, and
`run.json` records it per run.

## Parser

**P1. Errors are return values, not exceptions.** The hot path may not throw. `parse()` returns
the error kind, the record's file offset, its type byte and stated length; tools print that and
exit non-zero.

**P2. `F` (add with MPID) decodes into the same struct as `A`.** Nothing in the specification
uses the attribution. The header keeps the original type byte so `itch-stats` can still count
the two separately.

**P3. The ten unused message types are length-checked and delivered as a header only.** They
must be recognised so a new or corrupt type is still a hard error, but decoding fields nobody
reads would be dead code.

**P4. An invalid buy/sell byte decodes as Sell instead of failing.** Real files never contain
one, the specification allows only `B` and `S`, and a branch that can abort is a liability under
fuzzing. Trade-off: a corrupt side byte is not detected. A corrupt file is far more likely to
trip the length or type checks first.

**P5. The 6-byte timestamp is read as a 2-byte and a 4-byte load.** Reading 8 bytes and masking
is one instruction shorter but reads two bytes past the field, which is out of bounds on the
last message of a mapping whose size is a multiple of the page size.

**P6. The parse benchmark reports two handlers.** With an empty handler the compiler deletes
the decoders, so that number measures framing only. The checksum handler forces every field to
be read and is the figure quoted as "parse only".

**P7. The read() reader hands the parser its buffer and keeps the unconsumed tail.** Finding
record boundaries in the reader would mean walking the length prefixes twice. Instead the
parser reports "truncated" at the end of a buffer and `parse_stream` treats that as "read more"
unless the file has also ended.

**P8. Handlers can opt in to record offsets and early exit.** `parse()` calls `handler.at(offset)`
before a message and checks `handler.done()` after it, but only if the handler defines them
(`requires` expressions), so the benchmark and engine paths pay nothing. Alternative considered:
a second, slower parse loop for tools. Rejected because two framing loops can drift apart.

## Order book

**K1. One order store for all symbols, one book per possible locate.** Order references are
unique across symbols, and E, X, D and U messages identify an order by reference alone, so the
store must be global. Books live in a vector of 65,536 entries indexed by the 16-bit locate:
no bounds check, no resize, and a few megabytes of memory.

**K2. Both sides share one level container through a priority key.** Bids are keyed by price
and asks by the bitwise complement of price, so "larger key is better" on both sides and the
best level is always last. One implementation, one set of tests, no side branches in the
container. Trade-off: prices must be converted back when levels are reported.

**K3. Feed anomalies are counted, not fatal.** An execution for an unknown order, a re-used
reference or an over-sized reduction is ignored (or clamped) and counted. A backtest should
not abort hours into a run, but `book-check` fails if any count is non-zero. On the reference
day all three counts are zero.

**K4. Level totals are 64-bit.** Order quantities are 32-bit on the wire, but many orders share
a level and their sum can exceed 32 bits in principle.

**K5. Operations return a change record instead of the book being re-read.** The simulated
exchange needs the level's quantity before and after, whether the top moved, and the affected
order's sequence. Returning them from the operation that already has them in hand avoids a
second lookup per message.

**K6. `book-check` is an extra tool, not in the specification's list.** The invariants on the
real file need somewhere to run. Folding them into `itch-stats` would have mixed a parser tool
with a book tool.

**K7. A crossed book is tolerated for 100 ms after a halt release, and only then.** See the
entry in `DEBUGGING.md`. The specification's invariant ("not crossed during regular hours")
is violated 642 times on the reference day by the feed's own ordering of messages around
reopening crosses. The longest such state lasted 1.2 ms; the tolerance is 100 ms and applies
only while the book has been continuously crossed since the release.

**K8. The differential test compares text dumps from a separate process.** `book-dump --format
csv` prints every level at checkpoints and the Python reference prints the same rows. No
Python bindings to maintain, and the comparison exercises the real tool. Trade-off: the CI
Python job has to build the C++ tools first.

**K9. Published closing prices come from Yahoo Finance's chart API.** It returns split-adjusted
closes together with the split list, so the adjustment can be undone mechanically and shown.
The script is local-only; CI never touches the network for data.

## Simulation

**S1. One event queue for everything delayed.** Orders in flight, cancels in flight, order
updates on their way back and timers are all events in one fixed-capacity binary heap ordered
by release time, ties broken by the order they were queued. Alternative: separate queues per
kind, merged at release. One queue makes the tie-break rule a single line and the total order
obvious, which is what determinism rests on.

**S2. Latency in both directions; the firm learns late.** An action takes effect `order_ns` or
`cancel_ns` after it is sent. Every update from the exchange (accept, fill, cancel, exchange
reject) reaches the strategy `ack_ns` after it happened. The specification names the delay for
acknowledgements only; applying it to every update means the strategy and the risk gate never
know something the exchange has not had time to tell them. Consequence: `ctx.position()` is
the position as of the fills delivered so far, not the true position at the exchange.

**S3. Pre-trade rejects are immediate.** The specification says the checks run synchronously.
A risk reject is delivered as the very next event, at the same timestamp, with no latency.

**S4. Market data is not delayed.** The strategy sees each book change at its ITCH timestamp.
Only its own orders are subject to latency. Simpler than modelling a feed delay, and it is
the more common convention; a feed delay would be one more event kind.

**S5. Order ids are per symbol.** An id is the stock locate in the top bits and a per-symbol
counter below. Alternative: one global counter. Per-symbol ids depend only on that symbol's
history, which is what lets a sharded run reproduce single-threaded output byte for byte.

**S6. Symbols are resolved by a pre-scan of the directory.** A strategy names symbols, the
engine works in locates, and locates are only known from the file's stock directory messages.
`prepare()` scans the file up to the first order (the directory always precedes it), so
`on_start` can already resolve names and an unknown symbol is an error before the run begins.

**S7. The exchange and risk gate are configured late, not constructed late.** They need the
traded locates, known only after the pre-scan. The first version held them in `std::optional`;
clang-tidy could not prove any access was checked and flagged every use. They are now plain
members with a `configure()` method, which also removes an indirection from the hot path.

**S8. The state machine is one enum and one table, and "cancel requested" is a separate
flag.** The specified table has no edge from `PartiallyFilled` to `Cancelled` and allows
`PendingCancel` to go back to `PartiallyFilled` on a fill. So a pending cancel cannot be
remembered in the state alone. Every cancellation, including end-of-day expiry, passes through
`PendingCancel`, exactly as the table requires.

**S9. An IOC stays `PendingNew` through partial fills.** The table gives an IOC only
`PendingNew -> Filled` and `PendingNew -> Cancelled`. Its partial fills are reported with the
state unchanged and its end state follows. No `Accepted` update is sent for an IOC.

**S10. Fill model choices beyond the specification's rules.**
- Executions that are part of a cross (non-printable) move the queue but never fill a
  simulated order: auctions are a stated non-goal.
- Trades against hidden orders (`P`) can fill by trade-through only. The feed does not say
  which side was resting, so a hidden trade exactly at our price is not counted as queue
  progress. Conservative.
- A resting order whose price the historical book later crosses is not filled until a
  historical execution prints through it. Conservative, and it follows the specification's
  rules literally.
- Two simulated orders at one price do not queue behind each other. Optimistic, and part of
  "no market impact".
- A marketable limit order's queue position is taken before its sweep, against the book as it
  stood on arrival.

**S11. Positions are marked only while the symbol can trade.** See `DEBUGGING.md`. After hours
the visible book is not a market.

**S12. `build_all_books` is a switch, and it cannot change results.** Building 8,906 books to
trade ten symbols is nine tenths of the run time. With it off, messages for untraded symbols
are skipped before the book. Tests and a full-day real run confirm the four output files are
byte-identical either way.

**S13. The strategy API has one addition: `ctx.tradable(symbol)`.** Without it a strategy
cannot tell the session is closed and learns only from a stream of rejects, which then trips
the reject-rate kill switch. Also added: `symbols()`, `symbol_name()`, `find_symbol()`.

**S14. A strategy must not resubmit from inside a reject.** A reject is delivered at the same
timestamp it was caused, so resubmitting at once loops forever at one instant. The sample
strategies wait for the next book update. Documented rather than prevented: a cap on events
per timestamp would be a second, hidden rate limit.

**S15. `run.json` is deterministic except for its last member.** The specification asks for
wall time in `run.json` and for byte-identical output. Those conflict. The four `.bin` files
are byte-identical between runs; `run.json` is identical except for the final `timing` object
(wall time and recorder stall counts), which is last and on its own line so the rest can be
compared textually.

**S16. SHA-256 is written in the project.** About 80 lines, tested against the FIPS vectors,
used once per run. Adding a cryptography dependency for that would have needed the owner's
approval and brought far more than is used. `--skip-input-hash` exists because hashing 8 GB
takes longer than a traded-symbols-only backtest.

## Risk

**R1. Integer arithmetic, with PnL held doubled.** The mid of an odd-width spread is half a
price unit. Marks are kept as bid + ask and PnL as twice its value; the division by two
happens once, when a number is printed. No rounding anywhere in the engine.

**R2. Total PnL is exact; the realised/unrealised split uses average cost.** Total is cash
plus position at mid, which needs no cost basis. The split needs one; closing part of a
position removes the same fraction of its cost, with truncating division. The rounding (under
one price unit per fill) lands in the split and never in the total.

**R3. Checks run in a fixed, documented order and the first failure wins.** Kill switch,
validity, session, halt, quantity, notional, collar, position, open orders, gross notional,
rate. The rate limit is last so that an order rejected for another reason does not spend a
token.

**R4. Worst-case position counts each side separately.** A buy is checked against position
plus all open buys plus itself; open sells are not allowed to offset it, because they may
never fill.

**R5. Gross notional is positions at mid plus open orders at their limit prices plus the new
order.** The specification says "maximum gross notional across symbols" without defining the
treatment of open orders. Counting them at limit price is the conservative reading.

**R6. Cancels are never rate limited or rejected.** A risk control that can stop you
withdrawing an order is worse than none.

**R7. The kill switch cancels but does not liquidate.** As specified: it cancels all open
orders and rejects new ones. It does not flatten positions, so PnL keeps moving afterwards.
On the reference day the demo config trips at 09:30:51 down 25 dollars and ends the day up
750 on the position it was holding.

**R8. A hard ceiling of 2^56 on notional, whatever the config says.** See `DEBUGGING.md`. It
is what makes the gate's own arithmetic safe against absurd orders.

**R9. Money limits are whole dollars in the config, price units in the engine.** Fewer zeros
to mistype in a file a person edits. `run.json` reports them in price units, as held.

## Recorder

**C1. Explicit padding, not `#pragma pack`.** Records use natural alignment with named padding
fields, and `static_assert`s on size and on having no padding bits. The files are "packed" in
the sense that matters and no field access is unaligned.

**C2. Events are recorded where and when they happen.** Strategy-side events (submitted,
cancel requested, risk reject) at strategy time, exchange-side events at exchange time. Each
file is therefore in time order without sorting.

**C3. `bbo.bin` covers the configured symbols, not "symbols the strategy traded".** Which
symbols a strategy will trade is not known until it has, and markouts need the top of book
from before the first fill. Recording every configured symbol is a superset and is
deterministic.

**C4. One ring, tagged slots.** The four files share one single-producer single-consumer ring
of 56-byte slots (file tag, size, bytes). Alternative: a ring per file. One ring keeps a
single writer thread simple and preserves each file's order trivially.

**C5. A full ring blocks the engine and is counted.** As specified. The stall is timed with
the wall clock, which is the only place engine-thread code reads one; the reading goes to
`run.json` and nowhere else.

**C6. The writer sleeps 50 microseconds when idle.** A spinning writer would burn a core for
the whole run to save nothing: the engine waits on the writer only when the ring is full, and
then there is work.

**C7. Two declarations of each layout, compared by a test.** As specified, C++ structs and
NumPy dtypes. `backtest --print-schema` prints names, offsets and sizes from `offsetof` and
the Python test compares every field, not only total sizes. The same dump carries the
enumeration names, so those are compared too.

## Analysis

**A1. The notebook's cells are executed by a test.** The notebook is the deliverable, so a
test runs its code cells against a real run. It is committed without outputs: outputs would
put derived market data and one day's results into the repository.

**A2. Markouts beyond the end of the session are missing, not stale.** A fill 30 seconds
before the close has no 60-second markout.

**A3. Jupyter is not in the pinned requirements.** The specification pins numpy, pandas,
pyarrow, matplotlib and pytest. `requirements-notebook.txt` adds what is needed to open or
render the notebook; nothing in CI needs it.

## Performance

**F1. The engine's book is chosen at compile time by one alias.** `-DITCHBT_BASELINE_BOOK=ON`
selects the standard-library book. The book benchmark instead instantiates every combination
side by side in one binary, so a comparison never depends on two builds.

**F2. The flat map stores reference + 1 and erases by backward shift.** Zero as the empty
marker makes a fresh table a `memset`. Backward-shift deletion leaves no tombstones, which
matters when 140 million orders pass through a table that never holds more than 2 million.

**F3. Level search is a linear scan from the best.** Measured against binary search; see the
optimisation log.

**F4. Zero allocation is a property of sizing, and the test says so.** The order store is
sized from `expected_orders` and traded symbols' level arrays from `level_reserve`. With those
adequate, a counting `operator new` sees zero allocations on the engine thread for every
strategy. With all 8,906 books built, untraded symbols' level arrays still grow on demand;
those are amortised vector growths, not per-message allocations. A companion test shows that
undersized stores do allocate and that the rehash counter reports it.

**F5. Latency timing is a separate build.** `-DITCHBT_TIMING=ON` adds a counter read before and
after every message. Throughput is quoted from the untimed build and the cost of timing is
reported beside the latencies.

## Sharding

**H1. The router is the ordinary parser with a handler that only routes.** It reuses `parse()`
and its validation, so a worker never sees a malformed record. Alternative: a second framing
loop. Rejected for the same reason as P8.

**H2. Queues carry pointers into the mapping.** Nothing is copied; the mapping is read-only
and outlives the run.

**H3. Symbols are dealt round-robin in config order.** It balances the traded symbols, which
carry the work. Hashing the locate could put every liquid name in one shard.

**H4. Cross-symbol risk limits are refused, not approximated.** Gross notional, the total-PnL
loss limit, the rate limit and the reject-rate switch all depend on events across symbols. A
per-shard version would give answers that depend on the shard count. The run fails with the
list of offending settings instead.

**H5. Each shard writes its own files.** Merging into one file would need a second pass or a
lock. The loader concatenates shards and sorts stably by time, which preserves each symbol's
order because a symbol lives in exactly one shard.

**H6. System events go to every shard.** They carry locate 0 and every engine needs the
session state. Everything else with a locate goes to exactly one.

## Benchmarks

**M1. Results that cannot be trusted are deleted, not footnoted.** Measurements taken while
the laptop slept were removed from the repository, with a note listing them and the evidence.
Three sections of `BENCHMARKS.md` consequently read "not measured".

**M2. Which result directories are published is a committed list.** `bench/results/CURRENT`
names them. The renderer once picked "the latest" by directory name and chose the wrong one.

**M3. The verdict text in the optimisation log is chosen by the measured ratio.** The
hypothesis for each change is written by hand; the numbers, and whether the row says kept,
no gain or slower, come from the results.

**M4. Benchmarks are guarded against sleep on macOS.** Each is repeated if the power log shows
a sleep during it. On Linux the guard does nothing.

**M5. The micro-benchmarks replay a real reference trace.** Random keys would flatter or
punish a hash table arbitrarily; the trace has the real mix of sequential references and
lifetimes.

## Where this departs from the specification

Gathered in one place. Each is explained above or in `BENCHMARKS.md`.

| Specification | What was done | Why |
| --- | --- | --- |
| Linux x86-64, bare metal for benchmarks | Developed and benchmarked on macOS, Apple Silicon; Linux only in CI | The only machine available; the owner's decision |
| `perf stat` counters, flame graphs, `taskset`, governor | Not collected or not applicable; scripts exist for Linux and are marked as never run | Not available on macOS without root |
| `std::byteswap` | Compiler builtins | `std::byteswap` is C++23; the project is strict C++20 |
| Book not crossed during regular hours | Tolerated for 100 ms after a halt release while continuously crossed | The feed itself violates the stricter rule 642 times on the reference day |
| Acknowledgements return after `ack_latency_ns` | Every update from the exchange is delayed by it, not only acknowledgements | So the strategy never learns of a fill before it could have been told |
| `bbo.bin` for symbols the strategy traded | For every configured symbol | "Traded" is not known in advance |
| `run.json` holds wall time; output is byte-identical | Byte-identical except `run.json`'s final `timing` object | The two requirements conflict |
| 10 measured runs | 5 for the two baseline-book variants and for end-to-end runs | Minutes per run |
| Three tools | A fourth, `book-check` | The invariants on the real file needed a home |
| `Context` as listed | Plus `tradable`, `symbols`, `symbol_name`, `find_symbol` | A strategy needs to know when it can trade |
| Propose interfaces and wait for approval before each milestone | Done for M0 and M1; the owner then waived approval for the rest | The owner's instruction |
| Targets in 8.6 met or the gap explained | None met outright in the published run; explained | Battery, Low Power Mode, and a laptop CPU measured against desktop aims |
| At least six `DEBUGGING.md` entries from real bugs | Twenty-two real, two labelled exercises | Three of the eight categories never arose naturally |
| Hot path allocation-free | For traded symbols with adequately sized stores | Untraded symbols' level arrays grow on demand when every book is built |
