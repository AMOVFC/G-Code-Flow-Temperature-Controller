#include "sb53/FlowAnalysis.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>

using namespace sb53;
using Catch::Approx;

namespace {

std::vector<MoveSample> parseText(std::string_view text, DiagnosticList& diags)
{
    std::istringstream in{std::string(text)};
    return MoveDumpParser::parse(in, diags);
}

} // namespace

TEST_CASE("move dump parsing", "[flow][parse]")
{
    SECTION("extruding moves") {
        DiagnosticList diags;
        const auto m = parseText(
            "Flow = Some(5.5)\nTime = 0.25\n"
            "Flow = Some(7.25)\nTime = 0.5\n", diags);

        REQUIRE(m.size() == 2);
        CHECK(m[0].flow == Approx(5.5));
        CHECK(m[0].duration == Approx(0.25));
        CHECK(m[1].flow == Approx(7.25));
        CHECK_FALSE(diags.hasErrors());
    }

    SECTION("travel moves carry no flow") {
        DiagnosticList diags;
        const auto m = parseText("Flow = None\nTime = 0.1513\n", diags);
        REQUIRE(m.size() == 1);
        CHECK(m[0].flow == 0.0);
        CHECK(m[0].duration == Approx(0.1513));
    }

    SECTION("retract and the matching unretract are both zeroed") {
        // Neither is real extrusion; counting either would inject a spike into the flow
        // signal and drag the temperature plan with it.
        DiagnosticList diags;
        const auto m = parseText(
            "Flow = Some(-56.29)\nTime = 0.0855\n"   // retract
            "Flow = Some(56.29)\nTime = 0.0855\n"    // unretract
            "Flow = Some(5.5)\nTime = 0.1\n",        // real extrusion
            diags);

        REQUIRE(m.size() == 3);
        CHECK(m[0].flow == 0.0);
        CHECK(m[1].flow == 0.0);
        CHECK(m[2].flow == Approx(5.5));
    }

    SECTION("travel between retract and unretract does not disarm the flag") {
        // This is the real-world sequence: retract, several travel moves, unretract.
        // If `None` cleared the flag, the unretract would be counted as a flow spike.
        DiagnosticList diags;
        const auto m = parseText(
            "Flow = Some(-56.29)\nTime = 0.08\n"
            "Flow = None\nTime = 0.15\n"
            "Flow = None\nTime = 0.21\n"
            "Flow = Some(56.29)\nTime = 0.08\n"
            "Flow = Some(5.5)\nTime = 0.1\n",
            diags);

        REQUIRE(m.size() == 5);
        CHECK(m[3].flow == 0.0);          // unretract still suppressed
        CHECK(m[4].flow == Approx(5.5));  // and normal extrusion resumes
    }

    SECTION("locale-independent parsing") {
        // G-code is always '.'-decimal. The legacy did string surgery based on the
        // system locale (known-bugs.md #5); std::from_chars ignores locale entirely.
        DiagnosticList diags;
        const auto m = parseText("Flow = Some(5.5804673)\nTime = 0.0437\n", diags);
        REQUIRE(m.size() == 1);
        CHECK(m[0].flow == Approx(5.5804673));
    }

    SECTION("empty input is an error, not an empty success") {
        DiagnosticList diags;
        const auto m = parseText("", diags);
        CHECK(m.empty());
        CHECK(diags.hasErrors());
    }

    SECTION("unrecognised lines are counted, not silently dropped") {
        DiagnosticList diags;
        const auto m = parseText("Flow = Some(1.0)\nTime = 0.1\nwhat is this\n", diags);
        CHECK(m.size() == 1);
        CHECK_FALSE(diags.hasErrors());   // a warning, not fatal
        CHECK_FALSE(diags.empty());
    }
}

