# ADR 0003: Windows.UI.Composition visual tree + Direct2D painting

Status: accepted (2026-11-07)

## Context

We need per-pixel transparency over the glass, GPU animations (hover fades, WMC
breathing, transitions) at the display refresh rate, and no rendering while idle.
Candidates: GDI/GDI+ (no), raw D3D swap chain (re-implement compositor), DirectComposition
(no backdrop brush), XAML Islands / WinUI 3 (heavy runtime in a plugin DLL, Fluent look),
system Windows.UI.Composition (in the OS, effects, backdrop brushes, implicit/keyframe
animations).

## Decision

Use system `Windows.UI.Composition` (C++/WinRT, no Windows App SDK) with a
`DesktopWindowTarget` on a `WS_EX_NOREDIRECTIONBITMAP` window. Paint content with
Direct2D/DirectWrite into `CompositionDrawingSurface`s. Animate opacity/offset/scale in the
compositor. Use nine-grid and stretched strips so resizing does not repaint large surfaces.

## Consequences

- Smooth, high-refresh animations that cost no CPU per frame; idle UI renders nothing.
- Child HWNDs (hosted panels) are composed by DWM; ordering relative to our tree is set by
  the window target's topmost flag (ADR 0006).
- Device loss handling (RenderingDeviceReplaced) is still TODO (M1).
