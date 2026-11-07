# Win7 asset inventory (runtime resources, ADR 0007)

Source files (user-provided, from Windows 7 SP1): `wmploc.dll` 12.0.7601.24382, `ehres.dll`,
`aero.msstyles`. Extract previews with the script used in development (pefile + Pillow) into
`.local/assets-extract/`; contact sheets `sheet_<file>_NN.png` list `<type>_<id> WxH`.

| File | Resource types with images | Count |
| --- | --- | --- |
| wmploc.dll | RT_BITMAP (mini mode, taskbar thumbnails), type 256, type 257 (WMP 12 UI PNGs) | 1036 |
| ehres.dll | RT_RCDATA (Media Center PNGs + MCML markup) | 2286 |
| aero.msstyles | IMAGE (Aero theme parts) | 601 |

## WMP 12 transport (wmploc.dll, type 257) — first findings

| ID | Size | Content |
| --- | --- | --- |
| 4153-4156 | 182x51 | previous + play orb + next cluster, per state |
| 4160 | 52x52 | play orb glow ring |
| 4162 | 42x43 | play triangle glyph |
| 436-439 | 142x39 | compact transport (mini mode) |
| 331, 332, 350 | 1024x16 | seek bar track / fill variants |
| 2164, 2200 | 558x68 / 558x69 | transport capsule background |
| 2100-2119 | various | volume slider parts and thumbs |
| 1740-1745 | 25x15 | small buttons (repeat/shuffle states) |

To do: map every state (normal / hover / pressed / disabled) per control, the navigation bar
back/forward orbs, list header and selection art, then the Aero caption buttons and frame
parts in `aero.msstyles`, and the Media Center focus / menu art in `ehres.dll`.
