#include "sb53/Kinematics.hpp"

#include "sb53/FlowAnalysis.hpp"
#include "sb53/GcodeText.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <istream>
#include <numbers>
#include <string>

namespace sb53 {
namespace {

[[nodiscard]] double dot(const std::array<double, 3>& a,
                         const std::array<double, 3>& b) noexcept
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

[[nodiscard]] double length(const std::array<double, 3>& v) noexcept
{
    return std::sqrt(dot(v, v));
}

// --- a very small JSON reader ------------------------------------------------
//
// config.json is machine-generated and its shape is fixed, so a targeted reader is
// enough. Deliberately tolerant: unknown keys are ignored so newer estimator configs
// still load rather than failing.

[[nodiscard]] std::size_t skipSpace(std::string_view s, std::size_t i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        ++i;
    }
    return i;
}

// Finds `"key"` at brace depth `depth` within [from, to) and returns the offset just
// past its colon, or npos.
[[nodiscard]] std::size_t findKey(std::string_view s, std::string_view key,
                                  std::size_t from, std::size_t to)
{
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t i = s.find(needle, from);
    while (i != std::string_view::npos && i < to) {
        std::size_t j = skipSpace(s, i + needle.size());
        if (j < s.size() && s[j] == ':') {
            return skipSpace(s, j + 1);
        }
        i = s.find(needle, i + 1);
    }
    return std::string_view::npos;
}

[[nodiscard]] bool readNumber(std::string_view s, std::size_t at, double& out)
{
    if (at == std::string_view::npos || at >= s.size()) {
        return false;
    }
    const auto* begin = s.data() + at;
    const auto* end = s.data() + s.size();
    return std::from_chars(begin, end, out).ec == std::errc{};
}

void readInto(std::string_view json, std::string_view key, double& target,
              std::size_t from = 0,
              std::size_t to = std::string_view::npos)
{
    double v = 0.0;
    if (readNumber(json, findKey(json, key, from, std::min(to, json.size())), v)) {
        target = v;
    }
}

// Extent of the {...} or [...] starting at `open`.
[[nodiscard]] std::size_t matchBracket(std::string_view s, std::size_t open)
{
    if (open >= s.size()) {
        return std::string_view::npos;
    }
    const char c = s[open];
    const char closing = (c == '{') ? '}' : ']';
    int depth = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        if (s[i] == c) {
            ++depth;
        } else if (s[i] == closing) {
            if (--depth == 0) {
                return i;
            }
        }
    }
    return std::string_view::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// Machine limits
// ---------------------------------------------------------------------------

double MachineLimits::junctionDeviation() const noexcept
{
    // Klipper expresses cornering as a "square corner velocity": the speed at which a
    // 90-degree corner may be taken. Turning that into the deviation used for every
    // other angle gives scv^2 * (sqrt(2) - 1) / accel -- the radius of the arc that
    // fits inside a right-angle turn at that speed.
    if (maxAcceleration <= 0.0) {
        return 0.0;
    }
    const double scv = std::max(0.0, squareCornerVelocity);
    return (scv * scv) * (std::numbers::sqrt2 - 1.0) / maxAcceleration;
}

MachineLimits MachineLimits::fromJson(std::string_view json, DiagnosticList& diagnostics)
{
    MachineLimits m;

    readInto(json, "max_velocity", m.maxVelocity);
    readInto(json, "max_acceleration", m.maxAcceleration);
    readInto(json, "square_corner_velocity", m.squareCornerVelocity);
    readInto(json, "minimum_cruise_ratio", m.minimumCruiseRatio);
    readInto(json, "instant_corner_velocity", m.instantCornerVelocity);
    readInto(json, "mm_per_arc_segment", m.mmPerArcSegment);

    // move_checkers is an array of single-key objects.
    const auto checkersAt = findKey(json, "move_checkers", 0, json.size());
    if (checkersAt != std::string_view::npos && json[checkersAt] == '[') {
        const auto end = matchBracket(json, checkersAt);
        std::size_t cursor = checkersAt;

        while (cursor < end) {
            const auto axisAt = findKey(json, "axis_limiter", cursor, end);
            const auto extAt = findKey(json, "extruder_limiter", cursor, end);
            const auto next = std::min(axisAt == std::string_view::npos ? end : axisAt,
                                       extAt == std::string_view::npos ? end : extAt);
            if (next >= end) {
                break;
            }

            const auto objEnd = matchBracket(json, next);
            if (objEnd == std::string_view::npos) {
                break;
            }

            if (next == axisAt) {
                AxisLimit a;
                readInto(json, "max_velocity", a.maxVelocity, next, objEnd);
                readInto(json, "max_accel", a.maxAcceleration, next, objEnd);
                const auto arr = findKey(json, "axis", next, objEnd);
                if (arr != std::string_view::npos && json[arr] == '[') {
                    std::size_t p = arr + 1;
                    for (int k = 0; k < 3; ++k) {
                        p = skipSpace(json, p);
                        double v = 0.0;
                        if (readNumber(json, p, v)) {
                            a.axis[static_cast<std::size_t>(k)] = v;
                        }
                        const auto comma = json.find(',', p);
                        if (comma == std::string_view::npos) {
                            break;
                        }
                        p = comma + 1;
                    }
                }
                m.axisLimits.push_back(a);
            } else {
                ExtruderLimit e;
                readInto(json, "max_velocity", e.maxVelocity, next, objEnd);
                readInto(json, "max_accel", e.maxAcceleration, next, objEnd);
                m.extruderLimits.push_back(e);
            }
            cursor = objEnd + 1;
        }
    }

    if (m.maxVelocity <= 0.0 || m.maxAcceleration <= 0.0) {
        diagnostics.add(error(
            Code::PrinterConfigInvalid,
            "Printer config is missing max_velocity or max_acceleration."));
    }
    return m;
}

// ---------------------------------------------------------------------------
// Moves
// ---------------------------------------------------------------------------

bool PlannedMove::isExtrudeOnly() const noexcept
{
    const std::array<double, 3> d{end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    return length(d) < 1e-12 && std::abs(extrude) > 0.0;
}

double PlannedMove::flow(double filamentArea) const noexcept
{
    if (duration <= 0.0) {
        return 0.0;
    }
    return (extrude * filamentArea) / duration;
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------

std::vector<PlannedMove> extractMoves(std::istream& in, const MachineLimits& limits,
                                      DiagnosticList& diagnostics)
{
    std::vector<PlannedMove> moves;

    std::array<double, 3> pos{0.0, 0.0, 0.0};
    double feedrate = 0.0;          // mm/s
    bool absolutePositioning = true;
    bool absoluteExtrusion = false; // slicers we support emit M83

    std::string buffer;
    while (std::getline(in, buffer)) {
        std::string_view line{buffer};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::string_view cmd = detail::command(line);
        if (cmd.empty()) {
            continue;
        }

        if (detail::startsWith(cmd, "G90")) { absolutePositioning = true;  continue; }
        if (detail::startsWith(cmd, "G91")) { absolutePositioning = false; continue; }
        if (detail::startsWith(cmd, "M82")) { absoluteExtrusion = true;    continue; }
        if (detail::startsWith(cmd, "M83")) { absoluteExtrusion = false;   continue; }

        if (detail::startsWith(cmd, "G92")) {
            // Resets the logical position without moving.
            if (const auto x = detail::word(line, 'X')) { pos[0] = *x; }
            if (const auto y = detail::word(line, 'Y')) { pos[1] = *y; }
            if (const auto z = detail::word(line, 'Z')) { pos[2] = *z; }
            continue;
        }

        const bool isLinear = detail::startsWith(cmd, "G0") || detail::startsWith(cmd, "G1");
        const bool isArc = detail::startsWith(cmd, "G2") || detail::startsWith(cmd, "G3");
        if (!isLinear && !isArc) {
            continue;
        }

        if (const auto f = detail::word(line, 'F')) {
            feedrate = *f / 60.0;   // F is mm/min
        }

        const auto x = detail::word(line, 'X');
        const auto y = detail::word(line, 'Y');
        const auto z = detail::word(line, 'Z');
        const auto e = detail::word(line, 'E');

        if (!x && !y && !z && !e) {
            continue;   // a bare feedrate line produces no move
        }

        std::array<double, 3> target = pos;
        if (x) { target[0] = absolutePositioning ? *x : pos[0] + *x; }
        if (y) { target[1] = absolutePositioning ? *y : pos[1] + *y; }
        if (z) { target[2] = absolutePositioning ? *z : pos[2] + *z; }

        double extrude = 0.0;
        if (e) {
            // Absolute extrusion is rejected upstream by the scanner; treat it as
            // relative here rather than silently mis-accounting.
            extrude = absoluteExtrusion ? *e : *e;
        }

        if (isArc) {
            // Subdivide into chords of at most mm_per_arc_segment. Centre comes from
            // I/J offsets relative to the current position.
            const double i = detail::word(line, 'I').value_or(0.0);
            const double j = detail::word(line, 'J').value_or(0.0);
            const double cx = pos[0] + i;
            const double cy = pos[1] + j;
            const double radius = std::hypot(pos[0] - cx, pos[1] - cy);

            double a0 = std::atan2(pos[1] - cy, pos[0] - cx);
            double a1 = std::atan2(target[1] - cy, target[0] - cx);
            const bool clockwise = detail::startsWith(cmd, "G2");

            double sweep = a1 - a0;
            if (clockwise && sweep >= 0.0) { sweep -= 2.0 * std::numbers::pi; }
            if (!clockwise && sweep <= 0.0) { sweep += 2.0 * std::numbers::pi; }

            const double arcLength = std::abs(sweep) * radius;
            const double step = std::max(1e-6, limits.mmPerArcSegment);
            auto segments = static_cast<std::size_t>(std::ceil(arcLength / step));
            segments = std::clamp<std::size_t>(segments, 1, 100000);

            const double dz = (target[2] - pos[2]) / static_cast<double>(segments);
            for (std::size_t k = 1; k <= segments; ++k) {
                const double t = static_cast<double>(k) / static_cast<double>(segments);
                const double a = a0 + sweep * t;
                std::array<double, 3> p{cx + radius * std::cos(a),
                                        cy + radius * std::sin(a),
                                        pos[2] + dz * static_cast<double>(k)};
                PlannedMove m;
                m.start = pos;
                m.end = p;
                m.extrude = extrude / static_cast<double>(segments);
                m.requestedVelocity = feedrate;
                moves.push_back(m);
                pos = p;
            }
            continue;
        }

        // A command that neither travels nor extrudes is a no-op -- e.g. `G1 X10 Y10`
        // issued when already at X10 Y10. The reference implementation emits nothing for
        // these, and counting them would shift every subsequent move index.
        const double travel = std::hypot(std::hypot(target[0] - pos[0], target[1] - pos[1]),
                                         target[2] - pos[2]);
        if (travel < 1e-12 && std::abs(extrude) < 1e-12) {
            pos = target;
            continue;
        }

        PlannedMove m;
        m.start = pos;
        m.end = target;
        m.extrude = extrude;
        m.requestedVelocity = feedrate;
        moves.push_back(m);
        pos = target;
    }

    if (moves.empty()) {
        diagnostics.add(error(Code::NoExtrusionFound,
                              "No movement commands were found in this G-code."));
    }
    return moves;
}

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

namespace detail {

double junctionVelocity(const std::array<double, 3>& prevUnit,
                        const std::array<double, 3>& nextUnit,
                        double junctionDeviation, double acceleration,
                        double fallbackVelocity) noexcept
{
    if (junctionDeviation <= 0.0 || acceleration <= 0.0) {
        return fallbackVelocity;
    }

    // Negated so that +1 means a complete reversal and -1 means dead straight.
    double c = -dot(prevUnit, nextUnit);

    if (c > 0.999999) {
        return 0.0;   // reversal: must come to a stop
    }
    c = std::max(c, -0.999999);

    const double sinHalf = std::sqrt(0.5 * (1.0 - c));
    const double denom = 1.0 - sinHalf;
    if (denom <= 1e-9) {
        return fallbackVelocity;   // effectively straight
    }

    const double radius = junctionDeviation * sinHalf / denom;
    return std::sqrt(acceleration * radius);
}

double trapezoidTime(double distance, double entry, double cruise, double exit,
                     double acceleration) noexcept
{
    if (distance <= 0.0) {
        return 0.0;
    }
    if (acceleration <= 0.0) {
        const double v = std::max({entry, cruise, exit, 1e-9});
        return distance / v;
    }

    entry = std::max(0.0, entry);
    exit = std::max(0.0, exit);
    cruise = std::max(cruise, std::max(entry, exit));

    const double accelDist = (cruise * cruise - entry * entry) / (2.0 * acceleration);
    const double decelDist = (cruise * cruise - exit * exit) / (2.0 * acceleration);

    if (accelDist + decelDist <= distance) {
        const double cruiseDist = distance - accelDist - decelDist;
        return (cruise - entry) / acceleration
             + (cruise > 0.0 ? cruiseDist / cruise : 0.0)
             + (cruise - exit) / acceleration;
    }

    // Triangular: never reaches the cruise speed. Solve for the peak.
    const double peakSq =
        (2.0 * acceleration * distance + entry * entry + exit * exit) / 2.0;
    const double peak = std::sqrt(std::max(peakSq, std::max(entry * entry, exit * exit)));
    return (peak - entry) / acceleration + (peak - exit) / acceleration;
}

} // namespace detail

void planMoves(std::vector<PlannedMove>& moves, const MachineLimits& limits)
{
    if (moves.empty()) {
        return;
    }

    const double jd = limits.junctionDeviation();

    // --- per-move geometry and caps -----------------------------------------
    for (PlannedMove& m : moves) {
        const std::array<double, 3> d{m.end[0] - m.start[0],
                                      m.end[1] - m.start[1],
                                      m.end[2] - m.start[2]};
        const double travel = length(d);

        m.acceleration = limits.maxAcceleration;

        if (travel > 1e-12) {
            m.distance = travel;
            m.unit = {d[0] / travel, d[1] / travel, d[2] / travel};
            m.maxVelocity = std::min(m.requestedVelocity > 0.0 ? m.requestedVelocity
                                                               : limits.maxVelocity,
                                     limits.maxVelocity);

            // Directional limits (Z, typically). A move mostly along the limited axis
            // is capped in proportion to how much of it lies along that axis.
            for (const AxisLimit& a : limits.axisLimits) {
                const double along = std::abs(dot(m.unit, a.axis));
                if (along > 1e-9) {
                    if (a.maxVelocity > 0.0) {
                        m.maxVelocity = std::min(m.maxVelocity, a.maxVelocity / along);
                    }
                    if (a.maxAcceleration > 0.0) {
                        m.acceleration = std::min(m.acceleration,
                                                  a.maxAcceleration / along);
                    }
                }
            }

            // Extruder limits, expressed in mm/s OF FILAMENT, so convert through the
            // ratio of filament length to toolhead travel for this move.
            if (std::abs(m.extrude) > 0.0) {
                const double ratio = std::abs(m.extrude) / travel;
                for (const ExtruderLimit& e : limits.extruderLimits) {
                    if (e.maxVelocity > 0.0 && ratio > 1e-12) {
                        m.maxVelocity = std::min(m.maxVelocity, e.maxVelocity / ratio);
                    }
                    if (e.maxAcceleration > 0.0 && ratio > 1e-12) {
                        m.acceleration = std::min(m.acceleration,
                                                  e.maxAcceleration / ratio);
                    }
                }
            }
        } else {
            // Extrude-only: retract, unretract or prime. The "distance" is filament.
            m.distance = std::abs(m.extrude);
            m.unit = {0.0, 0.0, 0.0};
            m.maxVelocity = m.requestedVelocity > 0.0 ? m.requestedVelocity
                                                      : limits.maxVelocity;
            for (const ExtruderLimit& e : limits.extruderLimits) {
                if (e.maxVelocity > 0.0) {
                    m.maxVelocity = std::min(m.maxVelocity, e.maxVelocity);
                }
                if (e.maxAcceleration > 0.0) {
                    m.acceleration = std::min(m.acceleration, e.maxAcceleration);
                }
            }
        }

        m.maxVelocity = std::max(m.maxVelocity, 1e-9);
    }

    // --- junction limits ----------------------------------------------------
    moves.front().maxJunctionEntry = 0.0;   // starts from rest
    for (std::size_t i = 1; i < moves.size(); ++i) {
        PlannedMove& prev = moves[i - 1];
        PlannedMove& cur = moves[i];

        if (prev.distance <= 0.0 || cur.distance <= 0.0 ||
            length(prev.unit) < 0.5 || length(cur.unit) < 0.5) {
            // An extrude-only move breaks the motion chain; Klipper allows only a small
            // instantaneous velocity across such a junction.
            cur.maxJunctionEntry = limits.instantCornerVelocity;
        } else {
            cur.maxJunctionEntry = detail::junctionVelocity(
                prev.unit, cur.unit, jd, cur.acceleration,
                std::min(prev.maxVelocity, cur.maxVelocity));
        }
        cur.maxJunctionEntry =
            std::min(cur.maxJunctionEntry, std::min(prev.maxVelocity, cur.maxVelocity));
    }

    // --- backward pass ------------------------------------------------------
    //
    // Walking from the end, cap each move's entry speed by what still allows the
    // required exit speed to be reached within the move's length.
    //
    // The look-ahead uses a REDUCED acceleration -- `minimum_cruise_ratio` of the full
    // value is held back. Klipper does this so the planner does not schedule a velocity
    // it can only achieve by accelerating and immediately decelerating again, which on
    // dense geometry produces audible surging and, on a bowden or flexible drive,
    // visible extrusion artefacts. Timing below still uses the full acceleration; this
    // only makes the planner more conservative about how fast it commits to entering a
    // move.
    const double lookaheadScale =
        std::clamp(1.0 - limits.minimumCruiseRatio, 0.05, 1.0);

    double nextEntry = 0.0;   // the print ends at rest
    for (std::size_t i = moves.size(); i-- > 0;) {
        PlannedMove& m = moves[i];
        const double a = m.acceleration * lookaheadScale;
        const double reachable =
            std::sqrt(nextEntry * nextEntry + 2.0 * a * m.distance);
        m.entryVelocity = std::min({m.maxJunctionEntry, m.maxVelocity, reachable});
        nextEntry = m.entryVelocity;
    }

    // --- forward pass and timing --------------------------------------------
    double prevExit = 0.0;
    for (std::size_t i = 0; i < moves.size(); ++i) {
        PlannedMove& m = moves[i];
        // Cannot enter faster than the previous move could leave.
        if (i > 0) {
            m.entryVelocity = std::min(m.entryVelocity, prevExit);
        }

        const double reachable = std::sqrt(m.entryVelocity * m.entryVelocity
                                           + 2.0 * m.acceleration * lookaheadScale
                                                 * m.distance);
        const double nextEntryLimit =
            (i + 1 < moves.size()) ? moves[i + 1].entryVelocity : 0.0;
        m.exitVelocity = std::min({reachable, m.maxVelocity, nextEntryLimit});

        m.duration = detail::trapezoidTime(m.distance, m.entryVelocity, m.maxVelocity,
                                           m.exitVelocity, m.acceleration);
        prevExit = m.exitVelocity;
    }
}

std::vector<MoveSample> estimateMoves(std::istream& gcode, const MachineLimits& limits,
                                      Millimetres filamentDiameter,
                                      DiagnosticList& diagnostics)
{
    auto moves = extractMoves(gcode, limits, diagnostics);
    if (diagnostics.hasErrors()) {
        return {};
    }
    planMoves(moves, limits);

    const double area = filamentCrossSection(filamentDiameter);

    std::vector<MoveSample> samples;
    samples.reserve(moves.size());
    bool retracted = false;

    for (const PlannedMove& m : moves) {
        double flow = m.flow(area);

        // Same retract/unretract suppression the subprocess parser applies, so both
        // paths produce identical input to the flow analyser.
        if (flow < 0.0) {
            flow = 0.0;
            retracted = true;
        } else if (retracted && flow > 0.0) {
            flow = 0.0;
            retracted = false;
        }
        samples.push_back(MoveSample{m.duration, flow});
    }
    return samples;
}

} // namespace sb53
