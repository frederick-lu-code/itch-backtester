#include "itchbt/record/recorder.hpp"

#include <chrono>
#include <system_error>

namespace itchbt::record {

namespace {

struct FileSpec {
    const char* name;
    RecordType type;
    std::uint32_t size;
};

constexpr std::array<FileSpec, 4> kSpecs = {{
    {"orders.bin", RecordType::Order, sizeof(OrderRecord)},
    {"fills.bin", RecordType::Fill, sizeof(FillRecord)},
    {"bbo.bin", RecordType::Bbo, sizeof(BboRecord)},
    {"risk.bin", RecordType::Risk, sizeof(RiskRecord)},
}};

} // namespace

Recorder::~Recorder() {
    static_cast<void>(close());
}

bool Recorder::open(const std::filesystem::path& directory, std::string& error, RecorderMode mode,
                    std::size_t ring_capacity) {
    static_cast<void>(close());
    failed_.store(false, std::memory_order_relaxed);
    counts_ = {};
    stats_ = {};
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "cannot create " + directory.string() + ": " + ec.message();
        return false;
    }
    for (std::size_t i = 0; i < kSpecs.size(); ++i) {
        const std::filesystem::path path = directory / kSpecs[i].name;
        files_[i] = std::fopen(path.c_str(), "wb");
        FileHeader header;
        header.record_type = static_cast<std::uint32_t>(kSpecs[i].type);
        header.record_size = kSpecs[i].size;
        if (files_[i] == nullptr || std::fwrite(&header, sizeof(header), 1, files_[i]) != 1) {
            error = "cannot write " + path.string();
            enabled_ = true; // so close() releases the files opened so far
            static_cast<void>(close());
            return false;
        }
    }
    mode_ = mode;
    enabled_ = true;
    if (mode_ == RecorderMode::Async) {
        ring_ = std::make_unique<SpscRing<Slot>>(ring_capacity);
        stop_.store(false, std::memory_order_relaxed);
        writer_ = std::thread([this] { writer_loop(); });
    }
    return true;
}

bool Recorder::close() {
    if (!enabled_) {
        return !failed_.load(std::memory_order_relaxed);
    }
    if (writer_.joinable()) {
        // The writer drains everything already pushed before it honours the stop flag.
        stop_.store(true, std::memory_order_release);
        writer_.join();
    }
    ring_.reset();
    for (std::FILE*& file : files_) {
        if (file != nullptr) {
            if (std::fclose(file) != 0) {
                failed_.store(true, std::memory_order_relaxed);
            }
            file = nullptr;
        }
    }
    enabled_ = false;
    return !failed_.load(std::memory_order_relaxed);
}

void Recorder::wait_and_push(const Slot& slot) noexcept {
    // Wall-clock time is read only here, when the engine is already stalled, and only to
    // report the stall. It never feeds back into the simulation.
    const auto started = std::chrono::steady_clock::now();
    ++stats_.blocked_records;
    while (!ring_->try_push(slot)) {
        std::this_thread::yield();
    }
    stats_.blocked_ns +=
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count());
}

void Recorder::writer_loop() noexcept {
    Slot slot;
    for (;;) {
        if (ring_->try_pop(slot)) {
            if (std::fwrite(slot.bytes.data(), slot.size, 1, files_[slot.file]) != 1) {
                failed_.store(true, std::memory_order_relaxed);
            }
            continue;
        }
        // Empty. Stop only if the flag was set before this emptiness was observed: the flag
        // is set after the last push, so one more look at the ring catches any stragglers.
        if (stop_.load(std::memory_order_acquire)) {
            if (!ring_->try_pop(slot)) {
                return;
            }
            if (std::fwrite(slot.bytes.data(), slot.size, 1, files_[slot.file]) != 1) {
                failed_.store(true, std::memory_order_relaxed);
            }
            continue;
        }
        // Nothing to do: sleep briefly instead of spinning a core. The engine never waits on
        // this thread unless the ring is full, in which case there is work and no sleep.
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

} // namespace itchbt::record
