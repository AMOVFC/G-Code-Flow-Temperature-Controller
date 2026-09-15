#include "sb53/Diagnostics.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace sb53;

TEST_CASE("diagnostics carry severity and a stable code", "[diagnostics]")
{
    const auto d = error(Code::AbsoluteExtrusionUnsupported,
                         "This file uses absolute extrusion (M82).");

    CHECK(d.severity == Severity::Error);
    CHECK(d.code == Code::AbsoluteExtrusionUnsupported);
    CHECK(d.isError());
    CHECK_FALSE(d.message.empty());

    CHECK_FALSE(info(Code::Unknown, "x").isError());
    CHECK_FALSE(warning(Code::Unknown, "x").isError());
}

TEST_CASE("a list reports whether any diagnostic is fatal", "[diagnostics]")
{
    DiagnosticList list;
    CHECK(list.empty());
    CHECK_FALSE(list.hasErrors());

    list.add(info(Code::Unknown, "starting"));
    list.add(warning(Code::ProfileNotFound, "falling back to defaults"));
    CHECK_FALSE(list.hasErrors());   // warnings alone must not halt processing

    list.add(error(Code::EstimatorFailed, "estimator exited non-zero"));
    CHECK(list.hasErrors());
    CHECK(list.items().size() == 3);
}

TEST_CASE("every code has a distinct stable name", "[diagnostics]")
{
    // These names appear in logs users paste into bug reports and may be switched on by
    // frontends, so collisions would be actively harmful. This test exists to catch a
    // missing `case` after someone adds an enumerator — the compiler warns about the
    // switch, but only if warnings are read.

    const Code all[] = {
        Code::Unknown,
        Code::FileNotFound, Code::FileUnreadable, Code::FileEmpty,
        Code::AbsoluteExtrusionUnsupported, Code::ExtrusionModeUnknown,
        Code::AlreadyProcessed,
        Code::PrintBodyNotFound,
        Code::ProfileNotFound, Code::ProfileInvalid,
        Code::FlowPointsNotIncreasing, Code::TemperaturePointsNotMonotonic,
        Code::PrinterConfigMissing, Code::PrinterConfigInvalid,
        Code::EstimatorNotFound, Code::EstimatorFailed,
        Code::EstimatorOutputUnparsable,
        Code::NoExtrusionFound, Code::OutputWriteFailed, Code::Cancelled,
    };

    std::set<std::string_view> seen;
    for (const Code c : all) {
        const auto name = toString(c);
        INFO("code name: " << name);
        CHECK_FALSE(name.empty());
        if (c != Code::Unknown) {
            // A code falling through to "unknown" means a missing switch case.
            CHECK(name != "unknown");
        }
        CHECK(seen.insert(name).second);
    }
}

TEST_CASE("severity names are stable", "[diagnostics]")
{
    CHECK(toString(Severity::Info) == "info");
    CHECK(toString(Severity::Warning) == "warning");
    CHECK(toString(Severity::Error) == "error");
}
