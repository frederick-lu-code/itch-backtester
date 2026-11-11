#pragma once

// Shared plumbing for the macro benchmarks: wall-clock timing of repeated runs and a JSON
// result file that bench/render.py turns into the tables in BENCHMARKS.md.
//
// The harness reads the wall clock; the engine under test never does.

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace itchbt::bench {

class Stopwatch {
public:
    Stopwatch() : start_(std::chrono::steady_clock::now()) {}
    [[nodiscard]] double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

/// One benchmark's result: a name, a variant, free-form string and integer facts, and the
/// wall time of each measured run.
struct Result {
    std::string benchmark;
    std::string variant;
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    std::vector<double> run_seconds;
    std::vector<std::pair<std::string, std::string>> notes;
    std::vector<std::pair<std::string, std::uint64_t>> counters;
    /// Pre-formatted `"key": value` members to include as they are, for nested data.
    std::vector<std::string> raw_members;

    /// Writes the result as JSON. Returns false if the file cannot be written.
    [[nodiscard]] bool write(const std::string& path) const {
        std::FILE* out = path == "-" ? stdout : std::fopen(path.c_str(), "w");
        if (out == nullptr) {
            return false;
        }
        std::fprintf(out, "{\n  \"benchmark\": \"%s\",\n  \"variant\": \"%s\",\n",
                     benchmark.c_str(), variant.c_str());
        std::fprintf(out, "  \"messages\": %" PRIu64 ",\n  \"bytes\": %" PRIu64 ",\n", messages,
                     bytes);
        for (const auto& [key, value] : notes) {
            std::fprintf(out, "  \"%s\": \"%s\",\n", key.c_str(), value.c_str());
        }
        for (const auto& [key, value] : counters) {
            std::fprintf(out, "  \"%s\": %" PRIu64 ",\n", key.c_str(), value);
        }
        for (const std::string& member : raw_members) {
            std::fprintf(out, "  %s,\n", member.c_str());
        }
        std::fputs("  \"run_seconds\": [", out);
        for (std::size_t i = 0; i < run_seconds.size(); ++i) {
            std::fprintf(out, "%s%.6f", i == 0 ? "" : ", ", run_seconds[i]);
        }
        std::fputs("]\n}\n", out);
        if (out != stdout) {
            return std::fclose(out) == 0;
        }
        return true;
    }
};

} // namespace itchbt::bench
