# Debugging log

Real bugs found while building this project, and how each was tracked down. Entries are written
at the time of the fix, with real commands and trimmed real output, and each links the fixing
commit and the regression test that now covers it. Nothing here is invented; a deliberate
fault-injection exercise is labelled as an exercise.

Entries are in the order the bugs were found. Small ones get a short entry; they are listed
anyway so the record is complete.

## Time parser accepted a trailing decimal point

- Date / commit: 2026-11-11, `8c98a14` (found and fixed before the code was first committed)
- Area: build (tool support)
- Tools: GoogleTest

### Symptom
The unit test written alongside `parse_time` failed on its list of malformed inputs:

```
tests/unit/util_test.cpp:29: Failure
Value of: parse_time(bad)
  Actual: true
Expected: false
10:30:00.
```

### Investigation
The parser is a single pass with an `in_fraction` flag. A `.` sets the flag; digits after it
accumulate into the fraction. Nothing checked that at least one digit followed.

### Root cause
`"10:30:00."` set `in_fraction` and then hit the end of input with `fraction == 0`, which is
indistinguishable from a valid `"10:30:00.0"` unless the digit count is tracked.

### Fix
After the loop, reject input where `in_fraction` is set but the fraction scale never moved:
`if (in_fraction && fraction_scale == kNanosPerSecond) return std::nullopt;`

### Prevention
`Time.RejectsMalformedTimes` in `tests/unit/util_test.cpp` covers this input and eleven others.

## Benchmark results always claimed a dirty git tree

- Date / commit: 2026-11-11, `ca0431b`
- Area: performance (tooling)
- Tools: reading the rendered table

### Symptom
The first rendered machine table read `Commit: 0db9dc5372b9 (dirty tree)` although the benchmark
had been started from a freshly committed, clean checkout.

### Investigation
`run_bench.sh` did `mkdir -p "$OUT"` and then `scripts/sysinfo.sh >"$OUT/sysinfo.json"`. The
shell creates the redirect target before it starts `sysinfo.sh`, and `sysinfo.sh` decides
"dirty" from `git status --porcelain`, which lists the new untracked file.

### Root cause
The script observed its own output. Any run would have reported a dirty tree.

### Fix
Capture the system information into a variable before creating the results directory, then
write it. The misleading result directory was deleted and the benchmark re-run at `ca0431b`.

### Prevention
The ordering is now explicit in the script with a comment. `render.py` prints "(dirty tree)" in
the machine table, so a recurrence is visible in `BENCHMARKS.md` itself.

## Book reported crossed 642 times during regular hours

- Date / commit: 2026-11-11, `b7f942c`
- Area: book (validation)
- Tools: `book-check` with record offsets, a throwaway Python script to print raw messages
  around an offset

### Symptom
The first full-day run of `book-check` failed the specification's invariant that the book is
never crossed during regular trading hours:

```
CROSSED ZJZZT at offset 1248249450 (10:00:12.444352514, message 39778763, type C): bid 130000 x ask 130000
CROSSED CETXP at offset 1388550227 (10:04:47.932322086, message 44386130, type C): bid 7900 x ask 7700
CROSSED SLGL at offset 1566785620 (10:10:57.018494017, message 50259499, type C): bid 184800 x ask 160000
...
crossed/locked checks:     255041094
crossed/locked violations: 642
FAIL: see above
```

Every other invariant held (no unknown references, no failed recount), so the book's
arithmetic was not obviously broken.

### Investigation
The specification says not to assume the data is wrong, so the first hypothesis was a book bug
in handling `C` (executed with price): 631 of the 642 violations were on `C` messages.

1. Grouped the violations by symbol. Only ten symbols were involved and one, MKD, accounted for
   486. A bug in `C` handling would not be so concentrated: there are 99,917 `C` messages across
   thousands of symbols.

   ```
    486 MKD     57 ARDS    37 MBOT    24 SLGL    21 QUIK
     11 WKEY     3 ZWZZT    1 ZJZZT    1 YI       1 CETXP
   ```

2. Violations came in bursts with timestamps microseconds apart, which suggested one event per
   burst rather than a steady leak.

3. Printed the raw messages for SLGL's locate around the first reported offset:

   ```
   1566785513 10:10:57.018379306 C ref=58017628 qty=3548 match=1313757 printable=N px=175000
   1566785551 10:10:57.018379306 Q shares=7145 px=175000 type=H
   1566785593 10:10:57.018379306 H state=T reason=
   1566785620 10:10:57.018494017 C ref=57550236 qty=5 match=1313769 printable=N px=175000
   1566785658 10:10:57.018494251 C ref=59836540 qty=1 match=1313770 printable=N px=175000
   ... (30 more non-printable C messages at the same price) ...
   1566786565 10:10:57.018572264 C ref=58747744 qty=50 match=1313820 printable=N px=175000
   ```

   A cross of type `H` (IPO or halted security) prints, the trading action flips the symbol to
   `T`, and then non-printable executions at the cross price keep arriving for another 200
   microseconds. Those executions are the cross itself being unwound order by order.

