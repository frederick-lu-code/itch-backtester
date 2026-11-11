#include "itchbt/itch/reader.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace itchbt::itch {

namespace {

std::string describe(const char* what, const std::filesystem::path& path) {
    // std::strerror is not thread safe; the error category's message() is.
    return std::string(what) + " " + path.string() + ": " +
           std::error_code(errno, std::generic_category()).message();
}

/// Closes a file descriptor on scope exit unless released.
class FdGuard {
public:
    explicit FdGuard(int fd) noexcept : fd_(fd) {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    ~FdGuard() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

private:
    int fd_;
};

} // namespace

MappedFile::MappedFile(MappedFile&& other) noexcept
    : address_(std::exchange(other.address_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        reset();
        address_ = std::exchange(other.address_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

MappedFile::~MappedFile() {
    reset();
}

void MappedFile::reset() noexcept {
    if (address_ != nullptr) {
        ::munmap(address_, size_);
        address_ = nullptr;
        size_ = 0;
    }
}

std::optional<MappedFile> MappedFile::open(const std::filesystem::path& path, std::string& error) {
    const FdGuard fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) {
        error = describe("cannot open", path);
        return std::nullopt;
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        error = describe("cannot stat", path);
        return std::nullopt;
    }
    if (!S_ISREG(st.st_mode)) {
        error = "not a regular file: " + path.string();
        return std::nullopt;
    }

    MappedFile file;
    file.size_ = static_cast<std::size_t>(st.st_size);
    if (file.size_ == 0) {
        return file; // mmap rejects a zero length; an empty file is simply an empty span
    }
    void* const address = ::mmap(nullptr, file.size_, PROT_READ, MAP_PRIVATE, fd.get(), 0);
    if (address == MAP_FAILED) { // NOLINT(*-int-to-ptr, *-cstyle-cast): MAP_FAILED is a macro
        error = describe("cannot map", path);
        return std::nullopt;
    }
    file.address_ = address;
    // Advisory only: the replay reads the file front to back exactly once.
    ::madvise(address, file.size_, MADV_SEQUENTIAL);
    return file; // the mapping outlives the descriptor
}

BufferedFile::BufferedFile(int fd, std::size_t buffer_size) : fd_(fd), buffer_(buffer_size) {}

BufferedFile::BufferedFile(BufferedFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), buffer_(std::move(other.buffer_)), begin_(other.begin_),
      end_(other.end_), offset_(other.offset_), eof_(other.eof_), failed_(other.failed_) {}

BufferedFile& BufferedFile::operator=(BufferedFile&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
        buffer_ = std::move(other.buffer_);
        begin_ = other.begin_;
        end_ = other.end_;
        offset_ = other.offset_;
        eof_ = other.eof_;
        failed_ = other.failed_;
    }
    return *this;
}

BufferedFile::~BufferedFile() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

std::optional<BufferedFile> BufferedFile::open(const std::filesystem::path& path,
                                               std::string& error, std::size_t buffer_size) {
    FdGuard fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) {
        error = describe("cannot open", path);
        return std::nullopt;
    }
    return BufferedFile(fd.release(), std::max(buffer_size, kMinBufferSize));
}

std::span<const std::byte> BufferedFile::fill() {
    // Slide the unconsumed tail to the front so the free space is contiguous.
    if (begin_ > 0) {
        std::memmove(buffer_.data(), buffer_.data() + begin_, end_ - begin_);
        end_ -= begin_;
        begin_ = 0;
    }
    while (!eof_ && end_ < buffer_.size()) {
        const ssize_t n = ::read(fd_, buffer_.data() + end_, buffer_.size() - end_);
        if (n > 0) {
            end_ += static_cast<std::size_t>(n);
        } else if (n == 0) {
            eof_ = true;
        } else if (errno != EINTR) {
            failed_ = true;
            eof_ = true;
        }
    }
    return {buffer_.data(), end_};
}

void BufferedFile::consume(std::size_t count) noexcept {
    count = std::min(count, end_ - begin_);
    begin_ += count;
    offset_ += count;
}

} // namespace itchbt::itch
