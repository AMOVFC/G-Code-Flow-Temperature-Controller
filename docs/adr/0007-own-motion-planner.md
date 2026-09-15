# ADR-0007: Implement the motion planner in-process

**Status:** Accepted · 2026-08-07

## Context

Timing and flow come from `klipper_estimator.exe`, a vendored Rust binary invoked as a
subprocess. That works, but it makes the tool two artefacts rather than one:

1. **The OrcaSlicer cloud plugin cannot reasonably ship it.** A plugin that spawns a
   platform-specific executable is awkward at best and impossible in a sandboxed or
   server-side context. This is the stated long-term direction
   ([ADR-0002](0002-core-ui-separation.md)), so the dependency is on the critical path.
2. **Distribution friction.** Two binaries plus a `config.json` must travel together, and
   the config in particular has already caused a silent misconfiguration
   ([STATE.md](../STATE.md)).
3. **It is a fork.** The bundled estimator is `sb53systems/klipper_estimator`, not
   upstream, so there is no maintained source of truth to track.

## Decision

Implement Klipper's look-ahead motion planning inside `sb53_core`, and drop the
subprocess once it matches.

**Implemented from the documented algorithm and first principles — not translated from
any existing implementation.** Klipper is GPL-3.0; copying or transliterating its source
into this MIT project would relicense it, the same trap avoided in
[ADR-0004](0004-custom-chart-widget.md). Trapezoidal motion planning and junction
deviation are textbook; the risk is in the details, not the concepts.

### What has to be reproduced

| Stage | Notes |
|---|---|
| Move extraction | A move is a `G0`/`G1` with at least one of X/Y/Z/E. Measured on a real file: 61,189 move lines − 2,836 bare `F` lines = 58,353 against the estimator's 58,352 |
| Per-move velocity cap | requested `F`, machine `max_velocity`, and the `move_checkers` |
| Junction velocity | from `square_corner_velocity`, via junction deviation |
| Look-ahead | backward then forward pass over the move queue |
| Trapezoid timing | accelerate / cruise / decelerate, honouring `minimum_cruise_ratio` |
| Arc interpolation | `G2`/`G3` subdivided at `mm_per_arc_segment` |
| Limiters | `axis_limiter` (Z) and `extruder_limiter` |

### Validation

**We have a perfect oracle.** The vendored binary produces reference output for any input,
so this is not guesswork: every stage is checked against it on real files, and the
existing `dump-moves` fixtures already capture expected output.

Acceptance: per-move duration and flow matching the reference within tight tolerance
across the full sample set, and total time within a second on an eight-minute print.

## Consequences

**Good**
- One binary. No `config.json` hunting, no subprocess, no platform-specific dependency.
- The plugin path opens up — the core becomes linkable rather than spawnable.
- Tests get faster and fully hermetic; no recorded-output fixtures needed for timing.
- Errors become diagnosable. Today an estimator failure is opaque.

**Bad**
- The largest single piece of work in the project, and the highest-risk: every flow and
  temperature number downstream derives from these timings, so "close" is not good enough.
  A subtle mismatch would silently shift users' calibration.
- We take on maintenance of motion-planning code, which must track Klipper's behaviour as
  it evolves (`minimum_cruise_ratio` already replaced `max_accel_to_decel`).

**Mitigation**
- Build it behind the existing `IProcessRunner` seam as an alternative implementation, so
  both can run and be compared on the same input until confidence is earned.
- **Do not remove the subprocess path until parity is demonstrated** across the whole
  sample set. Keep it available as a fallback and as the differential oracle.