4. Checked a violation that was not a `C` (MKD, type `F`, 10:54:21.569624647). It sits in the
   middle of the same kind of burst: new orders arriving while the cross's executions are still
   being published.

5. Dead end worth recording: the opening cross at 09:30 produced no violations at all, so
   "crosses in general" was not the explanation. The market-open system event `Q` evidently
   arrives after the opening cross has finished printing; only intraday releases overlap.

### Root cause
Not a book defect and not bad data: the invariant was stated too broadly. After a halt or IPO
release, the feed sends the trading action before it has finished sending the executions of
the reopening cross, so the displayed book is legitimately crossed for a moment while the
symbol is already marked as trading.

### Fix
`book-check` now tracks, per symbol, whether the book has been continuously crossed since the
last `T` trading action. Such a state is counted as a "reopening transient", not a violation,
provided the release was less than 100 ms earlier. Anything else is still a violation.

```
reopening transients:      642 (longest 1.214 ms after release)
crossed/locked violations: 0
PASS: all invariants hold
```

Running with `--reopen-tolerance-ms 0` reproduces the original 642, which confirms the rule
explains every one of them and nothing else is hiding behind it.

### Prevention
Three end-to-end tests in `analysis/tests/test_tools.py`: a crossed book well after a release
still fails; a cross still printing just after a release passes; a cross that persists a second
after the release fails. The first version of the second test was itself wrong (it placed the
orders after the release, so the book was uncrossed in between and the tolerance correctly did
not apply), which was a useful check that the rule is as narrow as intended.

## Reconciliation script read its arguments off by one

- Date / commit: 2026-11-11, `0bb462d` (found and fixed before the script was first committed)
- Area: build (analysis tooling)
- Tools: Python traceback

### Symptom
```
  File "analysis/reconcile_close.py", line 36, in main
    day = dt.datetime.strptime(date_text, "%Y-%m-%d").replace(tzinfo=dt.timezone.utc)
ValueError: time data 'AAPL' does not match format '%Y-%m-%d'
```

### Investigation
The date was plainly on the command line, so `date_text` held the wrong element.

### Root cause
`main(sys.argv[1:])` strips the program name, but `main` indexed its list as if the program
name were still at position 0.

### Fix
Index from 0 inside `main` and adjust the argument-count check to match.

### Prevention
None beyond the fix: the script needs the network and the data file, so it is not in CI. It
fails loudly on its first line if this regresses.

## A shell heredoc executed the backticks in a debugging entry

- Date / commit: 2026-11-11, this commit
- Area: build (documentation tooling)
- Tools: reading the shell's error output

### Symptom
Writing the two entries above into this file produced a screen of shell errors, and the
committed text had lost every inline code span:

```
(eval):107: permission denied: apps/book_check.cpp
(eval):107: command not found: book-check
(eval):108: command not found: 486
(eval):107: command not found: --reopen-tolerance-ms
```

### Investigation
The errors named exactly the words that were inside backticks in the Markdown.

### Root cause
The text was passed to a script through an unquoted heredoc (`<<E`), in which the shell
performs command substitution. Each backtick-quoted span was run as a command and replaced by
its (empty) output. Nothing harmful ran, but only because none of the spans happened to be a
destructive command.

### Fix
Restored the file from the previous commit and rewrote the entries through a quoted heredoc
(`<<'EOF'`), which the shell passes through untouched.

### Prevention
Documentation is now always written through quoted heredocs. The mangled commit stays in the
history as the record of what happened.

## The compiler could not link anything on the development machine

- Date / commit: 2026-11-11, no code change (environment); worked around from the first build
- Area: build
- Tools: the linker's own error, `xcrun`, `ls`

### Symptom
The first CMake configure failed before reaching any project code:

```
ld: multiple errors: tapi error: malformed file
/Library/Developer/CommandLineTools/SDKs/MacOSX27.0.sdk/usr/lib/libSystem.B.tbd:4:20: error: unknown architecture
                   arm64e.x1-macos, arm64e.x1-maccatalyst ]
```

A one-line `int main(){}` failed the same way, so it was not CMake.

