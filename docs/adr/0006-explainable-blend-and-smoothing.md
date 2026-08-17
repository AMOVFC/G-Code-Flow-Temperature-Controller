# ADR-0006: An explainable blend curve, deliberately diverging from the legacy

**Status:** Accepted · 2026-08-07

## Context

[ALGORITHM.md §5.1](../ALGORITHM.md) blends per-second **average** flow and **maximum**
flow into a single target, weighted by the user's Speed↔Quality setting.

The README documents the *semantics* clearly — bias toward Speed to track flow
aggressively and print faster; bias toward Quality to smooth variation — but not the
formula. The legacy's actual expression is an undocumented arithmetic combination with no
stated derivation.

This is one of the ~15% of behaviours that exists only in the legacy source.

## Decision

Implement the blend as **linear interpolation** between the per-second average (Quality
end) and the per-second maximum (Speed end), parameterised by the bias setting.

Accept that this **will not** reproduce the legacy's numbers exactly.

## Rationale

1. **It reproduces the documented semantics exactly.** Bias toward maximum → higher
   targets, higher temperature, faster print. Bias toward average → smoother targets,
   fewer temperature swings. That is precisely what the README describes.
2. **It is explainable to users.** "Halfway between the average and peak flow of each
   second" can go in a tooltip. The legacy's formula cannot.
3. **The legacy formula has no stated justification** — no comment, no documentation, no
   derivation. Copying an arbitrary expression we do not understand would embed a
   mystery permanently, and mysteries are exactly what a months-long development pause
   destroys.
4. **Calibration absorbs the difference.** Users calibrate their own flow/temperature
   points against their own printer and filament ([ALGORITHM.md §3](../ALGORITHM.md)).
   The blend shifts where within their calibrated band a given second lands; it does not
   change the band.

The same reasoning applies to the smoothing window (§5.2): a centered moving average with
correct end-clamping, chosen because it is the obvious interpretation of the documented
"Max/Average Smoothing" control and because the README's recommended 10–30 range gives it
a sanity check.

## Consequences

**Good**
- Every part of the temperature path can be explained to a user, and to whoever resumes
  this project.
- The Speed↔Quality control becomes documentable rather than empirical.
- No unexplained constants inherited.

**Bad**
- Output diverges from the legacy by more than floating-point noise. This is the main
  reason [ADR-0005](0005-differential-verification.md) compares curve *shape* rather than
  values.
- Users migrating from the legacy may find a given bias setting behaves slightly
  differently and want to re-tune. Worth a release note.

**Revisit if**
- Physical testing (M9) shows the legacy's behaviour is materially better at some
  setting. In that case, characterise *why* and document the finding — do not simply copy
  the expression back.

---

## Addendum (2026-08-07): the legacy scale is INVERTED

Differential comparison against a known-good legacy output established this empirically.

The user's database stores `SPEED_QUALITY_OPT = 3`. Reproducing that run with our bias
set to **7** — not 3 — gives near-exact agreement:

| our bias | correlation | mean offset | RMS |
|---:|---:|---:|---:|
| 3 | +0.9542 | +14.22 °C | 3.14 °C |
| 5 | +0.9908 | +7.00 °C | 1.65 °C |
| **7** | **+0.9947** | **−0.24 °C** | **0.88 °C** |
| 10 | +0.9775 | −11.19 °C | 2.80 °C |

So the legacy's scale runs **0 = Speed, 10 = Quality**, the opposite of ours.

**Decision: keep our direction, and invert on import.** `0 = Quality, 10 = Speed` matches
how the README describes the control and is the more natural reading of a
"Speed ↔ Quality" slider. **Confirmed by the project owner on 2026-08-07:** zero means
quality, a higher number means more aggressive. This is settled — do not revisit it
because the legacy stores the opposite.

Any code reading the legacy schema **must** apply:

```
ourBias = 10 - storedSpeedQualityOpt
```

This is not cosmetic. Importing the stored value directly would have produced prints
**~14 °C colder** than the user is accustomed to, with no error and no warning — the same
class of silent wrongness catalogued in [known-bugs.md](../legacy/known-bugs.md). The
`SqliteProfileRepository` must invert, and it needs a test that pins the direction.

Note also that the legacy's own writer is buggy here: it persists `SPEED_QUALITY_OPT` from
the wrong control entirely ([known-bugs.md #1](../legacy/known-bugs.md)), so a stored value
may reflect a design-time default rather than anything the user chose. Treat imported
values as a starting point to confirm with the user, not as ground truth.
