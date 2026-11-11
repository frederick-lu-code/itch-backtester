#pragma once

// The few places where Linux and macOS differ. Benchmark and tool support only; the engine's
// hot path uses none of this.

#include <cstdint>
#include <optional>
#include <string>

namespace itchbt {

/// Asks the scheduler to keep the calling thread where benchmarks want it and describes what
/// was done. On Linux, pins to `cpu` if given. macOS has no affinity API on Apple Silicon, so
/// the thread is given the highest QoS class, which keeps it on performance cores.
std::string place_benchmark_thread(std::optional<unsigned> cpu);

/// Peak resident set size of this process in bytes, or 0 if unavailable.
std::uint64_t peak_rss_bytes();

/// Page faults this process has taken so far.
struct PageFaults {
    std::uint64_t minor = 0; // served without I/O
    std::uint64_t major = 0; // needed a read from disk
};
PageFaults page_faults();

} // namespace itchbt
