#include "itchbt/record/recorder.hpp"

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

bool Recorder::open(const std::filesystem::path& directory, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "cannot create " + directory.string() + ": " + ec.message();
        return false;
    }
    for (std::size_t i = 0; i < kSpecs.size(); ++i) {
        const std::filesystem::path path = directory / kSpecs[i].name;
        files_[i] = std::fopen(path.c_str(), "wb");
        if (files_[i] == nullptr) {
            error = "cannot write " + path.string();
            static_cast<void>(close());
            return false;
        }
        FileHeader header;
        header.record_type = static_cast<std::uint32_t>(kSpecs[i].type);
        header.record_size = kSpecs[i].size;
        if (std::fwrite(&header, sizeof(header), 1, files_[i]) != 1) {
            error = "cannot write " + path.string();
            static_cast<void>(close());
            return false;
        }
    }
    return true;
}

bool Recorder::close() {
    for (std::FILE*& file : files_) {
        if (file != nullptr) {
            if (std::fclose(file) != 0) {
                failed_ = true;
            }
            file = nullptr;
        }
    }
    return !failed_;
}

} // namespace itchbt::record
