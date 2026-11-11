#pragma once

// A machine-readable description of the run file formats, generated from the C++ structs
// themselves. `backtest --print-schema` prints it and the Python tests compare it with
// analysis/schema.py, so the two declarations of each layout cannot drift apart unnoticed.

#include <string>

namespace itchbt::record {

/// JSON: format version, header size, every record's size and each field's name, offset and
/// size, and the names of the enumerations stored in the records.
[[nodiscard]] std::string schema_json();

} // namespace itchbt::record
