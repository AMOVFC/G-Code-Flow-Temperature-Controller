// sb53-cli — the headless frontend.
//
// This is not a development convenience. It is simultaneously the test driver, the
// OrcaSlicer post-processing script (the tool's primary real-world use), and the process
// a future web backend will invoke. It reaches full capability at milestone M6, before
// the Qt GUI starts at M8. See ADR-0002.

#include "sb53/Diagnostics.hpp"
#include "sb53/Version.hpp"

#include <cstdio>
#include <string_view>
#include <vector>

namespace {

int printUsage()
{
    std::printf(
        "%s %s\n"
        "\n"
        "Usage:\n"
        "  sb53 process <input.gcode> [--out <path>]   process a G-code file\n"
        "  sb53 --version                              print version and exit\n"
        "  sb53 --help                                 print this message\n"
        "\n"
        "When --out is omitted the input file is overwritten in place, which is what\n"
        "a slicer post-processing hook expects.\n",
        sb53::kProductName.data(), sb53::kVersion.data());
    return 0;
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

    if (args[0] == "process") {
        // Milestone M6. The pipeline stages land in M2-M5; this is deliberately a stub
        // rather than a partial implementation, so that "not built yet" cannot be
        // mistaken for "built and misbehaving".
        std::fprintf(stderr, "error: not implemented yet (milestone M6)\n");
        std::fprintf(stderr, "see docs/STATE.md for current progress\n");
        return 2;
    }

    std::fprintf(stderr, "error: unknown command '%s'\n\n", args[0].data());
    printUsage();
    return 64;   // EX_USAGE
}
