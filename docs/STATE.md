# Project State

> **Read this file first when resuming work.** It is the single source of truth for
> "where are we and what happens next". Update it at the end of every work session,
> even a short one. A stale STATE.md is worse than none.

**Last updated:** 2026-08-07
**Current milestone:** M5 — G-code rewriter
**Build status:** ✅ green — 30/30 tests passing (MSVC 19.44, Ninja, C++20)

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
| **M5** | **G-code rewriter** | 🟡 in progress | Valid output G-code; feedrate clamp and PA gating verified |
| M6 | CLI end-to-end | ⚪ not started | `sb53 process in.gcode` produces a printable file; **usable as an Orca post-processing script** |
| M7 | Differential validation vs. legacy | ⚪ not started | Temperature curves track the legacy binary within tolerance on sample files |
| M8 | Qt frontend | ⚪ not started | Feature parity with the legacy UI, restructured per ARCHITECTURE.md |
| M9 | Physical validation | ⚪ not started | Test prints succeed on real hardware |

**M6 is the real milestone.** At that point the tool is genuinely useful with no GUI at
all, and it is the artifact the future OrcaSlicer cloud plugin needs.

---

## Current position

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

### In progress (M5)
Nothing implemented yet.

### Next concrete action
Implement `GcodeRewriter` ([ALGORITHM.md §6](ALGORITHM.md)) — the pass that actually
modifies the file. Walk the print body maintaining cumulative extruded filament, and at
each move:

1. Look up the planned temperature via `TemperaturePlan::temperatureAtFilament` (already
   written and tested). Emit `M104 S…` when it changes.
2. Invert to a flow budget with `temperatureToFlow` (already written).
3. Convert flow to feedrate using the extrusion cross-section
   `h·(w−h) + π(h/2)²`, tracking `;HEIGHT:` and `;WIDTH:` markers as they change.
4. **Clamp: `min(recommended, sliced)`.** Never faster than the slicer asked. This is the
   tool's core safety property ([ALGORITHM.md §2](ALGORITHM.md)) and needs an explicit
   test.
5. Emit `SET_PRESSURE_ADVANCE` only where `allowsPressureAdvanceChange` permits — already
   written and tested.

Output formatting confirmed from real legacy output: `M104 S212.7` (one decimal, no
trailing zero), and speed lines carry `; Keep Slicer Speed` / `; Reset Speed Before
Retraction` comments.

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
