# Architecture

> What the pieces are and how they fit. For *what the tool does*, see
> [ALGORITHM.md](ALGORITHM.md). For *why these choices*, see [adr/](adr/).

## The one rule

**`core/` links against nothing that knows a user exists.**

No Qt. No windowing. No `std::cout`. No message boxes. No subprocess spawning
implementation (only an interface). No knowledge of "the application directory".

This is enforced in CI, not by convention — a grep for `#include <Q` under `core/` fails
the build, and `core`'s CMake target never links Qt.

Everything else in this document follows from that rule.

## Layers

```
        ┌─────────────────────────────────────────┐
        │  sb53_core   (static lib, C++20)        │   the asset
        │  no Qt, no I/O to a human               │
        └─────────────────────────────────────────┘
              ▲                        ▲
              │                        │
    ┌─────────────────┐      ┌──────────────────┐
    │  sb53-cli       │      │  sb53-qt         │       disposable
    │  headless       │      │  Qt 6 Widgets    │
    └─────────────────┘      └──────────────────┘
              ▲
              │  (future)
    ┌─────────────────────────┐
    │  web / Orca cloud plugin │
    └─────────────────────────┘
```

**The CLI is not a development toy.** It is simultaneously:

- the test driver,
- the OrcaSlicer post-processing script (the tool's primary real-world use),
- and the process a future web backend invokes.

It reaches full capability at milestone **M6**, *before* the Qt frontend starts at M8.
That ordering is deliberate and is the main structural defence against sinking effort
into a UI layer that is planned to be replaced. See
[ADR-0002](adr/0002-core-ui-separation.md).

## Directory layout

```
core/
  include/sb53/          public headers — the entire API surface
    Model.hpp            data structures (§ below)
    Profiles.hpp         ExtruderProfile, FilamentProfile, IProfileRepository
    Ports.hpp            IProcessRunner, IProgressSink
    Workspace.hpp        scratch-directory ownership
    Pipeline.hpp         the facade
    Diagnostics.hpp      Diagnostic, Severity — how the core reports problems
  src/                   implementation
    GcodeScanner.cpp     §9 preconditions, marker + body-bounds extraction
    EstimatorRunner.cpp  klipper_estimator invocation
    MoveDumpParser.cpp   estimator output → MoveSample[]
    FlowAnalyzer.cpp     → per-second timeline
    TemperaturePlanner.cpp  blend → smooth → map → slew-limit
    GcodeRewriter.cpp    the rewrite pass
    OutputAssembler.cpp  splice + re-estimate
    platform/            the ONLY place OS-specific code is allowed
cli/                     sb53-cli
app/                     sb53-qt
tests/
  unit/                  fast, hermetic, no subprocesses
  reference/             differential comparison vs. the legacy binary (M7)
testdata/                fixtures — recorded estimator output, synthetic G-code
tools/                   capture scripts, dev utilities
docs/                    you are here
Source/                  LEGACY DELPHI — reference only, never built
bin/                     legacy runtime: the shipped .exe + vendored estimator
```

## The five seams

Everything the legacy did by reaching into a form becomes one of these. Each exists so
that the core can be driven by a GUI, a CLI, or an HTTP handler without change.

### `IProcessRunner`
Runs `klipper_estimator` and returns its output. The core never spawns a process itself.

Capturing stdout directly — rather than the legacy's shell redirection through `cmd.exe`
— removes three defects at once: the `cmd.exe` dependency, the documented failure when
the path contains spaces, and the inability to see the estimator's stderr.

**Testing consequence, and the reason this seam matters most:** unit tests inject a fake
that replays a recorded estimator output. *No test in `tests/unit/` spawns a subprocess.*
That keeps the suite fast and hermetic, and means the vendored estimator binary is not a
test dependency.

### `IProgressSink`
Replaces every `ShowMessage`, every `Label.Caption :=`, and the legacy's global
cancellation flag. Reports phase, progress fraction, and log messages; polled for
cancellation.

Qt implements it by queueing signals to the GUI thread. A web backend implements it as an
SSE or WebSocket writer. The core is unaware of either.

### `Workspace`
Owns the scratch directory and the intermediate files. Replaces the legacy's habit of
writing next to its own executable — which breaks under `Program Files` and races against
concurrent runs.

RAII: cleans up on destruction unless explicitly told to retain (which the reference
tests do, since the intermediates are the comparison points).

### `IProfileRepository`
Extruder and filament profiles. Two implementations planned:

- **SQLite** — reads the existing `Config.sdb` schema, so current users' calibration work
  migrates. This is the only artifact in the legacy project that took real human effort
  to produce; see [ADR-0003](adr/0003-sqlite-in-core.md).
- **JSON** — for the plugin case, where profiles arrive over the wire rather than from a
  local file.

The profile *logic* (validation, upsert semantics, cascade delete) lives in core, not in
the UI. In the legacy it lived in form event handlers, which is precisely why it would
have to be rewritten for every new frontend.

### `Pipeline`
The single entry point — plus each stage exposed individually, because the UI needs to
re-enter at different points (full run / re-plan only / re-rewrite only) for responsive
interaction. A web frontend wants exactly the same seams for incremental recompute.

## Data model

The legacy used its charting widgets as its data structures. The replacement is plain
types with no framework dependency:

| Type | Holds |
|---|---|
| `MoveSample` | one move: duration, flow |
| `FlowSecond` | one second: average flow, max flow, cumulative extruded filament |
| `ScanResult` | markers, printer/filament IDs, body bounds, extrusion mode |
| `SourceAnalysis` | move samples + the per-second timeline + totals |
| `TemperaturePlan` | blended flow, smoothed flow, desired and achievable temperature |
| `RewriteResult` | output path, statistics, diagnostics |

Charting consumes these. Never the reverse — no type in `core/` exists to serve a widget.

## Error handling

The core returns errors; it does not throw across its API boundary and never prints.

Problems are `Diagnostic` values carrying a severity, a stable machine-readable code, and
a human message. This lets the CLI print them, the GUI list them in a panel, and a web
backend serialise them to JSON — without the core knowing which is happening.

Preconditions from [ALGORITHM.md §9](ALGORITHM.md) (absolute extrusion, already-processed
input, unlocatable print body) surface as diagnostics with distinct codes, not as generic
failures. A user hitting the `M82` guard needs to be told *specifically* that their slicer
is set to absolute extrusion — that is the difference between a fixable problem and an
inexplicable one.

## Platform

Windows first. Windows-specific code is confined to `core/src/platform/`, and a Linux
build is kept green in CI.

That is not scope creep — a Linux build is the cheapest possible proof that the core
stayed genuinely UI-agnostic, and it is the target any future server-side deployment
needs. It costs one CI job.

## Dependencies

Deliberately few. Each is pinned to an exact version.

| Dependency | Where | Why |
|---|---|---|
| SQLite (vendored amalgamation) | core | Profile storage. Not QtSql — [ADR-0003](adr/0003-sqlite-in-core.md) |
| Catch2 v3 | tests | Table-driven cases and floating-point matchers |
| Qt 6 LTS | `app/` only | The GUI, and nothing else |
| klipper_estimator | vendored binary | A **custom fork**, not upstream — pinned by hash, never auto-upgraded |

**Charting uses a custom `QWidget`, not Qt Charts.** This project is MIT; Qt Charts and
QCustomPlot are GPL-3.0-or-commercial and would relicense it. See
[ADR-0004](adr/0004-custom-chart-widget.md).
