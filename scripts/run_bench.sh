#!/usr/bin/env bash
# Runs the benchmark suite and writes raw results to bench/results/<date>-<commit>/.
#
#   scripts/run_bench.sh [--input data/<file>] [--runs 10] [--runs-slow 5] [--cpu N]
#                        [--only parse|book|latency|e2e|shards|micro]
#
# Method (see BENCHMARKS.md): release preset, one discarded warm-up run then N measured runs
# per benchmark. The two baseline-book variants take minutes per run and use --runs-slow.
# Run on an otherwise idle machine, on mains power. Never run in CI.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

INPUT="data/12302019.NASDAQ_ITCH50"
RUNS=10
RUNS_SLOW=5
CPU_ARGS=()
ONLY=""
while [[ $# -gt 0 ]]; do
    case "$1" in
    --input) INPUT="$2"; shift 2 ;;
    --runs) RUNS="$2"; shift 2 ;;
    --runs-slow) RUNS_SLOW="$2"; shift 2 ;;
    --cpu) CPU_ARGS=(--cpu "$2"); shift 2 ;;
    --only) ONLY="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ ! -f "$INPUT" ]]; then
    echo "error: $INPUT not found; run scripts/fetch_data.sh first" >&2
    exit 1
fi

# On a laptop, keep the machine and display awake for the duration: a sleep in the middle of
# a run shows up as a multi-second "latency".
if [[ "$(uname -s)" == "Darwin" && -z "${ITCHBT_CAFFEINATED:-}" ]]; then
    ARGS=(--input "$INPUT" --runs "$RUNS" --runs-slow "$RUNS_SLOW")
    [[ -n "$ONLY" ]] && ARGS+=(--only "$ONLY")
    ITCHBT_CAFFEINATED=1 exec caffeinate -d -i -m "$0" "${ARGS[@]}" ${CPU_ARGS[@]+"${CPU_ARGS[@]}"}
fi

cmake --preset release >/dev/null
cmake --build --preset release >/dev/null
cmake --preset timing >/dev/null
cmake --build --preset timing >/dev/null

COMMIT="$(git rev-parse --short HEAD)"
OUT="bench/results/$(date +%Y-%m-%d)-$COMMIT"
# Capture before creating any output, so the results themselves do not make the tree look dirty.
SYSINFO="$(scripts/sysinfo.sh)"
mkdir -p "$OUT"
printf '%s\n' "$SYSINFO" >"$OUT/sysinfo.json"
echo "Results in $OUT"

# A laptop on battery with its lid shut naps regardless of caffeinate, and a nap in the middle
# of a run turns a 40-second measurement into a 400-second one. On macOS each benchmark is
# therefore run under `awake`, which checks the power log afterwards and repeats the benchmark
# (up to five times) if the machine slept while it ran.
sleep_events() {
    pmset -g log 2>/dev/null | grep -c "Entering Sleep" || true
}
awake() {
    if [[ "$(uname -s)" != "Darwin" ]]; then
        "$@"
        return
    fi
    local attempt before after
    for attempt in 1 2 3 4 5; do
        before="$(sleep_events)"
        "$@"
        after="$(sleep_events)"
        if [[ "$before" == "$after" ]]; then
            return 0
        fi
        echo "   machine slept during this run (attempt $attempt); repeating" >&2
    done
    echo "error: could not get a run without the machine sleeping: $*" >&2
    return 1
}

BIN=build/release/bench
CPU=(${CPU_ARGS[@]+"${CPU_ARGS[@]}"})
wanted() { [[ -z "$ONLY" || "$ONLY" == "$1" ]]; }

if wanted parse; then
    for reader in mmap read; do
        for handler in decode frame; do
            echo "== parse_only $reader/$handler"
            awake "$BIN/parse_bench" --input "$INPUT" --reader "$reader" --handler "$handler" \
                --runs "$RUNS" ${CPU[@]+"${CPU[@]}"} --out "$OUT/parse_${reader}_${handler}.json"
        done
    done
fi

# The order store is sized for the reference day's peak of 1.92 million live orders.
RESERVE=2097152

book() { # name, binary, book, runs, extra args...
    local name="$1" binary="$2" book="$3" runs="$4"
    shift 4
    echo "== book_build $name"
    awake "$binary" --input "$INPUT" --book "$book" --runs "$runs" "$@" ${CPU[@]+"${CPU[@]}"} \
        --out "$OUT/book_$name.json"
}

