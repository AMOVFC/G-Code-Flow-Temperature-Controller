// Comparing the temperature curve of two processed files.
//
// This is the differential verification described in ADR-0005. We do NOT compare bytes
// or absolute values: the blend formula was deliberately changed (ADR-0006), several
// legacy defects are fixed, and calibration profiles may differ. What must agree is the
// SHAPE -- does temperature respond to flow the same way?
//
// A curve that trends the wrong way, saturates, or is phase-shifted indicates a real
// misunderstanding of the algorithm. A constant offset does not.

#pragma once

#include "sb53/Model.hpp"

#include <iosfwd>
#include <vector>

namespace sb53 {

struct TemperaturePoint {
    // Cumulative extruded filament at the moment the command was issued.
    //
    // This is the x-axis rather than time or line number for the same reason the plan is
    // indexed by it (ALGORITHM.md §6): rewriting feedrates changes timing and inserting
    // commands changes line numbers, but the same geometry always consumes the same
    // filament. It is the only coordinate two differently-processed versions of one
    // print share.
    Millimetres usedFilament = 0.0;
    Celsius     temperature = 0.0;
};

// Extracts the commanded temperature sequence from a processed G-code file.
[[nodiscard]] std::vector<TemperaturePoint> extractTemperatureCurve(std::istream& in);

struct CurveStats {
    std::size_t points = 0;
    Celsius     minimum = 0.0;
    Celsius     maximum = 0.0;
    Celsius     mean = 0.0;
    Millimetres span = 0.0;   // cumulative filament covered
};

struct CurveComparison {
    CurveStats a;
    CurveStats b;

    std::size_t samples = 0;

    // Pearson correlation of the two curves resampled onto a shared filament grid.
    //
    // This is the headline number. Near 1.0 means both tools raise and lower temperature
    // at the same points in the print, which is what "the algorithm is understood
    // correctly" looks like. Near 0 means they are unrelated; negative means inverted.
    double correlation = 0.0;

    // Differences after removing each curve's mean, so a pure calibration offset does
    // not register as disagreement.
    Celsius rmsDifference = 0.0;
    Celsius maxDifference = 0.0;

    // Raw mean offset between the two, reported separately because it is expected and
    // benign when calibration profiles differ.
    Celsius meanOffset = 0.0;
};

// Resamples both curves onto a shared cumulative-filament grid over their overlapping
// range and compares them.
[[nodiscard]] CurveComparison compareCurves(const std::vector<TemperaturePoint>& a,
                                            const std::vector<TemperaturePoint>& b,
                                            std::size_t samples = 400);

// The temperature in effect at a given filament position: the most recent command at or
// before it. A step function, because that is what M104 actually is.
[[nodiscard]] Celsius temperatureAt(const std::vector<TemperaturePoint>& curve,
                                    Millimetres usedFilament) noexcept;

} // namespace sb53
