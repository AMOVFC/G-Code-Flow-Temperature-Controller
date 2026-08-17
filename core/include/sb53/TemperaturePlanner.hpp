// Turning a flow timeline into a temperature plan.
//
// ALGORITHM.md §5. Pure computation over SourceAnalysis -- no I/O, no subprocess, no
// database -- so every stage is directly unit testable.

#pragma once

#include "sb53/Diagnostics.hpp"
#include "sb53/Model.hpp"
#include "sb53/Profiles.hpp"

#include <span>
#include <vector>

namespace sb53 {

struct PlannerOptions {
    // Must match the width used by analyseFlow, since the plan is index-parallel to the
    // per-second timeline.
    Seconds bucketWidth = 1.0;
};

// Runs blend -> smooth -> map -> slew-limit and returns the plan.
[[nodiscard]] TemperaturePlan planTemperature(const SourceAnalysis& analysis,
                                              const ExtruderProfile& extruder,
                                              const FilamentProfile& filament,
                                              DiagnosticList& diagnostics,
                                              const PlannerOptions& options = {},
                                              const std::vector<LayerMark>& layers = {});

// --- layer-time cooling (ALGORITHM.md §5.5) ---------------------------------

// How long each layer takes, derived by looking up the time at each layer's start and
// end filament position in the per-second timeline.
//
// Layer boundaries are known in filament space; the timeline relates filament to time.
// Neither line numbers nor move indices can be used: lines shift when commands are
// inserted, and the estimator does not emit one move per move-line.
[[nodiscard]] std::vector<Seconds> computeLayerDurations(
    const std::vector<LayerMark>& layers, const std::vector<FlowSecond>& seconds);

// The temperature reduction for a layer of the given duration: zero at or above
// `thresholdSeconds`, ramping linearly to `maxDrop` at zero.
[[nodiscard]] Celsius coolingDropForLayerTime(Seconds layerTime, Seconds thresholdSeconds,
                                              Celsius maxDrop) noexcept;

// Spreads per-layer drops across the per-second timeline, so each second carries the
// reduction belonging to the layer it falls in.
[[nodiscard]] std::vector<Celsius> layerCoolingProfile(
    const std::vector<LayerMark>& layers, const std::vector<FlowSecond>& seconds,
    Seconds thresholdSeconds, Celsius maxDrop);

// --- stages, exposed individually so they can be tested and reused ----------

// Centered moving average. At the ends the window is clamped and the divisor is the
// number of samples ACTUALLY averaged, not the nominal width -- dividing by the full
// width would bias the first and last seconds toward zero.
//
// The centering matters for more than smoothness: it makes each value depend on flow
// slightly *ahead* in time, which is what lets the hotend start heating before a
// high-flow section arrives. Since this is post-processing rather than real-time
// control, that look-ahead is free, and it is why the slew limiter below can be a
// simple causal filter.
[[nodiscard]] std::vector<double> movingAverage(std::span<const double> values,
                                                int window);

// The three-point piecewise-linear calibration curve. Values outside low..high clamp to
// the endpoints.
[[nodiscard]] Celsius flowToTemperature(const FilamentProfile&, CubicMmPerSec flow) noexcept;

// The inverse: the flow the filament can sustain at a given temperature. Needed by the
// rewriter to convert a planned temperature into a feedrate budget (ALGORITHM.md §6).
//
// Requires strictly increasing flow points; FilamentProfile::validate() enforces that,
// because the segment widths appear in the denominator here.
[[nodiscard]] CubicMmPerSec temperatureToFlow(const FilamentProfile&, Celsius) noexcept;

// Pressure advance interpolated on the same three points.
[[nodiscard]] double temperatureToPressureAdvance(const FilamentProfile&, Celsius) noexcept;

// Limits a desired temperature curve to what the hotend can physically deliver.
//
// Asymmetric by nature: `riseRate` and `fallRate` differ because heating is driven and
// cooling is passive. The result therefore rises nearly on demand and decays gradually.
// That lag is correct, not an artefact.
[[nodiscard]] std::vector<Celsius> applySlewLimit(std::span<const Celsius> desired,
                                                  double riseRatePerSecond,
                                                  double fallRatePerSecond,
                                                  Seconds bucketWidth);

} // namespace sb53
