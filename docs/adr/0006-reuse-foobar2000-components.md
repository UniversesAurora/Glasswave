# ADR 0006: Reuse foobar2000 services and components; draw the chrome ourselves

Status: accepted (2026-11-07)

## Decision

- Audio, playlists, library, DSP, output (WASAPI shared/exclusive, ASIO), tagging, album art
  and preferences are used through foobar2000 core services. Glasswave never reimplements them.
- Complex features that mature components already provide (lyrics such as ESLyric,
  MilkDrop visualizations such as foo_vis_milk2, spectrum analyzers) are hosted as
  `ui_element` panels inside Glasswave where possible.
- Anything that defines the Win7/WMP/WMC look (frame, transport, lists, navigation, album
  grid, WMP-style visualizations) is drawn by Glasswave, because hosted panels render with
  their own styles and cannot sit on glass.

## Consequences

- Need a `ui_element_instance_callback` host implementation and config persistence (M2).
- Hosted panels are opaque child windows; they go in opaque content areas, not on glass.
