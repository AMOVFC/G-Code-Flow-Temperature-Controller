// sb53-cli â€” the headless frontend.
//
// This is not a development convenience. It is simultaneously the test driver, the
// OrcaSlicer post-processing script (the tool's primary real-world use), and the process
// a future web backend will invoke. It reaches full capability at milestone M6, before
// the Qt GUI starts at M8. See ADR-0002.

#include "sb53/CurveCompare.hpp"
#include "sb53/Diagnostics.hpp"
#include "sb53/FlowAnalysis.hpp"
#include "sb53/GcodeRewriter.hpp"
#include "sb53/GcodeScanner.hpp"
#include "sb53/Kinematics.hpp"
#include "sb53/SubprocessRunner.hpp"
#include "sb53/TemperaturePlanner.hpp"
#include "sb53/Version.hpp"

#include "WebUI.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Order matters: shellapi.h depends on types from windows.h and will not compile first.
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

// True when this process created its own console, i.e. it was started from Explorer
// rather than from an existing shell. A shell-launched process shares the shell's
// console, so more than one process is attached to it.
bool launchedByDoubleClick()
{
#ifdef _WIN32
    DWORD pids[4]{};
    return ::GetConsoleProcessList(pids, 4) == 1;
#else
    return false;
#endif
}

void openBrowser(const char* url)
{
#ifdef _WIN32
    ::ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)url;
#endif
}

int printUsage()
{
    std::printf(
        "%s %s\n"
        "\n"
        "Usage:\n"
        "  flowtemp serve [--port 8765]                    open the web interface\n"
        "  flowtemp scan <input.gcode>                     inspect a file, change nothing\n"
        "  flowtemp analyze <input.gcode>                  flow analysis, change nothing\n"
        "  flowtemp process <input.gcode> [--out <path>]   process a G-code file\n"
        "  flowtemp compare <a.gcode> <b.gcode>            compare temperature curves\n"
        "  flowtemp --version                              print version and exit\n"
        "  flowtemp --help                                 print this message\n"
        "\n"
        "klipper_estimator is run automatically -- you never invoke it yourself.\n"
        "It must sit beside a config.json describing your printer's motion limits.\n"
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
               const std::filesystem::path& exeDir,
               const sb53::ExtruderProfile& extruder,
               const sb53::FilamentProfile& filament)
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

    if (a.seconds.empty()) {
        return 0;
    }

    double sum = 0.0;
    double peakAverage = 0.0;
    for (const auto& s : a.seconds) {
        sum += s.averageFlow;
        peakAverage = std::max(peakAverage, s.averageFlow);
    }
    std::printf("mean flow      : %.2f mm3/s\n",
                sum / static_cast<double>(a.seconds.size()));
    std::printf("peak 1s average: %.2f mm3/s\n", peakAverage);

    // --- temperature plan ---------------------------------------------------
    //
    // Profiles are hard-coded defaults for now; the database repository lands with the
    // GUI at M8. Override the interesting knobs from the command line so the effect of
    // each can be seen without a rebuild.
    // Layer marks come from the scan, and layer-time cooling needs them.
    std::vector<sb53::LayerMark> layers;
    {
        std::ifstream in{std::string(path), std::ios::binary};
        sb53::DiagnosticList scanDiags;
        layers = sb53::GcodeScanner::scan(in, scanDiags).layers;
    }

    sb53::DiagnosticList planDiags;
    const auto plan = sb53::planTemperature(a, extruder, filament, planDiags, {}, layers);
    report(planDiags);
    if (planDiags.hasErrors()) {
        return 1;
    }

    std::printf("\nfilament       : %s  flow %.1f/%.1f/%.1f mm3/s -> %.0f/%.0f/%.0f C\n",
                filament.type.c_str(),
                filament.lowFlow, filament.midFlow, filament.highFlow,
                filament.lowTemp, filament.midTemp, filament.highTemp);
    std::printf("bias           : %d/10 (0=quality, 10=speed)   smoothing: %d s\n",
                filament.speedQualityBias, extruder.smoothingWindow);
    std::printf("hotend         : +%.1f C/s heating, -%.1f C/s cooling\n",
                extruder.riseRatePerSecond(), extruder.fallRatePerSecond());
    std::printf("\ninitial temp   : %.1f C\n", plan.initialTemperature);
    std::printf("planned range  : %.1f - %.1f C\n",
                plan.minTemperature, plan.maxTemperature);

    // A coarse profile, so the shape is visible without a chart. Flow and the resulting
    // temperature side by side is the whole point of the tool.
    std::printf("\n    time    flow  temp\n");
    const std::size_t step = std::max<std::size_t>(1, a.seconds.size() / 24);
    for (std::size_t i = 0; i < a.seconds.size(); i += step) {
        const int bars = peakAverage > 0.0
            ? static_cast<int>((a.seconds[i].averageFlow / peakAverage) * 34.0)
            : 0;
        std::printf("  %5.0fs %6.2f %5.1f %s\n",
                    a.seconds[i].time, a.seconds[i].averageFlow,
                    plan.achievableTemperature[i],
                    std::string(static_cast<std::size_t>(bars), '#').c_str());
    }

    return 0;
}

