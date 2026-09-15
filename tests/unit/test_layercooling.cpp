#include "sb53/GcodeScanner.hpp"
#include "sb53/TemperaturePlanner.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sstream>

using namespace sb53;
using Catch::Approx;

namespace {

std::vector<FlowSecond> timeline(std::size_t seconds, double filamentPerSecond)
{
    std::vector<FlowSecond> t;
    for (std::size_t i = 1; i <= seconds; ++i) {
        t.push_back(FlowSecond{static_cast<double>(i), 5.0, 6.0,
                               filamentPerSecond * static_cast<double>(i)});
    }
    return t;
}

FilamentProfile testFilament()
{
    FilamentProfile f;
    f.lowFlow = 1.0;   f.lowTemp = 200.0;
    f.midFlow = 10.0;  f.midTemp = 230.0;
    f.highFlow = 20.0; f.highTemp = 250.0;
    f.speedQualityBias = 5;
    return f;
}

ExtruderProfile testExtruder()
{
    ExtruderProfile e;
    e.tempRise = 100.0; e.tempRiseTime = 1.0;   // effectively unlimited, so the
    e.tempFall = 100.0; e.tempFallTime = 1.0;   // cooling drop is visible undamped
    e.smoothingWindow = 1;
    return e;
}

} // namespace

TEST_CASE("cooling drop ramps with layer time", "[cooling]")
{
    // Zero at or above the threshold, full drop at zero time, linear between.
    CHECK(coolingDropForLayerTime(20.0, 15.0, 10.0) == Approx(0.0));
    CHECK(coolingDropForLayerTime(15.0, 15.0, 10.0) == Approx(0.0));
    CHECK(coolingDropForLayerTime(7.5, 15.0, 10.0) == Approx(5.0));
    CHECK(coolingDropForLayerTime(3.0, 15.0, 10.0) == Approx(8.0));
    CHECK(coolingDropForLayerTime(0.0, 15.0, 10.0) == Approx(10.0));

    SECTION("monotonic: a shorter layer never gets less cooling") {
        double previous = 0.0;
        for (double t = 15.0; t >= 0.0; t -= 0.5) {
            const double drop = coolingDropForLayerTime(t, 15.0, 10.0);
            CHECK(drop >= previous - 1e-9);
            previous = drop;
        }
    }

    SECTION("disabled configurations produce no drop") {
        CHECK(coolingDropForLayerTime(1.0, 15.0, 0.0) == Approx(0.0));
        CHECK(coolingDropForLayerTime(1.0, 0.0, 10.0) == Approx(0.0));
        CHECK(coolingDropForLayerTime(-5.0, 15.0, 10.0) == Approx(10.0));
    }
}

TEST_CASE("layer durations come from the filament-to-time relationship", "[cooling]")
{
    // 10 seconds, 2 mm of filament per second.
    const auto seconds = timeline(10, 2.0);

    // Layers starting at 0, 8 and 16 mm -> 0 s, 4 s and 8 s.
    const std::vector<LayerMark> layers{{0.0, 0.2, 1}, {8.0, 0.2, 2}, {16.0, 0.2, 3}};
    const auto durations = computeLayerDurations(layers, seconds);

    REQUIRE(durations.size() == 3);
    CHECK(durations[0] == Approx(4.0));
    CHECK(durations[1] == Approx(4.0));
    CHECK(durations[2] == Approx(2.0));   // runs to the end of the timeline

    SECTION("durations are never negative") {
        for (const double d : durations) {
            CHECK(d >= 0.0);
        }
    }

    SECTION("empty inputs are handled") {
        CHECK(computeLayerDurations({}, seconds).empty());
        CHECK(computeLayerDurations(layers, {}).size() == 3);
    }
}

