#!/usr/bin/env bash
# Builds and tests the given presets (default: asan and tsan), stopping at the first failure.
# Run this before every commit; a stale binary can make a broken tree look green if the build
# step's result is ignored.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

PRESETS=("$@")
if [[ ${#PRESETS[@]} -eq 0 ]]; then
    PRESETS=(asan tsan)
fi

scripts/format.sh --check
for preset in "${PRESETS[@]}"; do
    echo "== $preset"
    cmake --preset "$preset" >/dev/null
    cmake --build --preset "$preset" 2>&1 | grep -E "error|FAILED|warning: " | grep -v character-conversion || true
    # Re-run the build to get its real exit status: the grep above hides it.
    cmake --build --preset "$preset" >/dev/null
    ctest --preset "$preset" 2>&1 | tail -3
    ctest --preset "$preset" >/dev/null
done
echo "OK"
