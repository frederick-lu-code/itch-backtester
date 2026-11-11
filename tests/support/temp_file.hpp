#pragma once

// A uniquely named temporary file that is removed on destruction.

#include "support/itch_writer.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace itchbt::test {

class TempFile {
public:
    explicit TempFile(std::span<const std::byte> contents) : path_(unique_path()) {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        for (const std::byte b : contents) {
            out.put(std::to_integer<char>(b));
        }
        if (!out) {
            throw std::runtime_error("TempFile: cannot write " + path_.string());
        }
    }
    explicit TempFile(const ItchWriter& writer) : TempFile(writer.bytes()) {}
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// A fresh path in the temp directory; nothing is created.
    [[nodiscard]] static std::filesystem::path unique_path() {
        static std::atomic<unsigned> counter{0};
        return std::filesystem::temp_directory_path() /
               ("itchbt_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    }

private:
    std::filesystem::path path_;
};

} // namespace itchbt::test
