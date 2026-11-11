// libFuzzer entry point for the ITCH parser and order book.
//
// M0 placeholder: it exists so the `fuzz` preset and the CI fuzz step are wired up and proven to
// build and run. It exercises nothing yet. M1 replaces the body with the reader and parser, and
// M2 adds the book.

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static_cast<void>(data);
    static_cast<void>(size);
    return 0;
}
