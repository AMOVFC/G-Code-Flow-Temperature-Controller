// The pass that actually modifies the file.
//
// ALGORITHM.md §6-§8. Walks the print body maintaining cumulative extruded filament,
// injecting temperature commands, clamping feedrates, and optionally adjusting pressure
// advance.

#pragma once

#include "sb53/Diagnostics.hpp"
#include "sb53/Model.hpp"
#include "sb53/Ports.hpp"
#include "sb53/Profiles.hpp"

#include <iosfwd>
#include <string_view>

namespace sb53 {

struct RewriteOptions {
    Millimetres filamentDiameter = 1.75;

    // Klipper only, and only in concealed features -- see allowsPressureAdvanceChange().
    bool adjustPressureAdvance = false;

    // Smallest temperature change worth emitting a command for. Without this the output
    // carries a near-duplicate M104 on almost every move; the legacy emits roughly one
    // per two seconds of print.
    Celsius temperatureStep = 0.1;

    // Emit the original feedrate as an annotated, immediately-superseded line before the
    // replacement, the way the legacy does. Off by default: it doubles the feedrate
    // lines for no functional effect. Useful when diffing against legacy output.
    bool echoSlicerSpeed = false;
};

// Comment vocabulary, matching the legacy so output stays familiar to existing users and
// directly comparable in differential testing (ADR-0005).
inline constexpr std::string_view kCommentRecommendedSpeed = "; Recommended Speed";
inline constexpr std::string_view kCommentKeepSlicerSpeed  = "; Keep Slicer Speed";
inline constexpr std::string_view kCommentSlicerSpeed      = "; Slicer Speed";
inline constexpr std::string_view kCommentResetBeforeRetraction =
    "; Reset Speed Before Retraction";
inline constexpr std::string_view kCommentResetInitialTemperature =
    "; Reset Initial Temperature";

// Rewrites `in` to `out`.
//
// `analysis` and `plan` must come from the same run: the plan is indexed by cumulative
// extruded filament, which is only comparable against the timeline it was built from.
[[nodiscard]] RewriteStats rewriteGcode(std::istream& in, std::ostream& out,
                                        const ScanResult& scan,
                                        const SourceAnalysis& analysis,
                                        const TemperaturePlan& plan,
                                        const ExtruderProfile& extruder,
                                        const FilamentProfile& filament,
                                        DiagnosticList& diagnostics,
                                        IProgressSink& progress,
                                        const RewriteOptions& options = {});

// --- helpers, exposed for testing -------------------------------------------

namespace detail {

// Cross-sectional area of an extruded bead: a rectangle with semicircular ends.
// ALGORITHM.md §7. Derived from geometry, not inherited from the legacy.
[[nodiscard]] double extrusionArea(Millimetres layerHeight, Millimetres lineWidth) noexcept;

// Feedrate (mm/min) that achieves `flow` given a bead of that cross-section.
[[nodiscard]] MillimetresPerMin flowToFeedrate(CubicMmPerSec flow, double area) noexcept;

// Extracts a G-code word (e.g. 'F', 'E') from a line, ignoring any trailing comment.
[[nodiscard]] std::optional<double> word(std::string_view line, char letter) noexcept;

// Maps an OrcaSlicer/PrusaSlicer feature-type marker to a FeatureType.
[[nodiscard]] FeatureType parseFeatureType(std::string_view value) noexcept;

// Formats a number the way G-code expects: locale-invariant, no trailing zeros.
[[nodiscard]] std::string formatNumber(double value, int maxDecimals);

} // namespace detail
} // namespace sb53
