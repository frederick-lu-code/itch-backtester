#pragma once

// A cheap, high-resolution tick counter for per-message latency measurement, and its
// calibration to nanoseconds. Benchmark support only: the engine never reads a clock.
//
//   x86-64:  the time stamp counter. rdtsc at entry and rdtscp at exit; rdtscp waits for
//            earlier instructions to finish, so the measured interval covers the work.
//            Requires an invariant TSC (constant_tsc and nonstop_tsc in /proc/cpuinfo).
//   AArch64: the virtual counter, cntvct_el0, read after an instruction barrier. Its rate is
//            fixed by the platform, and on Apple Silicon it is only 24 MHz: one tick is
//            41.7 ns, so anything shorter reads as zero or one tick. Percentiles from it are
//            quantised to that step.
//
// Either way the tick rate is measured against CLOCK_MONOTONIC_RAW at start-up, not assumed.

#include <cstdint>
#include <ctime>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace itchbt {

[[nodiscard]] inline std::uint64_t ticks_begin() noexcept {
#if defined(__x86_64__)
    return __rdtsc();
#elif defined(__aarch64__)
    std::uint64_t value = 0;
    // Two statements separated by a newline, not a semicolon: Apple's arm64 assembler treats
    // ';' as the start of a comment, which silently drops everything after it.
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value)); // NOLINT(hicpp-no-assembler)
    return value;
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

[[nodiscard]] inline std::uint64_t ticks_end() noexcept {
#if defined(__x86_64__)
    unsigned aux = 0;
    return __rdtscp(&aux);
#else
    return ticks_begin();
#endif
}

namespace detail {

[[nodiscard]] inline std::uint64_t raw_clock_ns() noexcept {
    timespec ts{};
#if defined(CLOCK_MONOTONIC_RAW)
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

} // namespace detail

/// Measures the counter's rate against the OS's raw monotonic clock over about 50 ms and
/// returns nanoseconds per tick.
[[nodiscard]] inline double calibrate_ns_per_tick() noexcept {
    const std::uint64_t ns0 = detail::raw_clock_ns();
    const std::uint64_t t0 = ticks_begin();
    std::uint64_t ns1 = ns0;
    while (ns1 - ns0 < 50'000'000ULL) {
        ns1 = detail::raw_clock_ns();
    }
    const std::uint64_t t1 = ticks_end();
    return t1 == t0 ? 1.0 : static_cast<double>(ns1 - ns0) / static_cast<double>(t1 - t0);
}

} // namespace itchbt
