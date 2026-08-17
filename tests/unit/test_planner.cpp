#include "sb53/TemperaturePlanner.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace sb53;
using Catch::Approx;

namespace {

FilamentProfile testFilament()
{
    // The README's own worked example (ALGORITHM.md §3).
    FilamentProfile f;
    f.name = "test";
    f.type = "PLA";
    f.lowFlow = 1.0;   f.lowTemp = 190.0;
    f.midFlow = 15.0;  f.midTemp = 220.0;
    f.highFlow = 22.0; f.highTemp = 235.0;
    f.speedQualityBias = 5;
    return f;
}

ExtruderProfile testExtruder()
{
    ExtruderProfile e;
    e.name = "test";
    e.tempRise = 10.0; e.tempRiseTime = 1.0;   // 10 C/s heating
    e.tempFall = 2.0;  e.tempFallTime = 1.0;   //  2 C/s cooling -- deliberately slower
    e.smoothingWindow = 1;                     // disabled unless a test wants it
    return e;
}

SourceAnalysis timeline(const std::vector<std::pair<double, double>>& avgMax)
{
    SourceAnalysis a;
    double t = 0.0;
    double filament = 0.0;
    for (const auto& [avg, mx] : avgMax) {
        t += 1.0;
        filament += avg;   // monotonic; exact value irrelevant here
        a.seconds.push_back(FlowSecond{t, avg, mx, filament});
    }
    a.totalTime = t;
    return a;
}

} // namespace

TEST_CASE("flow to temperature is piecewise linear on three points", "[planner][map]")
{
    const auto f = testFilament();

    SECTION("the calibration points map exactly") {
        CHECK(flowToTemperature(f, 1.0) == Approx(190.0));
        CHECK(flowToTemperature(f, 15.0) == Approx(220.0));
        CHECK(flowToTemperature(f, 22.0) == Approx(235.0));
    }

    SECTION("interpolates linearly within each segment") {
        // Midway between 1 and 15 is 8, so midway between 190 and 220 is 205.
        CHECK(flowToTemperature(f, 8.0) == Approx(205.0));
        // Midway between 15 and 22 is 18.5, so midway between 220 and 235 is 227.5.
        CHECK(flowToTemperature(f, 18.5) == Approx(227.5));
    }

    SECTION("clamps outside the calibrated range rather than extrapolating") {
        // Extrapolating would command temperatures the user never validated -- on a
        // real hotend that is a burnt-filament or clog risk.
        CHECK(flowToTemperature(f, 0.0) == Approx(190.0));
        CHECK(flowToTemperature(f, -5.0) == Approx(190.0));
        CHECK(flowToTemperature(f, 1000.0) == Approx(235.0));
    }

    SECTION("the mapping is monotonic across the whole range") {
        double previous = -1.0;
        for (double flow = 0.0; flow <= 30.0; flow += 0.1) {
            const double t = flowToTemperature(f, flow);
            CHECK(t >= previous - 1e-9);
            previous = t;
        }
    }
}

TEST_CASE("temperature to flow inverts the mapping", "[planner][map]")
{
    const auto f = testFilament();

    CHECK(temperatureToFlow(f, 190.0) == Approx(1.0));
    CHECK(temperatureToFlow(f, 220.0) == Approx(15.0));
    CHECK(temperatureToFlow(f, 235.0) == Approx(22.0));
    CHECK(temperatureToFlow(f, 205.0) == Approx(8.0));

    SECTION("round-trips within the calibrated range") {
        for (double flow = 1.0; flow <= 22.0; flow += 0.5) {
            CHECK(temperatureToFlow(f, flowToTemperature(f, flow)) == Approx(flow));
        }
    }

    SECTION("degenerate temperature points do not divide by zero") {
        // Two calibration points at the same temperature make the inverse ambiguous.
        // It must return something sane rather than NaN or infinity.
        FilamentProfile flat = f;
        flat.midTemp = flat.lowTemp;
        const double result = temperatureToFlow(flat, flat.lowTemp + 0.5);
        CHECK(std::isfinite(result));
    }
}

