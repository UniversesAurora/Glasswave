# Research: Aero glass on Windows 11 (2026-11)

| Approach | Scope | API status | Look control | Verdict |
| --- | --- | --- | --- | --- |
| DWMBlurGlass / OpenGlass (DWM injection) | system-wide | hooks private DWM | full Aero | rejected: global, fragile |
| SetWindowCompositionAttribute accent blur | per window | undocumented | fixed | fallback candidate only |
| DWMWA_SYSTEMBACKDROP_TYPE (Mica/Acrylic) | per window | documented | fixed Fluent recipe | rejected |
| DwmEnableBlurBehindWindow | per window | documented | transparency only on Win10+ | rejected |
| HostBackdropBrush + composition effects | per window | documented (22000+) | full post-processing | **chosen** |
| Screen capture + own blur | per window | documented | full | rejected: cost, breaks screen capture |

Notes:

- `DWMWA_USE_HOSTBACKDROPBRUSH` enables the host backdrop brush for non-UWP windows.
  Its blur amount is fixed by DWM (acrylic strength).
- The Win7 recipe (color / afterglow / blur balance) is documented by the OpenGlass and
  DWMBlurGlass communities; AeroThemePlasma (KDE) uses the same model and anchors the
  reflection texture to the monitor. We use only the math, no code.
- Inactive-window behavior of the host backdrop brush: observed working in M0; keep testing.
- Possible future refinement for a lighter blur: none available via documented APIs today.
