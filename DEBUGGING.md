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