TEST_CASE("moving average", "[planner][smooth]")
{
    SECTION("a window of 1 changes nothing") {
        const std::vector<double> v{1.0, 5.0, 2.0};
        const auto r = movingAverage(v, 1);
        CHECK(r == v);
    }

    SECTION("is centered, so it responds before a step arrives") {
        // This is what gives the hotend advance warning of a high-flow section. Because
        // we are post-processing rather than controlling in real time, that look-ahead
        // is free -- and it is why the slew limiter can be a simple causal filter.
        const std::vector<double> step{0.0, 0.0, 0.0, 10.0, 10.0, 10.0};
        const auto r = movingAverage(step, 3);
        CHECK(r[2] > 0.0);   // rises before the step at index 3
    }

    SECTION("end samples divide by the clamped count, not the nominal window") {
        // Dividing by the full window would drag the first and last values toward zero
        // and invent a cold start that the print does not have.
        const std::vector<double> flat(10, 6.0);
        const auto r = movingAverage(flat, 5);
        for (const double v : r) {
            CHECK(v == Approx(6.0));
        }
    }

    SECTION("preserves the mean of a constant signal at any window") {
        const std::vector<double> flat(50, 3.5);
        for (int w : {1, 2, 7, 20, 99}) {
            const auto r = movingAverage(flat, w);
            for (const double v : r) {
                CHECK(v == Approx(3.5));
            }
        }
    }

    SECTION("empty input") {
        CHECK(movingAverage({}, 5).empty());
    }
}

TEST_CASE("slew limiting respects hotend physics", "[planner][slew]")
{
    SECTION("heating is limited to the rise rate") {
        const std::vector<Celsius> desired{200.0, 250.0, 250.0, 250.0};
        const auto a = applySlewLimit(desired, 10.0, 2.0, 1.0);

        CHECK(a[0] == Approx(200.0));   // start is unconstrained
        CHECK(a[1] == Approx(210.0));   // +10 C in one second
        CHECK(a[2] == Approx(220.0));
        CHECK(a[3] == Approx(230.0));
    }

    SECTION("cooling is limited separately, and more tightly") {
        // Cooling is passive, so it is slower. Treating the two symmetrically would
        // command temperature drops the hotend cannot achieve.
        const std::vector<Celsius> desired{250.0, 200.0, 200.0, 200.0};
        const auto a = applySlewLimit(desired, 10.0, 2.0, 1.0);

        CHECK(a[1] == Approx(248.0));   // only -2 C in one second
        CHECK(a[2] == Approx(246.0));
        CHECK(a[3] == Approx(244.0));
    }

    SECTION("never overshoots the target") {
        const std::vector<Celsius> desired{200.0, 203.0, 203.0};
        const auto a = applySlewLimit(desired, 10.0, 10.0, 1.0);
        CHECK(a[1] == Approx(203.0));   // reachable in one step; does not go to 210
        CHECK(a[2] == Approx(203.0));
    }

    SECTION("the achievable curve never moves faster than the rates allow") {
        std::vector<Celsius> desired;
        for (int i = 0; i < 100; ++i) {
            desired.push_back(i % 2 == 0 ? 190.0 : 240.0);   // worst-case sawtooth
        }
        const auto a = applySlewLimit(desired, 5.0, 3.0, 1.0);

        for (std::size_t i = 1; i < a.size(); ++i) {
            const double delta = a[i] - a[i - 1];
            CHECK(delta <= 5.0 + 1e-9);
            CHECK(delta >= -3.0 - 1e-9);
        }
    }

    SECTION("a zero rate holds the temperature rather than producing NaN") {
        const std::vector<Celsius> desired{200.0, 250.0, 150.0};
        const auto a = applySlewLimit(desired, 0.0, 0.0, 1.0);
        CHECK(a[1] == Approx(200.0));
        CHECK(a[2] == Approx(200.0));
    }
}

