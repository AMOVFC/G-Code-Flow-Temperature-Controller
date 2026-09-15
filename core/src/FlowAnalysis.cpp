#include "sb53/FlowAnalysis.hpp"

#include "sb53/Ports.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <istream>
#include <numbers>
#include <string>

namespace sb53 {
namespace {

constexpr std::string_view kFlowPrefix = "Flow = ";
constexpr std::string_view kTimePrefix = "Time = ";
constexpr std::string_view kNone       = "None";
constexpr std::string_view kSome       = "Some(";

[[nodiscard]] bool startsWith(std::string_view s, std::string_view p) noexcept
{
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept
{
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string_view::npos) {
        return {};
    }
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

// Locale-invariant. std::from_chars ignores the global locale entirely, unlike
// std::stod/strtod -- which is how the legacy ended up doing string surgery on decimal
// separators before every parse (known-bugs.md #5).
[[nodiscard]] bool parseDouble(std::string_view text, double& out) noexcept
{
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

} // namespace

std::string_view toString(Phase p) noexcept
{
    switch (p) {
    case Phase::Scanning:     return "scanning";
    case Phase::Estimating:   return "estimating";
    case Phase::Analysing:    return "analysing";
    case Phase::Planning:     return "planning";
    case Phase::Rewriting:    return "rewriting";
    case Phase::ReEstimating: return "re-estimating";
    case Phase::Assembling:   return "assembling";
    }
    return "unknown";
}

double filamentCrossSection(Millimetres diameter) noexcept
{
    const double radius = diameter / 2.0;
    return std::numbers::pi * radius * radius;
}

void checkTimingAgainstSlicer(const ScanResult& scan, const SourceAnalysis& analysis,
                              DiagnosticList& diagnostics)
{
    if (scan.slicerEstimatedTime <= 0.0 || analysis.totalTime <= 0.0) {
        return;   // nothing to compare against
    }

    const double ratio = analysis.totalTime / scan.slicerEstimatedTime;

    // Slicer estimates are crude and routinely differ by 10-20%, so the threshold is
    // deliberately loose. It is here to catch a config describing the wrong machine --
    // which produces errors of 50% or more -- not to police normal disagreement.
    if (ratio < 1.4 && ratio > 0.6) {
        return;
    }

    const auto minutes = [](double s) {
        return std::to_string(static_cast<int>(s) / 60) + "m " +
               std::to_string(static_cast<int>(s) % 60) + "s";
    };

    diagnostics.add(warning(
        Code::PrinterConfigMismatch,
        "Computed print time (" + minutes(analysis.totalTime) + ") disagrees sharply "
        "with the slicer's estimate (" + minutes(scan.slicerEstimatedTime) + "). "
        "The loaded config.json probably describes a different printer. "
        "Flow and temperature are derived from these timings, so the result will look "
        "correct but be wrong. Check max_velocity and max_acceleration against your "
        "printer.cfg before using this file."));
}

std::vector<MoveSample> MoveDumpParser::parse(std::istream& in,
                                              DiagnosticList& diagnostics)
{
    std::vector<MoveSample> moves;

    std::string buffer;
    double pendingFlow = 0.0;
    bool haveFlow = false;
    bool retracted = false;
    std::size_t lineNumber = 0;
    std::size_t malformed = 0;

    while (std::getline(in, buffer)) {
        ++lineNumber;
        const std::string_view line = trim(buffer);

        if (line.empty() || line.front() == '#') {
            continue;   // fixture headers
        }

        if (startsWith(line, kFlowPrefix)) {
            const std::string_view value = line.substr(kFlowPrefix.size());

            if (startsWith(value, kNone)) {
                // A travel move. No extrusion, and crucially the retract flag is left
                // alone -- travel happens between the retract and the unretract.
                pendingFlow = 0.0;
            } else if (startsWith(value, kSome) && value.back() == ')') {
                const std::string_view inner =
                    value.substr(kSome.size(), value.size() - kSome.size() - 1);

                double flow = 0.0;
                if (!parseDouble(inner, flow)) {
                    ++malformed;
                    pendingFlow = 0.0;
                } else if (flow < 0.0) {
                    pendingFlow = 0.0;   // retraction
                    retracted = true;
                } else if (retracted) {
                    pendingFlow = 0.0;   // the matching unretract
                    retracted = false;
                } else {
                    pendingFlow = flow;
                }
            } else {
                ++malformed;
                pendingFlow = 0.0;
            }
            haveFlow = true;
            continue;
        }

        if (startsWith(line, kTimePrefix)) {
            double duration = 0.0;
            if (!parseDouble(line.substr(kTimePrefix.size()), duration)) {
                ++malformed;
                continue;
            }
            // A Time without a preceding Flow means the pairing is broken; treat the
            // move as non-extruding rather than silently reusing the last flow value.
            moves.push_back(MoveSample{duration, haveFlow ? pendingFlow : 0.0});
            haveFlow = false;
            pendingFlow = 0.0;
            continue;
        }

        ++malformed;
    }

    if (moves.empty()) {
        diagnostics.add(error(
            Code::EstimatorOutputUnparsable,
            lineNumber == 0
                ? "The motion estimator produced no output."
                : "The motion estimator's output could not be parsed: no moves found. "
                  "This usually means the estimator version does not match the one this "
                  "build expects."));
    } else if (malformed > 0) {
        diagnostics.add(warning(
            Code::EstimatorOutputUnparsable,
            "Skipped " + std::to_string(malformed) +
                " unrecognised line(s) in the estimator output."));
    }

    return moves;
}

SourceAnalysis analyseFlow(const std::vector<MoveSample>& moves,
                           const FlowAnalysisOptions& options)
{
    SourceAnalysis analysis;
    analysis.moves = moves;

    const double bucketWidth =
        options.bucketWidth > 0.0 ? options.bucketWidth : 1.0;
    const double crossSection = filamentCrossSection(options.filamentDiameter);

    double elapsed = 0.0;          // total time so far
    double filament = 0.0;         // total filament consumed, mm
    double bucketRemaining = bucketWidth;
    double bucketFlowTime = 0.0;   // Σ(flow × dt) within the current bucket
    double bucketElapsed = 0.0;    // Σ(dt) within the current bucket
    double bucketMaxFlow = 0.0;

    const auto emitBucket = [&] {
        // Divide by the time actually accumulated, not the nominal width: the final
        // bucket is usually partial, and dividing by the full width would understate it.
        const double average = bucketElapsed > 0.0 ? bucketFlowTime / bucketElapsed : 0.0;
        analysis.seconds.push_back(FlowSecond{elapsed, average, bucketMaxFlow, filament});
        bucketFlowTime = 0.0;
        bucketElapsed = 0.0;
        bucketMaxFlow = 0.0;
        bucketRemaining = bucketWidth;
    };

    for (const MoveSample& move : moves) {
        analysis.peakFlow = std::max(analysis.peakFlow, move.flow);

        double remaining = move.duration;
        if (!(remaining > 0.0)) {
            continue;   // zero-duration moves contribute nothing (also guards NaN)
        }

        // A move may span several buckets; split it rather than assigning it whole.
        while (remaining > 0.0) {
            const double take = std::min(remaining, bucketRemaining);

            bucketFlowTime += move.flow * take;
            bucketElapsed += take;
            bucketMaxFlow = std::max(bucketMaxFlow, move.flow);

            elapsed += take;
            filament += (move.flow * take) / crossSection;

            remaining -= take;
            bucketRemaining -= take;

            if (bucketRemaining <= 0.0) {
                emitBucket();
            }
        }
    }

    if (bucketElapsed > 0.0) {
        emitBucket();   // trailing partial second
    }

    analysis.totalTime = elapsed;
    analysis.totalFilament = filament;
    return analysis;
}

} // namespace sb53
