# ADR 0004: Self-drawn Win7 frame, caption buttons and shadow

Status: accepted (2026-11-07)

## Decision

- `WM_NCCALCSIZE` returns 0; frame, caption, caption buttons and resize borders are drawn
  and hit-tested by Glasswave. Caption buttons return real `HTMINBUTTON/HTMAXBUTTON/HTCLOSE`
  codes so Windows 11 Snap Layouts appear; clicks are completed on either the client or
  non-client button-up path (Win11 may steal capture on the maximize button).
- Win11 rounding and accent border are disabled; all four corners use Win7's 6-DIP radius.
- The DWM shadow is disabled (`DWMNCRP_DISABLED`) and replaced by four click-through layered
  windows rendering the shadow profile measured from native Win7 (heavier bottom-right).

## Consequences

- Pixel-level control and fidelity; behavior (snap, maximize, DPI changes) must be tested by
  us on each Windows release.
- Shadow windows can lag one frame behind fast window moves; acceptable (same technique as
  Office / Visual Studio glow borders).