// Differential verification (ADR-0005). Compares the SHAPE of two temperature curves,
// not their values: calibration profiles may differ, the blend formula was deliberately
// changed, and several legacy defects are fixed. What must agree is whether temperature
// rises and falls at the same points in the print.
int runCompare(std::string_view pathA, std::string_view pathB)
{
    const auto load = [](std::string_view p, std::vector<sb53::TemperaturePoint>& out) {
        std::ifstream in{std::string(p), std::ios::binary};
        if (!in) {
            std::fprintf(stderr, "error: cannot open '%s'\n", std::string(p).c_str());
            return false;
        }
        out = sb53::extractTemperatureCurve(in);
        if (out.empty()) {
            std::fprintf(stderr,
                         "error: '%s' contains no temperature commands. If it carries a\n"
                         "       processed marker but no M104, it is a silent failure.\n",
                         std::string(p).c_str());
            return false;
        }
        return true;
    };

    std::vector<sb53::TemperaturePoint> a;
    std::vector<sb53::TemperaturePoint> b;
    if (!load(pathA, a) || !load(pathB, b)) {
        return 1;
    }

    const auto c = sb53::compareCurves(a, b);

    const auto show = [](const char* label, std::string_view path,
                         const sb53::CurveStats& s) {
        std::printf("%s %s\n", label, std::string(path).c_str());
        std::printf("     %zu commands, %.1f - %.1f C (mean %.1f), over %.0f mm filament\n",
                    s.points, s.minimum, s.maximum, s.mean, s.span);
    };
    show("A:", pathA, c.a);
    show("B:", pathB, c.b);

    if (c.samples == 0) {
        std::fprintf(stderr,
                     "\nerror: the two files share no overlapping filament range, so "
                     "they cannot be\n       compared. Are they the same print?\n");
        return 1;
    }

    std::printf("\ncomparison over %zu samples of the shared range:\n", c.samples);
    std::printf("  correlation    : %+.4f\n", c.correlation);
    std::printf("  RMS difference : %.2f C   (after removing each curve's mean)\n",
                c.rmsDifference);
    std::printf("  max difference : %.2f C\n", c.maxDifference);
    std::printf("  mean offset    : %+.2f C  (expected when calibration differs)\n",
                c.meanOffset);

    // Thresholds are judgement calls, stated openly rather than hidden in a pass/fail.
    std::printf("\n  ");
    if (c.correlation >= 0.8) {
        std::printf("STRONG agreement - both tools move temperature together.\n");
    } else if (c.correlation >= 0.5) {
        std::printf("MODERATE agreement - same broad trend, notable local differences.\n");
    } else if (c.correlation >= 0.0) {
        std::printf("WEAK agreement - worth investigating before trusting the output.\n");
    } else {
        std::printf("INVERTED - the curves move in opposite directions. This is a bug.\n");
    }
    return 0;
}

