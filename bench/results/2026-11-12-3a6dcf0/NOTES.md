# Notes on this results directory

Run in the early hours of 12 November 2026, on battery with Low Power Mode on, with the sleep
guard in `scripts/run_bench.sh` active.

Only the micro-benchmarks are here. The machine was sleeping every one to three minutes by
this point (2,851 "Entering Sleep" entries in the power log), and the guard rejected every
attempt at the longer benchmarks: five consecutive attempts at the baseline latency run each
overlapped a sleep. The latency run with prefetch, the end-to-end backtests and the sharding
scaling runs were therefore not measured. They are reported as "not measured" in
BENCHMARKS.md, with the command to run them.

The micro-benchmark run took about three minutes and the guard saw no sleep during it. Its
figures are computed from CPU time, except the two-thread ring transfer, which uses wall time.
