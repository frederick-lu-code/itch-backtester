#!/usr/bin/env bash
# Formats every tracked C++ file in place, or with --check fails if any file needs formatting.
# CI pins clang-format 18.1.8; use the same version locally to avoid churn.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

MODE=(-i)
if [[ "${1:-}" == "--check" ]]; then
    MODE=(--dry-run --Werror)
fi

git ls-files -z --cached --others --exclude-standard -- '*.cpp' '*.hpp' | xargs -0 "${CLANG_FORMAT:-clang-format}" "${MODE[@]}"
