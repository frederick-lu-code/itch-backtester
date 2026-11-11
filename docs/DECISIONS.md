# Decisions and trade-offs

A running log of choices made while building the project, written at the time each was made.
Where a choice departs from the project specification, the entry says so. Measured choices
link to the optimisation log in [BENCHMARKS.md](../BENCHMARKS.md).

## Environment

**E1. Develop on macOS, treat Linux CI as authoritative.** The specification assumes Linux
x86-64. The only machine available is an Apple Silicon Mac (M5, 16 GB) with no Docker, so code
is written and tested locally with Apple Clang and every push is built and tested on Ubuntu
24.04 with GCC 13 and Clang 18, including ASan, UBSan, TSan, the fuzzer and clang-tidy.
Trade-off: Linux-only behaviour is only ever exercised in CI, never interactively.

**E2. Benchmarks run on the Mac, with the method adapted and labelled.** The specification says
not to report numbers from anything but bare-metal Linux. The owner chose to use the machine
available. Every result records the machine; `BENCHMARKS.md` opens with a table of what could
not be done (core pinning, governor, `perf stat`, TSC timing). Trade-off: absolute numbers are
not comparable with the specification's x86 targets; ratios between variants are still valid.

**E3. The `fuzz` preset only builds with upstream Clang.** Apple Clang ships no libFuzzer, so
fuzzing happens in CI (60 s per push). Trade-off: no long local fuzzing sessions.

**E4. Commits use GitHub's no-reply address.** The repository is public; the owner's work email
is kept out of the history.

## Build

**B1. Sanitizer flags are global, warnings are per target.** GoogleTest must be compiled with
the same sanitizer instrumentation as the code under test, but must not be held to this
project's `-Werror` warning set. Dependencies are also included as `SYSTEM`.

**B2. Dependencies are pinned in one file but fetched only when first used.** Keeps CI fast in
early milestones without scattering version pins.

**B3. `std::byteswap` replaced by compiler builtins.** The specification suggests
`std::byteswap`, which is C++23. The project builds as strict C++20 (`CMAKE_CXX_EXTENSIONS
OFF`), so `__builtin_bswap16/32/64` are used, as the specification allows.

## Input data

**I1. Reference day is 30 December 2019** (`12302019.NASDAQ_ITCH50`, 8,251,407,909 bytes,
268,744,780 messages). It is the smallest full day Nasdaq publishes in the classic file naming,
which matters on a 16 GB machine.

**I2. Downloads are verified by gzip CRC, not MD5.** Nasdaq lists `.md5sum` files but its server
returns 404 for them. The script prints a SHA-256 of the decompressed file instead, and
`run.json` records it per run.

## Parser

**P1. Errors are return values, not exceptions.** The hot path may not throw. `parse()` returns
the error kind, the record's file offset, its type byte and stated length; tools print that and
exit non-zero.

**P2. `F` (add with MPID) decodes into the same struct as `A`.** Nothing in the specification
uses the attribution. The header keeps the original type byte so `itch-stats` can still count
the two separately.

**P3. The ten unused message types are length-checked and delivered as a header only.** They
must be recognised so a new or corrupt type is still a hard error, but decoding fields nobody
reads would be dead code.

**P4. An invalid buy/sell byte decodes as Sell instead of failing.** Real files never contain
one, the specification allows only `B` and `S`, and a branch that can abort is a liability under
fuzzing. Trade-off: a corrupt side byte is not detected. A corrupt file is far more likely to
trip the length or type checks first.

**P5. The 6-byte timestamp is read as a 2-byte and a 4-byte load.** Reading 8 bytes and masking
is one instruction shorter but reads two bytes past the field, which is out of bounds on the
last message of a mapping whose size is a multiple of the page size.

**P6. The parse benchmark reports two handlers.** With an empty handler the compiler deletes
the decoders, so that number measures framing only. The checksum handler forces every field to
be read and is the figure quoted as "parse only".

**P7. The read() reader hands the parser its buffer and keeps the unconsumed tail.** Finding
record boundaries in the reader would mean walking the length prefixes twice. Instead the
parser reports "truncated" at the end of a buffer and `parse_stream` treats that as "read more"
unless the file has also ended.
