#pragma once

// File input for ITCH data: a memory-mapped reader, and a plain read() reader kept as the
// benchmark baseline. Both feed the same parse() function.

#include "itchbt/itch/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace itchbt::itch {

/// A whole file mapped read-only into memory.
class MappedFile {
public:
    MappedFile() = default;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile();

    /// Maps `path`. On failure returns nullopt and describes the problem in `error`.
    [[nodiscard]] static std::optional<MappedFile> open(const std::filesystem::path& path,
                                                        std::string& error);

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(address_), size_};
    }

private:
    void reset() noexcept;

    void* address_ = nullptr;
    std::size_t size_ = 0;
};

/// A file read through a fixed buffer with read(). Hands out the buffered bytes; the caller
/// says how many it consumed and the rest is carried over to the next fill.
class BufferedFile {
public:
    /// One megabyte: large enough that read() calls are rare, small enough to stay in L2.
    static constexpr std::size_t kDefaultBufferSize = std::size_t{1} << 20;
    /// The buffer must hold at least one maximum-size record (2 + 65535 bytes).
    static constexpr std::size_t kMinBufferSize = std::size_t{1} << 17;

    BufferedFile(BufferedFile&& other) noexcept;
    BufferedFile& operator=(BufferedFile&& other) noexcept;
    BufferedFile(const BufferedFile&) = delete;
    BufferedFile& operator=(const BufferedFile&) = delete;
    ~BufferedFile();

    [[nodiscard]] static std::optional<BufferedFile>
    open(const std::filesystem::path& path, std::string& error,
         std::size_t buffer_size = kDefaultBufferSize);

    /// Tops the buffer up from the file and returns everything buffered and not yet consumed.
    /// Empty once the file is exhausted and all bytes were consumed.
    [[nodiscard]] std::span<const std::byte> fill();
    /// Marks the first `count` bytes of the last fill() as processed.
    void consume(std::size_t count) noexcept;

    [[nodiscard]] bool at_end() const noexcept { return eof_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    /// File offset of the first unconsumed byte.
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }

private:
    BufferedFile(int fd, std::size_t buffer_size);

    int fd_ = -1;
    std::vector<std::byte> buffer_;
    std::size_t begin_ = 0; // first unconsumed byte in buffer_
    std::size_t end_ = 0;   // one past the last valid byte in buffer_
    std::uint64_t offset_ = 0;
    bool eof_ = false;
    bool failed_ = false;
};

/// Parses a BufferedFile to the end. Offsets in the result are absolute file offsets.
template <typename Handler>
[[nodiscard]] ParseResult
parse_stream(BufferedFile& file, Handler& handler,
             std::uint64_t max_messages = std::numeric_limits<std::uint64_t>::max()) {
    ParseResult total;
    while (total.messages < max_messages) {
        const std::span<const std::byte> data = file.fill();
        if (data.empty()) {
            break;
        }
        const std::uint64_t base = file.offset();
        ParseResult r = parse(data, handler, max_messages - total.messages);
        total.messages += r.messages;
        // A record cut off by the end of the buffer is not an error unless the file ends too:
        // keep the partial record and read more.
        const bool need_more = r.error == ParseError::Truncated && !file.at_end();
        if (!r.ok() && !need_more) {
            total.error = r.error;
            total.type = r.type;
            total.length = r.length;
            total.offset = base + r.offset;
            return total;
        }
        file.consume(static_cast<std::size_t>(r.offset));
    }
    total.offset = file.offset();
    return total;
}

} // namespace itchbt::itch
