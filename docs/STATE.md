# Project State

> **Read this file first when resuming work.** It is the single source of truth for
> "where are we and what happens next". Update it at the end of every work session,
> even a short one. A stale STATE.md is worse than none.

**Last updated:** 2026-08-10
**Current milestone:** M8 — Profile management UI (storage done, wiring next), then real print validation
**Build status:** ✅ green on Windows (MSVC 19.44, Ninja, C++20) — 61/61 tests passing.
Linux CI (GCC + Clang) was red on the first pipeline run and is fixed as of the latest
commit, **not yet re-confirmed green** — see "CI/CD pipeline" below.

> **The tool works end to end**, with a local web UI. `flowtemp serve` opens a browser
> page; `flowtemp process` runs headless as a slicer post-processing script. See
> [README.md](../README.md) for user-facing instructions — this file is the build log.
>
> **Still true after every change below: nothing has been validated by a real print.**

## How to build

```powershell
./tools/build.ps1
```

That is the whole thing. The script finds Visual Studio's bundled CMake, Ninja and MSVC
via `vswhere`, so **no separate CMake install and no Developer Prompt are needed** — none
of `cmake`, `ninja` or `cl` are on `PATH` in a normal shell, which is otherwise a
fifteen-minute rediscovery after a long gap.

`-Clean` wipes the build directory; `-Config Debug` changes configuration; `-NoTests`
skips `ctest`.

> Catch2 is fetched from GitHub on first configure, so the **first** build needs network
> access. Subsequent builds are offline. An installed Catch2 3.x is used instead if
> present.

---

## What this project is

A clean-room C++ rewrite of the SB53 G-Code Flow/Temperature Controller, originally a
Delphi/VCL Windows application. The tool post-processes 3D-printer G-code, adjusting
nozzle temperature to track volumetric flow rate.

- **Resuming after a break, or an AI picking this up:** [AI-CONTEXT.md](AI-CONTEXT.md) —
  the hard-won detail that fits nowhere else: stale-artefact traps, build gotchas, testing
  pitfalls, measurements already taken, and the user's real printer values. Read it first.
- **What it does:** [ALGORITHM.md](ALGORITHM.md) — read this before touching any code.
- **How it is put together:** [ARCHITECTURE.md](ARCHITECTURE.md)
- **Why it is put together that way:** [adr/](adr/) — one file per decision.
- **The old Delphi app:** `Source/V1.1/` (reference only, never built — see below)

## Why a rewrite rather than a port

The legacy source in `Source/V1.1/` **cannot be compiled**, for two independent reasons
established during review:

1. `Unit1.pas:244` imports `Unit9`, which has never existed in the repository's 738-commit
   history and is absent from the project file.
2. `Unit1.pas:171-172` declares `Button1`/`Button2`, but `Unit1.dfm` contains zero
   `TButton` objects, so form streaming would fail at runtime regardless.

Beyond that, the legacy has no domain layer at all — the charting widgets are used
directly as the data structures. A faithful port would have reproduced that structure in
a new language. See [ADR-0001](adr/0001-clean-room-rewrite.md).

The legacy tree is kept **as a reference document**, not as code to be migrated. It is
the only record of the ~15% of behaviour the public README does not describe.

---

## Milestones

Each milestone is independently valuable and independently resumable. Do not start the
next until the current one's exit criteria are met and this file is updated.

