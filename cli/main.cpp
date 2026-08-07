// sb53-cli — the headless frontend.
//
// This is not a development convenience. It is simultaneously the test driver, the
// OrcaSlicer post-processing script (the tool's primary real-world use), and the process
// a future web backend will invoke. It reaches full capability at milestone M6, before
// the Qt GUI starts at M8. See ADR-0002.

#include "sb53/Diagnostics.hpp"
#include "sb53/FlowAnalysis.hpp"
#include "sb53/GcodeScanner.hpp"
#include "sb53/SubprocessRunner.hpp"
#include "sb53/Version.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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
        "  sb53 analyze <input.gcode> [--estimator P]  flow analysis, change nothing\n"
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

// Locates the vendored estimator next to this executable, then in the repo's bin/.
std::filesystem::path findEstimator(const std::filesystem::path& exeDir)
{
    constexpr std::string_view name = "klipper_estimator.exe";
    for (const auto& candidate : {exeDir / name,
                                  exeDir / ".." / ".." / "bin" / name,
                                  exeDir / ".." / ".." / ".." / "bin" / name}) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return std::filesystem::canonical(candidate, ec);
        }
    }
    return {};
}

// `analyze` runs the real estimator and reports the flow timeline. It writes nothing,
// so it is safe to point at any file. This is the first command that exercises the
// IProcessRunner seam against a live subprocess.
int runAnalyze(std::string_view path, std::filesystem::path estimator,
               const std::filesystem::path& exeDir)
{
    if (estimator.empty()) {
        estimator = findEstimator(exeDir);
    }
    if (estimator.empty()) {
        std::fprintf(stderr,
                     "error: [%s] could not find klipper_estimator.exe.\n"
                     "Pass --estimator <path>.\n",
                     sb53::toString(sb53::Code::EstimatorNotFound).data());
        return 1;
    }

    const auto configPath = estimator.parent_path() / "config.json";
    std::error_code ec;
    if (!std::filesystem::exists(configPath, ec)) {
        std::fprintf(stderr,
                     "error: [%s] no config.json beside the estimator (%s).\n",
                     sb53::toString(sb53::Code::PrinterConfigMissing).data(),
                     configPath.string().c_str());
        return 1;
    }

    std::printf("estimator : %s\n", estimator.string().c_str());
    std::printf("running   : dump-moves ...\n");

    sb53::SubprocessRunner runner;
    const auto proc = runner.run(
        estimator,
        {"--config_file", configPath.string(), "dump-moves", std::string(path)},
        std::chrono::minutes{5});

    if (proc.launchFailed) {
        std::fprintf(stderr, "error: [%s] %s\n",
                     sb53::toString(sb53::Code::EstimatorNotFound).data(),
                     proc.stdErr.c_str());
        return 1;
    }
    if (proc.timedOut) {
        std::fprintf(stderr, "error: [%s] the estimator timed out.\n",
                     sb53::toString(sb53::Code::EstimatorFailed).data());
        return 1;
    }
    if (proc.exitCode != 0) {
        // The legacy could never show this, because it redirected through a shell.
        std::fprintf(stderr, "error: [%s] estimator exited %d\n%s\n",
                     sb53::toString(sb53::Code::EstimatorFailed).data(),
                     proc.exitCode, proc.stdErr.c_str());
        return 1;
    }

    std::istringstream dump{proc.stdOut};
    sb53::DiagnosticList diags;
    const auto moves = sb53::MoveDumpParser::parse(dump, diags);
    report(diags);
    if (diags.hasErrors()) {
        return 1;
    }

    const auto a = sb53::analyseFlow(moves);

    const auto hours = static_cast<int>(a.totalTime) / 3600;
    const auto minutes = (static_cast<int>(a.totalTime) % 3600) / 60;
    const auto seconds = static_cast<int>(a.totalTime) % 60;

    std::printf("\nmoves          : %zu\n", a.moves.size());
    std::printf("estimated time : %dh %dm %ds\n", hours, minutes, seconds);
    std::printf("filament used  : %.2f mm\n", a.totalFilament);
    std::printf("peak flow      : %.2f mm3/s\n", a.peakFlow);
    std::printf("timeline       : %zu one-second buckets\n", a.seconds.size());

    if (!a.seconds.empty()) {
        double sum = 0.0;
        double peakAverage = 0.0;
        for (const auto& s : a.seconds) {
            sum += s.averageFlow;
            peakAverage = std::max(peakAverage, s.averageFlow);
        }
        std::printf("mean flow      : %.2f mm3/s\n",
                    sum / static_cast<double>(a.seconds.size()));
        std::printf("peak 1s average: %.2f mm3/s\n", peakAverage);

        // A coarse profile, so the shape is visible without a chart.
        std::printf("\nflow over time (each row is one second, sampled):\n");
        const std::size_t step = std::max<std::size_t>(1, a.seconds.size() / 20);
        for (std::size_t i = 0; i < a.seconds.size(); i += step) {
            const auto& s = a.seconds[i];
            const int bars = peakAverage > 0.0
                ? static_cast<int>((s.averageFlow / peakAverage) * 40.0)
                : 0;
            std::printf("  %5.0fs %6.2f %s\n", s.time, s.averageFlow,
                        std::string(static_cast<std::size_t>(bars), '#').c_str());
        }
    }

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

    if (args[0] == "scan") {
        if (args.size() < 2) {
            std::fprintf(stderr, "error: scan requires an input file\n");
            return 64;
        }
        return runScan(args[1]);
    }

    if (args[0] == "analyze" || args[0] == "analyse") {
        if (args.size() < 2) {
            std::fprintf(stderr, "error: analyze requires an input file\n");
            return 64;
        }
        std::filesystem::path estimator;
        for (std::size_t i = 2; i + 1 < args.size(); ++i) {
            if (args[i] == "--estimator") {
                estimator = std::string(args[i + 1]);
            }
        }
        std::error_code ec;
        const auto exeDir =
            std::filesystem::absolute(std::filesystem::path(argv[0]), ec).parent_path();
        return runAnalyze(args[1], estimator, exeDir);
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
