// The seams that keep the core UI-agnostic (ADR-0002).
//
// Everything the legacy did by reaching into a form becomes one of these. The core
// depends on the interfaces; frontends supply the implementations.

#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sb53 {

// ---------------------------------------------------------------------------
// Process execution
// ---------------------------------------------------------------------------

struct ProcessResult {
    int         exitCode = -1;
    std::string stdOut;
    std::string stdErr;
    bool        timedOut = false;
    bool        launchFailed = false;   // the executable could not be started at all
};

// Runs an external program. The core never spawns processes itself.
//
// Deliberately takes an argument VECTOR rather than a command line. The legacy built a
// single string and passed it through `cmd.exe` with `>` redirection, which is where its
// "paths must not contain spaces" limitation came from (known-bugs.md #7) and why the
// estimator's stderr was invisible. Capturing stdout directly removes both problems and
// the shell dependency along with them.
class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;

    virtual ProcessResult run(const std::filesystem::path& executable,
                              const std::vector<std::string>& arguments,
                              std::chrono::milliseconds timeout) = 0;
};

// ---------------------------------------------------------------------------
// Progress and cancellation
// ---------------------------------------------------------------------------

enum class Phase {
    Scanning,
    Estimating,
    Analysing,
    Planning,
    Rewriting,
    ReEstimating,
    Assembling,
};

[[nodiscard]] std::string_view toString(Phase) noexcept;

// Replaces the legacy's ShowMessage calls, its chart-title status updates, its
// label assignments, and its global cancellation flag.
//
// Qt implements this by queueing signals to the GUI thread; a web backend implements it
// as an SSE or WebSocket writer; the CLI prints. The core knows none of that.
class IProgressSink {
public:
    virtual ~IProgressSink() = default;

    // `fraction` is 0..1 within the phase, or negative when indeterminate.
    virtual void onPhase(Phase phase, double fraction) = 0;
    virtual void onLog(std::string_view message) = 0;

    // Polled at safe points. Returning true unwinds the pipeline with Code::Cancelled.
    [[nodiscard]] virtual bool cancelRequested() = 0;
};

// A sink that discards everything. Useful for tests and for callers that do not care.
class NullProgressSink final : public IProgressSink {
public:
    void onPhase(Phase, double) override {}
    void onLog(std::string_view) override {}
    [[nodiscard]] bool cancelRequested() override { return false; }
};

} // namespace sb53
