#include "sb53/ProfileStore.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace sb53;
using Catch::Approx;

namespace {

SavedPrinter samplePrinter()
{
    SavedPrinter p;
    p.name = "AWD V0";
    p.rise = 5.0; p.fall = 1.0; p.smoothing = 20;
    p.maxVelocity = 2500; p.maxAcceleration = 250000;
    p.squareCornerVelocity = 50; p.zVelocity = 100; p.zAcceleration = 10000;

    SavedFilament f;
    f.name = "Elegoo HS PLA+";
    f.type = "PLA";
    f.lowFlow = 1; f.midFlow = 80; f.highFlow = 105;
    f.lowTemp = 220; f.midTemp = 280; f.highTemp = 310;
    f.bias = 7;
    p.filaments.push_back(f);
    return p;
}

} // namespace

TEST_CASE("profiles survive a round trip", "[profiles]")
{
    ProfileStore store;
    store.upsert(samplePrinter());

    DiagnosticList d;
    const auto restored = ProfileStore::fromJson(store.toJson(), d);
    REQUIRE_FALSE(d.hasErrors());
    REQUIRE(restored.printers().size() == 1);

    const auto& p = restored.printers().front();
    CHECK(p.name == "AWD V0");
    CHECK(p.rise == Approx(5.0));
    CHECK(p.smoothing == 20);
    CHECK(p.maxAcceleration == Approx(250000));
    CHECK(p.zVelocity == Approx(100));

    REQUIRE(p.filaments.size() == 1);
    const auto& f = p.filaments.front();
    CHECK(f.name == "Elegoo HS PLA+");
    CHECK(f.midFlow == Approx(80));
    CHECK(f.highTemp == Approx(310));
    CHECK(f.bias == 7);
}

TEST_CASE("a filament's name is not confused with its printer's", "[profiles]")
{
    // Both objects have a "name" key, and the printer's scalars are read from a range
    // that must stop before the filament list or it picks up the wrong one.
    ProfileStore store;
    store.upsert(samplePrinter());

    DiagnosticList d;
    const auto restored = ProfileStore::fromJson(store.toJson(), d);
    CHECK(restored.printers().front().name == "AWD V0");
    CHECK(restored.printers().front().filaments.front().name == "Elegoo HS PLA+");
}

TEST_CASE("several filaments under one printer", "[profiles]")
{
    // The point of the feature: change one value without re-entering the machine setup.
    auto printer = samplePrinter();
    auto hotter = printer.filaments.front();
    hotter.name = "Elegoo HS PLA+ (hot)";
    hotter.highTemp = 320;
    printer.filaments.push_back(hotter);

    ProfileStore store;
    store.upsert(printer);

    DiagnosticList d;
    const auto restored = ProfileStore::fromJson(store.toJson(), d);
    const auto& p = restored.printers().front();

    REQUIRE(p.filaments.size() == 2);
    CHECK(p.findFilament("Elegoo HS PLA+")->highTemp == Approx(310));
    CHECK(p.findFilament("Elegoo HS PLA+ (hot)")->highTemp == Approx(320));
    CHECK(p.findFilament("nope") == nullptr);

    // Machine settings are shared, not duplicated per filament.
    CHECK(p.maxAcceleration == Approx(250000));
}

TEST_CASE("upsert replaces by name rather than appending", "[profiles]")
{
    ProfileStore store;
    store.upsert(samplePrinter());

    auto edited = samplePrinter();
    edited.smoothing = 30;
    store.upsert(edited);

    REQUIRE(store.printers().size() == 1);
    CHECK(store.printers().front().smoothing == 30);
}

TEST_CASE("remove", "[profiles]")
{
    ProfileStore store;
    store.upsert(samplePrinter());
    CHECK(store.remove("AWD V0"));
    CHECK(store.printers().empty());
    CHECK_FALSE(store.remove("AWD V0"));
}

TEST_CASE("a missing or empty file is a first run, not an error", "[profiles]")
{
    DiagnosticList d;
    const auto empty = ProfileStore::fromJson("", d);
    CHECK(empty.printers().empty());
    CHECK_FALSE(d.hasErrors());

    const auto missing = ProfileStore::load("no-such-file-here.json", d);
    CHECK(missing.printers().empty());
    CHECK_FALSE(d.hasErrors());
}

TEST_CASE("unknown keys and missing fields keep defaults", "[profiles]")
{
    // An older or newer profiles.json must still open rather than failing the load.
    DiagnosticList d;
    const auto s = ProfileStore::fromJson(
        R"({"printers":[{"name":"P","somethingNew":42,"filaments":[{"name":"F"}]}]})", d);

    REQUIRE(s.printers().size() == 1);
    CHECK(s.printers().front().name == "P");
    CHECK(s.printers().front().rise == Approx(5.0));      // default preserved
    CHECK(s.printers().front().filaments.front().bias == 7);
}

TEST_CASE("names containing actual control characters survive", "[profiles]")
{
    // The previous test covers literal backslashes (path separators) and embedded
    // quotes, but never exercised the \n/\t/\r escape-sequence branches in the reader --
    // a real coverage gap, since a name with those characters had never been round
    // tripped. toJson()'s quote() writes them escaped; fromJson() must read them back as
    // the original control character, not as the two-character escape sequence itself.
    auto p = samplePrinter();
    p.name = "line one\nline two\tindented\r\nwindows-style";
    ProfileStore store;
    store.upsert(p);

    const std::string json = store.toJson();
    // Confirm the writer actually escaped it -- otherwise this test would trivially pass
    // by both sides being wrong in the same way.
    CHECK(json.find("line one\nline two") == std::string::npos);
    CHECK(json.find("\\n") != std::string::npos);

    DiagnosticList d;
    const auto restored = ProfileStore::fromJson(json, d);
    REQUIRE(restored.printers().size() == 1);
    CHECK(restored.printers().front().name == p.name);
}

TEST_CASE("names containing quotes and backslashes survive", "[profiles]")
{
    // OrcaSlicer writes filament ids wrapped in quotes, and Windows paths are full of
    // backslashes, so both reach this code in practice.
    auto p = samplePrinter();
    p.name = R"(C:\printers\"odd" name)";
    ProfileStore store;
    store.upsert(p);

    DiagnosticList d;
    const auto restored = ProfileStore::fromJson(store.toJson(), d);
    REQUIRE(restored.printers().size() == 1);
    CHECK(restored.printers().front().name == R"(C:\printers\"odd" name)");
}
