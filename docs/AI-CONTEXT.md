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

### Immediate: profile management in the UI (requested, not started)

The user wants to **save printers, and save profiles within a printer that inherit most
settings and override one or two**. Nothing exists yet — all calibration is typed in.

Suggested shape, matching the existing schema so `Config.sdb` import stays possible:

```
Printer (= EXTRUDER row)   name, kinematics/config.json, rise, fall, smoothing,
                           start macro, temperature token
  └─ Filament (= FILAMENT row)  name, type, 3 flow points, 3 temps, 3 PA values,
                                PA on/off, bias
```

- Store as JSON under `%LOCALAPPDATA%\flowtemp\profiles.json` — do **not** write to the
  legacy `.sdb`, which is irreplaceable user data (ADR-0003).
- Inheritance is the point: a filament belongs to a printer and should only carry what
  differs. A "duplicate this profile" action is probably the whole feature.
- `IProfileRepository` already exists in `Profiles.hpp` as the intended seam. Put the
  logic in **core**, not the web layer, or the future Orca plugin reimplements it
  (ADR-0002, ADR-0003).
- Import from `Config.sdb` should apply the bias inversion in §4 and **say so**.

### Also outstanding

- **Motion planner** (ADR-0007): extraction exact, timing 13% fast. Ranked suspects are in
  STATE.md. Do not delete the subprocess until parity is proven.
- **Reported time is the input's, not the output's.** No second estimator pass, and the
  `; estimated printing time` comment in the output is never rewritten.
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
