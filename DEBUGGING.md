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

- Date / commit: 2026-11-11, fix in the commit that adds 
- Area: book (validation)
- Tools:  with record offsets, a throwaway Python script to print raw messages
  around an offset

### Symptom
The first full-day run of  failed the specification's invariant that the book is
never crossed during regular trading hours:



Every other invariant held (no unknown references, no failed recount), so the book's
arithmetic was not obviously broken.

### Investigation
The specification says not to assume the data is wrong, so the first hypothesis was a book bug
in handling  (executed with price): 631 of the 642 violations were on  messages.

1. Grouped the violations by symbol. Only ten symbols were involved and one, MKD, accounted for
   486. A bug in  handling would not be so concentrated: there are 99,917  messages across
   thousands of symbols.

   

2. Violations came in bursts with timestamps microseconds apart, which suggested one event per
   burst rather than a steady leak.

3. Printed the raw messages for SLGL's locate around the first reported offset:

   

   A cross of type  (IPO or halted security) prints, the trading action flips the symbol to
   , and then non-printable executions at the cross price keep arriving for another 200
   microseconds. Those executions are the cross itself being unwound order by order.

4. Checked a violation that was not a  (MKD, type , 10:54:21.569624647). It sits in the
   middle of the same kind of burst: new orders arriving while the cross's executions are still
   being published.

5. Dead end worth recording: the opening cross at 09:30 produced no violations at all, so
   "crosses in general" was not the explanation. The market-open system event  evidently
   arrives after the opening cross has finished printing; only intraday releases have the
   overlap.

### Root cause
Not a book defect and not bad data: the invariant was stated too broadly. After a halt or IPO
release, the feed sends the trading action before it has finished sending the executions of
the reopening cross, so the displayed book is legitimately crossed for a moment while the
symbol is already marked as trading.

### Fix
 now tracks, per symbol, whether the book has been continuously crossed since the
last  trading action. Such a state is counted as a "reopening transient", not a violation,
provided the release was less than 100 ms earlier. Anything else is still a violation.



Running with  reproduces the original 642, which confirms the rule
explains every one of them and nothing else is hiding behind it.

### Prevention
Three end-to-end tests in : a crossed book well after a release
still fails; a cross still printing just after a release passes; a cross that persists a second
after the release fails. The first version of the second test was itself wrong (it placed the
orders after the release, so the book was uncrossed in between and the tolerance correctly did
not apply), which was a useful check that the rule is as narrow as intended.

## Reconciliation script read its arguments off by one

- Date / commit: 2026-11-11, fixed before the script was first committed
- Area: build (analysis tooling)
- Tools: Python traceback

### Symptom
analysis/reconcile_close.py,: cannot open `analysis/reconcile_close.py,' (No such file or directory)
line:                         cannot open `line' (No such file or directory)
36,:                          cannot open `36,' (No such file or directory)
in:                           cannot open `in' (No such file or directory)
main:                         cannot open `main' (No such file or directory)

### Investigation
The date argument was plainly on the command line, so  held the wrong element.

### Root cause
 strips the program name, but  indexed its list as if the program
name were still at position 0.

### Fix
Index from 0 inside  and adjust the argument-count check to match.

### Prevention
None beyond the fix: the script needs the network and the data file, so it is not in CI. It
fails loudly on the first line if this regresses.

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
