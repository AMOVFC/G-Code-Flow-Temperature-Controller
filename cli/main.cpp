// sb53-cli — the headless frontend.
//
// This is not a development convenience. It is simultaneously the test driver, the
// OrcaSlicer post-processing script (the tool's primary real-world use), and the process
// a future web backend will invoke. It reaches full capability at milestone M6, before
// the Qt GUI starts at M8. See ADR-0002.

#include "sb53/Diagnostics.hpp"
#include "sb53/GcodeScanner.hpp"
#include "sb53/Version.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int printUsage()
{
    std::printf(
        "%s %s\n"
        "\n"
        "Usage:\n"
        "  sb53 scan <input.gcode>                     inspect a file, change nothing\n"
        "  sb53 process <input.gcode> [--out <path>]   process a G-code file\n"
        "  sb53 --version                              print version and exit\n"
        "  sb53 --help                                 print this message\n"
        "\n"
        "When --out is omitted the input file is overwritten in place, which is what\n"
        "a slicer post-processing hook expects.\n",
        sb53::kProductName.data(), sb53::kVersion.data());
    return 0;
}

void report(const sb53::DiagnosticList& diags)
{
    for (const auto& d : diags) {
        std::fprintf(d.isError() ? stderr : stdout, "%s: [%s] %s\n",
                     sb53::toString(d.severity).data(),
                     sb53::toString(d.code).data(),
                     d.message.c_str());
    }
}

const char* describe(sb53::ExtrusionMode m)
{
    switch (m) {
    case sb53::ExtrusionMode::Relative: return "relative (M83)";
    case sb53::ExtrusionMode::Absolute: return "absolute (M82)";
    case sb53::ExtrusionMode::Unknown:  return "unknown";
    }
    return "unknown";
}

// `scan` exists so the analysis stages can be exercised long before the full pipeline
// works. It reads the file and reports what it found; it never writes anything.
int runScan(std::string_view path)
{
    std::ifstream in{std::string(path), std::ios::binary};
    if (!in) {
        std::fprintf(stderr, "error: [%s] cannot open '%s'\n",
                     sb53::toString(sb53::Code::FileNotFound).data(), path.data());
        return 1;
    }

    sb53::DiagnosticList diags;
    const auto r = sb53::GcodeScanner::scan(in, diags);

    std::printf("file            : %s\n", path.data());
    std::printf("lines           : %zu\n", r.totalLines);
    std::printf("extrusion mode  : %s\n", describe(r.extrusionMode));
    std::printf("already processed: %s\n", r.alreadyProcessed ? "yes" : "no");

    if (r.hasPrintBody()) {
        std::printf("print body      : lines %zu-%zu (%zu lines)\n",
                    r.bodyFirstLine, r.bodyLastLine,
                    r.bodyLastLine - r.bodyFirstLine + 1);
    } else {
        std::printf("print body      : NOT FOUND\n");
    }

    std::printf("printer profile : %s\n",
                r.printerSettingsId.empty() ? "(none)" : r.printerSettingsId.c_str());
    std::printf("filament profile: %s\n",
                r.filamentSettingsId.empty() ? "(none)" : r.filamentSettingsId.c_str());
    std::printf("filament type   : %s\n",
                r.filamentType.empty() ? "(none)" : r.filamentType.c_str());

    if (!diags.empty()) {
        std::printf("\n");
        report(diags);
    }

    return diags.hasErrors() ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string_view> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        return printUsage();
    }

    if (args[0] == "--version") {
        std::printf("%s\n", sb53::kVersion.data());
        return 0;
    }

    if (args[0] == "scan") {
        if (args.size() < 2) {
            std::fprintf(stderr, "error: scan requires an input file\n");
            return 64;
        }
        return runScan(args[1]);
    }

    if (args[0] == "process") {
        // Milestone M6. The remaining pipeline stages land in M3-M5; this stays an
        // explicit stub rather than a partial implementation, so that "not built yet"
        // cannot be mistaken for "built and misbehaving".
        std::fprintf(stderr, "error: not implemented yet (milestone M6)\n");
        std::fprintf(stderr, "try 'sb53 scan <file>'; see docs/STATE.md for progress\n");
        return 2;
    }

    std::fprintf(stderr, "error: unknown command '%s'\n\n", args[0].data());
    printUsage();
    return 64;   // EX_USAGE
}
