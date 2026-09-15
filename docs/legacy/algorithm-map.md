# Legacy source map

A navigation aid for `Source/V1.1/`, mapping legacy code to the specification in
[ALGORITHM.md](../ALGORITHM.md).

**Use this when** you need the ~15% of behaviour the public README does not describe.
**Do not use this** as a porting checklist — this is a clean-room rewrite
([ADR-0001](../adr/0001-clean-room-rewrite.md)). Read for understanding, then implement
from the specification.

> ⚠️ The legacy does not compile (see [unit9-recovered.md](unit9-recovered.md)). Read it;
> do not try to build it.

## Where the real logic is

`Unit1.pas` is 1,422 lines, but only ~750 are domain logic. The rest is repetitive UI
glue — fifteen near-identical `EditNChange` / `EditNKeyPress` handler pairs. `Unit1.dfm`
is 4,206 lines of generated form layout and contains no logic.

| Legacy | Lines | Spec section | Notes |
|---|---|---|---|
| `readOriginalGcode` | 743–993 | §4, §9 | Two passes: scan for markers/bounds, then aggregate flow |
| ↳ marker + bounds scan | 782–817 | §9 | Body extraction. Emission ordering is subtle — see below |
| ↳ estimator invocation | 853 | §4 | Shell redirection through `cmd.exe` |
| ↳ move dump parse | 857–906 | §4 | `Flow`/`Time` prefixed records |
| ↳ retract state machine | 863–874 | §4 | Negative flow zeroes and arms; next sample also zeroed |
| ↳ per-second bucketing | 887–904 | §4 | Bucket width fixed at 1 s (`842`) |
| ↳ profile auto-match | 910–956 | §3 | Matches slicer IDs against the database; falls back to row 0 |
| `calculateAverages` | 655–739 | §5 | The temperature planner |
| ↳ avg/max blend | 682 | §5.1 | **Deliberately not reproduced** — [ADR-0006](../adr/0006-explainable-blend-and-smoothing.md) |
| ↳ smoothing call | 686 | §5.2 | |
| ↳ flow → temperature map | 694–700 | §5.3 | Two linear segments pivoting at the mid point |
| ↳ slew limiting | 706–712 | §5.4 | Asymmetric rise/fall |
| ↳ initial temperature | 719 | §9 | First-layer value |
| `SmoothSeries` | 264–285 | §5.2 | Centered moving average; divisor is the *clamped* span (line 278) |
| `FindClosestIndex` | 316–339 | §6 | Binary search over cumulative filament. **Uninitialised on 3 paths** |
| `generateTempOutput` | 473–651 | §6, §7, §8 | The rewrite pass — the densest function |
| ↳ marker parsing | 514–549 | §7 | Layer height / line width / feature type |
| ↳ feedrate extraction | 552–557 | §6 | Comment stripping, `F` word |
| ↳ temperature lookup | 565 | §6 | Indexed by cumulative filament — the key insight |
| ↳ flow inversion | 582–593 | §6 | Temperature → permitted flow |
| ↳ PA gating | 596 | §8 | Exactly four feature types |
| ↳ cross-section + feedrate | 614–617 | §7 | |
| ↳ clamp to sliced speed | 621–622 | §2 | `min(recommended, sliced)` — the safety property |
| ↳ filament accumulation | 602–628 | §6 | Requires relative extrusion |
| `generateOutput` | 343–469 | §10 | Second estimator pass, splice, final assembly |
| ↳ duration formatting | 399–407 | §10 | **Buggy over 24 h** |
| ↳ start macro rewrite | 419–428 | §9 | **Fixed-width splicing** |
| `ExecNewProcess` | 289–312 | — | Replaced by `IProcessRunner` |
| `refreshList` | 1040–1060 | — | Replaced by `IProfileRepository` |
| Slicer integration | 1178–1198 | §10 | Overwrites `argv[1]` in place |
| Headless auto-run | 1333–1342 | — | Given `argv[1]`, runs with no interaction — this is what makes differential capture practical |
| Startup / locale | 1202–1230 | — | **Locale handling not reproduced** |

## Subtleties worth knowing

**Body-bounds emission ordering** (`782–817`). The line that *marks* the print start is
itself written to the extracted body; the line that marks the print end is **not**. Get
this wrong and every downstream artefact shifts by one line. Worth a unit test.

**The estimator is a custom fork.** README:169 links `sb53systems/klipper_estimator`, not
upstream Annex-Engineering. Its output format is not guaranteed to match upstream. Treat
`bin/klipper_estimator.exe` as pinned.

**Profile matching is implicit.** The legacy auto-selects extruder and filament profiles
by matching `; printer_settings_id` and `; filament_settings_id` comments against the
database, silently falling back to the first row when there is no match. Any differential
comparison must ensure both tools select the same profiles, or the outputs differ for reasons
having nothing to do with the algorithm.

## Other units

| Unit | Lines | Role |
|---|---|---|
| `Unit2` | 106 | Fetch printer config from Moonraker (`dump-config`), or import a local file |
| `Unit3`, `Unit4`, `Unit7` | 35 / 58 / 29 | About / help screens. Large `.dfm`, negligible logic |
| `Unit5` | 135 | Extruder profile dialog |
| `Unit6` | 118 | Filament profile dialog — **contains defect #1** |
| `Unit8` | 48 | Per-feature speed/quality dialog |
| `Unit9` | — | Missing. Serial console — see [unit9-recovered.md](unit9-recovered.md) |