### Investigation
`xcrun --show-sdk-path` reported `MacOSX27.0.sdk`, on a machine running macOS 26.6 with
Command Line Tools 26.6. `ls` on the SDK directory showed three SDKs, the 27.0 one dated a
month earlier than the others: a leftover from a beta. The installed linker predates it and
does not know an architecture name its library stubs use.

### Root cause
The default SDK was newer than the toolchain that had to read it.

### Fix
`export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk` for every build.
Nothing in the repository changes; the README's macOS note describes it.

### Prevention
None in the repository: it is a property of one machine. CI on Ubuntu is the authoritative
build precisely so that a broken local toolchain cannot hide or invent problems.

## A comment swallowed the line after it, but only for GCC

- Date / commit: 2026-11-11, `0c41dee`
- Area: build
- Tools: CI logs (GCC 13)

### Symptom
Every GCC job in CI failed, while every Clang job and every local build passed:

```
apps/book_dump.cpp:6:1: error: multi-line comment [-Werror=comment]
```

### Investigation
Line 6 was inside the usage comment at the top of the file, showing a shell command split
across two lines with a trailing backslash.

### Root cause
A backslash at the end of a `//` comment line continues the comment onto the next line. Here
the next line was also a comment, so nothing was lost, but GCC warns (rightly: had the next
line been code, it would have vanished) and `-Werror` made it fatal. Clang does not warn by
default. The failure also went unseen for several pushes because CI results were not being
checked after each one.

### Fix
Removed the backslash. From this point on every push is followed by a check of its CI run.

### Prevention
CI builds with both compilers; that is what caught it. A grep for comment lines ending in a
backslash found no others.

## The config loader accepted `build_all_books = 1`

- Date / commit: 2026-11-11, `d1e360f` (found and fixed before the loader was first committed)
- Area: build (configuration)
- Tools: GoogleTest

### Symptom
```
tests/unit/config_test.cpp:128: Failure
Value of: load("build_all_books = 1\n", error)
  Actual: true
Expected: false
```

### Investigation
The loader read booleans with toml++'s `node.value<bool>()`. Its documentation says
`value<T>()` "permits conversions": an integer is a valid source for a `bool`, a `bool` or a
float that happens to be whole is a valid source for an integer.

### Root cause
A wrong assumption about a library call. The loader's whole point is to be strict (unknown
keys are errors so a typo cannot leave a risk limit at its default), and a silent type
conversion defeats that: `max_position = true` would have meant a limit of one share.

### Fix
Every read uses `value_exact<T>()`, which requires the TOML type to match.

### Prevention
`Config.WrongTypesAndRangesAreRejected` now covers integer-for-boolean, boolean-for-integer,
float-for-integer and integer-for-string.

## Position was marked against an after-hours quote of 200.00 x 414.95

- Date / commit: 2026-11-11, `0b150b3`
- Area: risk
- Tools: an independent recomputation in Python from the run's own files

### Symptom
The first full-day PassiveMaker run reported, for one symbol:

```
TSLA     position      -55  realised     -8689.93  unrealised      5894.39
```

An unrealised gain of 5,894 dollars on 55 shares is 107 dollars a share, on a stock that moved
a few dollars all day.

### Investigation
1. First hypothesis: the average-cost bookkeeping in the risk gate drifts, since that is the
   intricate part (partial closes, trades through zero). Recomputed position, realised and
   unrealised PnL for every symbol in Python from `fills.bin` and `bbo.bin` alone:

   ```
   TSLA pos -55 -55 realised -86899294 -86899294 unreal2 117887888 117887888 ... last bid/ask 2000000 4149500
   AMZN pos -200 -200 realised -240484700 -240484700 unreal2 -92500000 -92500000 ... last bid/ask 18400000 19000000
   ```

   Each pair of numbers is Python's figure and then the engine's. They agree exactly for all
   ten symbols, so the bookkeeping was right. Dead end, but a useful one: the script became a
   permanent test.
2. The last column was the answer. The final top of book for TSLA was 200.0000 bid, 414.9500
   offered, a mid of 307.

### Root cause
Positions were marked to the mid on every top-of-book change, including long after the close,
when the visible book is a few stale orders far from the market. The arithmetic was correct
and the mark was meaningless. It could also have tripped the loss-limit kill switch on a
position that had not lost anything.

### Fix
The engine marks a position only while its symbol can trade (session open, or extended hours
enabled, and not halted). The last such mark stands afterwards. Each `bbo.bin` record carries
a "tradable" flag so analysis marks on exactly the same records.

### Prevention
`test_pnl_recomputed_from_fills_matches_the_engine_exactly` recomputes PnL in Python for every
symbol of a run and requires equality to the last price unit, and
`test_pnl_timeline_ends_at_the_engines_figure` does the same for the plotted timeline.

