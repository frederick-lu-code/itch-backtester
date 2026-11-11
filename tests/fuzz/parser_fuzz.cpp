// libFuzzer entry point: arbitrary bytes through the ITCH parser.
//
// The checksum handler reads every decoded field, so an out-of-bounds or uninitialised read
// anywhere in the decoders reaches a sanitizer.

#include "itchbt/itch/checksum.hpp"
#include "itchbt/itch/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace itchbt;
    const std::span<const std::byte> input = std::as_bytes(std::span(data, size));
    itch::ChecksumHandler handler;
    const itch::ParseResult result = itch::parse(input, handler);
    // Properties of the result that must hold whatever the input.
    if (result.offset > size || (result.ok() && result.offset != size) ||
        result.messages != handler.messages) {
        __builtin_trap();
    }
    return 0;
}
