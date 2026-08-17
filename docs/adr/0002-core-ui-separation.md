# ADR-0002: A UI-agnostic core, with the CLI reaching parity before the GUI

**Status:** Accepted · 2026-08-07

## Context

The frontend plan is explicitly two-stage: **Qt 6 Widgets now, a web frontend later** so
the tool can integrate with OrcaSlicer's cloud plugin system.

That makes the Qt layer known-disposable from the outset. The failure mode to avoid is
the one the legacy already demonstrates: logic accumulating inside UI event handlers,
so that a second frontend means rewriting the application rather than re-skinning it.

In the legacy, profile validation, insert-vs-update decisions, cascade deletes, and the
entire G-code algorithm all live in form code. None of it is reusable.

## Decision

**`core/` links against nothing that knows a user exists.** No Qt, no windowing, no
console I/O, no message boxes, no process spawning implementation.

Enforced mechanically, not by convention:
- `core`'s CMake target never links Qt.
- CI greps for `#include <Q` under `core/` and fails the build.

Everything the legacy did by reaching into a form becomes one of five injected seams:
`IProcessRunner`, `IProgressSink`, `Workspace`, `IProfileRepository`, `Pipeline`.

**Sequencing decision:** the CLI reaches full capability at **M6**, before the Qt
frontend starts at **M8**.

## Consequences

**Good**
- The web frontend becomes a routing layer over an existing API rather than a rewrite.
- The CLI is the OrcaSlicer post-processing script — the tool's primary real-world use —
  so M6 delivers genuine user value with no GUI at all.
- Unit tests inject a fake `IProcessRunner` replaying recorded estimator output, so the
  suite is fast, hermetic, and does not depend on the vendored binary.
- A Linux CI build is nearly free, and is continuous proof the separation is real.

**Bad**
- Indirection that a single-frontend application would not need. Justified only by the
  stated intent to build a second frontend; if that intent changes, revisit.
- Interactive progress reporting through a sink interface is more work than assigning to
  a label.

**Neutral**
- The GUI arrives later than in a UI-first approach. Given the CLI is the primary
  integration path, this is a reordering rather than a delay.
