# Legacy defects — do not reproduce

Defects found while reviewing the Delphi application (`Source/V1.1/`). Recorded so the
clean-room rewrite does not recreate them, and so anyone comparing behaviour against the
shipped binary understands why outputs differ.

**None of these are ported.** Where the legacy's behaviour is observable in its output,
expect divergence at that point — see [ADR-0005](../adr/0005-differential-verification.md).

---

### 1. Filament profile saves the wrong control

`Unit6.pas:41,64,89` persist `SPEED_QUALITY_OPT` from **`form8.TrackBar1.Position`**, but
`Unit1.pas:1079` *loads* it into **`Form1.TrackBar1.Position`**. These are two different
trackbars on two different forms.

Form8 is a separate dialog whose trackbar is never populated from the loaded profile, so
saving a filament profile silently writes Form8's design-time default rather than the
setting the user actually chose. The Speed↔Quality control therefore appears not to
persist.

**Severity:** high — silently discards user input.
**Rewrite:** the setting is a field on the filament profile; there is no widget in the
path.

---

### 2. Print time wrong for prints over 24 hours

`Unit1.pas:399-407` computes the duration breakdown as:

```
d = trunc(t / 86400)
h = trunc((t - d*86400) / 3600)
m = trunc((t - h*3600) / 60)      ← days never subtracted
s = trunc(t - h*3600 - m*60)      ← days never subtracted
```

Minutes and seconds are derived without removing the days component, so any print
exceeding 24 hours reports incorrect values. The result is written into the output file as
the estimated-time comment, so it is user-visible.

**Severity:** medium — cosmetic, but visible and wrong on exactly the long prints where
the estimate matters most.
**Rewrite:** normal decomposition, unit tested at 0 s, 59 s, 61 s, 3601 s, 86401 s, and
7 days.

---

### 3. Uninitialised return value

`Unit1.pas:316-339`, `FindClosestIndex`, leaves `Result` unassigned on three early-exit
paths: empty series, value below the first element, value above the last. Delphi returns
whatever happens to be in the register.

The single call site (`Unit1.pas:565`) is wrapped in `try/except`, and the subsequent `+1`
can also index past the end, so the practical behaviour is "the temperature update is
skipped for this move" — reached by accident rather than design.

**Severity:** medium — undefined behaviour, though its observable effect is benign.
**Rewrite:** the lookup returns an explicit optional; "no applicable plan entry" is a
represented state, and the boundary cases are unit tested.

---

### 4. Fixed-width temperature string surgery

`Unit1.pas:420` and `427` rewrite the initial temperature by splicing at hard-coded
character offsets (`+4`, `copy(Lines, 11, …)`) that assume a **three-digit** temperature.

A two-digit value (below 100 °C) or four-digit value leaves fragments of the old number or
truncates the rest of the line — corrupting the start macro.

**Severity:** high for affected users — produces malformed G-code, and the start macro is
the worst place for that.
**Rewrite:** parse and re-emit the parameter; never splice by offset.

---

### 5. Locale-dependent numeric parsing

`Unit1.pas:1213-1216` derives the decimal separator from the system locale, then performs
string substitution before every float parse and format (a dozen or more sites), including
**mutating the text in UI fields** at `578-580`.

Output correctness therefore depends on the machine's regional settings, and G-code is a
`.`-decimal format regardless of locale.

**Severity:** high — wrong output on comma-decimal systems.
**Rewrite:** core parsing and formatting are unconditionally locale-invariant. A user in
any locale gets identical, correct output.

---

### 6. Intermediates written beside the executable

All scratch files (`tempOutput`, `KEOutput`, `tempOut`, `Output.gcode`) are written to the
application's own directory (`Unit1.pas:1210` and throughout).

This fails when installed under `Program Files` or any read-only location, and two
concurrent runs overwrite each other's intermediates. The same `KEOutput` filename is
reused for both estimator passes, so the first pass's output is destroyed by the second.

**Severity:** medium — installation-dependent failure and a concurrency hazard.
**Rewrite:** `Workspace` owns a per-run scratch directory with distinct names per stage,
cleaned up via RAII.

---

### 7. Paths containing spaces are unsupported

Because the estimator is invoked through `cmd.exe` with an unquoted, shell-redirected
command line (`Unit1.pas:350`, `853`), any path containing a space breaks. The legacy
detects this and refuses to run (`Unit1.pas:779`) rather than fixing it.

**Severity:** medium — `C:\Users\First Last\…` is an ordinary Windows path.
**Rewrite:** `IProcessRunner` passes an argument vector and captures stdout directly. No
shell, no quoting problem, and the estimator's stderr becomes visible for the first time.

---

### 8. Duplicated form instantiation

`Project1.dpr:25-26` calls `Application.CreateForm(TForm7, Form7)` twice. The first
instance is leaked and orphaned.

**Severity:** low.
**Rewrite:** not applicable — no such construct exists.

---

### 9. Missing precondition check: absolute extrusion

`Unit1.pas:627` accumulates raw `E` values to track filament use. This is only meaningful
under **relative extrusion** (`M83`). Under absolute extrusion (`M82`) the accumulation is
nonsense and the entire flow→temperature mapping silently produces garbage.

The legacy never checks. A user with the wrong slicer setting gets a plausible-looking
file that is wrong throughout.

**Severity:** high — silent, total, and the user has no way to discover the cause.
**Rewrite:** detected during the scan pass and refused with a specific diagnostic naming
the slicer setting. See [ALGORITHM.md §9](../ALGORITHM.md).

---

### 10. SQL built by string concatenation

`Unit1.pas:1087` and `1135` construct filter expressions by concatenating quoted user
input. Profile names come from the user and from G-code comments.

**Severity:** low in a local desktop context — but the pattern is wrong and the input is
partly attacker-influenceable via a crafted G-code file's `filament_settings_id` comment.
**Rewrite:** prepared statements throughout ([ADR-0003](../adr/0003-sqlite-in-core.md)).

---

## Structural problems

Not bugs, but the reasons a port was rejected ([ADR-0001](../adr/0001-clean-room-rewrite.md)):

- **The charting widgets are the data model.** `SmoothSeries()` mutates chart points in
  place; `FindClosestIndex()` binary-searches a chart series; the generation routine uses
  chart series as its working arrays.
- **Domain functions read widgets mid-algorithm** — text fields and combo boxes are read
  inside the G-code generation loop, and results written straight to labels and chart
  titles.
- **The source does not compile.** `Unit1.pas:244` imports a `Unit9` that has never
  existed in 738 commits; `Unit1.pas:171-172` declares two buttons absent from the form
  definition.
