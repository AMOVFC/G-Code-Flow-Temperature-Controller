// The first pass over an input file.
//
// Establishes preconditions and locates the print body before any numerical work
// happens. See ALGORITHM.md §9.
//
// This stage needs no floating-point maths, no estimator and no database, which is why
// it is implemented first: it is the cheapest place to be certain we understand the
// input format.

#pragma once

#include "sb53/Diagnostics.hpp"
#include "sb53/Model.hpp"

#include <iosfwd>
#include <optional>
#include <string_view>

namespace sb53 {

class GcodeScanner {
public:
    // Scans the whole stream in a single forward pass.
    //
    // The stream must be read to the end even though the print body is found early:
    // OrcaSlicer writes `printer_settings_id` and `filament_settings_id` in the config
    // block *after* the executable block, not in the header. Profile auto-selection
    // depends on them, so an early exit would silently lose it.
    //
    // Errors are appended to `diagnostics`. On error the returned ScanResult is still
    // populated as far as scanning got, which makes failures diagnosable rather than
    // opaque.
    [[nodiscard]] static ScanResult scan(std::istream& in, DiagnosticList& diagnostics);
};

// --- helpers, exposed for testing -------------------------------------------

namespace detail {

// Strips a trailing CR (so CRLF input behaves) and any UTF-8 BOM on the first line.
[[nodiscard]] std::string_view normaliseLine(std::string_view line, bool isFirstLine);

// Extracts the value from a `; key = value` comment, or nothing if `line` is not that
// key. Tolerates arbitrary spacing around the separator.
//
// Surrounding double quotes are stripped: OrcaSlicer writes
// `; filament_settings_id = "Elegoo HS PLA+ awd hott"` and the quotes are its own
// escaping, not part of the profile name.
[[nodiscard]] std::optional<std::string_view> commentValue(std::string_view line,
                                                           std::string_view key);

} // namespace detail
} // namespace sb53
