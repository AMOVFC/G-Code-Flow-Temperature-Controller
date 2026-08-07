#include "sb53/TemperaturePlanner.hpp"

#include <algorithm>
#include <cmath>

namespace sb53 {
namespace {

[[nodiscard]] double lerp(double a, double b, double t) noexcept
{
    return a + (b - a) * t;
}

// Position of `value` within [lo, hi], clamped to 0..1. Caller must ensure hi > lo.
[[nodiscard]] double fraction(double value, double lo, double hi) noexcept
{
    return std::clamp((value - lo) / (hi - lo), 0.0, 1.0);
}

} // namespace

// ---------------------------------------------------------------------------
// Profile validation
// ---------------------------------------------------------------------------

double FilamentProfile::biasFraction() const noexcept
{
    return std::clamp(static_cast<double>(speedQualityBias) / 10.0, 0.0, 1.0);
}

void FilamentProfile::validate(DiagnosticList& diagnostics) const
{
    // Strictly increasing, not merely non-decreasing: temperatureToFlow divides by the
    // segment widths, so equal values are a division by zero. The legacy has this latent
    // bug and only avoids it because its spinner minimums happen to differ
    // (ALGORITHM.md §3).
    if (!(lowFlow < midFlow && midFlow < highFlow)) {
        diagnostics.add(error(
            Code::FlowPointsNotIncreasing,
            "Filament flow calibration points must be strictly increasing "
            "(low < mid < high). Got " +
                std::to_string(lowFlow) + ", " + std::to_string(midFlow) + ", " +
                std::to_string(highFlow) + " mm3/s."));
    }

    // Temperatures need not be strictly increasing -- a filament could plausibly want the
    // same temperature at two calibration points -- but decreasing would invert the
    // model, which is almost certainly a data-entry error.
    if (lowTemp > midTemp || midTemp > highTemp) {
        diagnostics.add(error(
            Code::TemperaturePointsNotMonotonic,
            "Filament temperature calibration points must not decrease as flow "
            "increases. Got " + std::to_string(lowTemp) + ", " +
                std::to_string(midTemp) + ", " + std::to_string(highTemp) + " C."));
    }

    if (speedQualityBias < 0 || speedQualityBias > 10) {
        diagnostics.add(warning(
            Code::ProfileInvalid,
            "Speed/quality bias should be between 0 and 10; clamping."));
    }
}

void ExtruderProfile::validate(DiagnosticList& diagnostics) const
{
    if (riseRatePerSecond() <= 0.0 || fallRatePerSecond() <= 0.0) {
        diagnostics.add(error(
            Code::ProfileInvalid,
            "Extruder heating and cooling rates must be positive. A zero rate would "
            "freeze the temperature plan at its starting value."));
    }
    if (smoothingWindow < 1) {
        diagnostics.add(warning(
            Code::ProfileInvalid,
            "Smoothing window must be at least 1; treating as 1 (no smoothing)."));
    }
}

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------

std::vector<double> movingAverage(std::span<const double> values, int window)
{
    std::vector<double> result(values.size());
    if (values.empty()) {
        return result;
    }

    const auto n = static_cast<std::ptrdiff_t>(values.size());
    const std::ptrdiff_t half = std::max(0, window) / 2;

    for (std::ptrdiff_t i = 0; i < n; ++i) {
        const std::ptrdiff_t first = std::max<std::ptrdiff_t>(0, i - half);
        const std::ptrdiff_t last = std::min<std::ptrdiff_t>(n - 1, i + half);

        double sum = 0.0;
        for (std::ptrdiff_t j = first; j <= last; ++j) {
            sum += values[static_cast<std::size_t>(j)];
        }
        // Divide by the count actually summed. Using the nominal window instead would
        // drag the first and last samples toward zero.
        const auto count = static_cast<double>(last - first + 1);
        result[static_cast<std::size_t>(i)] = sum / count;
    }
    return result;
}

Celsius flowToTemperature(const FilamentProfile& f, CubicMmPerSec flow) noexcept
{
    if (flow <= f.lowFlow) {
        return f.lowTemp;
    }
    if (flow >= f.highFlow) {
        return f.highTemp;
    }
    if (flow <= f.midFlow) {
        return lerp(f.lowTemp, f.midTemp, fraction(flow, f.lowFlow, f.midFlow));
    }
    return lerp(f.midTemp, f.highTemp, fraction(flow, f.midFlow, f.highFlow));
}

CubicMmPerSec temperatureToFlow(const FilamentProfile& f, Celsius temperature) noexcept
{
    if (temperature <= f.lowTemp) {
        return f.lowFlow;
    }
    if (temperature >= f.highTemp) {
        return f.highFlow;
    }
    // Guard the degenerate case where two temperature points coincide: the mapping is
    // ambiguous there, so pick the lower segment's endpoint rather than dividing by zero.
    if (temperature <= f.midTemp) {
        if (f.midTemp <= f.lowTemp) {
            return f.lowFlow;
        }
        return lerp(f.lowFlow, f.midFlow, fraction(temperature, f.lowTemp, f.midTemp));
    }
    if (f.highTemp <= f.midTemp) {
        return f.midFlow;
    }
    return lerp(f.midFlow, f.highFlow, fraction(temperature, f.midTemp, f.highTemp));
}

double temperatureToPressureAdvance(const FilamentProfile& f, Celsius temperature) noexcept
{
    if (temperature <= f.lowTemp) {
        return f.lowPressureAdvance;
    }
    if (temperature >= f.highTemp) {
        return f.highPressureAdvance;
    }
    if (temperature <= f.midTemp) {
        if (f.midTemp <= f.lowTemp) {
            return f.lowPressureAdvance;
        }
        return lerp(f.lowPressureAdvance, f.midPressureAdvance,
                    fraction(temperature, f.lowTemp, f.midTemp));
    }
    if (f.highTemp <= f.midTemp) {
        return f.midPressureAdvance;
    }
    return lerp(f.midPressureAdvance, f.highPressureAdvance,
                fraction(temperature, f.midTemp, f.highTemp));
}

std::vector<Celsius> applySlewLimit(std::span<const Celsius> desired,
                                    double riseRatePerSecond,
                                    double fallRatePerSecond,
                                    Seconds bucketWidth)
{
    std::vector<Celsius> achievable(desired.size());
    if (desired.empty()) {
        return achievable;
    }

    const double maxRise = std::max(0.0, riseRatePerSecond) * bucketWidth;
    const double maxFall = std::max(0.0, fallRatePerSecond) * bucketWidth;

    // The print starts at whatever the first second asks for: the nozzle is brought to
    // temperature before printing begins, so there is no slew constraint on the first
    // sample.
    achievable[0] = desired[0];

    for (std::size_t i = 1; i < desired.size(); ++i) {
        const double previous = achievable[i - 1];
        const double delta = desired[i] - previous;

        if (delta > 0.0) {
            achievable[i] = previous + std::min(delta, maxRise);
        } else if (delta < 0.0) {
            achievable[i] = previous - std::min(-delta, maxFall);
        } else {
            achievable[i] = previous;
        }
    }
    return achievable;
}

// ---------------------------------------------------------------------------
// The planner
// ---------------------------------------------------------------------------

TemperaturePlan planTemperature(const SourceAnalysis& analysis,
                                const ExtruderProfile& extruder,
                                const FilamentProfile& filament,
                                DiagnosticList& diagnostics,
                                const PlannerOptions& options)
{
    TemperaturePlan plan;

    filament.validate(diagnostics);
    extruder.validate(diagnostics);
    if (diagnostics.hasErrors()) {
        return plan;   // a plan built on invalid calibration would be actively dangerous
    }

    if (analysis.seconds.empty()) {
        diagnostics.add(error(
            Code::NoExtrusionFound,
            "No extrusion was found in this file, so there is no flow to plan a "
            "temperature curve against."));
        return plan;
    }

    const std::size_t n = analysis.seconds.size();

    // --- 1. blend average and maximum by the speed/quality bias --------------
    //
    // Linear interpolation, chosen because it reproduces the documented semantics and
    // can be explained to a user: "halfway between the average and peak flow of each
    // second". The legacy's expression is undocumented and deliberately not copied
    // (ADR-0006).
    const double bias = filament.biasFraction();
    plan.blendedFlow.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        plan.blendedFlow[i] =
            lerp(analysis.seconds[i].averageFlow, analysis.seconds[i].maxFlow, bias);
    }

    // --- 2. smooth ----------------------------------------------------------
    plan.smoothedFlow = movingAverage(plan.blendedFlow,
                                      std::max(1, extruder.smoothingWindow));

    // --- 3. map flow to temperature -----------------------------------------
    plan.desiredTemperature.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        plan.desiredTemperature[i] = flowToTemperature(filament, plan.smoothedFlow[i]);
    }

    // --- 4. constrain to what the hotend can deliver -------------------------
    plan.achievableTemperature = applySlewLimit(plan.desiredTemperature,
                                                extruder.riseRatePerSecond(),
                                                extruder.fallRatePerSecond(),
                                                options.bucketWidth);

    plan.initialTemperature = plan.achievableTemperature.front();

    const auto [lo, hi] = std::minmax_element(plan.achievableTemperature.begin(),
                                              plan.achievableTemperature.end());
    plan.minTemperature = *lo;
    plan.maxTemperature = *hi;

    const auto [flo, fhi] = std::minmax_element(plan.smoothedFlow.begin(),
                                                plan.smoothedFlow.end());
    plan.minFlow = *flo;
    plan.maxFlow = *fhi;

    return plan;
}

} // namespace sb53
