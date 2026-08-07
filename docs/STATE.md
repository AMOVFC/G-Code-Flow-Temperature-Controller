# Project State

> **Read this file first when resuming work.** It is the single source of truth for
> "where are we and what happens next". Update it at the end of every work session,
> even a short one. A stale STATE.md is worse than none.

**Last updated:** 2026-08-07
**Current milestone:** M2 — Data model + G-code scanner
**Build status:** ✅ green — 8/8 tests passing (MSVC 19.44, Ninja, C++20)

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
| **M2** | **Data model + G-code scanner** | 🟡 in progress | Scanner extracts markers, IDs, body bounds, M82/M83 guard from real files; unit tested |
| M3 | Estimator integration + flow aggregation | ⚪ not started | Per-second flow timeline produced from a recorded fixture, no live subprocess in tests |
| M4 | Temperature planner | ⚪ not started | Blend → smooth → map → slew-limit, each unit tested; plan dumped as CSV |
| M5 | G-code rewriter | ⚪ not started | Valid output G-code; feedrate clamp and PA gating verified |
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

### In progress (M2)
Nothing started yet. `Model.hpp` defines the types the scanner will populate
(`ScanResult`, `ExtrusionMode`); `GcodeScanner.cpp` does not exist.

### Next concrete action
Implement `GcodeScanner` — the first pass over an input file
([ALGORITHM.md §9](ALGORITHM.md)). It needs no floating-point maths, no estimator and no
database, which makes it the cheapest possible way to get real behaviour under test.

It must establish:
1. **Extrusion mode** (`M82`/`M83`) — reject absolute with `Code::AbsoluteExtrusionUnsupported`.
2. **Already-processed** detection via `kProcessedMarker` (`Version.hpp`).
3. **Print body bounds** — note the emission asymmetry documented in
   [legacy/algorithm-map.md](legacy/algorithm-map.md): the line marking the *start* is
   part of the body; the line marking the *end* is not. Worth a dedicated test.
4. **Slicer identity comments** (`printer_settings_id`, `filament_settings_id`,
   `filament_type`) for profile auto-selection.

Synthetic fixtures are enough to build and test all of this — real sample G-code is not
required until M3.

---

## Blocked / needs input

| Item | Needed from | Blocks |
|---|---|---|
| **Sample G-code files** — 3–4 sliced files, ideally from OrcaSlicer, at least one with arc moves (`G2`/`G3`) and one small file for fast iteration | User | M2 testing, M7 entirely |
| **A physical printer for validation** | User | M9 |

No sample G-code exists anywhere in this repository — this is the one hard external
dependency. Everything through M6 can be built without it using synthetic fixtures, but
M7 cannot start.

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
