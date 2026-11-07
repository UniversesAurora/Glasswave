# ADR 0005: Procedural rendering, no Microsoft bitmaps

Status: accepted (2026-11-07)

## Decision

Do not extract or ship bitmaps from aero.msstyles, WMP, WMC or other Microsoft binaries.
Recreate every element procedurally (Direct2D vector shapes and gradients) from pixel
measurements of reference screenshots. Reference screenshots stay in `.local/refs/`
(ignored by git) and only the measured numbers are committed (`docs/design/visual-spec.md`).

## Consequences

- No copyright exposure; the project can be published.
- Elements are resolution-independent and crisp at any DPI, unlike upscaled 96-DPI art.
- Matching requires careful measurement; every value must cite its source in the spec.
