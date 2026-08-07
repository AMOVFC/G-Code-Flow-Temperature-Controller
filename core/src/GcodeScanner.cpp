#include "sb53/GcodeScanner.hpp"

#include "sb53/GcodeText.hpp"
#include "sb53/Version.hpp"

#include <charconv>
#include <istream>
#include <string>

namespace sb53 {
namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";

// Body start. OrcaSlicer emits `;HEIGHT:`; PrusaSlicer-style output uses `; Z_HEIGHT:`.
constexpr std::string_view kHeightMarker  = ";HEIGHT:";
constexpr std::string_view kZHeightMarker = "; Z_HEIGHT:";

// Body end.
constexpr std::string_view kExecutableEnd = "; EXECUTABLE_BLOCK_END";
constexpr std::string_view kPrintEnd      = "; PRINT_END";

// The legacy tool's header marker. We must recognise it as well as our own, or a file
// it produced would be processed a second time and the adjustments would compound.
constexpr std::string_view kLegacyMarker = "; Edited by";

[[nodiscard]] bool startsWith(std::string_view s, std::string_view prefix) noexcept
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept
{
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

// True when `line` is a bare G-code command, i.e. the token appears at the start and is
// followed by end-of-line, whitespace, or a comment. Prevents `M83` from matching
// `M831` or a mention inside a comment.
[[nodiscard]] bool isCommand(std::string_view line, std::string_view command) noexcept
{
    if (!startsWith(line, command)) {
        return false;
    }
    if (line.size() == command.size()) {
        return true;
    }
    const char next = line[command.size()];
    return next == ' ' || next == '\t' || next == ';';
}

} // namespace

namespace detail {

std::string_view normaliseLine(std::string_view line, bool isFirstLine)
{
    if (isFirstLine && startsWith(line, kBom)) {
        line.remove_prefix(kBom.size());
    }
    // Input may be CRLF regardless of platform; std::getline only strips the LF.
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

std::optional<std::string_view> commentValue(std::string_view line, std::string_view key)
{
    if (line.empty() || line.front() != ';') {
        return std::nullopt;
    }

    std::string_view rest = trim(line.substr(1));
    if (!startsWith(rest, key)) {
        return std::nullopt;
    }
    rest.remove_prefix(key.size());

    rest = trim(rest);
    if (rest.empty() || rest.front() != '=') {
        return std::nullopt;   // e.g. `; filament_settings_id_extra` — not our key
    }
    rest = trim(rest.substr(1));

    // OrcaSlicer quotes values containing spaces; the quotes are its escaping, not part
    // of the name.
    if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') {
        rest = rest.substr(1, rest.size() - 2);
    }
    return rest;
}

} // namespace detail

ScanResult GcodeScanner::scan(std::istream& in, DiagnosticList& diagnostics)
{
    ScanResult result;

    std::string buffer;
    std::size_t lineNumber = 0;
    bool sawExtrusionMode = false;

    while (std::getline(in, buffer)) {
        ++lineNumber;
        const std::string_view line =
            detail::normaliseLine(buffer, lineNumber == 1);

        // --- preconditions ---------------------------------------------------

        // First extrusion-mode command wins. A later change would be unusual, and
        // honouring the first matches how the print actually begins.
        if (!sawExtrusionMode) {
            if (isCommand(line, "M83")) {
                result.extrusionMode = ExtrusionMode::Relative;
                sawExtrusionMode = true;
            } else if (isCommand(line, "M82")) {
                result.extrusionMode = ExtrusionMode::Absolute;
                sawExtrusionMode = true;
            }
        }

        if (!result.alreadyProcessed &&
            (startsWith(line, kProcessedMarker) || startsWith(line, kLegacyMarker))) {
            result.alreadyProcessed = true;
        }

        // --- print body bounds ------------------------------------------------
        //
        // Asymmetric by design, matching the legacy: the line that *marks* the start is
        // part of the body, but the line that marks the end is not. Getting this wrong
        // shifts every downstream artefact by one line.

        const bool isLayerMarker =
            startsWith(line, kHeightMarker) || startsWith(line, kZHeightMarker);

        if (result.bodyFirstLine == 0 && isLayerMarker) {
            result.bodyFirstLine = lineNumber;
        }

        // Within the body, track filament and layer starts.
        //
        // The accumulation rule MUST match GcodeRewriter's, or the layer boundaries land
        // at the wrong place in the plan. Both use detail::isExtrudingMove for exactly
        // that reason.
        if (result.bodyFirstLine != 0 && result.bodyLastLine == 0) {
            if (isLayerMarker) {
                LayerMark mark;
                mark.usedFilament = result.bodyFilament;
                mark.line = lineNumber;

                const auto colon = line.find(':');
                if (colon != std::string_view::npos) {
                    const std::string_view value = line.substr(colon + 1);
                    double height = 0.0;
                    const auto* begin = value.data();
                    if (std::from_chars(begin, begin + value.size(), height).ec ==
                        std::errc{}) {
                        mark.height = height;
                    }
                }
                result.layers.push_back(mark);
            } else if (detail::isExtrudingMove(line)) {
                if (const auto e = detail::word(line, 'E')) {
                    result.bodyFilament += *e;
                }
            }
        }

        if (result.bodyFirstLine != 0 && result.bodyLastLine == 0 &&
            (startsWith(line, kExecutableEnd) || startsWith(line, kPrintEnd))) {
            result.bodyLastLine = lineNumber - 1;   // exclusive of the marker itself
        }

        // --- slicer identity --------------------------------------------------
        //
        // These live in the config block *after* the executable block, so the scan
        // cannot stop once the body is bounded.

        if (result.printerSettingsId.empty()) {
            if (const auto v = detail::commentValue(line, "printer_settings_id")) {
                result.printerSettingsId = std::string(*v);
            }
        }
        if (result.filamentSettingsId.empty()) {
            if (const auto v = detail::commentValue(line, "filament_settings_id")) {
                result.filamentSettingsId = std::string(*v);
            }
        }
        if (result.filamentType.empty()) {
            if (const auto v = detail::commentValue(line, "filament_type")) {
                result.filamentType = std::string(*v);
            }
        }
    }

    result.totalLines = lineNumber;

    // --- report -------------------------------------------------------------

    if (lineNumber == 0) {
        diagnostics.add(error(Code::FileEmpty, "The G-code file is empty."));
        return result;
    }

    if (result.alreadyProcessed) {
        diagnostics.add(error(
            Code::AlreadyProcessed,
            "This file has already been processed. Re-processing would compound the "
            "temperature and speed adjustments already applied. Re-export it from your "
            "slicer instead."));
    }

    switch (result.extrusionMode) {
    case ExtrusionMode::Absolute:
        diagnostics.add(error(
            Code::AbsoluteExtrusionUnsupported,
            "This file uses absolute extrusion (M82). Flow tracking requires relative "
            "extrusion. Enable 'Use relative E distances' in your slicer's printer "
            "settings and re-export."));
        break;
    case ExtrusionMode::Unknown:
        diagnostics.add(error(
            Code::ExtrusionModeUnknown,
            "Neither M82 nor M83 was found, so relative extrusion could not be "
            "confirmed. Firmware defaults to absolute extrusion, which this tool cannot "
            "process correctly. Enable 'Use relative E distances' in your slicer."));
        break;
    case ExtrusionMode::Relative:
        break;
    }

    if (!result.hasPrintBody()) {
        // Distinguish the two ways this fails, because the fixes differ.
        const bool foundStart = result.bodyFirstLine != 0;
        diagnostics.add(error(
            Code::PrintBodyNotFound,
            foundStart
                ? "Found the start of the print but not its end. Expected a "
                  "'; EXECUTABLE_BLOCK_END' or '; PRINT_END' marker. If your printer's "
                  "end G-code is custom, add '; PRINT_END' as its first line."
                : "Could not locate the print body. Expected a ';HEIGHT:' or "
                  "'; Z_HEIGHT:' marker. This tool currently supports OrcaSlicer and "
                  "PrusaSlicer output."));
    }

    return result;
}

} // namespace sb53
