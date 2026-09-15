// Data model.
//
// This file exists because the legacy application had no domain layer at all: its
// charting widgets *were* its data structures. Everything here is a plain value type
// with no framework dependency. Charts consume these; nothing here exists to serve a
// widget. See ARCHITECTURE.md.
//
// Units are named in every field. Flow/temperature/feedrate confusion is the most
// likely source of a silent numerical bug in this codebase, and a bad number here
// becomes a bad print rather than a crash.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sb53 {

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------
// Deliberately transparent aliases rather than strong types. They document intent at
// zero cost. If unit confusion ever causes a real bug, promote these to distinct types.

using Seconds          = double;   // s
using Millimetres       = double;  // mm
using MillimetresPerMin = double;  // mm/min — G-code F words are per minute
using CubicMmPerSec     = double;  // mm^3/s — volumetric flow
using Celsius           = double;  // °C

// ---------------------------------------------------------------------------
// Extrusion mode (ALGORITHM.md §9)
// ---------------------------------------------------------------------------

enum class ExtrusionMode {
    Unknown,    // no M82/M83 seen — we cannot verify the precondition
    Relative,   // M83 — required
    Absolute,   // M82 — unsupported; rejected during the scan
};

// ---------------------------------------------------------------------------
// Feature types (ALGORITHM.md §8)
// ---------------------------------------------------------------------------
// Pressure advance may be changed ONLY in features concealed inside the part. Changing
// it in visible walls can cause surface bubbles. This is a documented correctness
// requirement, not a heuristic — do not widen the permitted set.

enum class FeatureType {
    Unknown,
    OuterWall,
    InnerWall,
    SparseInfill,         // PA permitted
    InternalSolidInfill,  // PA permitted
    TopSurface,
    Bridge,
    InternalBridge,       // PA permitted
    Support,              // PA permitted
    Overhang,
    Skirt,
};

[[nodiscard]] bool allowsPressureAdvanceChange(FeatureType) noexcept;

// ---------------------------------------------------------------------------
// Raw estimator output (ALGORITHM.md §4)
// ---------------------------------------------------------------------------

// One move, as reported by klipper_estimator.
struct MoveSample {
    Seconds       duration = 0.0;
    CubicMmPerSec flow     = 0.0;   // zero for retract/unretract and travel
};

// ---------------------------------------------------------------------------
// Per-second timeline (ALGORITHM.md §4)
// ---------------------------------------------------------------------------

// One second of the print.
//
// One second is the aggregation width because hotend thermal inertia is measured in
// seconds — it cannot track per-move flow changes occurring in milliseconds.
struct FlowSecond {
    Seconds       time          = 0.0;  // elapsed time at the end of this bucket
    CubicMmPerSec averageFlow   = 0.0;
    CubicMmPerSec maxFlow       = 0.0;

    // Cumulative filament extruded by the end of this second.
    //
    // This is the coordinate the temperature plan is indexed by, and the reason is
    // worth remembering: rewriting feedrates changes the *timing* of the print and
    // inserting commands changes *line numbers*, but the same geometry always consumes
    // the same filament. It is the only stable coordinate shared between the analysis
    // pass and the rewrite pass. See ALGORITHM.md §6.
    Millimetres   usedFilament  = 0.0;
};

// ---------------------------------------------------------------------------
// Scan pass (ALGORITHM.md §9)
// ---------------------------------------------------------------------------

// Where a layer begins.
//
// Recorded in FILAMENT space rather than by line or move index. Line numbers shift when
// commands are inserted, and the estimator does not emit one move per G0/G1 line
// (measured: 61,189 move lines produced 58,352 moves), so neither is a usable key.
// Cumulative filament is the same invariant the temperature plan uses.
struct LayerMark {
    Millimetres usedFilament = 0.0;   // cumulative extrusion when the layer starts
    Millimetres height = 0.0;         // layer height from the slicer marker
    std::size_t line = 0;             // 1-based, for diagnostics only
};

// What the first pass over the input file establishes, before any numerical work.
struct ScanResult {
    ExtrusionMode extrusionMode = ExtrusionMode::Unknown;
    bool alreadyProcessed = false;

    // Print body bounds, as 1-based line numbers into the input file. Everything
    // outside this range is start/end macro territory and is copied through untouched.
    std::size_t bodyFirstLine = 0;
    std::size_t bodyLastLine  = 0;

