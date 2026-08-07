# Algorithm Specification

> **This is the canonical description of what the tool does.** It is written to be
> readable without the legacy Delphi source and without this repository's history.
> If you are resuming this project after a long break, read this file first, then
> [STATE.md](STATE.md).

## Provenance markers

Every non-obvious claim below carries a tag saying where it came from. This matters:
the C++ implementation is a **clean-room rewrite**, and knowing which parts are
publicly documented behaviour versus inferred-from-legacy-code versus our own
decisions is what lets us change things confidently.

| Tag | Meaning |
|---|---|
| `[DOC]` | Stated explicitly in the project's public README. Authoritative. |
| `[CODE]` | Inferred by reading the legacy Delphi source. Reliable, but the legacy is buggy — see [legacy/known-bugs.md](legacy/known-bugs.md). |
| `[PHYS]` | Derivable from first principles (geometry, thermodynamics). Independent of the legacy. |
| `[DESIGN]` | Our decision in this rewrite. Not inherited. Change freely with an ADR. |

---

## 1. The problem

Slicers hold nozzle temperature constant for a whole print, but extrusion flow varies
continuously. The filament is therefore printed too hot when flow is low and too cold
when flow is high. `[DOC]`

The heat needed to melt filament depends primarily on volumetric flow rate: low flow
needs less heat, high flow needs more. `[DOC]`

**Core thesis: temperature should adapt to the real extrusion flow.** `[DOC]`

## 2. What the tool does

It is a **post-processing step**: it reads a sliced G-code file and writes a modified
one. It never talks to the printer. `[DOC]`

Two things are modified:

1. **Nozzle temperature** — `M104 S…` commands are injected so temperature tracks the
   upcoming flow. `[DOC]`
2. **Print speed** — feedrates are *reduced* where the sliced speed would exceed the
   flow the nozzle can sustain at that temperature. `[DOC]`

Optionally, **pressure advance** is adjusted with temperature, on Klipper only. `[DOC]`

### The critical asymmetry

> "The script is specifically programmed to reduce speeds only when they exceed the
> recommended flow rate, while lower speeds will remain as set in the G-Code."
> — README `[DOC]`

Speed is **clamped downward only, never increased**. If the slicer asked for something
slower than the flow budget allows, that is respected. This is a safety property: the
tool can never make a print more aggressive than the user's own profile. Preserve it.

## 3. Inputs

| Input | Source | Notes |
|---|---|---|
| Sliced G-code | User / slicer post-processing hook | Must use **relative extrusion** (`M83`) `[CODE]` — see §9 |
| Printer kinematic limits | `config.json` | Schema documented verbatim in README `[DOC]` |
| Extruder thermal profile | Local database | Heat-up and cool-down rates `[CODE]` |
| Filament flow/temp profile | Local database | Three calibration points `[DOC]` |
| Speed↔Quality bias | User, per filament | `[CODE]` |
| Smoothing window | User, per extruder | Recommended 10–30 `[DOC]` |

### Filament calibration model

The user supplies **exactly three (flow, temperature) points** — low, mid, high — and
the tool interpolates **linearly** between them. `[DOC]`

> "This script allows to play with only three `Flow/Temperature` values, which means it
> will adjust them linearly." — README `[DOC]`

Worked example from the README `[DOC]`:

| Volumetric flow | Temperature |
|---:|---:|
| 1 mm³/s | 190 °C |
| 15 mm³/s | 220 °C |
| 22 mm³/s | 235 °C |

Two segments, each linear; the mid point is the pivot. Values outside the low..high
range clamp to the endpoints.

> ⚠️ **Invariant:** `lowFlow < midFlow < highFlow` strictly. The inverse mapping
> (temperature → flow, §6) divides by segment width, so equal values are a division by
> zero. The legacy app has this latent bug and only avoids it because its spinner
> minimums happen to differ. `[CODE]` We validate explicitly. `[DESIGN]`

These calibration numbers are **user data, not constants**. The README documents a
visual calibration procedure (vase-mode cylinder at controlled flow, gradually lowering
temperature) for the user to derive their own. `[DOC]` We ship no magic numbers.

## 4. Flow analysis

The tool does **not** compute kinematics itself. It shells out to a vendored build of
**klipper_estimator**, which applies Klipper look-ahead kinematics to produce, for every
move, its **duration** and its **volumetric flow**. `[DOC]`

Accuracy is ±1 s total on Klipper, within ~5% on other firmwares. `[DOC]`

> The bundled binary is a **custom fork**, not upstream klipper_estimator. `[DOC]`
> Treat it as a pinned dependency; do not upgrade casually.