## Largest possible order overflowed the notional calculation

- Date / commit: 2026-11-11, `7fbf55b`
- Area: risk
- Tools: UndefinedBehaviorSanitizer

### Symptom
A risk test that sent a deliberately huge order aborted under the `asan` preset:

```
include/itchbt/risk/risk.hpp:233:59: runtime error: signed integer overflow:
4000000000 * 4000000000 cannot be represented in type 'std::int64_t' (aka 'long long')
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior include/itchbt/risk/risk.hpp:233:59
```

### Investigation
Line 233 computed `std::int64_t{price} * qty`. Price and quantity are both 32-bit unsigned, so
their product can reach about 1.8e19; a signed 64-bit integer stops at 9.2e18.

### Root cause
The widening was to the wrong type. The comment beside the code said prices were widened "so
the product cannot overflow", which was true only for realistic orders. The risk gate is the
one component whose job is to cope with unrealistic ones: with every limit at its default,
a strategy bug that sent this order would have been undefined behaviour instead of a reject.

### Fix
The product is formed in unsigned 64-bit, which always fits, and compared with the limit
before being narrowed. Independently of configuration, a hard ceiling of 2^56 price units x
shares (about 7.2 trillion dollars) applies to one order and to gross exposure, leaving a
factor of 128 of headroom for doubled PnL and adverse marks.

### Prevention
`RiskChecks.AbsurdNotionalIsRejectedNotOverflowed` sends the largest representable order and
`RiskChecks.HardNotionalCeilingAppliesWhateverTheConfigSays` tests the ceiling at its exact
boundary. Both run under UBSan in CI.

## A test expected the wrong answer from the reject-rate window

- Date / commit: 2026-11-11, `7fbf55b`
- Area: risk (test)
- Tools: GoogleTest, pencil

### Symptom
```
tests/unit/risk_test.cpp:444: Failure
Value of: gate.note_reject(1'150)
  Actual: true
Expected: false
```

### Investigation
Threshold 3, window 1,000 ns, rejects at 100, 200, 300, 1101, 1150. Worked by hand: at 1150
the last four rejects are 200, 300, 1101 and 1150, which span 950 ns. Four rejects inside one
window is "more than three": the switch should trip, and it did.

### Root cause
The test was wrong, not the code. It had been written from the idea "the first three are old
by now" without redoing the window arithmetic for each new reject.

### Fix
The test now states, beside each call, which four timestamps are being compared and their
span.

### Prevention
Boundary tests in this file carry the arithmetic in a comment, so a reviewer can check the
expectation without running anything.

## A commit that did not compile was pushed

- Date / commit: 2026-11-11, broken in `fcf1632`, fixed in `2535144`
- Area: build (process)
- Tools: reading the build log that had scrolled past

### Symptom
After a lint clean-up, the summary line said `100% tests passed` and the commit was pushed.
The output above that line contained:

```
include/itchbt/util/spsc_ring.hpp:33:11: error: no matching constructor for
initialization of 'std::vector<itchbt::record::Recorder::Slot>'
   33 |           slots_(std::make_unique<T[]>(capacity_)) {}
```

### Investigation
The member had been changed from `std::unique_ptr<T[]>` to `std::vector<T>` by a scripted
text replacement, and the matching replacement in the constructor silently did nothing:
clang-format had re-wrapped that line since the pattern was written. The build failed. The
test step ran anyway and executed the previous, stale binaries, which passed.

### Root cause
Two things. A scripted edit that fails to match must be an error, not a no-op. And the
commit step did not depend on the build step's exit status, only on having reached the end
of the script.

### Fix
The constructor was corrected. `scripts/check.sh` now formats, builds and tests under
`set -e` and is what runs before a commit; scripted edits assert that they matched.

### Prevention
`scripts/check.sh`, and CI, which would have caught it within minutes regardless. The broken
commit stays in the history.

## New files reached CI unformatted

- Date / commit: 2026-11-11, `f5d5b26`
- Area: build (tooling)
- Tools: CI logs

### Symptom
```
src/record/schema.cpp:23:37: error: code should be clang-formatted [-Wclang-format-violations]
```

although `scripts/format.sh` had been run before the commit.

### Investigation
`format.sh` took its file list from `git ls-files`, which lists tracked files. `schema.cpp`
was new: it was formatted by nobody, then added and committed.

### Root cause
The formatter only saw files git already knew about.

### Fix
The file list is now `git ls-files --cached --others --exclude-standard`: tracked files plus
untracked ones that are not ignored.

### Prevention
`scripts/check.sh` runs `format.sh --check` first, over the same list.

