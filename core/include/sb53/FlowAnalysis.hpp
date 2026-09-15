// Parsing the estimator's move dump, and aggregating it into a per-second timeline.
//
// ALGORITHM.md §4.

#pragma once

#include "sb53/Diagnostics.hpp"
#include "sb53/Model.hpp"

#include <iosfwd>
#include <vector>

namespace sb53 {

// ---------------------------------------------------------------------------
// Move dump parsing
// ---------------------------------------------------------------------------

// Parses `klipper_estimator dump-moves` output.
//
// The format is pairs of lines:
//
//     Flow = Some(5.5804673)     an extruding move
//     Time = 0.0437
//     Flow = None                a travel move -- no extrusion
//     Time = 0.1513
//
// Negative flow is a retraction. Retract and unretract moves are not real extrusion and
// must not contribute to the flow signal, so both are zeroed: a negative sample arms a
// flag, and the next extruding sample is zeroed and disarms it. `None` yields zero flow
// and leaves the flag alone, because travel moves happen *between* the retract and the
// unretract.
class MoveDumpParser {
public:
    [[nodiscard]] static std::vector<MoveSample> parse(std::istream& in,
                                                       DiagnosticList& diagnostics);
};

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

struct FlowAnalysisOptions {
    // One second, because hotend thermal inertia is measured in seconds -- it cannot
    // track per-move flow changes occurring in milliseconds. Configurable so the
    // assumption can be tested rather than merely inherited (ALGORITHM.md, open
    // question 2).
    Seconds bucketWidth = 1.0;

    // Needed to convert volumetric flow (mm^3/s) into length of filament consumed (mm),
    // which is the coordinate the temperature plan is indexed by. Read from the
    // slicer's `; filament_diameter` comment where available.
    Millimetres filamentDiameter = 1.75;
};

// Aggregates raw move samples into the per-second timeline.
//
// Moves are split across bucket boundaries rather than assigned whole, so a single long
// slow move contributes correctly to every second it spans. Flow is averaged
// time-weighted, which is what "the average flow rate reached in this second" means
// physically -- a plain mean over moves would over-weight short ones.
[[nodiscard]] SourceAnalysis analyseFlow(const std::vector<MoveSample>& moves,
                                         const FlowAnalysisOptions& options = {});

// Cross-sectional area of the filament itself, mm^2.
[[nodiscard]] double filamentCrossSection(Millimetres diameter) noexcept;

// Compares our computed print time against the slicer's own estimate and warns when they
// disagree sharply.
//
// This is the single highest-value sanity check in the tool. A printer config describing
// the wrong machine produces a completely self-consistent result -- flow, temperature and
// speed are all computed correctly from timings that are simply wrong -- so nothing else
// looks amiss. The slicer's estimate is the only independent second opinion available.
//
// Observed in practice: a config claiming 6000 mm/s^2 on a machine capable of 150000
// reported 13m 50s where the slicer said 7m 18s, and halved every flow figure with it.
void checkTimingAgainstSlicer(const ScanResult& scan, const SourceAnalysis& analysis,
                              DiagnosticList& diagnostics);

} // namespace sb53
