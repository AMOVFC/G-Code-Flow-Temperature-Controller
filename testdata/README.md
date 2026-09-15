# Test data

Two kinds of file, with different rules.

## `fixtures/` — committed

Small (~90 KB), structurally complete G-code, generated from real sliced files by
[`tools/make-fixture.ps1`](../tools/make-fixture.ps1). Header and footer are verbatim; the
print body is truncated.

**These are not printable.** They exist to be parsed. Never send one to a printer.

| Fixture | Derived from | Exercises |
|---|---|---|
| `orca-basic.gcode` | benchy, OrcaSlicer 2.4.2 | The normal path: `;HEIGHT:` / `;WIDTH:` / `;TYPE:` markers, `M83`, `PRINT_START`, `EXECUTABLE_BLOCK_END`. Two layer transitions. |
| `orca-arcs.gcode` | benchy with arc fitting | The same, plus 145 `G2`/`G3` moves. Arcs change how the estimator behaves and are a documented slow path. |
| `legacy-processed.gcode` | output of the legacy binary | An **already-processed** file: carries `; Edited by …` and 18 `M104` commands. The re-processing guard must reject this. |

Regenerate with:

```powershell
./tools/make-fixture.ps1 -InputFile testdata/reference/raw-basic.gcode -Output testdata/fixtures/orca-basic.gcode -BodyLines 2000
```

## `reference/` — NOT committed

Full sliced files, 1.7–2.2 MB each, 7.6 MB total. Deliberately git-ignored: too large to
version, and they are inputs rather than source.

Needed for milestones M3 and later, where real move volume matters, and for M7's
differential comparison against the legacy binary. Identified by SHA-256 in
[`manifest.json`](manifest.json), which **is** committed — so a mismatch is detectable
even though the bytes are not stored here.

| File | Role |
|---|---|
| `raw-basic.gcode` | Clean unprocessed input. The primary end-to-end subject. |
| `raw-arcs.gcode` | Unprocessed, with ~3990 arc moves. |
| `legacy-out-basic.gcode` | Known-good legacy output, 468 `M104` commands. |
| `legacy-out-basic2.gcode` | A second known-good legacy output, 392 `M104` commands. |

If these are missing, tests that need them **skip with a clear message** rather than
failing — a fresh clone must still go green.

## Provenance

All captured 2026-08-07 from the user's own printer setup:

- **Slicer:** OrcaSlicer 2.4.2
- **Printer profile:** `AWD V0 @ 0.4 nozzle`
- **Filament profile:** `"Elegoo HS PLA+ awd hott"` — note the embedded quote characters,
  which are part of the value as OrcaSlicer writes it. Any profile-matching code must
  cope with them.
- **Extrusion mode:** relative (`M83`) throughout — satisfying the precondition in
  [ALGORITHM.md §9](../docs/ALGORITHM.md).

## A caution about the source set

These came from a troubleshooting session, so most of the processed files are **failed
runs**, not references. Of nine files carrying the legacy's `; Edited by` header, only two
contain any `M104` commands. The other seven have an **empty** estimated-time field
(`; estimated printing time (normal mode) = `), which is what the legacy emits when total
computed time is zero — the estimator returned no moves.

Their bodies are otherwise unmodified, so the legacy produced a plausible-looking file
that had not actually been processed, and said nothing about it. That behaviour is
recorded as [known-bugs.md #11](../docs/legacy/known-bugs.md) and is a direct motivation
for the loud-failure design in `Diagnostics.hpp`.

**Only `legacy-out-basic.gcode` and `legacy-out-basic2.gcode` are valid references.** Do
not treat the others as expected output.

Note also that the 19 source files are 19 *different* slices, not one input processed
repeatedly — so they do not form raw/processed pairs. Matched pairs for M7 are generated
by running `bin/SB53-Systems.exe` against a raw file directly.