## Flat order map treated an empty slot as a match for one reference

- Date / commit: 2026-11-11, `8c3c017` (found and fixed before the map was first committed)
- Area: book
- Tools: GoogleTest

### Symptom
```
tests/property/book_property_test.cpp:240: Failure
Expected equality of these values:
  flat.find(0xFFFFFFFFFFFFFFFFULL)
    Which is: 0x615000000088
  nullptr
```

`find` returned a pointer for a reference that had never been inserted.

### Investigation
The table stores `reference + 1` so that a stored key of zero can mean "empty". For reference
2^64 - 1 the stored key wraps to zero. `insert` already refused that reference, but `find`
probed to the first empty slot and compared its key (zero) with the wanted key (zero).

### Root cause
The empty marker and one legal key were the same value, and only one of the three operations
guarded against it. `erase` had the same hole and was worse: it would have "erased" an empty
slot and decremented the size.

### Fix
`find` and `erase` both treat a stored key equal to the empty marker as absent.

### Prevention
`FlatOrderMap.ExtremeReferences` covers insert, find and erase of that reference and checks
the size afterwards. ITCH references are small sequential numbers, so the feed could never
have triggered this; the test exists because the fuzzer feeds the book arbitrary references.

## The tick counter read nothing on Apple Silicon

- Date / commit: 2026-11-11, `48dfcd9` (found and fixed before the code was first committed)
- Area: performance (measurement)
- Tools: the benchmark's own output

### Symptom
The first run of the timing build reported:

```
"tick_ns": 1.000,
"latency": [{"type": "all", "count": 60000000, "p50_ns": 18446754034956730368.0,
             "p90_ns": 18446754034956730368.0, ... "mean_ns": 969637653.1}
```

A median latency of 1.8e19 ns is 2^64: an unsigned subtraction that went negative.

### Investigation
`tick_ns` of exactly 1.000 is the calibration routine's fallback for "the counter did not
advance". So `ticks_begin()` was returning a constant, or garbage. Its body was one line of
inline assembly: `"isb; mrs %0, cntvct_el0"`.

### Root cause
In Apple's assembler for arm64, `;` begins a comment. Everything after `isb` was discarded,
the `mrs` that reads the counter never ran, and the function returned whatever was in the
output register. On Linux the same string assembles as two instructions, so CI could not
have caught it either: nothing there runs the timing build.

### Fix
The two instructions are separated by a newline (`"isb\n\tmrs %0, cntvct_el0"`), which both
assemblers accept.

### Prevention
`Ticks.CounterAdvancesAndCalibratesToAPlausibleRate` requires the counter to advance and the
calibrated rate to be physically plausible. It runs on both platforms.

## Benchmark throughput halved between two sessions, and one message took 112 seconds

- Date / commit: 2026-11-11, addressed in `scripts/run_bench.sh` and `scripts/sysinfo.sh`
- Area: performance
- Tools: `pmset`, `uptime`, `ps`, the latency histogram's maximum

### Symptom
Re-running the baseline book build to compare it with the new book gave less than half the
throughput recorded a few hours earlier (4.8 million messages a second then):

```
baseline
measured run 0: 27.360 s, 2.19 M msg/s
```

and the first latency run reported a maximum of 112 seconds for a single message while the
whole run took 131:

```
measured run 0: 131.025 s, 0.46 M msg/s
... {"type": "E", "count": 1017818, "p50_ns": 125.0, ... "max_ns": 112754770653.9, ...}
```

### Investigation
1. Code regression? The baseline book had not changed, and the earlier binary gave the same
   low number. Not the code.
2. Something else using the machine? `uptime` showed a load average of 2 to 3 but `ps` showed
   nothing above 12% of a core.
3. A 112-second gap inside one message is not a slow message, it is a stopped process. The
   machine was unattended; `pmset -g` showed `sleep 1` and `displaysleep 2`. It had gone to
   sleep in the middle of the run.
4. `pmset -g batt` showed "Now drawing from 'Battery Power'" where the earlier results had
   recorded AC power, and `pmset -g` showed `lowpowermode 1`. Low Power Mode caps the
   performance cores.

### Root cause
Two changes in the machine's state, neither in the code: it had been unplugged, which switched
on Low Power Mode, and nothing was stopping it from sleeping during a run.

### Fix
`run_bench.sh` re-executes itself under `caffeinate -d -i -m` on macOS so the machine cannot
sleep mid-run. `sysinfo.sh` records Low Power Mode and the renderer prints it in the machine
table in bold. The whole suite is always run in one session and `render.py` reads one results
directory by default, so every ratio on the page compares runs taken in the same state.