// Compares the built-in motion planner against the reference implementation on the same
// input. ADR-0007 requires parity before the subprocess can be dropped.
int runKinematicsCheck(std::string_view path, const std::filesystem::path& estimator)
{
    if (estimator.empty()) {
        std::fprintf(stderr, "error: need klipper_estimator to compare against\n");
        return 1;
    }
    const auto configPath = estimator.parent_path() / "config.json";

    std::string configJson;
    {
        std::ifstream cfg{configPath, std::ios::binary};
        if (!cfg) {
            std::fprintf(stderr, "error: no config.json at %s\n",
                         configPath.string().c_str());
            return 1;
        }
        configJson.assign(std::istreambuf_iterator<char>(cfg), {});
    }

    sb53::DiagnosticList diags;
    const auto limits = sb53::MachineLimits::fromJson(configJson, diags);
    if (diags.hasErrors()) {
        report(diags);
        return 1;
    }
    std::printf("config: v=%.0f a=%.0f scv=%.1f mcr=%.2f  (%zu axis, %zu extruder limits)\n",
                limits.maxVelocity, limits.maxAcceleration, limits.squareCornerVelocity,
                limits.minimumCruiseRatio, limits.axisLimits.size(),
                limits.extruderLimits.size());

    // --- ours ---
    std::vector<sb53::MoveSample> mine;
    {
        std::ifstream in{std::string(path), std::ios::binary};
        if (!in) { std::fprintf(stderr, "error: cannot open %s\n", path.data()); return 1; }
        mine = sb53::estimateMoves(in, limits, 1.75, diags);
    }
    if (diags.hasErrors()) { report(diags); return 1; }

    // --- reference ---
    sb53::SubprocessRunner runner;
    const auto proc = runner.run(
        estimator, {"--config_file", configPath.string(), "dump-moves", std::string(path)},
        std::chrono::minutes{10});
    if (proc.exitCode != 0 || proc.launchFailed) {
        std::fprintf(stderr, "error: reference estimator failed: %s\n", proc.stdErr.c_str());
        return 1;
    }
    std::istringstream dump{proc.stdOut};
    const auto theirs = sb53::MoveDumpParser::parse(dump, diags);

    // --- compare ---
    const auto sum = [](const std::vector<sb53::MoveSample>& v) {
        double t = 0.0;
        for (const auto& m : v) { t += m.duration; }
        return t;
    };
    const double tMine = sum(mine);
    const double tTheirs = sum(theirs);

    std::printf("\n%-14s %10s %12s\n", "", "moves", "total time");
    std::printf("%-14s %10zu %10.2fs\n", "reference", theirs.size(), tTheirs);
    std::printf("%-14s %10zu %10.2fs\n", "built-in", mine.size(), tMine);
    std::printf("%-14s %+10zd %+10.2fs  (%+.1f%%)\n", "difference",
                static_cast<std::ptrdiff_t>(mine.size()) -
                    static_cast<std::ptrdiff_t>(theirs.size()),
                tMine - tTheirs,
                tTheirs > 0.0 ? 100.0 * (tMine - tTheirs) / tTheirs : 0.0);

    if (mine.size() == theirs.size() && !mine.empty()) {
        double worst = 0.0, sumAbs = 0.0;
        std::size_t worstAt = 0, within1pct = 0;
        for (std::size_t i = 0; i < mine.size(); ++i) {
            const double d = std::abs(mine[i].duration - theirs[i].duration);
            sumAbs += d;
            if (d > worst) { worst = d; worstAt = i; }
            if (theirs[i].duration > 0 &&
                d / theirs[i].duration < 0.01) { ++within1pct; }
        }
        std::printf("\nper-move duration:\n");
        std::printf("  mean |diff|   : %.6f s\n", sumAbs / static_cast<double>(mine.size()));
        std::printf("  worst         : %.6f s at move %zu "
                    "(ref %.6f, ours %.6f)\n",
                    worst, worstAt, theirs[worstAt].duration, mine[worstAt].duration);
        std::printf("  within 1%%     : %zu / %zu (%.1f%%)\n", within1pct, mine.size(),
                    100.0 * static_cast<double>(within1pct) /
                        static_cast<double>(mine.size()));
    } else {
        std::printf("\nmove counts differ - fix extraction before comparing timings.\n");
    }
    return 0;
}

