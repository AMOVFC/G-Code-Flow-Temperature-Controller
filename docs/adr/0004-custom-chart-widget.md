# ADR-0004: A custom chart widget, not Qt Charts

**Status:** Accepted · 2026-08-07

## Context

The application plots flow and temperature curves over the print timeline — the legacy
draws eleven line series on two charts. The Qt-native option is **Qt Charts**;
**QCustomPlot** is the common third-party alternative.

## Decision

Write a **custom `QWidget` + `QPainter` chart** in `app/`.

## Rationale

1. **Licensing — the decisive factor.** This project is **MIT** (`LICENSE`, © 2024 Salim
   BELAYEL). **Qt Charts is GPL-3.0-or-commercial — it is not part of Qt's LGPL modules.**
   QCustomPlot is likewise GPL-3.0-or-commercial. Linking either would force this project
   to GPL-3.0, changing the terms for everyone who redistributes it. That is not a
   decision to make as a side effect of picking a plotting library.
2. **The requirement is small.** Line series on shared axes, a legend, and rubber-band
   zoom. The legacy's only chart interaction is zoom.
3. **Accessibility needs painter control.** The UI brief requires that series not be
   distinguished by colour alone — each needs a distinct (colour, dash pattern, marker)
   triple, plus high-contrast and monochrome palettes. This is straightforward in an owned
   paint routine and awkward to retrofit into a charting library's styling model.
4. **It is not a long-term investment either way.** The planned web frontend will re-render
   with a JavaScript charting library, so a heavyweight Qt charting dependency buys
   nothing beyond the Qt frontend's own lifetime.

## Alternatives

- **Qt Charts / QCustomPlot** — rejected on licensing. Would be the fastest path if this
  project were GPL-3.0.
- **Qwt** — LGPL-with-exceptions, so MIT distribution is fine. A viable fallback if the
  custom widget proves harder than expected; the trade is an older API and another
  vendored dependency.

## Consequences

**Good**
- MIT licensing preserved.
- Full control over accessible rendering.
- No charting dependency to vendor, pin, or update.

**Bad**
- Roughly 400–600 lines to write and maintain, including axis tick selection and zoom
  interaction — the fiddly parts of charting.
- Features a library provides free (export to image, rich tooltips) become our work if
  wanted later.

**Mitigation**
- The chart consumes the core's plain data types and holds no state of its own, so
  swapping to Qwt later is a contained change if the custom widget disappoints.