### Prevention
The machine table makes the state visible next to every number. Changing Low Power Mode needs
administrator rights, so it could not be turned off for these runs; `BENCHMARKS.md` says so.

## A micro-benchmark said virtual calls were twice as fast as inlined ones

- Date / commit: 2026-11-11, replaced before the benchmark was first committed
- Area: performance (measurement)
- Tools: Google Benchmark, reading the loop

### Symptom
```
BM_DispatchConcept        2.14 ns
BM_DispatchVirtual       0.983 ns
BM_SpscPushPop           0.914 ns
```

A strategy callback reached through a virtual call measured faster than the same callback
inlined, and a push plus a pop of a 56-byte slot took under a nanosecond.

### Investigation
Each benchmark was a tight loop around one call with `DoNotOptimize` on an argument. In the
"concept" loop that forced the argument through memory on every iteration and the inlined
callback's accumulator joined the same store-to-load chain; in the "virtual" loop the call
boundary happened to break the chain. The numbers measured the loops' dependency chains, not
dispatch. The ring benchmark ran producer and consumer on one thread, where the optimiser can
see both ends.

### Root cause
The benchmarks were too small to contain the thing they claimed to measure.

### Fix
Dispatch is now measured by replaying the same 400,000-message stream through the whole engine
with a concept-bound strategy and again through the virtual adapter, and dividing the
difference by the number of callbacks. The ring is measured with a producer and a consumer on
separate threads, the consumer checking sequence numbers.

### Prevention
Both benchmarks now fail rather than report if their output is wrong (the replay must succeed,
the sequence must be intact). The write-up in `BENCHMARKS.md` gives the per-callback figure
with the replay counts it was derived from.

## The benchmark's "same book" fingerprint was zero for every implementation

- Date / commit: 2026-11-12, `ea838ac`
- Area: performance (measurement)
- Tools: reading the rendered table

### Symptom
The first full benchmark table had a fingerprint column meant to prove that every book
implementation builds the same books:

```
| baseline                   | ... | `00000000` |
| flat_orders/reserve        | ... | `00000000` |
| optimised/reserve/prefetch | ... | `00000000` |
```

Identical, as hoped, and identically zero.

### Investigation
In quick trials on the first 60 million messages the fingerprint had been a real number
(`224614349`) and equal across implementations, which is why it was trusted. The fingerprint
hashes every level of every book once the run has finished. `book-check` had already reported
the relevant fact about the full day: "live orders at end: 0".

### Root cause
At the end of the trading day every order has been deleted, so every book is empty and the
hash of nothing is zero. On the full file the check passed for any implementation at all,
including one that built wrong books and then emptied them. The benchmark's own guard ("abort
if the fingerprint changes between runs") was equally blind.

### Fix
The fingerprint now folds in the state of every book every 2^24 book operations (16
checkpoints across the reference day) and once at the end. Re-run over the full day, all five
variants report `1972692282` with 16 checkpoints.

### Prevention
The rendered table prints the number of checkpoints beside the fingerprint and says "not
recorded" when there were none, so an end-of-day-only fingerprint cannot be mistaken for
evidence again. Equivalence does not rest on this check alone: the property test compares
implementations after every message and the Python reference book is compared level by level.

## Half the benchmark suite was measured while the laptop was asleep

- Date / commit: 2026-11-12, `ea838ac` (sleep guard) and `a398838` (results removed)
- Area: performance (measurement)
- Tools: `pmset -g log`, file modification times, the results themselves

### Symptom
The suite ran unattended for three hours. The earlier results were tight; the later ones were
absurd:

```
optimised/reserve/prefetch   all messages   p99 799 ns   max 710920.50 ms
passive_maker/async   123.05 (42.29 to 230.35) s
passive_maker/none    159.21 (40.70 to 362.72) s
passive_maker/sync    325.32 (45.52 to 1023.37) s
```

A message taking 710 seconds, and the same end-to-end workload taking between 42 and 1,023
seconds, with recording off apparently slower than recording on.

### Investigation
1. The suite already ran under `caffeinate` after the earlier sleep incident, so sleep was not
   the first suspect. But a 710-second gap in one message is not something code can do.
2. `pmset -g log | grep "Entering Sleep"`, bucketed by minute, against the modification time of
   each result file:

   ```
   ... 21:21(1) 21:22(1) 22:45(1) 22:50(1) 23:03(1) 23:04(1) 23:14(1) 23:15(1) ...
   21:22 parse_mmap_decode.json  ...  22:31 book_optimised_prefetch.json
   22:38 latency_baseline.json   22:44 latency_optimised.json
   23:15 latency_optimised_prefetch.json   23:29 e2e_async.json   00:05 e2e_sync.json
   ```

   No sleep at all between 21:22 and 22:45; "Maintenance Sleep" every few minutes after that.
   Everything finished by 22:44 is clean. Everything after is not.
