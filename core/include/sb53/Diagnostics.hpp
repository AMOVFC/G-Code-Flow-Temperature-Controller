// Diagnostics — how the core reports problems.
//
// The core never prints and never throws across its API boundary. Problems are values.
// A CLI prints them, a GUI lists them in a panel, a web backend serialises them to JSON;
// the core does not know which is happening. See ADR-0002.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sb53 {

enum class Severity {
    Info,       // progress and context; never indicates a problem
    Warning,    // processing continues, but the user should know
    Error,      // processing cannot continue
};

// Stable, machine-readable identifiers.
//
// These are part of the API: a frontend may switch on them to offer a targeted fix, and
// they may appear in logs users paste into bug reports. Add freely; never renumber or
// repurpose an existing one.
enum class Code {
    Unknown = 0,

    // --- input file problems -------------------------------------------------
    FileNotFound,
    FileUnreadable,
    FileEmpty,

    // --- preconditions (ALGORITHM.md §9) -------------------------------------
    //
    // These three are the difference between a user who can fix their problem and one
    // who cannot. The legacy application detected none of them.

    // The G-code uses absolute extrusion (M82). Flow tracking accumulates raw E values
    // and is meaningless under absolute mode, so the entire temperature mapping would
    // silently produce garbage. The legacy did not check for this at all.
    AbsoluteExtrusionUnsupported,

    // Neither M82 nor M83 appears, so relative extrusion cannot be confirmed.
    //
    // Treated as an error rather than a warning: firmware defaults to ABSOLUTE when
    // unspecified, so proceeding risks producing a file that looks fine and prints
    // badly. That silent-wrongness mode is the single worst outcome for this tool
    // (legacy/known-bugs.md #9, #11), and a warning in a CLI scrolls past unread.
    ExtrusionModeUnknown,

    // The file already carries our header marker. Re-processing would compound the
    // adjustments already applied.
    AlreadyProcessed,

    // No recognisable print body — we could not locate where real printing starts and
    // ends, so we cannot avoid rewriting start/end macros.
    PrintBodyNotFound,

    // --- profile / configuration --------------------------------------------
    ProfileNotFound,
    ProfileInvalid,

    // Calibration flow points are not strictly increasing. The temperature-to-flow
    // inversion divides by segment width, so equal values are a division by zero.
    // See ALGORITHM.md §3.
    FlowPointsNotIncreasing,

    // Calibration temperature points are not monotonic.
    TemperaturePointsNotMonotonic,

    PrinterConfigMissing,
    PrinterConfigInvalid,

    // --- estimator -----------------------------------------------------------
    EstimatorNotFound,
    EstimatorFailed,
    EstimatorOutputUnparsable,

    // --- processing ----------------------------------------------------------
    NoExtrusionFound,       // nothing to analyse
    OutputWriteFailed,
    Cancelled,
};

struct Diagnostic {
    Severity    severity = Severity::Error;
    Code        code     = Code::Unknown;
    std::string message;        // human-readable; safe to show directly

    // Optional context. Zero means "not applicable" — G-code lines are 1-based.
    std::size_t line = 0;

    [[nodiscard]] bool isError() const noexcept { return severity == Severity::Error; }
};

// Convenience constructors.
[[nodiscard]] Diagnostic info(Code, std::string message);
[[nodiscard]] Diagnostic warning(Code, std::string message);
[[nodiscard]] Diagnostic error(Code, std::string message);

// A stable short name for a code, for logs and tests. Never localised.
[[nodiscard]] std::string_view toString(Code) noexcept;
[[nodiscard]] std::string_view toString(Severity) noexcept;

// A collection of diagnostics, as returned by each pipeline stage.
class DiagnosticList {
public:
    void add(Diagnostic d) { m_items.push_back(std::move(d)); }

    [[nodiscard]] bool hasErrors() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return m_items.empty(); }
    [[nodiscard]] const std::vector<Diagnostic>& items() const noexcept { return m_items; }

    auto begin() const noexcept { return m_items.begin(); }
    auto end()   const noexcept { return m_items.end(); }

private:
    std::vector<Diagnostic> m_items;
};

} // namespace sb53
