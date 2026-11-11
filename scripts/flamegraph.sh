#!/usr/bin/env bash
# Records a CPU profile of the book build with perf and renders an SVG flame graph into the
# newest results directory.
#
#   scripts/flamegraph.sh [--input data/<file>] [--book optimised] [--max-messages N]
#
# Linux only: it needs `perf` and Brendan Gregg's FlameGraph scripts (stackcollapse-perf.pl and
# flamegraph.pl) on PATH or in $FLAMEGRAPH_DIR.
#
# This script has never been run. The project was developed on macOS, where perf does not
# exist, and no Linux machine was available. It is the script the specification asks for,
# written from the tools' documentation, and should be treated as untested.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "flamegraph.sh needs Linux perf; on macOS use Instruments or 'sample' instead" >&2
    exit 1
fi

INPUT="data/12302019.NASDAQ_ITCH50"
BOOK="optimised"
MAX_MESSAGES=""
while [[ $# -gt 0 ]]; do
    case "$1" in
    --input) INPUT="$2"; shift 2 ;;
    --book) BOOK="$2"; shift 2 ;;
    --max-messages) MAX_MESSAGES="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-}"
find_tool() {
    if [[ -n "$FLAMEGRAPH_DIR" && -x "$FLAMEGRAPH_DIR/$1" ]]; then
        echo "$FLAMEGRAPH_DIR/$1"
    else
        command -v "$1"
    fi
}
COLLAPSE="$(find_tool stackcollapse-perf.pl)"
RENDER="$(find_tool flamegraph.pl)"

# Frame pointers and symbols, same optimisation level as the release build.
cmake --preset relwithdebinfo >/dev/null
cmake --build --preset relwithdebinfo >/dev/null

OUT="$(ls -d bench/results/*/ 2>/dev/null | tail -1)"
OUT="${OUT:-bench/results/$(date +%Y-%m-%d)-$(git rev-parse --short HEAD)/}"
mkdir -p "$OUT"

ARGS=(--input "$INPUT" --book "$BOOK" --reserve 2097152 --runs 1 --warmup 0 --out /dev/null)
[[ -n "$MAX_MESSAGES" ]] && ARGS+=(--max-messages "$MAX_MESSAGES")

perf record -g --call-graph fp -o "$OUT/perf.data" \
    build/relwithdebinfo/bench/book_bench "${ARGS[@]}"
perf script -i "$OUT/perf.data" | "$COLLAPSE" | "$RENDER" --title "book build ($BOOK)" \
    >"$OUT/flamegraph_$BOOK.svg"
rm -f "$OUT/perf.data"
echo "wrote $OUT/flamegraph_$BOOK.svg"
