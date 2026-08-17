# ADR-0001: Clean-room rewrite rather than a port

**Status:** Accepted · 2026-08-07

## Context

The original is a Delphi/VCL Windows application (`Source/V1.1/`). The goal is a modern
C++ application, with a Qt frontend now and a web frontend later for OrcaSlicer's cloud
plugin.

Two options were considered: a faithful port verified byte-for-byte against the shipped
binary, or a clean-room rewrite using the original as a reference document.

Findings that decided it:

1. **The legacy source cannot be compiled.** `Unit1.pas:244` imports `Unit9`, which has
   never existed in 738 commits. Independently, `Unit1.pas:171-172` declares
   `Button1`/`Button2` while `Unit1.dfm` contains zero `TButton` objects, so form
   streaming would fail at runtime even if `Unit9` were recovered. There is no buildable
   baseline.
2. **There is no domain layer to port.** The charting widgets are the data structures —
   `SmoothSeries()` mutates chart points in place, `FindClosestIndex()` binary-searches a
   chart series, and the generation loop reads text fields mid-algorithm. A faithful port
   reproduces this structure in a new language.
3. **The public README is unusually complete.** It documents the flow→temperature model,
   the speed-clamp asymmetry, the pressure-advance gating rules, the macro protocol, and
   the config schema — roughly 85% of the concept, independent of the source.
4. **Byte-parity would have been expensive and low-value.** The shipped binaries are
   32-bit x86, so Delphi accumulates floating point at 80-bit extended precision.
   Matching that from SSE2 doubles, plus emulating Delphi's banker's rounding and
   `FloatToStr` formatting, is substantial work — to reproduce output from a tool with
   known bugs we intend to fix anyway.

## Decision

Rewrite clean, treating `Source/V1.1/` as a **reference document** rather than code to
migrate. Capture the derived behaviour in [ALGORITHM.md](../ALGORITHM.md) with provenance
markers distinguishing documented behaviour, inferred behaviour, physics, and our own
choices.

Verification is **differential, not exact**: compare temperature curves against the
legacy binary within tolerance (see [ADR-0005](0005-differential-verification.md)), not
bytes.

## Consequences

**Good**
- The ~750 lines of real logic can be structured properly instead of transliterated.
- The catalogued legacy defects ([legacy/known-bugs.md](../legacy/known-bugs.md)) are
  simply never written.
- No effort spent on x87 emulation, banker's rounding, or `FloatToStr` byte-matching.
- The data model can be designed for clarity rather than to mirror a charting widget.

**Bad**
- We forfeit the claim "produces identical output to the tool people already trust".
- Physical validation becomes the real correctness gate — slow, requires hardware, and
  cannot be automated. This is the genuine cost and is budgeted as milestone M9.
- The ~15% of behaviour that exists only in the Pascal must be read carefully rather than
  compiled. Mitigated by [legacy/algorithm-map.md](../legacy/algorithm-map.md).

**Neutral**
- Calibration values are unaffected — they are user data in `Config.sdb`, not constants
  in the code, and the README documents the procedure users follow to derive their own.
  We keep reading the existing database schema so that work migrates.