3. Why `caffeinate` stopped helping at 22:45 is not known. The log entries read "Using Batt",
   and a laptop on battery with its lid shut is allowed to nap whatever a process asserts.

### Root cause
The machine was asleep for parts of the later benchmarks, and wall-clock timing counts sleep
as work.

### Fix
The corrupt result files were deleted, not averaged or footnoted; `NOTES.md` in the results
directory lists them and the evidence. `run_bench.sh` now counts "Entering Sleep" entries in
the power log before and after each benchmark and repeats any during which the count changed,
up to five times. The deleted measurements were repeated under that guard.

### Prevention
The guard, and the habit that found the problem: results are read before they are published,
and a number that cannot be true is investigated, not reported.

## The sleep guard's retry was killed by the pipe it was writing to

- Date / commit: 2026-11-12, `cd9db4e`
- Area: performance (tooling)
- Tools: reading the suite's log

### Symptom
With the new sleep guard in place, the end-to-end and sharding parts of the suite each printed
one run, one "machine slept during this run (attempt 1); repeating", and then stopped without
an error. No result file was written for either.

```
== end_to_end passive_maker/async
strategy passive_maker on 10 symbols: 268744780 messages in 338.93 s (0.79 M msg/s)
   machine slept during this run (attempt 1); repeating
Results in bench/results/2026-11-12-3a6dcf0
== sharding 1
```

Separately, the latency part failed all five attempts, as designed, but left the fifth
attempt's sleep-affected `latency_baseline.json` in the results directory.

### Investigation
The two parts that stopped silently were the two whose command line ended in `| head -1`, to
show only the summary line of each backtest.

### Root cause
`head -1` exits after one line. The first attempt printed its line and `head` was gone; the
retry then wrote to a closed pipe, was killed by SIGPIPE, and `set -o pipefail` ended the
script. The guard had only ever been tried on commands that were not piped. The leftover file
was a second gap: a benchmark that gives up should not leave its last output where the
renderer will find it.

### Fix
Those commands write to a log file and the summary line is extracted afterwards. When the
guard gives up it deletes the output named by `--out` or `--benchmark_out`.

### Prevention
None automated: the suite needs the data file and minutes of machine time, so it is not in
CI. The stray result was caught by listing the results directory before rendering it.

## EXERCISE (fault injection): reading the 6-byte timestamp as 8 bytes

This is a deliberate exercise, not a bug that occurred. No memory error arose naturally while
building the project, so the classic one for ITCH parsers was injected to see what would catch
it. The result changed the test suite.

- Date / commit: 2026-11-12, regression test added in the commit that adds this entry
- Area: parser
- Tools: AddressSanitizer

### Injected fault
`load_be48` was changed from a 2-byte plus a 4-byte load to `load_be<std::uint64_t>(p) >> 16`:
the "read eight bytes and shift" shortcut, which is correct for every message except one that
ends within two bytes of the end of its buffer.

### Symptom
The five tests written specifically for the endian loaders all passed. Twelve other tests
aborted under the `asan` preset, every one of them a stream ending in a 12-byte system event:

```
==35975==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x60200000339e
READ of size 8 at 0x60200000339e thread T0
    #0 itchbt::itch::decode_system_event(std::byte const*) parser.hpp:101
    #1 void itchbt::itch::dispatch<itchbt::test::Collector>(std::byte const*, ...) parser.hpp:237
    #2 itchbt::itch::ParseResult itchbt::itch::parse<itchbt::test::Collector>(...)
    #3 itchbt::itch::(anonymous namespace)::Parser_DecodesSystemEvent_Test::TestBody() parser_test.cpp:101
0x6020000033a0 is located 0 bytes after 16-byte region [0x602000003390,0x6020000033a0)
```

### Investigation
The report reads directly: an 8-byte read starting two bytes before the end of a 16-byte
allocation (a 2-byte length plus a 12-byte message, rounded), from the system event decoder.
The timestamp is at offset 5 of a 12-byte message, so bytes 5 to 12 were read and byte 12 does
not exist.

The more useful finding was the tests that did *not* fail. `Endian.LoadsSixByteTimestamp` and
`Endian.SixByteLoadIgnoresNeighbouringBytes` build their input in stack arrays of constants;
at `-O1` the compiler folds the whole load away, so there is no memory access left for the
sanitizer to check.

### Root cause (of the gap)
The fault was caught, but by accident of which message happened to end the streams in other
tests, not by the test that exists for this function.

