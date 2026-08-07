# Architecture Decision Records

One file per decision. Short, and written at the moment the decision is made.

The point of an ADR is not the decision — that is visible in the code. The point is the
**reasoning and the alternatives rejected**, which are exactly what evaporate over a
months-long gap and cannot be reconstructed from the source.

| # | Decision | Status |
|---|---|---|
| [0001](0001-clean-room-rewrite.md) | Clean-room rewrite rather than a port | Accepted |
| [0002](0002-core-ui-separation.md) | UI-agnostic core; CLI reaches parity before the GUI | Accepted |
| [0003](0003-sqlite-in-core.md) | Vendored SQLite in core, not QtSql | Accepted |
| [0004](0004-custom-chart-widget.md) | Custom chart widget, not Qt Charts (licensing) | Accepted |
| [0005](0005-differential-verification.md) | Differential verification, not byte-parity | Accepted |
| [0006](0006-explainable-blend-and-smoothing.md) | Explainable blend curve, diverging from legacy | Accepted |

## Writing a new one

Copy the shape of an existing file: **Context** (what forced a choice), **Decision**
(what we did), **Consequences** (good, bad, and what would make us revisit).

Number sequentially. Never edit an accepted ADR to change its decision — write a new one
that supersedes it, and mark the old one `Superseded by ADR-NNNN`. The record of what we
used to think is part of the value.

Write one whenever a choice is not obvious from the code, could reasonably have gone
another way, or was made for a reason outside the code — licensing, hardware, a constraint
someone told us about.