### Aggregation to a per-second timeline

Per-move samples are aggregated into **one-second buckets**. `[CODE]` For each second we
retain:

- **average flow** over the second
- **maximum flow** within the second
- **cumulative extruded filament** at the end of the second

Rationale `[PHYS]`: the hotend has thermal inertia measured in seconds, so it cannot
track per-move flow changes that occur in milliseconds. One second is the natural
resolution — matching the README's own phrasing, "the average flow rate that can be
reached every second". `[DOC]`

### Retraction handling

Retract and unretract moves have negative or meaningless flow and must not contribute to
the flow signal — they are treated as zero flow. `[CODE]` `[PHYS]`

## 5. Temperature planning

Four stages, in order:

### 5.1 Blend average and maximum flow

The per-second average and maximum are blended into a single target using the user's
**Speed↔Quality bias**. `[CODE]`

Interpretation `[DOC]`:
- **Toward Speed** — track flow more aggressively; higher temperatures, higher sustained
  flow, shorter print. "Aim for Speed Optimization as long as it doesn't affect the
  desired quality."
- **Toward Quality** — smooth over flow variation; fewer temperature swings. "If your
  filament is very sensitive… aim for Quality Optimization to reduce flow variation."

> `[DESIGN]` The exact blend curve is **not** documented, and the legacy formula is
> arbitrary. We define it as a straight linear interpolation between the average (quality
> end) and the maximum (speed end), which reproduces the documented *semantics* and is
> explainable to users. This is a deliberate departure — see
> [ADR-0006](adr/0006-explainable-blend-and-smoothing.md).

### 5.2 Smooth

A **centered moving average** over the blended flow, window width = the user's smoothing
setting. `[CODE]` Recommended range 10–30. `[DOC]`

At the sequence ends the window is clamped and the divisor is the *actual* number of
samples averaged, not the nominal window width. `[CODE]` `[PHYS]` (Anything else biases
the first and last seconds toward zero.)

Purpose: the hotend physically cannot follow a jagged flow signal, and chasing it wastes
time in heat-up/cool-down. `[DOC]` notes that changing this value measurably affects both
result and print time.

### 5.3 Map flow to temperature

Apply the three-point piecewise-linear map from §3 to the smoothed flow. Result: a
**desired** temperature for each second.

### 5.4 Constrain by what the hotend can physically do

The desired curve is generally unachievable — a hotend has finite heat-up and cool-down
rates, and they are **not symmetric** (heating is driven, cooling is passive and usually
much slower). `[PHYS]`

The extruder profile stores both rates, each as a °C-per-N-seconds pair. `[CODE]` The
desired curve is walked forward and slew-limited by the applicable rate, producing the
**achievable** temperature plan.

> `[PHYS]` Because cooling is slower than heating, the achievable curve lags the desired
> one asymmetrically — it rises nearly on demand but decays gradually. This is correct
> and expected, not a bug.

The README's advice to calibrate PID between 70% and 90% of maximum temperature `[DOC]`
exists because this stage assumes the hotend can actually hit its commanded rates.

## 6. Rewriting the G-code

The planned curve is in the **time domain**, but it must be applied to **moves**.

### The coordinate problem — and the key insight

You cannot index the plan by time, because rewriting speeds *changes* the timing. You
cannot index by line number, because lines are inserted. `[PHYS]`

**The plan is indexed by cumulative extruded filament.** `[CODE]`

This is the single most important design decision inherited from the legacy tool, and it
is worth stating plainly: total filament extruded at a given point in the print is
**invariant** under speed rewriting. The same geometry always consumes the same filament,
no matter how fast you print it. It is therefore the only stable coordinate shared
between the analysis pass and the rewrite pass.

We keep this deliberately. `[DESIGN]`

### Per-move rewrite

Walking the print body, maintaining cumulative extruded filament:

1. **Look up planned temperature** for the current filament position. Emit `M104 S…`
   when it differs from the last commanded value.
2. **Compute the flow budget** for that temperature by inverting the §3 map
   (temperature → flow).
3. **Convert flow to feedrate** using the extrusion cross-section (§7).
4. **Clamp**: `feedrate = min(recommended, sliced)` — never faster than the slicer asked
   (§2). `[DOC]`
5. **Optionally emit pressure advance** (§8).

Retract/unretract moves are skipped for speed purposes but still accumulate filament.
`[CODE]`

## 7. Extrusion geometry

Converting a volumetric flow budget into a feedrate requires the cross-sectional area of
the extruded bead. For a bead of given layer height `h` and line width `w`, modelled as a
rectangle with semicircular ends `[PHYS]`:

