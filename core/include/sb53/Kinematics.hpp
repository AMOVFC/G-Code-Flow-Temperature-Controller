// Look-ahead motion planning.
//
// Replaces the klipper_estimator subprocess with an in-process implementation, so the
// tool is a single artefact and can be linked into a plugin rather than spawned.
// See ADR-0007.
//
// Implemented from the documented algorithm and first principles. Klipper is GPL-3.0 and
// this project is MIT; nothing here is derived from its source.

#pragma once

#include "sb53/Diagnostics.hpp"
#include "sb53/Model.hpp"

#include <array>
#include <iosfwd>
#include <string_view>
#include <vector>

namespace sb53 {

// ---------------------------------------------------------------------------
// Machine limits
// ---------------------------------------------------------------------------

// An extra constraint applied to moves along a particular direction, or to extrusion.
// Mirrors klipper_estimator's `move_checkers` so existing config.json files load
// unchanged.
struct AxisLimit {
    std::array<double, 3> axis{0.0, 0.0, 1.0};   // unit vector; Z by default
    double maxVelocity = 0.0;                    // mm/s along that axis
    double maxAcceleration = 0.0;
};

struct ExtruderLimit {
    double maxVelocity = 0.0;      // mm/s OF FILAMENT, not of the toolhead
    double maxAcceleration = 0.0;
};

struct MachineLimits {
    double maxVelocity = 300.0;            // mm/s
    double maxAcceleration = 3000.0;       // mm/s^2
    double squareCornerVelocity = 5.0;     // mm/s
    double minimumCruiseRatio = 0.5;
    double instantCornerVelocity = 1.0;    // mm/s, for extrude-only moves
    double mmPerArcSegment = 0.1;

    std::vector<AxisLimit> axisLimits;
    std::vector<ExtruderLimit> extruderLimits;

    // Klipper derives the junction deviation used for cornering from the square corner
    // velocity: a 90-degree corner taken at exactly `squareCornerVelocity` defines the
    // allowed deviation, and every other angle follows from the same circle.
    [[nodiscard]] double junctionDeviation() const noexcept;

    // Parses klipper_estimator's config.json. Unknown fields are ignored so newer configs
    // still load.
    [[nodiscard]] static MachineLimits fromJson(std::string_view json,
                                                DiagnosticList& diagnostics);
};

// ---------------------------------------------------------------------------
// Moves
// ---------------------------------------------------------------------------

// One planned move. Positions are absolute; E is a length of filament for this move
// (relative extrusion), which is what the tool requires anyway.
struct PlannedMove {
    std::array<double, 3> start{};
    std::array<double, 3> end{};
    double extrude = 0.0;          // mm of filament, may be negative (retract)
    double requestedVelocity = 0.0;// from the F word, mm/s

    // Filled in by the planner.
    double distance = 0.0;         // mm of toolhead travel (or |E| for extrude-only)
    std::array<double, 3> unit{};  // direction of travel
    double maxVelocity = 0.0;      // after machine and checker limits
    double acceleration = 0.0;
    double maxJunctionEntry = 0.0; // cornering limit against the previous move
    double entryVelocity = 0.0;
    double exitVelocity = 0.0;
    double duration = 0.0;         // seconds

    [[nodiscard]] bool isExtrudeOnly() const noexcept;
    [[nodiscard]] bool isTravel() const noexcept { return extrude <= 0.0; }

    // Volumetric flow, mm^3/s, or nothing for a non-extruding move. Matches the
    // estimator's `Flow = Some(x)` / `Flow = None`.
    [[nodiscard]] double flow(double filamentArea) const noexcept;
};

// Extracts moves from G-code. Handles G0/G1, G2/G3 arcs, G92 (position reset),
// G90/G91 and M82/M83.
//
// A move is any G0/G1 carrying at least one of X/Y/Z/E; a bare `G1 F9000` sets the
// feedrate and produces nothing. Verified against the reference implementation:
// 61,189 move lines minus 2,836 feedrate-only lines gives 58,353 moves.
[[nodiscard]] std::vector<PlannedMove> extractMoves(std::istream& in,
                                                    const MachineLimits& limits,
                                                    DiagnosticList& diagnostics);

// Runs the look-ahead planner over the extracted moves, filling in velocities and
// durations. Modifies in place.
void planMoves(std::vector<PlannedMove>& moves, const MachineLimits& limits);

// Convenience: extract, plan, and convert to the MoveSample form the flow analyser
// consumes -- the same shape MoveDumpParser produces from the subprocess.
[[nodiscard]] std::vector<MoveSample> estimateMoves(std::istream& gcode,
                                                    const MachineLimits& limits,
                                                    Millimetres filamentDiameter,
                                                    DiagnosticList& diagnostics);

// --- exposed for testing ----------------------------------------------------

namespace detail {

// Highest speed at which the corner between two unit directions can be taken.
// `junctionDeviation` comes from MachineLimits.
[[nodiscard]] double junctionVelocity(const std::array<double, 3>& prevUnit,
                                      const std::array<double, 3>& nextUnit,
                                      double junctionDeviation,
                                      double acceleration,
                                      double fallbackVelocity) noexcept;

// Time to traverse `distance` starting at `entry`, peaking no higher than `cruise`,
// finishing at `exit`, under constant `acceleration`.
[[nodiscard]] double trapezoidTime(double distance, double entry, double cruise,
                                   double exit, double acceleration) noexcept;

} // namespace detail
} // namespace sb53