    // Slicer identity comments, used to auto-select profiles. Empty when absent.
    std::string printerSettingsId;
    std::string filamentSettingsId;
    std::string filamentType;

    std::size_t totalLines = 0;

    // The slicer's own print-time estimate, in seconds, or 0 if absent.
    //
    // Kept because it is an independent second opinion on our own timing. A large
    // disagreement almost always means the printer config does not describe this
    // machine, which silently corrupts every flow and temperature number downstream.
    Seconds slicerEstimatedTime = 0.0;

    // Layer starts, in order. Used for layer-time cooling (ALGORITHM.md §5.5).
    std::vector<LayerMark> layers;

    // Total filament extruded within the print body, by the same rule the rewriter uses.
    Millimetres bodyFilament = 0.0;

    [[nodiscard]] bool hasPrintBody() const noexcept {
        return bodyFirstLine > 0 && bodyLastLine >= bodyFirstLine;
    }
};

// ---------------------------------------------------------------------------
// Analysis of the source file (ALGORITHM.md §4)
// ---------------------------------------------------------------------------

struct SourceAnalysis {
    std::vector<MoveSample> moves;
    std::vector<FlowSecond> seconds;

    Seconds       totalTime    = 0.0;
    CubicMmPerSec peakFlow     = 0.0;
    Millimetres   totalFilament = 0.0;
};

// ---------------------------------------------------------------------------
// Temperature plan (ALGORITHM.md §5)
// ---------------------------------------------------------------------------

// The planner's output. All vectors are parallel to SourceAnalysis::seconds.
struct TemperaturePlan {
    // §5.1 — average and maximum blended by the speed/quality bias.
    std::vector<CubicMmPerSec> blendedFlow;

    // §5.2 — centered moving average of blendedFlow.
    std::vector<CubicMmPerSec> smoothedFlow;

    // §5.3 — what the filament calibration asks for.
    std::vector<Celsius> desiredTemperature;

    // §5.5 — per-second temperature reduction applied for short layer times. Empty when
    // the feature is off. Retained so the UI can show why a section runs cooler than the
    // flow alone would suggest.
    std::vector<Celsius> layerCoolingDrop;

    // §5.4 — what the hotend can actually deliver, after slew limiting.
    //
    // This is the curve that gets written to the file. It lags `desiredTemperature`
    // asymmetrically: heating is driven and nearly on demand, cooling is passive and
    // slower. That asymmetry is physical and expected, not an artefact.
    std::vector<Celsius> achievableTemperature;

    // First-layer temperature, applied before flow-derived control begins (§9).
    Celsius initialTemperature = 0.0;

    // Summary of the achievable curve and the smoothed flow it came from. Reported to
    // the user so "it did nothing" is visible rather than inferred -- the failure mode
    // that made the legacy so hard to diagnose (legacy/known-bugs.md #11).
    Celsius minTemperature = 0.0;
    Celsius maxTemperature = 0.0;
    CubicMmPerSec minFlow = 0.0;
    CubicMmPerSec maxFlow = 0.0;

    [[nodiscard]] std::size_t size() const noexcept { return achievableTemperature.size(); }
    [[nodiscard]] bool empty() const noexcept { return achievableTemperature.empty(); }

    // Planned temperature at a given cumulative filament position.
    //
    // Returns nothing when the position falls outside the planned range — a represented
    // state, not an error. The legacy's equivalent returned uninitialised memory on
    // exactly these paths (legacy/known-bugs.md #3).
    [[nodiscard]] std::optional<Celsius> temperatureAtFilament(
        const std::vector<FlowSecond>& seconds, Millimetres usedFilament) const;
};

// ---------------------------------------------------------------------------
// Rewrite result (ALGORITHM.md §10)
// ---------------------------------------------------------------------------

struct RewriteStats {
    std::size_t linesRead            = 0;
    std::size_t linesWritten         = 0;
    std::size_t temperatureCommands  = 0;
    std::size_t feedratesReduced     = 0;   // never "increased" — see ALGORITHM.md §2
    std::size_t pressureAdvanceCommands = 0;

    Celsius minTemperature = 0.0;
    Celsius maxTemperature = 0.0;
};

} // namespace sb53
