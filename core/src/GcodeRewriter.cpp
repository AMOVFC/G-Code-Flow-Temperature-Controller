#include "sb53/GcodeRewriter.hpp"

#include "sb53/FlowAnalysis.hpp"
#include "sb53/TemperaturePlanner.hpp"
#include "sb53/Version.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <istream>
#include <numbers>
#include <ostream>
#include <string>

namespace sb53 {
namespace detail {

double extrusionArea(Millimetres layerHeight, Millimetres lineWidth) noexcept
{
    // A bead of height h and width w, modelled as a rectangle with semicircular ends:
    //     h*(w - h) + pi*(h/2)^2
    // Degenerate inputs would make the feedrate infinite, so refuse them.
    if (!(layerHeight > 0.0) || !(lineWidth > 0.0) || lineWidth < layerHeight) {
        return 0.0;
    }
    const double radius = layerHeight / 2.0;
    return layerHeight * (lineWidth - layerHeight) + std::numbers::pi * radius * radius;
}

MillimetresPerMin flowToFeedrate(CubicMmPerSec flow, double area) noexcept
{
    if (!(area > 0.0) || !(flow > 0.0)) {
        return 0.0;
    }
    return 60.0 * flow / area;
}

FeatureType parseFeatureType(std::string_view value) noexcept
{
    const std::string_view v = trim(value);

    // OrcaSlicer names, which are what the samples use. PrusaSlicer's differ slightly;
    // both spellings are accepted where they diverge.
    if (v == "Outer wall" || v == "External perimeter") { return FeatureType::OuterWall; }
    if (v == "Inner wall" || v == "Perimeter")          { return FeatureType::InnerWall; }
    if (v == "Sparse infill" || v == "Internal infill") { return FeatureType::SparseInfill; }
    if (v == "Internal solid infill" || v == "Solid infill") {
        return FeatureType::InternalSolidInfill;
    }
    if (v == "Top surface" || v == "Top solid infill")  { return FeatureType::TopSurface; }
    if (v == "Bridge" || v == "Bridge infill" || v == "External bridge") {
        return FeatureType::Bridge;
    }
    if (v == "Internal Bridge" || v == "Internal bridge") {
        return FeatureType::InternalBridge;
    }
    if (v == "Support" || v == "Support material")      { return FeatureType::Support; }
    if (v == "Overhang wall" || v == "Overhang perimeter") { return FeatureType::Overhang; }
    if (v == "Skirt" || v == "Brim" || v == "Skirt/Brim") { return FeatureType::Skirt; }
    return FeatureType::Unknown;
}

} // namespace detail

// ---------------------------------------------------------------------------

using detail::command;
using detail::extrusionArea;
using detail::flowToFeedrate;
using detail::formatNumber;
using detail::parseFeatureType;
using detail::startsWith;
using detail::trim;
using detail::word;

RewriteStats rewriteGcode(std::istream& in, std::ostream& out,
                          const ScanResult& scan,
                          const SourceAnalysis& analysis,
                          const TemperaturePlan& plan,
                          const ExtruderProfile& extruder,
                          const FilamentProfile& filament,
                          DiagnosticList& diagnostics,
                          IProgressSink& progress,
                          const RewriteOptions& options)
{
    RewriteStats stats;

    if (plan.empty() || analysis.seconds.empty()) {
        diagnostics.add(error(Code::NoExtrusionFound,
                              "There is no temperature plan to apply."));
        return stats;
    }

    // E words are already a length of filament in mm, which is exactly the coordinate
    // the plan is indexed by -- no volumetric conversion is needed on this side.
    const std::string initialTemp = formatNumber(plan.initialTemperature, 1);

    // Rewrite state, all of it slicer-marker driven.
    double layerHeight = 0.0;
    double lineWidth = 0.0;
    FeatureType feature = FeatureType::Unknown;

    double usedFilament = 0.0;      // mm, the coordinate the plan is indexed by
    double slicerFeedrate = 0.0;    // most recent F word from the slicer

    // Tool position, needed to derive each move's ACTUAL extrusion per millimetre.
    // The slicer's ;WIDTH:/;HEIGHT: markers describe the nominal bead, but individual
    // moves -- gap fill, overlaps, seams -- routinely extrude more than that, so a cap
    // computed from the markers alone is not a cap at all. Measured on a real benchy:
    // moves reached 120 mm3/s while the declared geometry implied about 105.
    double posX = 0.0;
    double posY = 0.0;
    const double filamentArea = filamentCrossSection(options.filamentDiameter);
    double lastEmittedTemp = -1000.0;
    double lastEmittedPa = -1000.0;
    bool headerMarkerWritten = false;
    bool startMacroRewritten = false;

    stats.minTemperature = plan.minTemperature;
    stats.maxTemperature = plan.maxTemperature;

    const auto emitLine = [&](std::string_view text) {
        out << text << '\n';
        ++stats.linesWritten;
    };

    std::string buffer;
    std::size_t lineNumber = 0;

    while (std::getline(in, buffer)) {
        ++lineNumber;
        ++stats.linesRead;

        std::string_view line{buffer};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if ((lineNumber & 0x3FFFu) == 0) {
            if (progress.cancelRequested()) {
                diagnostics.add(error(Code::Cancelled, "Cancelled."));
                return stats;
            }
            progress.onPhase(Phase::Rewriting,
                             scan.totalLines > 0
                                 ? static_cast<double>(lineNumber) /
                                       static_cast<double>(scan.totalLines)
                                 : -1.0);
        }

        // --- header: stamp the file so it cannot be processed twice --------
        if (!headerMarkerWritten && startsWith(line, "; HEADER_BLOCK_START")) {
            emitLine(line);
            emitLine(std::string(kProcessedMarker) + " " + std::string(kVersion));
            headerMarkerWritten = true;
            continue;
        }

        // --- start macro: replace the first-layer temperature ---------------
        //
        // Parsed and re-emitted rather than spliced at a fixed offset, which is how the
        // legacy corrupts the line for 2- and 4-digit temperatures (known-bugs.md #4).
        if (!startMacroRewritten && !extruder.startMacro.empty() &&
            startsWith(line, extruder.startMacro) && !extruder.temperatureToken.empty()) {
            const auto tokenPos = line.find(extruder.temperatureToken + "=");
            if (tokenPos != std::string_view::npos) {
                const auto valueStart = tokenPos + extruder.temperatureToken.size() + 1;
                auto valueEnd = line.find_first_of(" \t;", valueStart);
                if (valueEnd == std::string_view::npos) {
                    valueEnd = line.size();
                }
                std::string rewritten;
                rewritten.append(line.substr(0, valueStart));
                rewritten.append(initialTemp);
                rewritten.append(line.substr(valueEnd));
                rewritten.append("    ");
                rewritten.append(kCommentResetInitialTemperature);
                emitLine(rewritten);
                startMacroRewritten = true;
                continue;
            }
        }

        // --- outside the print body: copy through untouched -----------------
        if (lineNumber < scan.bodyFirstLine || lineNumber > scan.bodyLastLine) {
            emitLine(line);
            continue;
        }

        // --- slicer markers -------------------------------------------------
        if (startsWith(line, ";HEIGHT:")) {
            std::string_view rest = trim(line.substr(8));
            double parsed = 0.0;
            if (std::from_chars(rest.data(), rest.data() + rest.size(), parsed).ec ==
                std::errc{}) {
                layerHeight = parsed;
            }
            emitLine(line);
            continue;
        }
        if (startsWith(line, ";WIDTH:")) {
            std::string_view rest = trim(line.substr(7));
            double parsed = 0.0;
            if (std::from_chars(rest.data(), rest.data() + rest.size(), parsed).ec ==
                std::errc{}) {
                lineWidth = parsed;
            }
            emitLine(line);
            continue;
        }
        if (startsWith(line, ";TYPE:")) {
            feature = parseFeatureType(line.substr(6));
            emitLine(line);
            continue;
        }

        const std::string_view cmd = command(line);

        // --- retraction: restore the slicer's own feedrate ------------------
        //
        // A retract must run at the speed the slicer chose; applying a flow-derived
        // feedrate to a non-printing move would be meaningless and slow.
        const auto e = word(line, 'E');
        const bool isRetraction = e.has_value() && *e < 0.0;

        if (isRetraction) {
            if (const auto f = word(line, 'F'); f.has_value()) {
                slicerFeedrate = *f;
            }
            emitLine(line);
            continue;
        }

        // --- a bare feedrate line -------------------------------------------
        //
        // The slicer emits "G1 F9000" on its own before a run of moves. That is where
        // the clamp is applied, so it affects everything that follows.
        const bool isMove = startsWith(cmd, "G1") || startsWith(cmd, "G0");
        const auto f = word(line, 'F');

        if (isMove && f.has_value() && !e.has_value() &&
            !word(line, 'X').has_value() && !word(line, 'Y').has_value() &&
            !word(line, 'Z').has_value()) {
            slicerFeedrate = *f;

            const auto planned = plan.temperatureAtFilament(analysis.seconds, usedFilament);
            const double area = extrusionArea(layerHeight, lineWidth);

            if (planned.has_value() && area > 0.0) {
                double budget = temperatureToFlow(filament, *planned);

                // User-imposed bounds, applied on top of the calibration curve.
                if (options.maxFlow > 0.0) {
                    budget = std::min(budget, options.maxFlow);
                }
                if (options.minFlow > 0.0) {
                    budget = std::max(budget, options.minFlow);
                }

                const double recommended = flowToFeedrate(budget, area);

                // THE core safety property (ALGORITHM.md Ã‚Â§2): speed is clamped downward
                // only. If the slicer asked for something slower, that is respected.
                if (recommended > 0.0 && recommended < slicerFeedrate) {
                    if (options.echoSlicerSpeed) {
                        emitLine("G1 F" + formatNumber(slicerFeedrate, 0) + "    " +
                                 std::string(kCommentSlicerSpeed));
                    }
                    emitLine("G1 F" + formatNumber(std::round(recommended), 0) + "    " +
                             std::string(kCommentRecommendedSpeed));
                    ++stats.feedratesReduced;
                    continue;
                }
            }

            emitLine("G1 F" + formatNumber(slicerFeedrate, 0) + "    " +
                     std::string(kCommentKeepSlicerSpeed));
            continue;
        }

        // Replaces the value of a word in place, preserving everything else on the line.
        const auto replaceWord = [](std::string_view source, char letter,
                                    const std::string& value) {
            const auto at = source.find(letter);
            if (at == std::string_view::npos) {
                return std::string(source);
            }
            auto end = at + 1;
            while (end < source.size() &&
                   (std::isdigit(static_cast<unsigned char>(source[end])) ||
                    source[end] == '.' || source[end] == '-')) {
                ++end;
            }
            // Deliberately not named `out`: that is the output stream parameter of the
            // enclosing function, and shadowing it here compiled fine on MSVC while GCC
            // rejected it.
            std::string rewritten;
            rewritten.append(source.substr(0, at + 1));
            rewritten.append(value);
            rewritten.append(source.substr(end));
            return rewritten;
        };

        // --- an extruding move ----------------------------------------------
        //
        // Shared with the scanner via detail::isExtrudingMove, deliberately: the scanner
        // establishes the layer boundaries and the analyser the plan, both in filament
        // space, and any disagreement about what counts as extrusion misaligns them.
        // That has already happened once here -- see the unretract regression test.
        if (detail::isExtrudingMove(line) && e.has_value()) {
            usedFilament += *e;

            if (const auto planned =
                    plan.temperatureAtFilament(analysis.seconds, usedFilament)) {
                if (std::abs(*planned - lastEmittedTemp) >= options.temperatureStep) {
                    emitLine("M104 S" + formatNumber(*planned, 1));
                    lastEmittedTemp = *planned;
                    ++stats.temperatureCommands;
                }

                // Pressure advance: Klipper only, and only in features hidden inside the
                // part. Changing it in a visible wall can leave surface bubbles
                // (ALGORITHM.md Ã‚Â§8) -- this gate is a correctness requirement.
                if (options.adjustPressureAdvance && filament.adjustPressureAdvance &&
                    allowsPressureAdvanceChange(feature)) {
                    const double pa = temperatureToPressureAdvance(filament, *planned);
                    if (std::abs(pa - lastEmittedPa) >= 0.001) {
                        emitLine("SET_PRESSURE_ADVANCE ADVANCE=" + formatNumber(pa, 4));
                        lastEmittedPa = pa;
                        ++stats.pressureAdvanceCommands;
                    }
                }
            }
        }

        // --- per-move flow cap ----------------------------------------------
        //
        // The clamp above works from the slicer's declared bead geometry, which is only
        // nominal. This one uses the move's OWN extrusion per millimetre, so it is exact
        // and is what makes a hard flow limit actually hold:
        //
        //     flow = (extruded / distance) * velocity * filamentArea
        //
        // Rearranged, the fastest this move may go without exceeding the cap is
        // velocity = cap / ((extruded / distance) * filamentArea).
        //
        // Engaged ONLY when an explicit maxFlow is set. Applying it unconditionally
        // clamps essentially every move to the temperature budget -- 53,119 reductions
        // against 2,308 -- which is arguably more correct but is a large behavioural
        // change that has not been justified against a real print. Opt in.
        if (options.maxFlow > 0.0 && detail::isExtrudingMove(line) && filamentArea > 0.0) {
            const double nx = word(line, 'X').value_or(posX);
            const double ny = word(line, 'Y').value_or(posY);
            const double distance = std::hypot(nx - posX, ny - posY);
            const double extruded = word(line, 'E').value_or(0.0);
            posX = nx;
            posY = ny;

            const double effective = f.has_value() ? *f : slicerFeedrate;

            if (distance > 1e-9 && extruded > 0.0 && effective > 0.0) {
                const auto planned =
                    plan.temperatureAtFilament(analysis.seconds, usedFilament);

                double cap = options.maxFlow;   // 0 means "no explicit ceiling"
                if (planned.has_value()) {
                    double budget = temperatureToFlow(filament, *planned);
                    if (options.maxFlow > 0.0) {
                        budget = std::min(budget, options.maxFlow);
                    }
                    if (options.minFlow > 0.0) {
                        budget = std::max(budget, options.minFlow);
                    }
                    cap = budget;
                }

                if (cap > 0.0) {
                    const double perMm = (extruded / distance) * filamentArea;   // mm^3 per mm
                    if (perMm > 1e-12) {
                        const double maxFeedrate = 60.0 * cap / perMm;
                        if (maxFeedrate < effective) {
                            emitLine(replaceWord(line, 'F',
                                                 formatNumber(std::round(maxFeedrate), 0))
                                     + (f.has_value() ? "" : " F" +
                                        formatNumber(std::round(maxFeedrate), 0)));
                            ++stats.feedratesReduced;
                            continue;
                        }
                    }
                }
            }
        } else {
            // Keep the position tracker honest across travel and Z moves.
            posX = word(line, 'X').value_or(posX);
            posY = word(line, 'Y').value_or(posY);
        }

        emitLine(line);
    }

    if (stats.temperatureCommands == 0) {
        // The legacy's defining failure was producing a file that looked processed but
        // was not, with nothing said (known-bugs.md #11). Never repeat it silently.
        diagnostics.add(warning(
            Code::NoExtrusionFound,
            "No temperature commands were emitted. The flow analysis may not correspond "
            "to this file, or the calibrated temperature range may be too narrow to "
            "produce any change."));
    }

    return stats;
}

} // namespace sb53
