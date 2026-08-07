#include "sb53/CurveCompare.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sstream>

using namespace sb53;
using Catch::Approx;

namespace {

std::vector<TemperaturePoint> extract(std::string_view gcode)
{
    std::istringstream in{std::string(gcode)};
    return extractTemperatureCurve(in);
}

// Builds a curve directly, for comparison tests that do not need G-code parsing.
std::vector<TemperaturePoint> curve(const std::vector<std::pair<double, double>>& pts)
{
    std::vector<TemperaturePoint> c;
    for (const auto& [f, t] : pts) {
        c.push_back(TemperaturePoint{f, t});
    }
    return c;
}

} // namespace

TEST_CASE("temperature curves are extracted against cumulative filament", "[compare]")
{
    // Filament is the x-axis because it is the only coordinate two differently-processed
    // versions of the same print share (ALGORITHM.md §6).
    const auto c = extract(
        "M104 S200\n"
        "G1 X10 Y10 E1.0\n"
        "G1 X20 Y20 E2.0\n"
        "M104 S210\n"
        "G1 X30 Y30 E1.5\n"
        "M104 S220\n");

    REQUIRE(c.size() == 3);
    CHECK(c[0].usedFilament == Approx(0.0));
    CHECK(c[0].temperature == Approx(200.0));
    CHECK(c[1].usedFilament == Approx(3.0));
    CHECK(c[1].temperature == Approx(210.0));
    CHECK(c[2].usedFilament == Approx(4.5));
    CHECK(c[2].temperature == Approx(220.0));
}

TEST_CASE("retractions do not advance the filament coordinate", "[compare]")
{
    // A retract is filament pulled back, not consumed. Counting it would drift the
    // coordinate and misalign the two curves.
    const auto c = extract(
        "G1 X10 E5.0\n"
        "G1 E-2 F1800\n"
        "G1 E2 F1800\n"
        "M104 S215\n");

    REQUIRE(c.size() == 1);
    CHECK(c[0].usedFilament == Approx(7.0));   // 5 + the 2 mm unretract, not the -2
}

TEST_CASE("M109 is treated as a temperature command too", "[compare]")
{
    const auto c = extract("M109 S250\nG1 X1 E1\nM104 S240\n");
    REQUIRE(c.size() == 2);
    CHECK(c[0].temperature == Approx(250.0));
}

TEST_CASE("temperature lookup is a step function", "[compare]")
{
    // M104 sets a value that holds until the next one; interpolating between them would
    // invent a ramp the printer never performed.
    const auto c = curve({{0.0, 200.0}, {10.0, 220.0}, {20.0, 210.0}});

    CHECK(temperatureAt(c, -5.0) == Approx(200.0));
    CHECK(temperatureAt(c, 0.0) == Approx(200.0));
    CHECK(temperatureAt(c, 9.9) == Approx(200.0));
    CHECK(temperatureAt(c, 10.0) == Approx(220.0));
    CHECK(temperatureAt(c, 15.0) == Approx(220.0));
    CHECK(temperatureAt(c, 100.0) == Approx(210.0));
    CHECK(temperatureAt({}, 5.0) == Approx(0.0));
}

TEST_CASE("curve comparison", "[compare]")
{
    SECTION("a curve is perfectly correlated with itself") {
        const auto c = curve({{0, 200}, {10, 230}, {20, 205}, {30, 240}, {40, 210}});
        const auto r = compareCurves(c, c);
        CHECK(r.correlation == Approx(1.0));
        CHECK(r.rmsDifference == Approx(0.0).margin(1e-9));
        CHECK(r.meanOffset == Approx(0.0).margin(1e-9));
    }

    SECTION("a constant offset does not count as disagreement") {
        // Different calibration profiles shift the whole curve. That is expected and
        // benign -- the shape is what must agree (ADR-0005).
        const auto a = curve({{0, 200}, {10, 230}, {20, 205}, {30, 240}});
        const auto b = curve({{0, 230}, {10, 260}, {20, 235}, {30, 270}});
        const auto r = compareCurves(a, b);

        CHECK(r.correlation == Approx(1.0));
        CHECK(r.rmsDifference == Approx(0.0).margin(1e-9));
        CHECK(r.meanOffset == Approx(-30.0).margin(1e-9));   // reported, not penalised
    }

    SECTION("an inverted curve is detected") {
        // This is the failure mode that matters: temperature responding the wrong way to
        // flow means the algorithm was misunderstood.
        const auto a = curve({{0, 200}, {10, 240}, {20, 200}, {30, 240}});
        const auto b = curve({{0, 240}, {10, 200}, {20, 240}, {30, 200}});
        const auto r = compareCurves(a, b);
        CHECK(r.correlation < 0.0);
    }

    SECTION("a flat curve yields zero correlation rather than NaN") {
        const auto flat = curve({{0, 220}, {10, 220}, {20, 220}});
        const auto varying = curve({{0, 200}, {10, 240}, {20, 210}});
        const auto r = compareCurves(flat, varying);
        CHECK(r.correlation == Approx(0.0));
        CHECK(std::isfinite(r.correlation));
    }

    SECTION("non-overlapping ranges are reported rather than compared") {
        const auto a = curve({{0, 200}, {10, 220}});
        const auto b = curve({{100, 200}, {110, 220}});
        const auto r = compareCurves(a, b);
        CHECK(r.samples == 0);   // caller must not read a meaningless correlation
    }

    SECTION("empty inputs") {
        CHECK(compareCurves({}, curve({{0, 200}})).samples == 0);
        CHECK(compareCurves(curve({{0, 200}}), {}).samples == 0);
    }

    SECTION("summary statistics") {
        const auto c = curve({{0, 200}, {10, 240}, {20, 220}});
        const auto r = compareCurves(c, c);
        CHECK(r.a.points == 3);
        CHECK(r.a.minimum == Approx(200.0));
        CHECK(r.a.maximum == Approx(240.0));
        CHECK(r.a.mean == Approx(220.0));
        CHECK(r.a.span == Approx(20.0));
    }
}