if wanted book; then
    book baseline "$BIN/book_bench" baseline "$RUNS_SLOW"
    book baseline_reserve "$BIN/book_bench" baseline "$RUNS_SLOW" --reserve "$RESERVE"
    book flat_orders "$BIN/book_bench" flat_orders "$RUNS" --reserve "$RESERVE"
    book binary_levels "$BIN/book_bench" binary_levels "$RUNS" --reserve "$RESERVE"
    book optimised "$BIN/book_bench" optimised "$RUNS" --reserve "$RESERVE"
    book optimised_prefetch "$BIN/book_bench_prefetch" optimised "$RUNS" --reserve "$RESERVE"
fi

if wanted latency; then
    TBIN=build/timing/bench
    echo "== book_latency baseline"
    awake "$TBIN/book_bench" --input "$INPUT" --book baseline --runs 2 ${CPU[@]+"${CPU[@]}"} \
        --out "$OUT/latency_baseline.json"
    echo "== book_latency optimised"
    awake "$TBIN/book_bench" --input "$INPUT" --book optimised --reserve "$RESERVE" --runs 3 \
        ${CPU[@]+"${CPU[@]}"} --out "$OUT/latency_optimised.json"
    echo "== book_latency optimised/prefetch"
    awake "$TBIN/book_bench_prefetch" --input "$INPUT" --book optimised --reserve "$RESERVE" --runs 3 \
        ${CPU[@]+"${CPU[@]}"} --out "$OUT/latency_optimised_prefetch.json"
fi

if wanted e2e; then
    SCRATCH="$(mktemp -d)"
    for recorder in async sync none; do
        echo "== end_to_end passive_maker/$recorder"
        for i in $(seq 0 "$RUNS_SLOW"); do # run 0 is the warm-up
            awake build/release/apps/backtest --input "$INPUT" \
                --config configs/passive_maker.toml --out "$SCRATCH/$recorder-$i" \
                --recorder "$recorder" --skip-input-hash | head -1
        done
        python3 - "$SCRATCH" "$recorder" "$RUNS_SLOW" "$INPUT" >"$OUT/e2e_$recorder.json" <<'PY'
import json, os, sys
scratch, recorder, runs, path = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
metas = [json.load(open(f"{scratch}/{recorder}-{i}/run.json")) for i in range(1, runs + 1)]
first = metas[0]
print(json.dumps({
    "benchmark": "end_to_end",
    "variant": f"passive_maker/{recorder}",
    "messages": first["stats"]["messages"],
    "bytes": os.path.getsize(path),
    "orders_submitted": first["stats"]["orders_submitted"],
    "fills": first["stats"]["fills"],
    "records": sum(first["records"].values()),
    "recorder_blocked_records": [m["timing"]["recorder_blocked_records"] for m in metas],
    "recorder_blocked_ms": [m["timing"]["recorder_blocked_ns"] / 1e6 for m in metas],
    "run_seconds": [m["timing"]["wall_time_s"] for m in metas],
}, indent=2))
PY
    done
    rm -rf "$SCRATCH"
fi

if wanted shards; then
    SCRATCH="$(mktemp -d)"
    for shards in 1 2 4 8; do
        echo "== sharding $shards"
        SHARD_ARGS=()
        [[ "$shards" -gt 1 ]] && SHARD_ARGS=(--shards "$shards")
        for i in $(seq 0 "$RUNS_SLOW"); do # run 0 is the warm-up
            awake build/release/apps/backtest --input "$INPUT" --config configs/noop.toml \
                --out "$SCRATCH/$shards-$i" --recorder none --skip-input-hash \
                ${SHARD_ARGS[@]+"${SHARD_ARGS[@]}"} | head -1
        done
        python3 - "$SCRATCH" "$shards" "$RUNS_SLOW" "$INPUT" >"$OUT/shards_$shards.json" <<'PY'
import json, os, sys
scratch, shards, runs, path = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
metas = [json.load(open(f"{scratch}/{shards}-{i}/run.json")) for i in range(1, runs + 1)]
print(json.dumps({
    "benchmark": "sharding",
    "variant": f"{shards} shard" + ("" if shards == "1" else "s"),
    "shards": int(shards),
    "messages": metas[0]["stats"]["messages"],
    "bytes": os.path.getsize(path),
    "router_waits": [m["timing"]["router_waits"] for m in metas],
    "run_seconds": [m["timing"]["wall_time_s"] for m in metas],
}, indent=2))
PY
    done
    rm -rf "$SCRATCH"
fi

if wanted micro; then
    echo "== micro"
    ITCHBT_DATA="$INPUT" awake "$BIN/micro_bench" --benchmark_min_time=1s \
        --benchmark_out="$OUT/micro.json" --benchmark_out_format=json
fi

echo "Done. Regenerate the tables with: python3 bench/render.py"