TEST_CASE("speed/quality bias blends average and maximum flow", "[planner][blend]")
{
    auto extruder = testExtruder();
    auto filament = testFilament();
    const auto analysis = timeline({{5.0, 15.0}, {5.0, 15.0}, {5.0, 15.0}});

    SECTION("quality end tracks the average") {
        filament.speedQualityBias = 0;
        DiagnosticList d;
        const auto plan = planTemperature(analysis, extruder, filament, d);
        REQUIRE_FALSE(d.hasErrors());
        CHECK(plan.blendedFlow[0] == Approx(5.0));
    }

    SECTION("speed end tracks the maximum") {
        filament.speedQualityBias = 10;
        DiagnosticList d;
        const auto plan = planTemperature(analysis, extruder, filament, d);
        REQUIRE_FALSE(d.hasErrors());
        CHECK(plan.blendedFlow[0] == Approx(15.0));
    }

    SECTION("midpoint sits between them") {
        filament.speedQualityBias = 5;
        DiagnosticList d;
        const auto plan = planTemperature(analysis, extruder, filament, d);
        CHECK(plan.blendedFlow[0] == Approx(10.0));
    }

    SECTION("more speed bias never lowers the planned temperature") {
        // The documented semantics: bias toward Speed means higher sustained flow and a
        // hotter nozzle. If this ever inverts, the control is backwards.
        double previous = 0.0;
        for (int bias = 0; bias <= 10; ++bias) {
            filament.speedQualityBias = bias;
            DiagnosticList d;
            const auto plan = planTemperature(analysis, extruder, filament, d);
            CHECK(plan.maxTemperature >= previous - 1e-9);
            previous = plan.maxTemperature;
        }
    }
}

TEST_CASE("planner rejects invalid calibration rather than guessing", "[planner][validate]")
{
    const auto analysis = timeline({{5.0, 10.0}, {5.0, 10.0}});

    SECTION("flow points must be strictly increasing") {
        // temperatureToFlow divides by segment width, so equal points are a division by
        // zero. The legacy has this latent bug (ALGORITHM.md §3).
        auto filament = testFilament();
        filament.midFlow = filament.lowFlow;

        DiagnosticList d;
        const auto plan = planTemperature(analysis, testExtruder(), filament, d);
        CHECK(d.hasErrors());
        CHECK(plan.empty());
    }

    SECTION("temperatures must not decrease as flow rises") {
        auto filament = testFilament();
        filament.highTemp = 150.0;

        DiagnosticList d;
        const auto plan = planTemperature(analysis, testExtruder(), filament, d);
        CHECK(d.hasErrors());
    }

    SECTION("an empty timeline is an error, not an empty plan") {
        DiagnosticList d;
        const auto plan = planTemperature(SourceAnalysis{}, testExtruder(),
                                          testFilament(), d);
        CHECK(d.hasErrors());
        CHECK(plan.empty());
    }
}

TEST_CASE("planned temperature stays inside the calibrated band", "[planner][safety]")
{
    // The single most important safety property: whatever the flow does, we never
    // command a temperature the user did not calibrate for.
    auto filament = testFilament();
    auto extruder = testExtruder();
    extruder.smoothingWindow = 10;

    std::vector<std::pair<double, double>> wild;
    for (int i = 0; i < 200; ++i) {
        wild.emplace_back(i % 3 == 0 ? 0.0 : 40.0, i % 2 == 0 ? 60.0 : 0.5);
    }

    DiagnosticList d;
    const auto plan = planTemperature(timeline(wild), extruder, filament, d);
    REQUIRE_FALSE(d.hasErrors());

    for (const double t : plan.achievableTemperature) {
        CHECK(t >= filament.lowTemp - 1e-9);
        CHECK(t <= filament.highTemp + 1e-9);
    }
    CHECK(plan.minTemperature >= filament.lowTemp - 1e-9);
    CHECK(plan.maxTemperature <= filament.highTemp + 1e-9);
}

TEST_CASE("planner output is parallel to the flow timeline", "[planner]")
{
    const auto analysis = timeline({{1.0, 2.0}, {5.0, 8.0}, {12.0, 20.0}, {3.0, 4.0}});
    DiagnosticList d;
    const auto plan = planTemperature(analysis, testExtruder(), testFilament(), d);

    REQUIRE_FALSE(d.hasErrors());
    const auto n = analysis.seconds.size();
    CHECK(plan.blendedFlow.size() == n);
    CHECK(plan.smoothedFlow.size() == n);
    CHECK(plan.desiredTemperature.size() == n);
    CHECK(plan.achievableTemperature.size() == n);
    CHECK(plan.size() == n);

    // The lookup in Model.hpp indexes this by cumulative filament, so the two must line
    // up exactly.
    const auto t = plan.temperatureAtFilament(analysis.seconds,
                                              analysis.seconds.back().usedFilament);
    REQUIRE(t.has_value());
    CHECK(*t == Approx(plan.achievableTemperature.back()));
}
