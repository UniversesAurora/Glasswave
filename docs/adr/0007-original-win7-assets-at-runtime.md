# ADR 0007: Use original Win7 / WMP 12 / WMC artwork, loaded at runtime from user files

Status: accepted (2026-11-07). Amends ADR 0005.

## Context

Procedurally redrawn controls did not satisfy the owner ("the icons you drew are not right").
The owner supplied files copied from a Windows 7 installation: `wmploc.dll` (WMP 12 UI
resources), `ehres.dll` (Windows Media Center resources) and `aero.msstyles` (Aero theme),
plus icon archives. Windows 11 does not ship usable copies (its `wmploc.dll` is a 12 KB stub,
its `aero.msstyles` is the Windows 11 theme).

## Decision

- Glasswave reads PNG / bitmap resources **at runtime** from those files when the user
  provides them (a folder under the foobar2000 profile; dev builds copy them from
  `.local/refs` with a script).
- The repository only records **which** resource is used for what (resource type and ID,
  slicing, states) in `docs/design/win7-assets.md`. No Microsoft binaries or extracted
  images are committed; extracted previews live in `.local/assets-extract` (ignored).
- When the files are absent, the procedural painters (ADR 0005) remain the fallback.

## Consequences

- Pixel-identical artwork where originals exist (transport cluster, seek and volume bars,
  capsule, caption buttons, WMC focus art); vector rendering remains for the frame and glass.
- Original art is 96/120-DPI raster: at 175% it is scaled (high-quality cubic). Where a
  part looks soft, the procedural painter for that part can be preferred.
- Using the artwork is the end user's responsibility under their Windows license; the
  project itself distributes nothing.
