#!/usr/bin/env bash
# Runs the benchmark suite and writes raw results to bench/results/<date>-<commit>/.
#
#   scripts/run_bench.sh [--input data/<file>] [--runs 10] [--cpu N] [--only parse|book|...]
#
# Method (see BENCHMARKS.md): release preset, one discarded warm-up run then N measured runs
# per benchmark. Run on an otherwise idle machine, on mains power. Never run in CI.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

INPUT="data/12302019.NASDAQ_ITCH50"
RUNS=10
CPU_ARGS=()
ONLY=""
while [[ $# -gt 0 ]]; do
    case "$1" in
    --input) INPUT="$2"; shift 2 ;;
    --runs) RUNS="$2"; shift 2 ;;
    --cpu) CPU_ARGS=(--cpu "$2"); shift 2 ;;
    --only) ONLY="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ ! -f "$INPUT" ]]; then
    echo "error: $INPUT not found; run scripts/fetch_data.sh first" >&2
    exit 1
fi

cmake --preset release >/dev/null
cmake --build --preset release >/dev/null

COMMIT="$(git rev-parse --short HEAD)"
OUT="bench/results/$(date +%Y-%m-%d)-$COMMIT"
# Capture before creating any output, so the results themselves do not make the tree look dirty.
SYSINFO="$(scripts/sysinfo.sh)"
mkdir -p "$OUT"
printf '%s\n' "$SYSINFO" >"$OUT/sysinfo.json"
echo "Results in $OUT"

BIN=build/release/bench
wanted() { [[ -z "$ONLY" || "$ONLY" == "$1" ]]; }

if wanted parse; then
    for reader in mmap read; do
        for handler in decode frame; do
            echo "== parse_only $reader/$handler"
            "$BIN/parse_bench" --input "$INPUT" --reader "$reader" --handler "$handler" \
                --runs "$RUNS" ${CPU_ARGS[@]+"${CPU_ARGS[@]}"} \
                --out "$OUT/parse_${reader}_${handler}.json"
        done
    done
fi

echo "Done. Regenerate the tables with: python3 bench/render.py"
