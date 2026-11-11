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
