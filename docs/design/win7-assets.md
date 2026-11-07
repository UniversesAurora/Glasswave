# Win7 asset inventory (runtime resources, ADR 0007)

Source files (user-provided, from Windows 7 SP1): `wmploc.dll` 12.0.7601.24382, `ehres.dll`,
`aero.msstyles`. Extract previews with the script used in development (pefile + Pillow) into
`.local/assets-extract/`; contact sheets `sheet_<file>_NN.png` list `<type>_<id> WxH`.

| File | Resource types with images | Count |
| --- | --- | --- |
| wmploc.dll | RT_BITMAP (mini mode, taskbar thumbnails), type 256, type 257 (WMP 12 UI PNGs) | 1036 |
| ehres.dll | RT_RCDATA (Media Center PNGs + MCML markup) | 2286 |
| aero.msstyles | IMAGE (Aero theme parts) | 601 |

Run time: Glasswave loads the files from `<profile>\glasswave\` (`wmploc.dll`,
`aero.msstyles`, `ehres.dll`) with `LOAD_LIBRARY_AS_IMAGE_RESOURCE`; `scripts/run-dev.ps1`
copies them there from `.local/refs/`. Code: `win7/win7_resources.*`, `wmp/wmp_transport.*`.

## WMP 12 transport (wmploc.dll, type 257) — in use

Positions are relative to the cluster bitmap origin. They were found by matching the
bitmaps against a native WMP 12 capture (Win7 SP1 VM, `.local/refs`): the cluster matched
with zero error, every glyph within the capture's noise. The orb center is (108, 26).

| ID | Size | Content | Placed at |
| --- | --- | --- | --- |
| 4153 / 4155 / 4154 | 182x51 | cluster normal / hover / pressed (stop plate, prev, orb, next) | (0, 0) |
| 4156 | 182x51 | cluster, darker rim (not used yet; probably focus) | |
| 2164 | 558x68 | capsule incl. divider and volume groove | (-171, -9) |
| 4160 | 52x52 | orb glow ring, fades in on hover | (82, 0) |
| 4162 / 4163, 4164 / 4165 | 42x43 | play, pause glyphs (normal / disabled) | (87, 4) |
| 4168 / 4169 | 53x25 | previous glyph (normal / disabled) | (38, 13) |
| 4166 / 4167 | 53x25 | next glyph (normal / disabled) | (126, 13) |
| 4170-4173 | 53x25 | rewind / fast-forward glyphs | |
| 4179 / 4180 | 25x25 | stop glyph (normal / disabled) | (2, 13) |
| 4175 / 4174 | 25x25 | shuffle off / lit (on, hover) | (-74, 13) |
| 4177 / 4176 | 25x25 | repeat off / lit | (-43, 13) |
| 5244 / 5245 / 5246 / 5247 | 25x25 | speaker: muted / 0 / 1 / 2 waves | (192, 13) |
| 5678 | 80x23 | volume fill, drawn up to the thumb | (222, 13) |
| 5679 / 5681 / 5680 | 14x23 | volume thumb normal / hover / pressed; center x 233..288 | |
| 4205-4212 | 47x25 | white glyph set (dark skins) | |
| 436-439 | 142x39 | compact transport (mini mode) | |

Band: 56 px from the dark top rule (#9DA1A6) to the client edge; the cluster origin is 5 px
below the rule, the orb centered horizontally.

## Navigation bar, library

| ID | Size | Content |
| --- | --- | --- |
| 5350 / 5352 / 5351 / 5353 | 54x27 | back + forward orbs normal / hover / pressed / disabled (27 px each) |
| ALBUM_0_PNG..ALBUM_6_PNG | 200x200 | generic "no album art" covers |
| ALBUMDROPSHADOW_200X200_PNG, 4161 | 200x200 | album cover drop shadow (8 px blur margin) |
| STARS_PNG | 429x16 | rating stars |
| LISTVIEWHEADER_PNG | 6x92 | list header states |

In the 150 % capture the back/forward orbs sit 8 px from the client edge and 4 px below it.
WMP 12 draws these bitmaps 1:1 at every DPI; Glasswave draws one bitmap pixel per DIP
(WMP's 96-DPI proportions) and resamples with high-quality cubic filtering.

## DWM atlas (aero.msstyles, STREAM 971) — in use

Win7's uDWM.dll (6.1.7600, 328 KB) holds no images. The caption buttons, their hover glow,
the frame parts, the top-left glare and the reflection texture live in one 1357x780 atlas,
resource `STREAM` 971 of aero.msstyles. Each part's rectangle is the `TMT_ATLASRECT`
(8002) property of class DWMWINDOW in the theme's `VARIANT/NORMAL` data; records are
32-byte headers (name, type, class, part, state, flag, reserved, size) followed by the
value padded to 8 bytes. Code: `aero/aero_atlas.*`.

| Part | Atlas rect | States | Sizing (l, r, t, b) | Content (l, r, t, b) | Use |
| --- | --- | --- | --- | --- | --- |
| 5 / 6 | 83,88-114,176 / 83,264-114,352 | 4 | 12,12,6,8 | 3,1,2,4 | minimize, active / inactive |
| 3 / 4 | 83,352-114,440 / 83,176-114,264 | 4 | 12,12,6,8 | 2,3,2,4 | maximize, active / inactive |
| 7 / 8 | 83,568-134,656 / 134,0-185,88 | 4 | 20,20,6,8 | 1,2,2,4 | close, active / inactive |
| 9 / 10 | 83,656-134,744 / 83,480-133,568 | 4 | 20,20,6,8 | 2,1,2,4 | close alone (both corners) |
| 16 | 83,440-132,480 | 1 | 9,9,8,7 | | min / max hover glow |
| 11 | 261,0-327,40 | 1 | 9,8,8,7 | | close hover glow |
| 12-15 | | 4 | | | close glyph at 96 / 120 / 144 / 192 DPI |
| 17-20 | | 4 | | | help glyph |
| 21-24 / 25-28 / 29-32 | | 4 | | | maximize / minimize / restore glyphs |
| 48-51 | | 4 | | | small (tool window) close glyph |
| 34 | 327,0-555,350 | 2 | | | top-left glare, active / inactive (opacity 75) |
| 40 | 555,0-1357,604 | 1 | | | reflection streaks (opacity 90 active, 75 inactive) |
| 39, 54, 56, 58, 62, 66, 70, 1, 2, 36... | | | | | frame edges and corners (not used yet) |

Button states are stacked top to bottom: normal, hot, pressed, disabled. The glyph set is
chosen by DPI (192 at 200 %), so caption glyphs are drawn 1:1 on the dev machine.

To do: list header and selection art, scrollbars and Explorer parts in `aero.msstyles`, the
Media Center focus / menu art in `ehres.dll`.