```
area = h · (w − h)  +  π · (h/2)²
feedrate_mm_per_min = 60 · flow_mm3_per_s / area
```

`h` and `w` are read from slicer comment markers in the G-code, which change per feature
and per layer. `[CODE]`

This is why the tool handles **variable layer height** and **precise Z height** well —
it recomputes area continuously rather than assuming a fixed profile. `[DOC]`

> `[PHYS]` This is the standard slicer extrusion model, not something inherited. It is
> reproduced here from geometry, independently of the legacy source.

## 8. Pressure advance

Pressure advance may be interpolated with temperature (hotter filament is less viscous,
needing less advance), using the same three-point structure as flow. `[CODE]`

Two hard constraints, both documented:

- **Klipper only.** Other firmwares cannot change PA mid-print; the feature must be
  switchable off. `[DOC]`
- **Only in concealed features.** Changing PA mid-print can cause bubbles in walls, so it
  is applied **only** in *sparse infill*, *internal solid infill*, *support*, and
  *internal bridges*. `[DOC]`

That feature list is a correctness requirement, not a heuristic. Do not widen it.

## 9. Preconditions and limits

### Required

- **Relative extrusion (`M83`).** The tool accumulates raw `E` values; under absolute
  extrusion (`M82`) that accumulation is meaningless and the entire flow→temperature
  mapping silently collapses. The legacy tool does not check this. `[CODE]`
  `[DESIGN]` **We detect it and refuse to run**, with a clear diagnostic.
- **Not already processed.** Re-processing an already-modified file would compound
  adjustments. Detected via a header marker. `[CODE]`
- **A recognisable print body.** The tool must locate where the real printing starts and
  ends, so start/end macros are left untouched. `[CODE]`

### First-layer temperature

The first layer is handled separately, because the flow-derived curve does not apply
before printing begins. The tool rewrites the initial temperature in either `[DOC]`:

- a named **start macro** (Klipper style), or
- an `M109 S…` command immediately preceded by the marker comment `; Temp_To_Edit`.

The marker `; PRINT_END` at the start of the machine end G-code lets the tool skip
end-of-print moves. `[DOC]`

### Known limitations `[DOC]`

- No multi-tool / multi-material.
- Arc moves (`G2`/`G3`) are supported but slow, because the estimator subdivides them
  into ~0.1 mm segments. The legacy cannot chart temperature for arc prints.
- Ironing, adaptive pressure advance, and Bambu flow-calibration / first-layer-inspection
  are incompatible.
- Delta kinematics unsupported by the estimator.
- Output is 10–30% larger than input, from the injected commands.

## 10. Output

A G-code file that is byte-identical to the input outside the print body, with the body
rewritten per §6, plus:

- a header line marking the file as processed (also the re-processing guard, §9)
- a corrected total print-time estimate — obtained by running the estimator a **second
  time** on the rewritten body, since the speed changes altered the duration `[CODE]`

When invoked as a slicer post-processing script the output replaces the input file in
place; when invoked interactively the user chooses a destination. `[DOC]`

---

## Pipeline summary

```
input.gcode
   │
   ├─ scan ──────────► markers, printer/filament IDs, body bounds, M82/M83 guard   (§9)
   │
   ├─ extract body ──► klipper_estimator dump-moves ──► (duration, flow) per move  (§4)
   │
   ├─ aggregate ─────► per-second avg flow / max flow / cumulative filament        (§4)
   │
   ├─ blend ─────────► speed↔quality weighted target flow                          (§5.1)
   ├─ smooth ────────► centered moving average                                     (§5.2)
   ├─ map ───────────► desired temperature per second                              (§5.3)
   ├─ slew-limit ────► achievable temperature plan                                 (§5.4)
   │
   ├─ rewrite ───────► M104 / clamped feedrates / optional PA,                     (§6)
   │                   indexed by cumulative extruded filament
   │
   ├─ re-estimate ───► corrected total print time                                  (§10)
   │
   └─ assemble ──────► output.gcode
```

## Open questions

Recorded so they are not silently forgotten. None block implementation.

1. **Blend curve shape** (§5.1) — linear is our choice; the legacy formula differs. Worth
   revisiting once real prints can be compared.
2. **Bucket width** (§4) — one second is inherited and physically sensible, but has never
   been tested against alternatives.
3. **Slew limiting is open-loop** (§5.4) — it assumes commanded rates are achieved. A
   closed-loop variant using actual thermal response is the natural evolution, and is
   what the legacy author's unreleased serial-console work was aiming at.
