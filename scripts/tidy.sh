#!/usr/bin/env bash
# Runs clang-tidy over every tracked .cpp file using the debug preset's compile commands.
# CI uses clang-tidy 18 on Linux and is authoritative; this is the local preview.
#
#   scripts/tidy.sh                 # all files
#   scripts/tidy.sh apps/foo.cpp    # some files
#   CLANG_TIDY="uvx --from clang-tidy==21.1.6 clang-tidy" scripts/tidy.sh
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [[ ! -f build/debug/compile_commands.json ]]; then
    cmake --preset debug >/dev/null
fi

read -r -a TIDY <<<"${CLANG_TIDY:-clang-tidy}"
EXTRA=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    # A clang-tidy that is not Apple's needs to be told where the SDK is.
    EXTRA=(--extra-arg=-isysroot --extra-arg="${SDKROOT:-$(xcrun --show-sdk-path)}")
fi

if [[ $# -gt 0 ]]; then
    FILES=("$@")
else
    FILES=()
    while IFS= read -r f; do FILES+=("$f"); done < <(git ls-files -- '*.cpp')
fi

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
printf '%s\0' "${FILES[@]}" |
    xargs -0 -n 1 -P "$JOBS" "${TIDY[@]}" --quiet -p build/debug ${EXTRA[@]+"${EXTRA[@]}"}
