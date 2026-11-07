# ADR 0002: Per-window Aero glass via HostBackdropBrush + Win7 color matrix

Status: accepted (2026-11-07)

## Context

Windows 11 has no Aero glass. Options:

1. DWM hooking/injection (DWMBlurGlass, OpenGlass, Aero Glass for Win8): system-wide,
   fragile across builds, requires elevated injection. Rejected by requirement.
2. `SetWindowCompositionAttribute` accent blur: undocumented API, fixed look.
3. `DWMWA_SYSTEMBACKDROP_TYPE` (Mica/Acrylic): documented, but a fixed Fluent recipe
   (noise, luminosity) that cannot be turned into Aero.
4. `DwmEnableBlurBehindWindow`: on Win10/11 only transparency, no blur.
5. Windows.UI.Composition `CreateHostBackdropBrush` + `DWMWA_USE_HOSTBACKDROPBRUSH`:
   documented for Win32 since Windows 11 22000; returns the live blurred backdrop of this
   window, which we can post-process with composition effects.
6. Screen capture + own blur: costly, breaks screenshots/screen sharing.

## Decision

Option 5. The Win7 DWM colorization formula is linear and is applied as a single D2D
ColorMatrix effect (`aero_colorization.h`); with Win7 "Sky" defaults it reproduces the
measured Win7 glass color exactly (#b8dbfd over white). Reflection streaks are our own
screen-anchored layer.

## Consequences

- Only our window is affected; documented API; GPU-composited; zero CPU while idle.
- The blur radius is DWM's (acrylic-strength), somewhat stronger than Win7's. There is no
  API to change it; revisit if Windows exposes one (tracked in docs/research/aero-on-windows-11.md).
- With "Transparency effects" off we fall back to an opaque Aero-Basic-like color.