### Fix
The fault was reverted. `Endian.SixByteLoadReadsExactlySixBytes` was added: a heap buffer of
exactly six bytes, read through a volatile pointer so the load cannot be folded. With the
fault re-injected it fails on its own:

```
==36167==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000003317
READ of size 8 at 0x602000003317 thread T0
```

### Prevention
That test, plus the fuzz target, whose inputs are exactly-sized heap buffers.

## EXERCISE (fault injection): publishing a ring slot before writing it

This is a deliberate exercise, not a bug that occurred. The recorder and the sharded runner
passed ThreadSanitizer from their first run, so an ordering bug was injected into the ring
buffer they share to confirm the tests and the tool would catch one.

- Date / commit: 2026-11-12, no code change kept
- Area: recorder (shared ring buffer)
- Tools: ThreadSanitizer

### Injected fault
In `SpscRing::try_push` the two statements were swapped, so the new tail index was published
before the slot was written:

```cpp
tail_.store(tail + 1, std::memory_order_release); // published first
slots_[tail & mask_] = value;
```

Single-threaded, every test still passes. Across threads the consumer can see the new tail and
read a slot the producer is still writing.

### Symptom
`SpscRing.TwoThreadsTransferEveryItemInOrderIntact` under the `tsan` preset:

```
WARNING: ThreadSanitizer: data race (pid=36281)
  Read of size 8 at 0x00010a102dc0 by thread T1:
    #0 ... SpscRing_TwoThreadsTransferEveryItemInOrderIntact_Test::TestBody()::$_0 ...
  Previous write of size 8 at 0x00010a102dc0 by main thread:
    #0 ... SpscRing_TwoThreadsTransferEveryItemInOrderIntact_Test::TestBody() spsc_ring_test.cpp:92
  Location is heap block of size 192 at 0x00010a102dc0 allocated by main thread:
    #1 ... TestBody() spsc_ring_test.cpp:73
```

### Investigation
The racing address is inside a 192-byte heap block allocated where the test constructs the
ring: eight 24-byte slots. The write is the producer's line 92 (`try_push`) and the read is in
the consumer lambda. So a slot is being read while it is written, which can only happen if the
consumer was told the slot existed too early. That points straight at the order of the store
to `tail_` and the slot write.

### Root cause
The release store is what makes the slot's contents visible to the consumer; it has to come
after them.

### Fix
The fault was reverted. No change was needed: the existing test and CI's TSan job catch it.

### Prevention
The two-thread transfer test deliberately uses an 8-slot ring so that "full" and "empty" are
hit constantly, and it checks every item's three fields against its sequence number, so even
without TSan a torn read would fail it.

## Coverage against the specification's list

The specification asks for entries covering as many of eight categories as arise.

| Category | Arose naturally? | Entries |
| --- | --- | --- |
| Memory error | No | One labelled exercise (8-byte timestamp read), which exposed a real gap in the tests |
| Undefined behaviour | Yes | Notional overflow, found by UBSan |
| Data race or ordering bug | No | One labelled exercise (ring slot published early) |
| Crash analysed after the fact | No | None. No crash occurred outside sanitizer aborts, and an attempted exercise with `lldb` was abandoned |
| Data discrepancy traced through the pipeline | Yes | Crossed book at reopening crosses; after-hours marks |
| Performance regression | Yes | Throughput halved by Low Power Mode; runs measured during sleep |
| Logic bug caught by a watchpoint or reverse execution | No | None. `rr` does not exist on macOS, and no bug here needed a watchpoint |
| System-level surprise | Yes | Page faults making `mmap` slower than `read()`; Apple's assembler dropping an instruction; the unlinkable toolchain |

The other entries are logic and tooling bugs caught by tests, by CI or by reading output:
the time parser, the config loader, the flat map's empty marker, a wrong test, a broken
commit, unformatted files, a vacuous fingerprint, a misleading micro-benchmark, a GCC-only
warning and four script bugs. Twenty entries describe bugs that happened; two are exercises.

## Entry template

```markdown
## <short title>

- Date / commit: <date>, <sha of the fix>
- Area: parser | book | exchange | risk | recorder | build | performance
- Tools: gdb, ASan, perf, ...

### Symptom
What was observed, and how it was noticed (failing test, sanitizer report,
reconciliation mismatch, crash, slowdown).

### Investigation
The steps taken in order, including dead ends. Commands and trimmed output.

### Root cause
The actual defect, in one or two sentences, with the offending code.

### Fix
What changed and why this fixes the cause rather than the symptom.

### Prevention
The test, assertion, sanitizer or check added so it cannot return silently.
```
