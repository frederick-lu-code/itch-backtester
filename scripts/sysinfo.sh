#!/usr/bin/env bash
# Prints a JSON description of the machine and toolchain, for storing next to benchmark results.
#
# The Linux branch follows the project specification (lscpu, governor, THP). It has only ever
# been exercised in CI, never on the bare-metal Linux machine the specification asks for. The
# macOS branch is the one behind every published number so far.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

json_escape() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | tr '\n' ' '
}
field() {
    printf '  "%s": "%s"' "$1" "$(json_escape "$2")"
}
read_or() {
    if [[ -r "$1" ]]; then cat "$1"; else printf '%s' "$2"; fi
}

CXX_BIN="${CXX:-c++}"
OS="$(uname -s)"

{
    echo "{"
    field os "$OS"; echo ","
    field kernel "$(uname -r)"; echo ","
    field arch "$(uname -m)"; echo ","
    field git_commit "$(git rev-parse HEAD 2>/dev/null || echo unknown)"; echo ","
    field git_dirty "$(if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then echo yes; else echo no; fi)"; echo ","
    field compiler "$("$CXX_BIN" --version 2>/dev/null | head -1)"; echo ","
    field cmake "$(cmake --version 2>/dev/null | head -1)"; echo ","
    if [[ "$OS" == "Linux" ]]; then
        field cpu_model "$(lscpu | sed -n 's/^Model name: *//p' | head -1)"; echo ","
        field logical_cpus "$(nproc)"; echo ","
        field memory_kb "$(sed -n 's/^MemTotal: *\([0-9]*\).*/\1/p' /proc/meminfo)"; echo ","
        field governor "$(read_or /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor unavailable)"; echo ","
        field turbo_disabled "$(read_or /sys/devices/system/cpu/intel_pstate/no_turbo unavailable)"; echo ","
        field transparent_hugepage "$(read_or /sys/kernel/mm/transparent_hugepage/enabled unavailable)"; echo ","
        field tsc_flags "$(grep -m1 -o -E 'constant_tsc|nonstop_tsc' /proc/cpuinfo | sort -u | tr '\n' ' ' || true)"; echo ","
        field lscpu "$(lscpu)"; echo ","
    elif [[ "$OS" == "Darwin" ]]; then
        field os_version "$(sw_vers -productVersion)"; echo ","
        field cpu_model "$(sysctl -n machdep.cpu.brand_string)"; echo ","
        field logical_cpus "$(sysctl -n hw.ncpu)"; echo ","
        field performance_cores "$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo unknown)"; echo ","
        field efficiency_cores "$(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo unknown)"; echo ","
        field memory_kb "$(($(sysctl -n hw.memsize) / 1024))"; echo ","
        field timebase_hz "$(sysctl -n hw.tbfrequency)"; echo ","
        field page_size "$(sysctl -n hw.pagesize)"; echo ","
        field power "$(pmset -g batt 2>/dev/null | head -1)"; echo ","
        field governor "not applicable (macOS)"; echo ","
        field transparent_hugepage "not applicable (macOS)"; echo ","
    fi
    field date_utc "$(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo
    echo "}"
}
