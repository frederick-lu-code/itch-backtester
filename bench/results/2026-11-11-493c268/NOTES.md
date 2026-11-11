# Notes on this results directory

Run on 11 November 2026 on battery power with Low Power Mode on.

Four sets of results from this run were deleted and are not published, because the machine
slept while they were being measured: `latency_optimised_prefetch.json`, the three
`e2e_*.json` files and `micro.json`. The macOS power log (`pmset -g log`) shows no sleep
between 21:22 and 22:45 local time, when everything kept here was measured, and repeated
"Maintenance Sleep" entries from 22:45 onwards. The deleted files showed it plainly: one
message with a latency of 710 seconds, and end-to-end runs of the same workload taking
anywhere from 42 to 1,023 seconds.

`parse_mmap_decode.json` finished at 21:22, alongside the last sleep of the earlier cluster.
One of its ten runs took 31 seconds against about 9 for the others; the median is unaffected
and the outlier is visible in the table's range.

The `book_fingerprint_low32` values in the `book_*.json` files here are zero. That was a bug
in the benchmark, not in the books: the fingerprint was taken only at the end of the day, when
every book is empty. It now folds in the state of every book at checkpoints through the day.

The deleted measurements were repeated with `scripts/run_bench.sh`, which now re-runs any
benchmark during which the machine slept; see the later directory named in BENCHMARKS.md.
