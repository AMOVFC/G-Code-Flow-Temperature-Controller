# Context for an AI assistant resuming this project

Everything here was learned the hard way and is **not** obvious from the code. Read this
before touching anything; it will save you the same hours it cost.

For *what the tool does* see [ALGORITHM.md](ALGORITHM.md). For *where work stopped* see
[STATE.md](STATE.md). For *why decisions were made* see [adr/](adr/). This file is the
tacit knowledge that fits in none of those.

---

## 1. The single biggest source of wrong results: stale artefacts

**This burned three separate hours across one session, in three different disguises.**
Whenever a result looks like the *old* behaviour, suspect a stale artefact before
suspecting the logic.

| What went stale | Symptom | How it was caught |
|---|---|---|
| `bin/config.json` (the original author's printer, 300 mm/s / 6000 mm/s²) | 13-minute estimate, every flow figure ~halved | Compared against the slicer's own estimate |
| `C:\flowtemp\flowtemp.exe` after a `package.ps1` that aborted | A safety fix "verified working" while still overwriting the input | Output filename lacked the expected suffix |
| A running `flowtemp.exe` holding the binary | `LNK1168: cannot open flowtemp.exe for writing` | Build failure |

**Rules that follow:**

- Before testing a fix, assert the packaged and built binaries have the same size.
  `package.ps1` prints both; check them.
- `Stop-Process -Name flowtemp -Force` before every build. Windows locks a running exe.
- Never run `build/bin/flowtemp.exe` without `--estimator`. It auto-detects the repo's
  `bin/config.json`, which describes a **different printer**. Use `C:\flowtemp\` instead.

## 2. Build gotchas that will cost you a cycle each

- **MSVC caps a string literal at ~16 KB.** The embedded web page outgrew it and failed
  with `C2026`. It is now split into `kPagePart1/2/3` and joined by `fullPage()`. If you
  add much markup, split again rather than fighting it.
- **`windows.h` defines `min`/`max` macros** which break `std::min`. Always define
  `NOMINMAX` alongside `WIN32_LEAN_AND_MEAN`.
- **`shellapi.h` must be included *after* `windows.h`.** Alphabetising the includes breaks
  the build.
- **Warnings are errors on `core`.** `-Wconversion` and shadowing (`C4456`) will stop you.
  That is deliberate — this code mixes doubles and integer indices constantly.
- `tools/build.ps1` tees to `build/build.log` and prints matching error lines, because
  PowerShell's native-command handling silently ate compiler output more than once.
- **MSVC is meaningfully more permissive than GCC/Clang about the things that matter
  here**, and CI's first Linux run found several real defects at once (all fixed by
  2026-08-10, see `STATE.md` "M8c" for the full table): a local variable shadowing an
  outer parameter of the same name, a dead function `-Wunused-function` catches and MSVC
  doesn't, and — the one worth internalising — **`std::isfinite`/`std::abs(double)` used
  without `#include <cmath>`**, relying on MSVC's header graph pulling it in
  transitively. The `std::abs` case was not merely a build failure risk: if only
  `<cstdlib>`'s integer overload had been visible, a `double` argument would have
  silently truncated via implicit conversion rather than failing to compile. **Always
  include what you use directly; never rely on a header providing something transitively,
  even when it happens to compile.**
- **There is no local Linux/GCC/Clang toolchain on the Windows dev machine** — checked:
  no WSL distro installed (`wsl.exe --list` reports none), no Docker, no native compiler.
  Linux-only build failures can only be diagnosed by reading the CI log output precisely
  (`gh run view --job <id> --log-failed`) and reasoning from the error text — not by
  reproducing locally. Fix, rebuild+test on MSVC to confirm no regression there, push, and
  treat the **next CI run** as the actual verification. Do not claim a Linux fix is
  "verified" from this machine; say "fixed per the error text, not locally reproducible,
  pending CI confirmation" instead.
- **`-Wnull-dereference` produces a false positive inside libstdc++'s own `<streambuf>`**
  on the CI image's GCC 13, triggered by ordinary `std::ofstream` use at `-O2`. Confirmed
  by reading the error location (`/usr/include/c++/13/streambuf`, not project code) — this
  is a known category of GCC issue where an optimizer-level diagnostic (emitted after
  inlining) fails to attribute correctly to system headers. It is removed from
  `cmake/SB53Warnings.cmake`'s GCC/Clang warning set with a comment, not suppressed
  per-file or ignored silently.

## 3. Testing traps

- **`/api/process` and `flowtemp process` overwrite the input when no output is given.**
  That is correct for a slicer hook, but it means running a test twice in a row hits the
  already-processed guard and returns 400. Always pass an output path, or work on a copy.
- **Native file dialogs cannot be tested headlessly** — `GetOpenFileNameW` blocks for a
  human. Verify the endpoint and wiring; the dialog itself needs a real click.
- **The legacy GUI cannot be driven from a script.** It opens a modal and needs an
  interactive desktop, so `SendKeys` fails from a non-interactive shell.
  `tools/make-testpair.ps1` detects this and prints manual steps.
- **Verify UI work by driving the real page** (`mcp__Claude_Browser__javascript_tool`),
  not by grepping the HTML for element IDs. Markup being present proved nothing on at
  least one occasion where the behaviour was broken.

## 4. Facts about the user's setup

Established by inspection; do not re-derive.

- **Printer:** CoreXY, `max_velocity 2500`, `max_accel 250000`, `SCV 50`,
  Z `100 mm/s / 10000 mm/s²`, extruder `120 mm/s / 10000`. Arcs at `0.1 mm`.
  Correct config lives at `testdata/printer-configs/awd-v0.json`.
- **Filament** (`"Elegoo HS PLA+ awd hott"`): flow **1 / 80 / 105** mm³/s →
  **220 / 280 / 310** °C, smoothing 20, PA off, `SPEED_QUALITY_OPT` 3.
- **Extruder** (`awd v0`): rise 5 °C/s, fall **1 °C/s**.
- **Their hotend runs hot by design** — the probe sits high and reads low, so ~270 °C for
  PLA is intentional. **Never flag it as a calibration error.**
- Their real database: `D:\SB53_G-Code_Flow_Temperature_Controller_V1.1\Config\Config.sdb`.
  Read it with `python tools/dump-profiles.py <path>`.

> ⚠️ **Bias is inverted.** Their DB stores `SPEED_QUALITY_OPT = 3`; our equivalent is
> **7**. Any importer must apply `ourBias = 10 - stored`. Verified empirically by sweep;
> getting it wrong yields prints ~14 °C colder with no warning.

## 5. Measurements already taken — do not repeat these

Each cost a full estimator run or more.

| Question | Answer |
|---|---|
| Does raising accel/velocity/SCV speed up a benchy? | **No.** Unlimited motion changes 7m 58s → 7m 52s |
| What *does*? | **Z only.** 20 → 100 mm/s took 7m 58s → 7m 15s (192 layers = 192 Z moves) |
| Does slicing faster help? | Saturates. 3× speeds only reaches 6m 20s |
| Where does the time go? | 63% extruding, 37% travel/Z/accel |
| Cost of processing | +22 s on a 7m 15s print (~5%) |
| Ours vs legacy, temperature | correlation **+0.9947**, RMS 0.88 °C, offset −0.24 °C |
| Ours vs legacy, feedrates | 92.7% vs 93.4% of moves slowed; **neither ever speeds up**; median 1.6% apart; **no phase shift** |
| Hard flow limit on | reductions 2,247 → 49,476; time 7m 37s → 7m 36s; peak 106.6 → 69.1 |

## 6. Bugs found by differential testing that unit tests could not have caught

Worth internalising, because the same class will recur.

- **Unretract drift.** `MoveDumpParser` zeroed retract *and* unretract; `GcodeRewriter`
  counted any positive `E`. The rewriter's filament coordinate ran ahead of the plan, and
  because the plan holds its last value past the end, **temperature commands silently
  stopped partway through the print.** Every component was individually correct and
  tested. The bug lived only in the agreement between two of them. Correlation went
  +0.4455 → +0.8672 on the fix. Both now share `detail::isExtrudingMove`.
- **Flow cap did nothing.** Three layers: only bare `G1 F####` lines were clamped; the
  peak-producing moves carry no `F` at all; and the recommended feedrate came from the
  *declared* `;WIDTH:`/`;HEIGHT:` markers, which understate real extrusion on gap fill and
  seams. Fixed by using each move's own `E ÷ distance`.

## 7. Where the next work is

### Immediate: wire the (already-built) profile store into the UI

**Storage is done as of 2026-08-10** — do not redesign it, extend it. The user wants to
**save printers, and save profiles within a printer that inherit most settings and
override one or two**. `core/include/sb53/ProfileStore.hpp` + `.cpp` implement exactly
this, tested (8 tests, part of the 61 total):

```
SavedPrinter    name, rise, fall, smoothing, cooling settings, start macro/token,
                machine limits (velocity, accel, SCV, Z velocity, Z accel)
  └─ SavedFilament  name, type, 3 flow points, 3 temps, bias, flow bounds, PA switch
```

Stored as hand-rolled JSON (no library dependency) at
`%LOCALAPPDATA%\flowtemp\profiles.json`, atomic write (temp file + rename). **Not**
written to the legacy `.sdb` — that stays irreplaceable user data (ADR-0003), read-only
via `tools/dump-profiles.py` for import.

**What is missing is only the UI wiring**, all in `cli/WebUI.cpp`:
1. `GET /api/profiles` — serialise `ProfileStore::load(ProfileStore::defaultPath(), ...)`
   to the same JSON shape the store already produces (`toJson()` exists; reuse it or adapt).
2. `POST /api/profiles/save` — read the current form fields, build a `SavedPrinter`/
   `SavedFilament`, `store.upsert(...)`, `store.save(...)`.
3. Two `<select>` elements (printer, then filament-within-that-printer) + a name input +
   Save/Duplicate buttons in the page's calibration fieldset.
4. Selecting a printer fills the machine-limit fields only; selecting a filament fills
   only the filament fields. Do not let one clobber the other.
5. A "Duplicate" action is copy-then-rename — that alone delivers "keep most settings,
   change one," which is the actual feature being asked for.

This is scoped small on purpose. Do not expand it into a bigger redesign; the storage
layer was deliberately finished as a clean stopping point in the previous session for
exactly this reason.

### Also outstanding

- **CI/CD pipeline**: Windows jobs green, Linux jobs failed on first run and were fixed
  by reading error text (no local Linux toolchain available — see §2). **Not yet
  re-confirmed green** — check `gh run list --branch <branch>` before assuming the fix
  landed. If still red, get the exact error with `gh run view --job <id> --log-failed`
  before changing anything; do not guess.
- **Motion planner** (ADR-0007): extraction exact, timing 13% fast. Ranked suspects are in
  STATE.md. Do not delete the subprocess until parity is proven.
- **Reported time is the input's, not the output's.** No second estimator pass, and the
  `; estimated printing time` comment in the output is never rewritten.
- **`SqliteProfileRepository`** (reading the legacy `.sdb` as an *import* source into
  `ProfileStore`, not a live read path) is designed but not built. Must invert the bias
  on import (§4) and say so explicitly.
- **Nothing has been validated by a real print.** This remains the only gap that cannot be
  closed from a desk.

## 8. How to work on this

- The user tests on real hardware and is direct about what is broken. Believe the report;
  reproduce before theorising.
- **State uncertainty plainly.** Several things here were reported as working and were not.
  Where a measurement is one sample on one file, say so.
- Commit messages carry the *why* and the numbers — they have been the most useful record
  when picking this back up.
- Keep `STATE.md` current; it is the agreed resumption contract.
- The user's constraint from day one: **work is intermittent and pauses for weeks.**
  Compartmentalise, and leave things in a state where the next step is written down.
- **GitHub work uses `gh`, authenticated as the user (`AMOVFC`), against their fork.**
  `origin` is the fork, `upstream` is `sb53systems/...` — never push or open a PR against
  `upstream` without being asked explicitly. One PR (#1) tracks this branch against
  `origin/main`; push to the branch and it updates automatically, no new PR needed.
  `gh` is at `$env:ProgramFiles\GitHub CLI\gh.exe` and not on `PATH` by default in a
  fresh shell — prepend it. Repo-level settings (branch protection, secret scanning,
  vulnerability reporting) were configured via `gh api`, not the web UI; see the commit
  that introduced `.github/workflows/` for the exact calls if they need reproducing.