TEST_CASE("cooling profile assigns each second its own layer's drop", "[cooling]")
{
    const auto seconds = timeline(10, 2.0);

    // First layer 4 s (fast -> cooled), second 6 s... construct so they differ.
    const std::vector<LayerMark> layers{{0.0, 0.2, 1}, {8.0, 0.2, 2}};
    const auto drops = layerCoolingProfile(layers, seconds, 10.0, 20.0);

    REQUIRE(drops.size() == seconds.size());

    // The timeline reaches 2 mm, 4 mm, 6 mm, 8 mm ... at seconds 1, 2, 3, 4.
    // Layer 1 starts at 8 mm, so index 3 is already the FIRST second of layer 1 --
    // the boundary is inclusive.
    //
    // Layer 0 spans 0-8 mm = 4 s -> drop = 20 * (1 - 4/10) = 12.
    CHECK(drops[0] == Approx(12.0));
    CHECK(drops[2] == Approx(12.0));
    // Layer 1 spans 8 mm to the end = 6 s -> drop = 20 * (1 - 6/10) = 8.
    CHECK(drops[3] == Approx(8.0));
    CHECK(drops.back() == Approx(8.0));

    SECTION("no layers means no cooling, not a crash") {
        CHECK(layerCoolingProfile({}, seconds, 10.0, 20.0).size() == seconds.size());
        for (const double d : layerCoolingProfile({}, seconds, 10.0, 20.0)) {
            CHECK(d == Approx(0.0));
        }
    }
}

TEST_CASE("layer cooling lowers temperature on fast layers", "[cooling][planner]")
{
    SourceAnalysis analysis;
    analysis.seconds = timeline(10, 2.0);

    // Two layers: the first very fast (2 s), the second slow (8 s).
    const std::vector<LayerMark> layers{{0.0, 0.2, 1}, {4.0, 0.2, 2}};

    auto extruder = testExtruder();
    const auto filament = testFilament();

    DiagnosticList without;
    const auto baseline = planTemperature(analysis, extruder, filament, without, {}, layers);
    REQUIRE_FALSE(without.hasErrors());

    extruder.coolingLayerTime = 10.0;
    extruder.coolingMaxDrop = 15.0;

    DiagnosticList with;
    const auto cooled = planTemperature(analysis, extruder, filament, with, {}, layers);
    REQUIRE_FALSE(with.hasErrors());

    SECTION("the fast layer runs cooler than it would otherwise") {
        CHECK(cooled.achievableTemperature.front() <
              baseline.achievableTemperature.front());
    }

    SECTION("the slow layer is cooled less than the fast one") {
        REQUIRE(cooled.layerCoolingDrop.size() == analysis.seconds.size());
        CHECK(cooled.layerCoolingDrop.front() > cooled.layerCoolingDrop.back());
    }

    SECTION("cooling never goes below the calibrated floor") {
        // The user has validated nothing below lowTemp; a big drop must clamp, not
        // extrapolate into untested territory.
        extruder.coolingMaxDrop = 500.0;
        DiagnosticList d;
        const auto extreme = planTemperature(analysis, extruder, filament, d, {}, layers);
        for (const double t : extreme.achievableTemperature) {
            CHECK(t >= filament.lowTemp - 1e-9);
        }
    }

    SECTION("disabled by default") {
        CHECK(baseline.layerCoolingDrop.empty());
    }

    SECTION("enabling it without layer markers warns rather than silently doing nothing") {
        DiagnosticList d;
        const auto none = planTemperature(analysis, extruder, filament, d, {}, {});
        CHECK_FALSE(d.empty());
        CHECK_FALSE(d.hasErrors());   // a warning, not fatal
    }
}

TEST_CASE("the scanner records layer boundaries in filament space", "[cooling][scanner]")
{
    // Layer marks must use the SAME filament accounting as the rewriter, or the cooling
    // lands on the wrong part of the print.
    const std::string_view source =
        "M83\n"
        ";HEIGHT:0.2\n"          // layer 1 starts at 0 mm
        "G1 X10 Y10 E1.0\n"
        "G1 E-2 F1800\n"         // retract: must not count
        "G1 E2 F1800\n"          // unretract: must not count
        "G1 X20 Y20 E1.0\n"
        ";HEIGHT:0.2\n"          // layer 2 starts at 2 mm, not 4 mm
        "G1 X30 Y30 E1.5\n"
        "; EXECUTABLE_BLOCK_END\n";

    std::istringstream in{std::string(source)};
    DiagnosticList d;
    const auto scan = GcodeScanner::scan(in, d);

    REQUIRE(scan.layers.size() == 2);
    CHECK(scan.layers[0].usedFilament == Approx(0.0));
    CHECK(scan.layers[0].height == Approx(0.2));
    CHECK(scan.layers[1].usedFilament == Approx(2.0));
    CHECK(scan.bodyFilament == Approx(3.5));
}
