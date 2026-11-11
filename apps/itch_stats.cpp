// itch-stats: message counts by type, first and last timestamp, and symbol count for an
// ITCH 5.0 file. Also the simplest end-to-end check that a file parses cleanly.

#include "itchbt/itch/parser.hpp"
#include "itchbt/itch/reader.hpp"
#include "itchbt/util/cli.hpp"
#include "itchbt/util/time.hpp"

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using namespace itchbt;

struct StatsHandler {
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t symbols = 0;
    Timestamp first_ts = 0;
    Timestamp last_ts = 0;
    bool any = false;

    void note(const itch::Header& h) noexcept {
        ++counts[static_cast<unsigned char>(h.type)];
        if (!any) {
            first_ts = h.ts;
            any = true;
        }
        last_ts = h.ts;
    }

    void on(const itch::StockDirectory& m) noexcept {
        note(m.h);
        ++symbols;
    }
    template <typename Message>
    void on(const Message& m) noexcept {
        note(m.h);
    }
    void on_skipped(const itch::Header& h) noexcept { note(h); }
};

const char* name_of(char type) {
    switch (type) {
    case 'S': return "System event";
    case 'R': return "Stock directory";
    case 'H': return "Stock trading action";
    case 'Y': return "Reg SHO restriction";
    case 'L': return "Market participant position";
    case 'V': return "MWCB decline level";
    case 'W': return "MWCB status";
    case 'K': return "IPO quoting period update";
    case 'J': return "LULD auction collar";
    case 'h': return "Operational halt";
    case 'A': return "Add order";
    case 'F': return "Add order with MPID";
    case 'E': return "Order executed";
    case 'C': return "Order executed with price";
    case 'X': return "Order cancel";
    case 'D': return "Order delete";
    case 'U': return "Order replace";
    case 'P': return "Trade (non-cross)";
    case 'Q': return "Cross trade";
    case 'B': return "Broken trade";
    case 'I': return "NOII";
    case 'N': return "RPII";
    case 'O': return "Direct listing with capital raise";
    default: return "?";
    }
}

int usage() {
    std::fputs("usage: itch-stats --input <file> [--max-messages N] [--reader mmap|read]\n",
               stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    Args args(argc, argv);
    const auto input = args.value("--input");
    bool bad_number = false;
    const std::uint64_t max_messages =
        args.number("--max-messages", bad_number).value_or(UINT64_MAX);
    const std::string reader(args.value("--reader").value_or("mmap"));
    if (!input || bad_number || !args.unused().empty() || (reader != "mmap" && reader != "read")) {
        return usage();
    }

    StatsHandler stats;
    itch::ParseResult result;
    std::uint64_t file_size = 0;
    std::string error;

    if (reader == "mmap") {
        const auto file = itch::MappedFile::open(std::string(*input), error);
        if (!file) {
            std::fprintf(stderr, "itch-stats: %s\n", error.c_str());
            return 1;
        }
        file_size = file->bytes().size();
        result = itch::parse(file->bytes(), stats, max_messages);
    } else {
        auto file = itch::BufferedFile::open(std::string(*input), error);
        if (!file) {
            std::fprintf(stderr, "itch-stats: %s\n", error.c_str());
            return 1;
        }
        result = itch::parse_stream(*file, stats, max_messages);
        if (file->failed()) {
            std::fprintf(stderr, "itch-stats: read error on %s\n", std::string(*input).c_str());
            return 1;
        }
        file_size = result.offset;
    }

    std::printf("file:        %s\n", std::string(*input).c_str());
    std::printf("bytes read:  %" PRIu64 "%s\n", result.offset,
                reader == "mmap" && result.offset != file_size ? " (stopped early)" : "");
    std::printf("messages:    %" PRIu64 "\n", result.messages);
    std::printf("symbols:     %" PRIu64 "\n", stats.symbols);
    if (stats.any) {
        std::printf("first ts:    %s\n", format_time(stats.first_ts).c_str());
        std::printf("last ts:     %s\n", format_time(stats.last_ts).c_str());
    }
    std::printf("\n%-4s %-36s %14s\n", "type", "name", "count");
    for (std::size_t i = 0; i < stats.counts.size(); ++i) {
        if (stats.counts[i] != 0) {
            const char type = static_cast<char>(i);
            std::printf("%-4c %-36s %14" PRIu64 "\n", type, name_of(type), stats.counts[i]);
        }
    }

    if (!result.ok()) {
        std::fprintf(stderr,
                     "itch-stats: %s at file offset %" PRIu64 " (type byte 0x%02x, record "
                     "length %u) after %" PRIu64 " messages\n",
                     itch::to_string(result.error), result.offset,
                     static_cast<unsigned>(static_cast<unsigned char>(result.type)),
                     static_cast<unsigned>(result.length), result.messages);
        return 1;
    }
    return 0;
}
