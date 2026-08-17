#include "sb53/Model.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace sb53;

namespace {

// A three-second plan with strictly increasing filament use, so lookups are
// unambiguous.
struct Fixture {
    std::vector<FlowSecond> seconds;
    TemperaturePlan         plan;

    Fixture()
    {
        seconds = {
            FlowSecond{1.0, 5.0, 6.0, 10.0},
            FlowSecond{2.0, 8.0, 9.0, 25.0},
            FlowSecond{3.0, 3.0, 4.0, 30.0},
        };
        plan.achievableTemperature = {200.0, 220.0, 205.0};
    }
};

} // namespace

TEST_CASE("pressure advance is permitted only in concealed features", "[model][pa]")
{
    // ALGORITHM.md §8. This is a documented correctness requirement rather than a
    // heuristic: changing pressure advance in a visible wall can produce surface
    // bubbles. If this test is ever "fixed" by widening the set, read the spec first.

    SECTION("permitted") {
        CHECK(allowsPressureAdvanceChange(FeatureType::SparseInfill));
        CHECK(allowsPressureAdvanceChange(FeatureType::InternalSolidInfill));
        CHECK(allowsPressureAdvanceChange(FeatureType::InternalBridge));
        CHECK(allowsPressureAdvanceChange(FeatureType::Support));
    }

    SECTION("not permitted — visible surfaces") {
        CHECK_FALSE(allowsPressureAdvanceChange(FeatureType::OuterWall));
        CHECK_FALSE(allowsPressureAdvanceChange(FeatureType::InnerWall));
        CHECK_FALSE(allowsPressureAdvanceChange(FeatureType::TopSurface));
        CHECK_FALSE(allowsPressureAdvanceChange(FeatureType::Overhang));
        CHECK_FALSE(allowsPressureAdvanceChange(FeatureType::Bridge));
        CHECK_FALSE(allowsPressureAdvanceChange(FeatureType::Skirt));
    }

    SECTION("unknown features are treated conservatively") {
        // An unrecognised marker must not silently enable PA changes.
        CHECK_FALSE(allowsPressureAdvanceChange(FeatureType::Unknown));
    }
}

TEST_CASE("temperature lookup by filament position", "[model][plan]")
{
    // The plan is indexed by cumulative extruded filament because that coordinate is
    // invariant under speed rewriting. See ALGORITHM.md §6.
    //
    // Every boundary here returns an explicit value. The legacy equivalent returned
    // uninitialised memory for three of these cases (legacy/known-bugs.md #3), which is
    // exactly why they are tested individually.

    Fixture f;

    SECTION("exact bucket boundaries") {
        CHECK(f.plan.temperatureAtFilament(f.seconds, 10.0) == 200.0);
        CHECK(f.plan.temperatureAtFilament(f.seconds, 25.0) == 220.0);
        CHECK(f.plan.temperatureAtFilament(f.seconds, 30.0) == 205.0);
    }

    SECTION("between boundaries resolves to the covering bucket") {
        CHECK(f.plan.temperatureAtFilament(f.seconds, 15.0) == 220.0);
        CHECK(f.plan.temperatureAtFilament(f.seconds, 27.5) == 205.0);
    }

    SECTION("before the first sample") {
        // Printing has not reached flow-derived control yet; the first planned value
        // applies. The caller handles the initial-temperature case separately.
        CHECK(f.plan.temperatureAtFilament(f.seconds, 0.0) == 200.0);
        CHECK(f.plan.temperatureAtFilament(f.seconds, 5.0) == 200.0);
    }

    SECTION("past the end holds the final temperature") {
        // Must not extrapolate, and must not read out of bounds.
        CHECK(f.plan.temperatureAtFilament(f.seconds, 100.0) == 205.0);
    }

    SECTION("empty inputs yield no value rather than undefined behaviour") {
        const std::vector<FlowSecond> none;
        CHECK_FALSE(f.plan.temperatureAtFilament(none, 10.0).has_value());

        const TemperaturePlan emptyPlan;
        CHECK_FALSE(emptyPlan.temperatureAtFilament(f.seconds, 10.0).has_value());
    }

    SECTION("a truncated plan does not read past the shorter of the two") {
        // Defensive: a malformed plan should degrade, not corrupt memory.
        TemperaturePlan shortPlan;
        shortPlan.achievableTemperature = {200.0};
        const auto result = shortPlan.temperatureAtFilament(f.seconds, 100.0);
        REQUIRE(result.has_value());
        CHECK(*result == 200.0);
    }
}

TEST_CASE("plan size reflects the achievable curve", "[model][plan]")
{
    TemperaturePlan plan;
    CHECK(plan.empty());
    CHECK(plan.size() == 0);

    plan.achievableTemperature = {200.0, 210.0};
    CHECK_FALSE(plan.empty());
    CHECK(plan.size() == 2);
}