TEST_CASE("flow aggregation into per-second buckets", "[flow][aggregate]")
{
    FlowAnalysisOptions opts;
    opts.bucketWidth = 1.0;

    SECTION("constant flow over exactly two seconds") {
        std::vector<MoveSample> moves(20, MoveSample{0.1, 6.0});
        const auto a = analyseFlow(moves, opts);

        REQUIRE(a.seconds.size() == 2);
        CHECK(a.totalTime == Approx(2.0));
        for (const auto& s : a.seconds) {
            CHECK(s.averageFlow == Approx(6.0));
            CHECK(s.maxFlow == Approx(6.0));
        }
    }

    SECTION("averaging is time-weighted, not per-move") {
        // A plain mean over moves would give (10+2)/2 = 6. Time-weighting gives
        // (10*0.1 + 2*0.9)/1.0 = 2.8, which is what actually happened physically.
        const std::vector<MoveSample> moves{{0.1, 10.0}, {0.9, 2.0}};
        const auto a = analyseFlow(moves, opts);

        REQUIRE(a.seconds.size() == 1);
        CHECK(a.seconds[0].averageFlow == Approx(2.8));
        CHECK(a.seconds[0].maxFlow == Approx(10.0));
    }

    SECTION("a long move is split across the buckets it spans") {
        // A single 2.5 s move must contribute to three buckets, not one.
        const std::vector<MoveSample> moves{{2.5, 4.0}};
        const auto a = analyseFlow(moves, opts);

        REQUIRE(a.seconds.size() == 3);
        CHECK(a.seconds[0].averageFlow == Approx(4.0));
        CHECK(a.seconds[1].averageFlow == Approx(4.0));
        CHECK(a.seconds[2].averageFlow == Approx(4.0));   // partial, still 4.0
        CHECK(a.totalTime == Approx(2.5));
    }

    SECTION("the trailing partial bucket divides by elapsed time, not bucket width") {
        // Otherwise the last second is understated in proportion to how partial it is.
        const std::vector<MoveSample> moves{{1.0, 5.0}, {0.25, 5.0}};
        const auto a = analyseFlow(moves, opts);

        REQUIRE(a.seconds.size() == 2);
        CHECK(a.seconds[1].averageFlow == Approx(5.0));
    }

    SECTION("cumulative filament is monotonic and matches volume extruded") {
        // usedFilament is the coordinate the temperature plan is indexed by
        // (ALGORITHM.md §6), so monotonicity is a correctness requirement, not a nicety.
        std::vector<MoveSample> moves(30, MoveSample{0.1, 6.0});
        const auto a = analyseFlow(moves, opts);

        double previous = -1.0;
        for (const auto& s : a.seconds) {
            CHECK(s.usedFilament > previous);
            previous = s.usedFilament;
        }

        // 3 s at 6 mm^3/s = 18 mm^3, over a 1.75 mm filament cross-section.
        const double expected = 18.0 / filamentCrossSection(1.75);
        CHECK(a.totalFilament == Approx(expected));
    }

    SECTION("travel moves consume time without consuming filament") {
        const std::vector<MoveSample> moves{{0.5, 0.0}, {0.5, 8.0}};
        const auto a = analyseFlow(moves, opts);

        REQUIRE(a.seconds.size() == 1);
        CHECK(a.seconds[0].averageFlow == Approx(4.0));   // half the second was idle
        CHECK(a.seconds[0].maxFlow == Approx(8.0));
        CHECK(a.totalFilament == Approx(4.0 / filamentCrossSection(1.75)));
    }

    SECTION("zero-duration moves are ignored") {
        const std::vector<MoveSample> moves{{0.0, 99.0}, {1.0, 5.0}};
        const auto a = analyseFlow(moves, opts);

        REQUIRE(a.seconds.size() == 1);
        CHECK(a.seconds[0].averageFlow == Approx(5.0));
    }

    SECTION("no moves yields no timeline rather than a spurious bucket") {
        const auto a = analyseFlow({}, opts);
        CHECK(a.seconds.empty());
        CHECK(a.totalTime == 0.0);
    }
}

TEST_CASE("filament cross-section", "[flow]")
{
    CHECK(filamentCrossSection(1.75) == Approx(std::numbers::pi * 0.875 * 0.875));
    CHECK(filamentCrossSection(2.85) == Approx(std::numbers::pi * 1.425 * 1.425));
}

TEST_CASE("analysis of a recorded estimator run", "[flow][fixture]")
{
    // Fed from a recording, so this test spawns no subprocess and does not depend on
    // the vendored estimator binary. See ADR-0002.
    const std::filesystem::path path =
        std::filesystem::path{SB53_TESTDATA_DIR} / "fixtures" / "movedump-basic.txt";
    std::ifstream in{path};
    REQUIRE(in);

    DiagnosticList diags;
    const auto moves = MoveDumpParser::parse(in, diags);

    INFO("diagnostics: " << (diags.empty() ? "none" : diags.items().front().message));
    CHECK_FALSE(diags.hasErrors());
    CHECK(moves.size() == 2000);      // 2000 `Time =` lines in the fixture

    const auto a = analyseFlow(moves);

    CHECK(a.totalTime > 0.0);
    CHECK(a.peakFlow > 0.0);
    CHECK_FALSE(a.seconds.empty());

    // Every bucket must be internally consistent and the filament trace monotonic.
    double previousFilament = -1.0;
    double previousTime = -1.0;
    for (const auto& s : a.seconds) {
        CHECK(s.averageFlow <= s.maxFlow + 1e-9);
        CHECK(s.averageFlow >= 0.0);
        CHECK(s.usedFilament >= previousFilament);
        CHECK(s.time > previousTime);
        previousFilament = s.usedFilament;
        previousTime = s.time;
    }

    // The opening retract/travel/unretract must not leak into the flow signal. The
    // recording starts with Flow = Some(-56.2916), three None travels, then
    // Some(56.2916). If the state machine were wrong, that 56.2916 would appear as the
    // peak. Anything below it is real extrusion -- this print genuinely peaks around
    // 33.5 mm^3/s, which is plausible for a high-flow 0.4 nozzle.
    CHECK(a.peakFlow < 56.0);
    CHECK(a.peakFlow > 0.0);

    // Stronger: no individual sample may carry the retract magnitude.
    for (const auto& m : moves) {
        CHECK(m.flow < 56.0);
        CHECK(m.flow >= 0.0);   // negatives must have been zeroed, never stored
    }
}