// Everything the CLI lets you override. Real profiles arrive with the database at M8;
// these exist so the pipeline is usable and explorable now.
struct Settings {
    sb53::ExtruderProfile extruder;
    sb53::FilamentProfile filament;
    std::filesystem::path estimator;
    bool adjustPressureAdvance = false;
};

void parseProfileFlags(const std::vector<std::string_view>& args, Settings& s)
{
    const auto number = [](std::string_view text, double fallback) {
        try {
            return std::stod(std::string(text));
        } catch (...) {
            std::fprintf(stderr, "warning: could not parse '%s'; using %g\n",
                         std::string(text).c_str(), fallback);
            return fallback;
        }
    };

    s.extruder.startMacro = "PRINT_START";
    s.extruder.temperatureToken = "EXTRUDER_TEMP";

    for (std::size_t i = 2; i + 1 < args.size(); ++i) {
        const auto& k = args[i];
        const auto& v = args[i + 1];
        if      (k == "--estimator") { s.estimator = std::string(v); }
        else if (k == "--bias")      { s.filament.speedQualityBias = static_cast<int>(number(v, 5)); }
        else if (k == "--smoothing") { s.extruder.smoothingWindow = static_cast<int>(number(v, 20)); }
        else if (k == "--low")       { s.filament.lowFlow = number(v, 1.0); }
        else if (k == "--mid")       { s.filament.midFlow = number(v, 15.0); }
        else if (k == "--high")      { s.filament.highFlow = number(v, 22.0); }
        else if (k == "--low-temp")  { s.filament.lowTemp = number(v, 190.0); }
        else if (k == "--mid-temp")  { s.filament.midTemp = number(v, 220.0); }
        else if (k == "--high-temp") { s.filament.highTemp = number(v, 235.0); }
        else if (k == "--rise")      { s.extruder.tempRise = number(v, 10.0); }
        else if (k == "--fall")      { s.extruder.tempFall = number(v, 5.0); }
        else if (k == "--start-macro") { s.extruder.startMacro = std::string(v); }
        else if (k == "--temp-token")  { s.extruder.temperatureToken = std::string(v); }
        else if (k == "--cool-below")  { s.extruder.coolingLayerTime = number(v, 15.0); }
        else if (k == "--cool-drop")   { s.extruder.coolingMaxDrop = number(v, 0.0); }
        else if (k == "--adjust-pa")   { s.adjustPressureAdvance = true; }
    }
}

// Reports progress to the terminal without scrolling.
class ConsoleProgress final : public sb53::IProgressSink {
public:
    void onPhase(sb53::Phase phase, double fraction) override
    {
        if (phase == m_phase && fraction >= 0.0 && fraction - m_fraction < 0.05) {
            return;
        }
        m_phase = phase;
        m_fraction = fraction;
        if (fraction >= 0.0) {
            std::printf("\r  %-14s %3.0f%%   ", sb53::toString(phase).data(),
                        fraction * 100.0);
        } else {
            std::printf("\r  %-14s ...    ", sb53::toString(phase).data());
        }
        std::fflush(stdout);
    }
    void onLog(std::string_view message) override
    {
        std::printf("\n  %.*s\n", static_cast<int>(message.size()), message.data());
    }
    [[nodiscard]] bool cancelRequested() override { return false; }

