# Visual specification (measured)

All values are in DIP (pixels at 96 DPI) unless noted. Each section names its reference.
Reference images live in `.local/refs/` (not committed, ADR 0005). Rendering rule: hairlines
are `floor(scale)` device pixels, rectangles snap to the device grid; we reproduce the
geometry and colors, not the pixelation of 96-DPI originals.

References used so far:

- R1: WMP 12 transport, dark "now playing" skin, 2x crop (orb 92 px).
- R2: Win7 caption buttons over light glass, 2x crop.
- R3: Windows Media Center window with alpha channel, 1x (frame lines, shadow, corners).
- R4: WMP 12 library mode on the Win7 desktop, **120% DPI** (caption 36 px = 30 DIP).
- R5: "Play To" dialog (WMP 12 transport with enabled, blue glyphs).
- R6: Win7-style desktop (1918x948) for reflection direction and general look.

## Glass colorization

Win7 "Sky": ColorizationColor = ColorizationAfterglow = `0x6B74B8FC`, balances color 8 /
afterglow 43 / blur 49, reflection intensity 50. Check: over a white backdrop the formula
gives `#b8dbfd`, identical to the glass measured in R2.

## Window frame (R3, R4)

From the outside in, per side:

| Line | Color | Width |
| --- | --- | --- |
| Outer line | `#020303` a 0.84 | 1 |
| Highlight | `#f5f5f6` a 0.84 (top edge `#f9fafa` a 0.90) | 1 |
| Glass | colorized backdrop | 4 (sides/bottom) |
| Client edge, light | `#e0e5e9` a 0.65 | 1 |
| Client edge, dark | `#1b1e21` a 0.66 | 1 |

- Caption: outer edge to client = 30 (maximized: 22). Corners: radius 6 on all four corners.
- Client inner glow (R4): first 3 px of the client `#d7f7ff`, `#def6fe`, `#e5f7fc`, then `#e9f2f8`.

## Window shadow (R3)

Alpha per pixel away from the window edge (index 0 = adjacent):

- left `126 98 75 58 48 39 31 23 17 12 8 4 2`
- top `115 86 63 47 37 29 23 16 12 7 4 2`
- right `158 137 121 106 96 84 72 60 49 40 31 24 17 12 8 4 2`
- bottom `162 144 129 116 106 95 84 72 60 49 39 31 24 17 12 7 4 2`

Corners use a 10-DIP rounded core blending the two side profiles.

## Caption buttons (R2, R4)

- Sizes: minimize 26, maximize 27, close 47, height 20; 1 below the outer top edge,
  6 from the outer right edge. Cluster bottom corners radius 3, faint light line outside.
- Body: split at 47% height. Solved from the same buttons over light (`#b8dbfd`) and dark
  (`#3d70a1`) glass:
  - min/max upper `rgb(210,232,253)` a 0.72 -> `rgb(200,226,252)` a 0.40; lower navy veil
    `rgb(0,20,50)` a 0.16 -> 0.08 plus a faint bottom glow; inner rim top white a 0.88,
    sides a 0.35, bottom `rgb(200,230,255)` a 0.5; outline `rgb(0,18,40)` a 0.72.
  - close upper `#e2968f`..`#cc6f66`, lower `#b13a2e` -> `#c2584a`, rim `#fff0f0` / `#daa8a7`.
- Glyphs (R4, 120% assets which Win7 uses at high DPI): minimize bar 12.5 x 2.5, 4 below
  center; maximize frame 13 x 10, 2.5 thick; close X 14 x 10.5 with flat ends, bar 3.6
  measured horizontally. White fill, one-hairline outline `rgb(0,20,40)` a 0.5.
- Inactive window: close loses its red; all buttons become paler glass (Layer::Inactive).

## Caption text

Segoe UI 9pt black; glow = soft white block (blur sigma 3.5) at a 0.62, padded 10 so it is
never clipped.

## WMP 12 library layout (R4, converted from 120%)

| Area | Height | Colors |
| --- | --- | --- |
| Navigation bar | 29 | `#daefff` -> `#e2ebf8` -> `#e7eefe`, rule `#c3cccf` |
| Command bar | 28 | `#f5feff`, `#e9f3fd` / `#dce6f2`, `#e1eaf9`; rules `#cbd2e2`, `#abb1bf` |
| Content | rest | `#e0e7f5` -> `#f2f8ff` over the first ~14, then flat |
| Transport band | 45 | rules `#99a1aa`, `#ecf3fc`; fill `#e8f0f9` |

## WMP 12 transport (R1, R4, R5)

- Capsule 333 x 26, fully rounded; outline `#9ba3ac` -> `#b3bdca`, inner highlight `#dee6ee`,
  fill `#e4ebf4` -> `#e1eaf7`.
- Offsets from the orb center: shuffle -141, repeat -116, divider -97, stop -78,
  volume +78, slider +92..+156.
- Play orb 36 + 1 white halo ring (`#e0f2ff`). Outline `#7d8396` (top) -> `#437b81` (bottom).
  Upper gloss `#c0cae2` -> `#85a8cc` ending at ~45%; lower body `#1c357a` -> `#2685d0` ->
  `#3fb9e8` -> `#5fcde5`; bottom rim `#99f4ff`; navy edges `#0d3277`. Play triangle
  12.5 x 15, left edge 4.6 left of center.
- Previous/next segments: height 21, reach 60 from the orb center, outer radius 4;
  upper `#e5f2f9` -> `#d4e3f5`, lower `#b1c2d3` -> `#d2e2ef`, outline `#a3afbb` -> `#6f7d8b`.
- Glyphs blue when enabled (`#3c88da` -> `#154ea2`, R5), grey when disabled (`#aab4bf`).

## Reflection (R6)

Soft white bands leaning like "/", anchored to the monitor, intensity 50.

## Open items

- Blur strength: DWM host backdrop blur is stronger than Win7's (ADR 0002).
- Hover/pressed colors are extrapolated (no reference yet): capture Win7 hover states.
- WMC visuals (menus, focus glow, transitions) not measured yet.
