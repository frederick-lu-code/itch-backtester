# Debugging log

Real bugs found while building this project, and how each was tracked down. Entries are written
at the time of the fix, with real commands and trimmed real output, and each links the fixing
commit and the regression test that now covers it. Nothing here is invented; a deliberate
fault-injection exercise is labelled as an exercise.

No entries yet.

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
