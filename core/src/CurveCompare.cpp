#include "sb53/CurveCompare.hpp"

#include "sb53/GcodeRewriter.hpp"   // detail::word

#include <algorithm>
#include <cmath>
#include <istream>
#include <numeric>
#include <string>

namespace sb53 {
namespace {

[[nodiscard]] bool startsWith(std::string_view s, std::string_view p) noexcept
{
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

[[nodiscard]] CurveStats summarise(const std::vector<TemperaturePoint>& c)
{
    CurveStats s;
    s.points = c.size();
    if (c.empty()) {
        return s;
    }

    s.minimum = c.front().temperature;
    s.maximum = c.front().temperature;
    double sum = 0.0;
    for (const auto& p : c) {
        s.minimum = std::min(s.minimum, p.temperature);
        s.maximum = std::max(s.maximum, p.temperature);
        sum += p.temperature;
    }
    s.mean = sum / static_cast<double>(c.size());
    s.span = c.back().usedFilament - c.front().usedFilament;
    return s;
}

} // namespace

std::vector<TemperaturePoint> extractTemperatureCurve(std::istream& in)
{
    std::vector<TemperaturePoint> curve;
    std::string buffer;
    double usedFilament = 0.0;

    while (std::getline(in, buffer)) {
        std::string_view line{buffer};
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        // Accumulate before recording, so a command is attributed to the filament
        // position reached by the moves preceding it.
        if (startsWith(line, "G1") || startsWith(line, "G0")) {
            if (const auto e = detail::word(line, 'E'); e.has_value() && *e > 0.0) {
                usedFilament += *e;
            }
            continue;
        }

        if (startsWith(line, "M104") || startsWith(line, "M109")) {
            if (const auto s = detail::word(line, 'S'); s.has_value()) {
                curve.push_back(TemperaturePoint{usedFilament, *s});
            }
        }
    }
    return curve;
}

Celsius temperatureAt(const std::vector<TemperaturePoint>& curve,
                      Millimetres usedFilament) noexcept
{
    if (curve.empty()) {
        return 0.0;
    }
    if (usedFilament <= curve.front().usedFilament) {
        return curve.front().temperature;
    }

    // Last command at or before the requested position -- M104 is a step, not a ramp.
    const auto it = std::upper_bound(
        curve.begin(), curve.end(), usedFilament,
        [](Millimetres value, const TemperaturePoint& p) { return value < p.usedFilament; });

    return (it == curve.begin() ? curve.front() : *(it - 1)).temperature;
}

CurveComparison compareCurves(const std::vector<TemperaturePoint>& a,
                              const std::vector<TemperaturePoint>& b,
                              std::size_t samples)
{
    CurveComparison result;
    result.a = summarise(a);
    result.b = summarise(b);

    if (a.empty() || b.empty() || samples < 2) {
        return result;
    }

    // Compare only where both curves have data. Beyond that we would be comparing a real
    // value against a held-constant endpoint, which flatters the correlation.
    const double lo = std::max(a.front().usedFilament, b.front().usedFilament);
    const double hi = std::min(a.back().usedFilament, b.back().usedFilament);
    if (!(hi > lo)) {
        return result;
    }

    std::vector<double> sa;
    std::vector<double> sb;
    sa.reserve(samples);
    sb.reserve(samples);

    for (std::size_t i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(samples - 1);
        const double x = lo + (hi - lo) * t;
        sa.push_back(temperatureAt(a, x));
        sb.push_back(temperatureAt(b, x));
    }

    const auto n = static_cast<double>(samples);
    const double meanA = std::accumulate(sa.begin(), sa.end(), 0.0) / n;
    const double meanB = std::accumulate(sb.begin(), sb.end(), 0.0) / n;
    result.meanOffset = meanA - meanB;

    double covariance = 0.0;
    double varianceA = 0.0;
    double varianceB = 0.0;
    double squaredError = 0.0;
    double worst = 0.0;

    for (std::size_t i = 0; i < samples; ++i) {
        const double da = sa[i] - meanA;
        const double db = sb[i] - meanB;
        covariance += da * db;
        varianceA += da * da;
        varianceB += db * db;

        // Mean-removed, so a pure calibration offset does not count as disagreement.
        const double diff = da - db;
        squaredError += diff * diff;
        worst = std::max(worst, std::abs(diff));
    }

    result.samples = samples;
    result.rmsDifference = std::sqrt(squaredError / n);
    result.maxDifference = worst;

    // A flat curve has zero variance and no meaningful correlation; report 0 rather than
    // dividing by zero and producing NaN.
    const double denominator = std::sqrt(varianceA * varianceB);
    result.correlation = denominator > 0.0 ? covariance / denominator : 0.0;

    return result;
}

} // namespace sb53