    void finish() { std::printf("\r%-40s\r", ""); std::fflush(stdout); }

private:
    sb53::Phase m_phase = sb53::Phase::Scanning;
    double m_fraction = -1.0;
};

// Runs the estimator over a slice of a file and returns the parsed moves.
bool estimateMoves(const std::filesystem::path& estimator,
                   const std::filesystem::path& configPath,
                   const std::filesystem::path& gcode,
                   std::vector<sb53::MoveSample>& moves,
                   sb53::DiagnosticList& diags)
{
    sb53::SubprocessRunner runner;
    const auto proc = runner.run(
        estimator,
        {"--config_file", configPath.string(), "dump-moves", gcode.string()},
        std::chrono::minutes{10});

    if (proc.launchFailed || proc.timedOut || proc.exitCode != 0) {
        diags.add(sb53::error(
            sb53::Code::EstimatorFailed,
            proc.launchFailed ? proc.stdErr
            : proc.timedOut   ? "The motion estimator timed out."
                              : "The motion estimator exited " +
                                    std::to_string(proc.exitCode) + ": " + proc.stdErr));
        return false;
    }

    std::istringstream dump{proc.stdOut};
    moves = sb53::MoveDumpParser::parse(dump, diags);
    return !diags.hasErrors();
}

// The full pipeline: scan -> extract body -> estimate -> analyse -> plan -> rewrite.
int runProcess(std::string_view inputPath, std::string outputPath,
               const std::filesystem::path& exeDir, Settings s)
{
    if (s.estimator.empty()) {
        s.estimator = findEstimator(exeDir);
    }
    if (s.estimator.empty()) {
        std::fprintf(stderr, "error: [%s] could not find klipper_estimator.exe. "
                             "Pass --estimator <path>.\n",
                     sb53::toString(sb53::Code::EstimatorNotFound).data());
        return 1;
    }
    const auto configPath = s.estimator.parent_path() / "config.json";

    ConsoleProgress progress;
    sb53::DiagnosticList diags;

    // --- scan ---------------------------------------------------------------
    progress.onPhase(sb53::Phase::Scanning, -1.0);
    sb53::ScanResult scan;
    {
        std::ifstream in{std::string(inputPath), std::ios::binary};
        if (!in) {
            progress.finish();
            std::fprintf(stderr, "error: [%s] cannot open '%s'\n",
                         sb53::toString(sb53::Code::FileNotFound).data(),
                         std::string(inputPath).c_str());
            return 1;
        }
        scan = sb53::GcodeScanner::scan(in, diags);
    }
    if (diags.hasErrors()) {
        progress.finish();
        report(diags);
        return 1;
    }

    // --- extract the print body --------------------------------------------
    //
    // The estimator sees ONLY the body. Start and end macros contain moves (purge lines,
    // wipes) that must not influence the temperature plan -- and, critically, the
    // rewriter accumulates extruded filament from the same starting point, so the two
    // must cover exactly the same span or the plan lookup is offset.
    std::error_code ec;
    const auto scratch = std::filesystem::temp_directory_path(ec) /
                         ("sb53-" + std::to_string(
                              std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(scratch, ec);
    const auto bodyPath = scratch / "body.gcode";

    {
        std::ifstream in{std::string(inputPath), std::ios::binary};
        std::ofstream body{bodyPath, std::ios::binary};
        std::string line;
        std::size_t n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (n >= scan.bodyFirstLine && n <= scan.bodyLastLine) {
                if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                body << line << '\n';
            }
        }
    }

    // --- estimate + analyse -------------------------------------------------
    progress.onPhase(sb53::Phase::Estimating, -1.0);
    std::vector<sb53::MoveSample> moves;
    if (!estimateMoves(s.estimator, configPath, bodyPath, moves, diags)) {
        progress.finish();
        report(diags);
        std::filesystem::remove_all(scratch, ec);
        return 1;
    }

    progress.onPhase(sb53::Phase::Analysing, -1.0);
    const auto analysis = sb53::analyseFlow(moves);

    // --- plan ---------------------------------------------------------------
    progress.onPhase(sb53::Phase::Planning, -1.0);
    const auto plan = sb53::planTemperature(analysis, s.extruder, s.filament, diags, {},
                                            scan.layers);
    if (diags.hasErrors()) {
        progress.finish();
        report(diags);
        std::filesystem::remove_all(scratch, ec);
        return 1;
    }

    // --- rewrite ------------------------------------------------------------
    //
    // Written to a scratch file first, then moved into place only on success. A failed
    // run must never leave a half-written file where the input was -- especially when
    // invoked as a post-processing hook, where the input IS the user's only copy.
    const auto stagedPath = scratch / "out.gcode";
    sb53::RewriteStats stats;
    {
        std::ifstream in{std::string(inputPath), std::ios::binary};
        std::ofstream out{stagedPath, std::ios::binary};
        sb53::RewriteOptions options;
        options.adjustPressureAdvance = s.adjustPressureAdvance;
        stats = sb53::rewriteGcode(in, out, scan, analysis, plan, s.extruder, s.filament,
                                   diags, progress, options);
    }
    progress.finish();

    if (diags.hasErrors()) {
        report(diags);
        std::filesystem::remove_all(scratch, ec);
        return 1;
    }

    if (outputPath.empty()) {
        outputPath = std::string(inputPath);   // in place, as a slicer hook expects
    }
    std::filesystem::copy_file(stagedPath, outputPath,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "error: [%s] could not write '%s': %s\n",
                     sb53::toString(sb53::Code::OutputWriteFailed).data(),
                     outputPath.c_str(), ec.message().c_str());
        std::filesystem::remove_all(scratch, ec);
        return 1;
    }
    std::filesystem::remove_all(scratch, ec);

    report(diags);

    const auto secs = static_cast<int>(analysis.totalTime);
    std::printf("wrote %s\n", outputPath.c_str());
    std::printf("  estimated time     : %dm %ds\n", secs / 60, secs % 60);
    std::printf("  temperature range  : %.1f - %.1f C (start %.1f)\n",
                plan.minTemperature, plan.maxTemperature, plan.initialTemperature);
    std::printf("  M104 commands      : %zu\n", stats.temperatureCommands);
    std::printf("  feedrates reduced  : %zu\n", stats.feedratesReduced);
    if (s.extruder.layerCoolingEnabled() && !plan.layerCoolingDrop.empty()) {
        double maxDrop = 0.0;
        std::size_t cooled = 0;
        for (const double d : plan.layerCoolingDrop) {
            maxDrop = std::max(maxDrop, d);
            if (d > 0.01) { ++cooled; }
        }
        std::printf("  layer cooling      : %zu of %zu seconds, up to -%.1f C\n",
                    cooled, plan.layerCoolingDrop.size(), maxDrop);
    }
    if (stats.pressureAdvanceCommands > 0) {
        std::printf("  pressure advance   : %zu\n", stats.pressureAdvanceCommands);
    }
    std::printf("  lines %zu in, %zu out\n", stats.linesRead, stats.linesWritten);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string_view> args(argv + 1, argv + argc);

    // Double-clicked from Explorer: launch the web UI and open a browser.
    //
    // Without this the program prints its usage and exits, the console window it created
    // closes with it, and it looks like nothing happened at all. Someone who double-clicks
    // an application wants the application, not a usage message they cannot read.
    if (args.empty() && launchedByDoubleClick()) {
        std::error_code ec;
        const auto exeDir =
            std::filesystem::absolute(std::filesystem::path(argv[0]), ec).parent_path();
        openBrowser("http://127.0.0.1:8765");
        return sb53::web::runServe(8765, exeDir, findEstimator);
    }

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

        // Hard-coded defaults, overridable per-run. Real profiles come from the database
        // at M8; these exist so the planner's behaviour can be explored now. The values
        // are the README's own worked example, NOT a recommendation -- calibrate your
        // own (ALGORITHM.md Â§3).
        sb53::ExtruderProfile extruder;
        sb53::FilamentProfile filament;

        const auto number = [&](std::string_view text, double fallback) {
            try {
                return std::stod(std::string(text));
            } catch (...) {
                std::fprintf(stderr, "warning: could not parse '%s'; using %g\n",
                             std::string(text).c_str(), fallback);
                return fallback;
            }
        };

        for (std::size_t i = 2; i + 1 < args.size(); ++i) {
            const auto& key = args[i];
            const auto& value = args[i + 1];
            if (key == "--estimator")      { estimator = std::string(value); }
            else if (key == "--bias")      { filament.speedQualityBias =
                                                 static_cast<int>(number(value, 5)); }
            else if (key == "--smoothing") { extruder.smoothingWindow =
                                                 static_cast<int>(number(value, 20)); }
            else if (key == "--low")       { filament.lowFlow = number(value, 1.0); }
            else if (key == "--mid")       { filament.midFlow = number(value, 15.0); }
            else if (key == "--high")      { filament.highFlow = number(value, 22.0); }
            else if (key == "--low-temp")  { filament.lowTemp = number(value, 190.0); }
            else if (key == "--mid-temp")  { filament.midTemp = number(value, 220.0); }
            else if (key == "--high-temp") { filament.highTemp = number(value, 235.0); }
        }

        std::error_code ec;
        const auto exeDir =
            std::filesystem::absolute(std::filesystem::path(argv[0]), ec).parent_path();
        return runAnalyze(args[1], estimator, exeDir, extruder, filament);
    }

    if (args[0] == "serve") {
        unsigned short port = 8765;
        for (std::size_t i = 1; i + 1 < args.size(); ++i) {
            if (args[i] == "--port") {
                port = static_cast<unsigned short>(std::stoi(std::string(args[i + 1])));
            }
        }
        std::error_code ec;
        const auto exeDir =
            std::filesystem::absolute(std::filesystem::path(argv[0]), ec).parent_path();
        return sb53::web::runServe(port, exeDir, findEstimator);
    }

    // Development command: runs the built-in planner against the reference
    // implementation on the same file. Exists to earn confidence in ADR-0007 before the
    // subprocess is removed; not documented in --help.
    if (args[0] == "kinematics-check") {
        if (args.size() < 2) {
            std::fprintf(stderr, "error: needs a G-code file\n");
            return 64;
        }
        std::filesystem::path estimator;
        for (std::size_t i = 2; i + 1 < args.size(); ++i) {
            if (args[i] == "--estimator") { estimator = std::string(args[i + 1]); }
        }
        std::error_code ec;
        const auto exeDir =
            std::filesystem::absolute(std::filesystem::path(argv[0]), ec).parent_path();
        if (estimator.empty()) { estimator = findEstimator(exeDir); }
        return runKinematicsCheck(args[1], estimator);
    }

    if (args[0] == "compare") {
        if (args.size() < 3) {
            std::fprintf(stderr, "error: compare requires two processed G-code files\n");
            return 64;
        }
        return runCompare(args[1], args[2]);
    }

    if (args[0] == "process") {
        if (args.size() < 2) {
            std::fprintf(stderr, "error: process requires an input file\n");
            return 64;
        }

        Settings s;
        std::string outPath;
        for (std::size_t i = 2; i + 1 < args.size(); ++i) {
            if (args[i] == "--out") { outPath = std::string(args[i + 1]); }
        }
        parseProfileFlags(args, s);

        std::error_code ec;
        const auto exeDir =
            std::filesystem::absolute(std::filesystem::path(argv[0]), ec).parent_path();
        return runProcess(args[1], outPath, exeDir, s);
    }

    std::fprintf(stderr, "error: unknown command '%s'\n\n", args[0].data());
    printUsage();
    return 64;   // EX_USAGE
}
