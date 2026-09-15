# ADR-0005: Differential verification against the legacy binary, not byte-parity

**Status:** Accepted · 2026-08-07

## Context

[ADR-0001](0001-clean-room-rewrite.md) chose a clean-room rewrite, which forfeits
byte-exact comparison against the legacy. The question remains: how do we know the
rewrite is *right*?

The legacy binary still runs, and usefully, it runs **headlessly** — given a G-code path
as its first argument it processes the file with no user interaction. So a reference
implementation is available even though its source is not buildable.

## Decision

Verify at three levels, none of them byte-exact:

### 1. Unit tests — physical and logical properties
Hermetic, fast, no subprocesses. These assert *invariants*, not recorded values:

- Speed is never increased above the sliced feedrate ([ALGORITHM.md §2](../ALGORITHM.md)).
- Temperature stays within the calibrated low..high band.
- The achievable curve never exceeds the extruder's slew rates.
- Pressure advance is emitted only in the four permitted feature types.
- Cumulative extruded filament is monotonic.
- Absolute extrusion (`M82`) is rejected.

These are the tests that survive refactoring, because they encode the specification
rather than an implementation.

### 2. Differential comparison — curve shape, within tolerance
Feed the same G-code to both tools; extract the commanded temperature sequence from each
output; compare as curves.

Tolerance is on **shape and magnitude**, not identity — a few °C of divergence is
expected and acceptable, since we deliberately changed the blend formula
([ADR-0006](0006-explainable-blend-and-smoothing.md)) and fixed several legacy defects.
What this catches is the failure mode that matters: *misunderstanding the algorithm*.
A curve that trends the wrong way, saturates, or is phase-shifted indicates a real
conceptual error.

### 3. Physical validation — the real gate
Test prints on real hardware. Levels 1 and 2 cannot detect "technically correct but prints
badly". This is milestone M9 and is the only true correctness criterion.

## Explicitly rejected

**Byte-exact output parity.** It would require emulating Delphi's banker's rounding, its
`FloatToStr` formatting, and — hardest — the 80-bit x87 extended-precision accumulation
used by the 32-bit shipped binaries, whose divergence from SSE2 doubles compounds over
tens of thousands of moves. Substantial effort to reproduce, exactly, the output of a tool
whose bugs we are deliberately fixing.

## Consequences

**Good**
- We can improve on the legacy rather than being pinned to it.
- Unit tests document the specification and survive refactoring.
- The differential harness is cheap: the legacy binary already runs headlessly.

**Bad**
- No single automated gate says "correct". Confidence comes from three weaker signals.
- Physical validation is slow, needs hardware and filament, and cannot run in CI.
- A subtle regression that stays within tolerance and prints acceptably could go unnoticed.

**Operational notes**
- Differential comparison needs sample G-code, which **does not exist in this repository**.
  This is the one hard external dependency and blocks M7 entirely.
- When capturing reference output, do not use the legacy's Save action — it deletes its
  own intermediate files, which are the interesting comparison points.