| # | Milestone | Status | Exit criteria |
|---|---|---|---|
| **M1** | **Foundations** — docs, ADRs, CMake skeleton, test harness | ✅ **done** | ~~Build and tests green; docs reviewed~~ |
| **M2** | **Data model + G-code scanner** | ✅ **done** | ~~Scanner extracts markers, IDs, body bounds, M82/M83 guard from real files; unit tested~~ |
| **M3** | **Estimator integration + flow aggregation** | ✅ **done** | ~~Per-second flow timeline produced from a recorded fixture, no live subprocess in tests~~ |
| **M4** | **Temperature planner** | ✅ **done** | ~~Blend → smooth → map → slew-limit, each unit tested~~ |
| **M5** | **G-code rewriter** | ✅ **done** | ~~Valid output G-code; feedrate clamp and PA gating verified~~ |
| **M6** | **CLI end-to-end** | ✅ **done** | ~~`sb53 process in.gcode` produces a printable file~~ |
| **M7** | **Differential validation vs. legacy** | ✅ **done** | ~~Temperature curves track the legacy binary within tolerance~~ — correlation **+0.9947** against the user's real calibration |
| **M8a** | **Web UI** (superseded the planned Qt frontend — see [ADR-0004 addendum below](#web-ui-superseded-qt)) | ✅ **done** | ~~Charts, controls, file picker, safe (non-overwriting) output~~ |
| **M8b** | **Saved printer/filament profiles** | 🟡 **in progress** | Storage layer done and tested; **UI wiring (list/save/duplicate/load) not started** |
| **M8c** | **CI/CD pipeline** | 🟡 **in progress** | Windows jobs green; Linux fix pushed, not yet re-confirmed |
| M9 | Physical print validation | ⚪ not started | Test prints succeed on real hardware — **the only gap that can't be closed from a desk** |

**M6 was the real functional milestone** — at that point the tool became genuinely useful
with no GUI at all, which is the artifact the future OrcaSlicer cloud plugin needs. M8a
(web UI) is what makes it comfortable for daily use; M8b (profiles) is what removes the
remaining workflow friction (retyping calibration every run).

---

## Current position

> **Note on the binary name in older entries below.** The executable was originally
> `sb53.exe` and was renamed to `flowtemp.exe` partway through the project (so it cannot
> be confused with the legacy `SB53-Systems.exe` in a folder listing or taskbar). The
> milestone log below is left as written — each entry is an accurate record of what was
> true when it was written — so commands quoted in the M2–M6 entries still say
> `sb53.exe`. Substitute `flowtemp.exe` if copy-pasting one. Every command in "Using it"
> and everything from M8a onward already uses the current name.

### Done
- Reviewed the legacy application in full; established that it cannot be built.
- Recovered the purpose of the missing `Unit9` from the shipped V1.2 binary's embedded
  form resources: an unreleased serial-port printer console (calibration / fan control).
  **Deliberately out of scope** — recorded in [legacy/unit9-recovered.md](legacy/unit9-recovered.md).
- Derived the algorithm specification from the public README plus legacy source, with
  provenance marked per claim: [ALGORITHM.md](ALGORITHM.md).
- Catalogued legacy defects so they are not reproduced: [legacy/known-bugs.md](legacy/known-bugs.md).
- Created the directory skeleton.

- **M1 complete.** Buildable skeleton, 8/8 tests green:
  - CMake project (C++20, warnings-as-errors on core, `-Wconversion`, FP contraction off)
  - `sb53_core` static library — no Qt, verified by the `core-has-no-qt` test
  - `sb53-cli` — builds and runs; `process` is an explicit stub returning exit 2, so
    "not built yet" cannot be mistaken for "built and misbehaving"
  - Catch2 test harness with the first real unit tests
  - `tools/build.ps1`

  The `core-has-no-qt` guard was verified to actually **fail** on a file containing
  `#include <QString>` — a guard never seen to fail is not a guard.

- **Test data captured** (2026-08-07). Nineteen OrcaSlicer 2.4.2 files supplied by the
  user. Analysis produced one substantive finding, now recorded as
  [known-bugs.md #11](legacy/known-bugs.md): the legacy **fails silently** — seven of nine
  processed files carry the `; Edited by` header and an empty estimated-time field but
  contain no `M104` commands at all. They look processed and are not. This is the
  strongest available argument for the loud-failure design in `Diagnostics.hpp`.

  Confirmed from real output: temperatures emit as `M104 S212.7` (one decimal, no
  trailing zero); the start macro is rewritten as
  `PRINT_START EXTRUDER_TEMP=213.3 BED_TEMP=65    ; Reset Initial Temperature`; speed
  lines carry `; Keep Slicer Speed` / `; Reset Speed Before Retraction` comments.

- **M2 complete.** `GcodeScanner` works on real 73k-line files; 18/18 tests green.

  Try it:
  ```powershell
  ./build/bin/sb53.exe scan testdata/fixtures/orca-basic.gcode
  ```

  Two structural findings worth remembering:
  - **`printer_settings_id` and `filament_settings_id` live in the config block at the
    *end* of the file**, after `; EXECUTABLE_BLOCK_END` — not in the header. The scan
    cannot stop once the body is bounded or profile auto-selection is silently lost.
  - **`filament_settings_id` values are quoted by OrcaSlicer** (`"Elegoo HS PLA+ awd
    hott"`). The quotes are its escaping and are stripped.

  Decision made here: absence of both `M82` and `M83` is an **error**, not a warning.
  Firmware defaults to absolute extrusion when unspecified, so silence is not consent,
  and a warning in a CLI scrolls past unread.

- **M3 complete.** `IProcessRunner` + `SubprocessRunner` + `MoveDumpParser` +
  `analyseFlow`; 22/22 tests green. Try it:
  ```powershell
  ./build/bin/sb53.exe analyze <your.gcode> --estimator bin/klipper_estimator.exe
  ```

  **Independent validation passed.** On a real 73k-line benchy the slicer declares
  `; filament used [mm] = 3475.25`; we computed **3475.54 mm** by an unrelated route —
  integrating volumetric flow over time and dividing by filament cross-section. 0.008%
  apart, which cross-checks estimator parsing, retract suppression, time-weighted
  integration and the volume-to-length conversion all at once.

  Estimator output format, recorded as a fixture rather than guessed:
  `Flow = Some(<double>)` / `Flow = None`, each followed by `Time = <double>`. Negative
  flow is a retract; `None` is travel. Travel must **not** disarm the retract flag,
  because travel happens between the retract and the unretract.

> ⚠️ **`bin/config.json` does not match the printer these samples came from.**
> The G-code requests `ACCEL=70000` and `SQUARE_CORNER_VELOCITY=12/15`; the config
> declares `max_acceleration: 6000` and `square_corner_velocity: 5.0` — roughly 12x
> slower. The estimator therefore predicts 13m 27s where the slicer says 7m 52s.
>
> This is not a defect in our code, but it **invalidates the analysis**: per-move timing
> drives the flow curve, which drives the temperature plan. Anyone running with a
> mismatched config gets a plausible-looking but wrong result, and may well explain some
> of the difficulty in the 2026-08-06 session.
>
> **Planned diagnostic:** compare `SET_VELOCITY_LIMIT` values found in the G-code against
> the loaded printer config and warn on significant mismatch. Cheap, and catches a silent
> misconfiguration the legacy never mentioned. Needs `Code::PrinterConfigMismatch`.

- **M4 complete.** `Profiles.hpp` + `TemperaturePlanner`; 30/30 tests green. The full
  blend → smooth → map → slew-limit chain runs on real files:
  ```powershell
  ./build/bin/sb53.exe analyze <your.gcode> --estimator bin/klipper_estimator.exe `
      --high 35 --high-temp 240 --smoothing 20 --bias 5
  ```
  Calibration flags are overridable per run so the planner's behaviour can be explored
  without a rebuild. The defaults are the README's worked example, **not** a
  recommendation — real profiles come from the database at M8.

  **The insight worth not losing:** the centered moving average is what gives the hotend
  *advance warning* of a high-flow section. Because this is post-processing rather than
  real-time control, look-ahead is free — each smoothed value already depends on flow up
  to half a window into the future. That is precisely why the slew limiter can be a
  simple causal filter and still not lag badly, and why the README says the smoothing
  value materially affects results. Recorded in `TemperaturePlanner.hpp`.

  Safety property under test: whatever the flow does, the planned temperature **never
  leaves the calibrated low..high band**. Extrapolating past a user's calibration would
  command temperatures they never validated.

- **M5 + M6 complete.** `GcodeRewriter` plus the full `sb53 process` pipeline; 42/42
  green. Verified on a real 73k-line benchy: 549 `M104` commands, 1204 feedrates reduced,
  temperature 203.0–231.0 °C inside the calibrated 195–240 band, start macro rewritten
  cleanly, and the output correctly **refused by its own scanner** on a second pass.

  Design points worth keeping:
  - **The estimator sees only the print body**, extracted to a scratch file. Start and end
    macros contain purge and wipe moves that must not influence the plan — and critically,
    the rewriter accumulates extruded filament from the same starting line, so the two
    must cover exactly the same span or every plan lookup is offset.
  - **Output is staged then moved into place.** A failed run must never leave a
    half-written file where the input was — when invoked as a post-processing hook, the
    input *is* the user's only copy.
  - **The start macro is parsed and re-emitted**, not spliced at a fixed offset. The
    legacy assumes exactly three digits and corrupts the line otherwise
    ([known-bugs.md #4](legacy/known-bugs.md)); there are tests for 2-, 3- and
    4-character replacements.

### Using it

```powershell
./build/bin/sb53.exe process <in.gcode> --out <out.gcode> `
    --estimator bin/klipper_estimator.exe `
    --low 1 --mid 15 --high 35 --low-temp 195 --mid-temp 215 --high-temp 240 `
    --smoothing 20 --bias 5
```

Omit `--out` to overwrite in place, which is what a slicer post-processing hook expects.
Calibration flags are placeholders until the profile database lands at M8.

- **M7 complete, and it earned its keep.** `sb53 compare` extracts the commanded
  temperature sequence from two processed files, indexes both by cumulative extruded
  filament, resamples onto a shared grid, and reports correlation plus mean-removed
  differences.

  **A matched pair was identified by fingerprinting the move sequence:**
  `fixingfix3799_PLA_7m19s.gcode` (raw) is the source of `..._7m27s.gcode` (legacy
  output) — identical 51,240-move hash. The tool does not alter `G1 X/Y/E` lines, so that
  hash is a reliable way to pair a processed file with its input.

  **The first comparison immediately found a real bug.** Correlation was only **+0.4455**,
  and the diagnostic was in the spans: the legacy curve covered 5616 mm of filament while
  ours stopped at 3475 mm on the same print.

  Cause: `MoveDumpParser` zeroes both the retract and its matching unretract, but
  `GcodeRewriter` was accumulating *any* positive `E` — including unretract moves like
  `G1 E2 F1800`. The rewriter's coordinate therefore ran ahead of the plan's, and since
  the plan holds its last value past the end, **temperature commands silently stopped
  partway through the print.** No crash, no warning — exactly the failure class this
  project exists to eliminate.

  Fix: only accumulate `E` on moves that actually travel (have `X` or `Y`). Result:

  | | before | after |
  |---|---|---|
  | correlation | +0.4455 (weak) | **+0.8672 (strong)** |
  | max difference | 28.90 °C | 13.17 °C |
  | mean offset | +3.50 °C | +0.97 °C |

  A regression test pins it, and the comment explains the coupling so nobody "optimises"
  the two accumulators back out of agreement.

- **Validated against the user's real setup (2026-08-07).** Their actual profile database
  was located at `D:\SB53_G-Code_Flow_Temperature_Controller_V1.1\Config\Config.sdb` and
  dumped with `tools/dump-profiles.py`.

  Real calibration for `"Elegoo HS PLA+ awd hott"`: flow **1 / 80 / 105** mm³/s →
  **220 / 280 / 310** °C, `A_M_SMOOTH` 20, `ADJUST_PA` 0, `SPEED_QUALITY_OPT` 3.
  Extruder `awd v0`: rise 5 °C/s, fall **1 °C/s** (cooling five times slower than
  heating — exactly the asymmetry the slew limiter models).

  With that calibration and the printer config taken from `EXTRUDER.PRINTER_CONFIG`:

  ```
  correlation    : +0.9947
  RMS difference : 0.88 C
  max difference : 2.96 C
  mean offset    : -0.24 C
  ```

  Temperature range came out **227.6 – 270.6 °C, start 228.3** — identical to the legacy
  output to one decimal.

  **Two corrections to earlier conclusions, recorded so they are not repeated:**
  1. The "printer config is 12x wrong" finding applies only to the stale bundled
     `bin/config.json`. The legacy stores the real config in `EXTRUDER.PRINTER_CONFIG` and
     writes it out on extruder selection, so actual legacy runs used
     `max_acceleration: 150000` — not 6000. The planned mismatch diagnostic is still worth
     having, but it is not the cause of anything observed.
  2. The Speed↔Quality scale is **inverted** relative to ours — see the addendum in
     [ADR-0006](adr/0006-explainable-blend-and-smoothing.md). Importing
     `SPEED_QUALITY_OPT` directly would produce prints ~14 °C colder than the user
     expects, silently.

- **Feedrate behaviour verified move-by-move (2026-08-07), correcting an earlier claim.**

  I previously flagged "2261 feedrate reductions vs the legacy's 5207" as a real
  behavioural difference worth investigating on a test print. **That was wrong.** Those
  are counts of *emitted lines*, not of decisions: the legacy sometimes writes a
  superseded `G1 F<slicer>  ; Slicer Speed` line before its replacement, and we do not.

  Comparing the feedrate actually in force at each of the 51,239 extruding moves — valid
  because the `G1 X/Y/E` sequence is identical across all three files:

  | | legacy | ours |
  |---|---|---|
  | moves slowed | 92.7% | 93.4% |
  | moves sped up | **0.0%** | **0.0%** |
  | median slowed to | 66% of slicer speed | 67% |
  | most extreme reduction | 31% | 33% |

  Neither tool ever increases a feedrate, confirming the safety property in
  [ALGORITHM.md §2](ALGORITHM.md) holds in both.

  Residual difference: median **1.6%**, 79% of moves within 5%, mean absolute 1265 mm/min
  against a mean feedrate of 29,510 mm/min. Bidirectional (ours higher on 59% of moves,
  lower on 35%), consistent with small temperature-curve differences propagating through
  the flow→feedrate inversion rather than any systematic bias.

  **No phase shift.** Correlating the two feedrate traces at offsets from −400 to +400
  moves, zero offset fits best by a factor of three (1265 vs ~4000 mm/min mean error at
  any shift). So the difference is not a lead/lag effect.

### M8a complete: local web UI <a name="web-ui-superseded-qt"></a>

**Supersedes the Qt frontend planned in earlier phases.** No Qt was installed on the
development machine; rather than treat that as a blocker, a local web UI was built
instead, and on reflection it is the better long-term choice — it is the same shape as
the eventual OrcaSlicer cloud-plugin integration, whereas Qt was always going to be
thrown away once that plugin existed. `sb53::web` lives in `cli/`, calls core through the
same interfaces the CLI uses, and contains no algorithm logic — the `core-has-no-qt`
architectural guard is unaffected.

`flowtemp serve [--port 8765]` binds to **127.0.0.1 only** (never exposed to the
network — the process reads/writes arbitrary files and opens native dialogs) and serves a
single-page UI: file picker (`GetOpenFileNameW` via `/api/browse`), calibration form,
Analyse, Process & write.

**Safety property, verified end to end:** the web UI **never overwrites the input file.**
Output is a filename *suffix* (default `-flowtemp`); a second run numbers the file rather
than clobbering the first; if a suffix would resolve back to the input the request is
refused. This was a deliberate fix after the first design took a full path and defaulted
to overwriting — correct for the CLI's slicer-hook use case, dangerous for a UI where you
just browsed to a file and clicked a button. The CLI's `process --out` still overwrites
in place when no `--out` is given, because OrcaSlicer's post-processing contract requires
it; that split is intentional, not an inconsistency.

**Chart controls:** expand (fullscreen, Escape closes), zoom in/out/reset, drag to pan,
scroll to zoom about the cursor. Below the chart, **"worth a look"** auto-detects and
highlights the moments a user actually wants to inspect — peak flow, hottest/coolest
points (flagged when they sit at the calibrated band's edges), the sharpest one-second
temperature swing, and the fastest layer. Clicking one zooms straight there. This is not
decorative: on a real 192-layer benchy it surfaced **layer 167 completing in 0.31 s**,
which nobody would spot by eye and is exactly the case fast-layer cooling exists for.
Verified by driving the actual rendered page (`mcp__Claude_Browser__javascript_tool`),
not by grepping the HTML — markup being present has previously masked broken behaviour.

**User-editable overrides**, all optional and all off/blank by default:
- **Printer limits** (max velocity, max acceleration, square corner velocity, Z velocity,
  Z acceleration) — written into an amended `config.json` in the scratch directory, never
  the user's file. Z is called out specifically in the UI because it is the *only* machine
  limit measured to change print time on a small model (192 layers = 192 Z moves; 20→100
  mm/s took 7m58s→7m15s on the reference benchy).
- **Flow bounds** (`--min-flow`, `--max-flow`) with an explicit on/off switch for the
  hard ceiling. **The hard limit is marked experimental in the UI itself**, in plain
  language: it does not hold flow at the entered number (the real constraint is what the
  filament can flow at the *planned temperature*; the ceiling only removes moves that
  were escaping that budget), it changes feedrate reductions from ~2,000 to ~49,000
  (essentially every extruding move), and no physical print has tested it. Off is the
  default and the behaviour validated against the legacy tool.
- **Fast-layer cooling** (`--cool-below`, `--cool-drop`) — off by default, applied before
  slew limiting, clamped to the filament's calibrated minimum.

**A cross-check against silent misconfiguration was added and is load-bearing:** the
scanner now reads the slicer's own `; estimated printing time` comment, and
`checkTimingAgainstSlicer` warns (`Code::PrinterConfigMismatch`) when computed time
disagrees with it by more than ~40%. This is the fix for the exact failure class recorded
below at "printer config is 12x wrong" — a config describing the wrong printer produces
an entirely self-consistent but wrong result, and this is the only independent check
available. Verified firing correctly against a deliberately stale config (13m 1s
computed vs. 7m 19s slicer estimate) and staying silent once the correct `printer.cfg`
values were loaded (7m 15s vs. 7m 19s, under 1% apart).

### M8b in progress: saved profiles — storage done, UI not started

Requested workflow improvement: **save printers, and save filament profiles within a
printer that inherit most settings and override one or two.** All calibration is
currently typed into the form on every run, which is the main remaining friction.

**`core/include/sb53/ProfileStore.hpp` + `.cpp` are complete and tested** (8 new tests,
61/61 total). Design mirrors the legacy `EXTRUDER` → `FILAMENT` containment deliberately,
because that containment *is* the requested feature: a `SavedFilament` belongs to a
`SavedPrinter` and carries only what differs from it, so "keep most settings, change one"
means duplicating a filament under the same printer and editing a field — never
re-entering machine or hotend values. Stored as hand-rolled JSON (no dependency pulled in
for this) at `%LOCALAPPDATA%\flowtemp\profiles.json`; writes go to a temp file and are
renamed into place so an interrupted save cannot truncate existing data. **Deliberately
not written to the legacy `Config.sdb`** — that file is irreplaceable user calibration
data (ADR-0003). The logic lives in `core`, not the web layer, so the eventual OrcaSlicer
plugin inherits it rather than reimplementing it.

**What is NOT done: nothing in the web UI reads or writes this yet.** The form still
starts blank every time. Remaining work, small and fully specified:
- `GET /api/profiles` → the printer/filament tree as JSON
- `POST /api/profiles/save` → upsert a `SavedPrinter`/`SavedFilament` from the current
  form fields
- Two dropdowns (printer, filament-within-printer) + a name field + Save/Duplicate buttons
- Selecting a printer fills the machine-limit fields; selecting a filament fills only the
  filament fields, leaving the machine fields alone
- **Importing from `Config.sdb` must invert the bias** (`ourBias = 10 - stored`) — see the
  ADR-0006 addendum — and should say so explicitly when it does, the same way the timing
  mismatch warning is explicit rather than silent.

### M8c in progress: CI/CD pipeline

`.github/workflows/ci.yml` and `security.yml` were added: build matrix (Windows MSVC
release+debug, Linux GCC+Clang), ctest, ASan+UBSan, clang-tidy, coverage with a 60% floor,
CodeQL, gitleaks, licence compliance, CycloneDX SBOM, libFuzzer over the scanner/parser/
profile-reader input boundaries, and two architectural guards (`core` has no Qt, `core`
never prints). Branch protection on `main` requires `windows-msvc-release`, `linux-gcc`,
`architectural guards`, `licence compliance` to merge; deliberately does **not** require
coverage/fuzz/CodeQL, which are slower and occasionally flaky — a required check people
learn to ignore is worse than none. `enforce_admins` is off so the repo owner is never
locked out. GitHub-side settings (private vulnerability reporting, secret scanning +
push protection, Dependabot alerts) were enabled via `gh api`, not the web UI.

**First pipeline run: all 3 Windows jobs passed, all Linux/CodeQL/fuzz/coverage jobs
failed.** This is exactly the value proposition of a second compiler — every failure
traced to portability defects invisible on MSVC:

| Defect | Where | Why MSVC didn't catch it |
|---|---|---|
| `std::string out` shadowing the `std::ostream& out` parameter | `GcodeRewriter.cpp` (inside `replaceWord` lambda) | MSVC has no `-Wshadow` equivalent enabled by default; GCC/Clang do |
| Unused `slice()` helper, dead since an earlier refactor | `ProfileStore.cpp` | MSVC doesn't warn on unused free functions the way `-Wunused-function` does |
| `std::isfinite` used without `#include <cmath>` | `test_curvecompare.cpp`, `test_planner.cpp` | MSVC's header graph pulls in `<cmath>` transitively; not guaranteed elsewhere |
| `std::abs(double)` used with only `<algorithm>` included | `cli/main.cpp` (feedrate diff in `kinematics-check`) | **Not just a portability nit** — `std::abs(double)` lives in `<cmath>`; if only `<cstdlib>`'s `int std::abs(int)` were visible, the double argument would silently truncate via implicit conversion. Every duration compared there is under 1 second, so this would have made the whole per-move diagnostic silently report 0 for every move — exactly the "looks fine, is wrong" failure class this project exists to catch, just in the test tooling instead of the product |
| `-Wnull-dereference` false positive inside libstdc++'s own `<streambuf>` | (not our code) | GCC-only; confirmed by reading the error location (`/usr/include/c++/13/streambuf`, not any file in this repo) — an optimizer-level diagnostic emitted after inlining, which does not reliably attribute to the header it came from. Removed from the warning set with a comment explaining why, rather than silenced project-wide or ignored |

Fixed in the commit immediately following the pipeline's first run. **Rebuilt and
re-tested on MSVC only (61/61 green)** — there is no local Linux/GCC/Clang toolchain
available on the development machine (checked: no WSL distro installed, no Docker, no
native compiler), so these fixes are verified by precisely reading each compiler's error
output and reasoning about the fix, not by reproducing the failure locally. **The
authoritative verification is the next CI run on this branch** — check
`gh run list --branch claude/frontend-modernization-plan-9444f8` before assuming green.

A broader sweep for the same class of mistake (missing `<functional>`, `<memory>`,
`<numeric>`, `<limits>`, `<optional>`) was run across the whole tree and found no further
instances — the remaining hits were false positives resolved through the file's own
paired header (e.g. `WebUI.cpp` gets `<functional>` via `WebUI.hpp`, which does include it
directly), which is a controlled, intentional transitive path and not the kind of
implicit standard-library-internals dependency that caused the real bugs above.

### In progress: own motion planner (ADR-0007)

Goal: drop the `klipper_estimator.exe` subprocess so the tool is a single artefact and
can be **linked** into the OrcaSlicer plugin rather than spawned. See
[ADR-0007](adr/0007-own-motion-planner.md).

**Status: move extraction is exact; timing is 13% fast.**

```
                    moves   total time
reference           58352     478.62s
built-in            58352     416.43s
difference             +0     -62.19s  (-13.0%)

per-move: mean |diff| 0.00107 s, worst 0.00976 s at move 52487
          (reference 0.01740 s, ours 0.00764 s), only 10% within 1%
```

Reproduce with:
```powershell
./build/bin/flowtemp.exe kinematics-check <body.gcode> --estimator bin/klipper_estimator.exe
```
(undocumented dev command; runs both planners on one file and diffs them)

**Done and verified**
- Move extraction matches exactly — 58,352 both. The rule: a `G0`/`G1` with at least one
  of X/Y/Z/E, **excluding** commands that neither travel nor extrude (`G1 X10 Y10` when
  already there). Dropping those no-ops is what fixed the last off-by-one.
- `config.json` parsing, including `move_checkers`.
- Junction-deviation formula verified analytically: a 90° corner at scv 30 and accel
  150000 yields exactly 30 mm/s, as it must by definition.

**Ruled out**
- `minimum_cruise_ratio` as a reduced look-ahead acceleration: changed the total by only
  2 s of the 62 s gap. Kept anyway (it is real Klipper behaviour) but it is not the cause.

**Next investigation.** We are uniformly *too fast*, so something limits velocity in the
reference that we are not applying. In likely order:

1. **Dump per-move detail for the worst offenders.** `dump-moves` gives only time and
   flow, but implied velocity = distance / duration, so comparing that against our
   `maxVelocity`, `entryVelocity` and `exitVelocity` at move 52487 should show which cap
   is missing. Do this first — it is the highest-information step.
2. **Check whether the reference caps velocity per-axis**, not just along the
   `axis_limiter` direction. Our `maxVelocity / |dot(unit, axis)|` may be too permissive
   on moves that are mostly XY with a little Z.
3. **Verify the extruder limiter.** Ours converts filament mm/s to toolhead mm/s via the
   E-to-distance ratio; the reference may apply it to acceleration differently.
4. **Confirm F is per-minute in every context** and that we track the most recent value
   across bare `G1 F####` lines correctly.

**Do not remove the subprocess path until parity is demonstrated** — ADR-0007 requires it,
and every flow and temperature number downstream depends on these timings.

### Next concrete action: wire ProfileStore into the web UI

Storage is done (M8b above). What remains is small and fully specified — see the bullet
list under "M8b in progress" above for the exact endpoints and UI elements needed. Budget
this as the next single session; it does not require re-deriving anything.

### After that, in rough priority order

1. **Re-confirm CI is green**, including on Linux. Not yet verified — see "M8c" above.
2. **`SqliteProfileRepository`** — vendor the SQLite amalgamation and read the existing
   `Config.sdb` schema unchanged ([ADR-0003](adr/0003-sqlite-in-core.md)) as an *import*
   path into `ProfileStore`, not a live read path. Auto-select by matching the G-code's
   `printer_settings_id` / `filament_settings_id`, and **say so when falling back** — the
   legacy silently used row 0. **Must invert the bias on import** (`10 - stored`).
3. **Continue the motion planner** (ADR-0007) — see its own section above for the ranked
   next investigation steps. Not urgent: the subprocess path works and is validated.
4. Real print validation (M9) — blocked on the user having hardware time, not on any
   remaining engineering task.

Qt frontend is **no longer planned** — superseded by the web UI (M8a, see above).

---

## Blocked / needs input

| Item | Needed from | Blocks |
|---|---|---|
| ~~Sample G-code files~~ | ~~User~~ | ✅ **resolved 2026-08-07** |
| **A physical printer for validation** | User | M9 only |

Sample G-code was supplied: 19 OrcaSlicer 2.4.2 files from a real troubleshooting session.
See [testdata/README.md](../testdata/README.md) for provenance and an important caveat —
most of the *processed* files are silent failures, not references, and only two are usable
as expected output.

Committed fixtures (~90 KB each) cover the normal path, arcs, and an already-processed
file. Full-size references are git-ignored but hash-pinned in `testdata/manifest.json`;
tests that need them **skip with a message** when absent, so a fresh clone stays green.

---

## Conventions

- **One milestone per branch**, merged when its exit criteria are met.
- **Commits are small and self-describing.** A commit that needs the conversation around
  it to be understood is too big.
- **Every non-obvious decision gets an ADR**, numbered sequentially in `docs/adr/`.
  Cheap to write, and the reason a decision was made is exactly what evaporates over a
  months-long gap.
- **Tests encode intent, not implementation.** A test that breaks on refactoring is a
  liability; a test that documents a physical constraint is an asset.
- **`core/` never includes Qt.** Enforced in CI. See [ADR-0002](adr/0002-core-ui-separation.md).
- **No magic numbers.** Calibration values are user data, not constants
  ([ALGORITHM.md §3](ALGORITHM.md)). If a number appears in the source, it needs a comment
  explaining where it came from.

## If you are picking this up cold

1. Read [ALGORITHM.md](ALGORITHM.md) end to end. It is written for exactly this situation.
2. Skim the ADR titles in [adr/](adr/); read any that touch what you are about to change.
3. Check the milestone table above for where work stopped.
4. Build and run the tests before changing anything, to confirm the baseline is green.
5. When you stop, update this file. That is the whole contract.
