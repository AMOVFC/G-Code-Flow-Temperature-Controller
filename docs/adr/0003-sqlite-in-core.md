# ADR-0003: Vendored SQLite in core, not QtSql

**Status:** Accepted · 2026-08-07

## Context

Extruder and filament profiles are stored in a SQLite database (`Config/Config.sdb`).
The legacy accessed it through FireDAC, with the surrounding logic — validation, upsert
decisions, cascade deletes — living in form event handlers.

The obvious Qt-native choice would be `QtSql`.

## Decision

Vendor the **SQLite amalgamation** into `core/`, and put the profile repository there.
Do not use QtSql.

Keep the **existing `Config.sdb` schema unchanged**.

## Rationale

1. **Profile handling is domain logic, not presentation.** Validation rules, insert-vs-
   update semantics, and the cascade delete are business behaviour. If they live in the
   Qt layer, the "disposable" layer holds the most valuable non-algorithmic code in the
   application, and the web frontend must reimplement it — the exact failure mode
   [ADR-0002](0002-core-ui-separation.md) exists to prevent.
2. **QtSql would make core depend on Qt**, violating ADR-0002's single rule.
3. **QtSql needs a runtime plugin** (`qsqlite`). The CLI is intended to be invoked by
   OrcaSlicer as a post-processing script and, later, by a web backend. Requiring it to
   locate Qt plugin directories is unacceptable for a headless binary.
4. **The amalgamation is a single `.c` file** — no external dependency, deterministic
   across platforms, and the same code QtSql wraps.
5. It enables **prepared statements**, replacing the legacy's string-concatenated filter
   expressions.

**Schema stability** is a separate and firmer commitment: the existing database is
irreplaceable user data. The README instructs users to carry their `Config` folder across
upgrades, and the calibration values in it represent hours of test prints per filament.
We read and write the existing schema as-is. Any future migration must be additive,
reversible, and explicitly opted into.

## Consequences

**Good**
- Profile logic is reusable by every frontend, present and future.
- The CLI is a self-contained binary.
- Existing users' calibration work carries over untouched.
- Prepared statements remove a string-concatenation pattern.

**Bad**
- We maintain a vendored third-party source file and must track its security updates.
- A small hand-written RAII wrapper is needed instead of a ready-made Qt API. Deliberately
  small — no ORM.

**Neutral**
- Inheriting a schema we did not design, including its quirks (e.g. boolean stored as
  integer). Verify actual stored representations against the shipped database rather than
  assuming.
