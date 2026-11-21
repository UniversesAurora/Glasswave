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

The other colours of Win7's "Window Color and Appearance" page (Preferences > Window color),
read from Win7's `themecpl.dll` (swatch bitmaps 1-8, 21-28; zh-CN names: its .mui strings
101-116), in the page's order: Sky `74B8FC`, Twilight `0046AD`, Sea `32CDCD`, Leaf
`14A600`, Lime `97D937`, Sun `FADC0E`, Pumpkin `FF9C00`, Ruby `CE0F0F`, Fuchsia `FF0099`,
Blush `FCC7F8`, Violet `6E3BA1`, Lavender `8D5A94`, Taupe `98844C`, Chocolate `4F1B1B`,
Slate `555555`, Frost `DEDEDE`. A swatch sets only the colour; the intensity slider stays.

Intensity slider (0..100) to what themecpl passes `DwmpSetColorizationParameters`, from its
code: `alpha = (0.10 + 0.75 s/100) * 255`, `v = trunc((alpha/255 - 0.10) / 0.75 * 100 + 10)`;
transparency on: `v < 50` colour 5 / afterglow v-5 / blur 100-v; `50 <= v < 95` afterglow
95-v / blur 50-(v-50)/2 / colour the rest of 100; `v >= 95` colour v-25 / afterglow 0 / blur
125-v; reflection 50. Sky's alpha `0x6B` is slider 43 (v 52): 8 / 43 / 49, as above.

Lyrics pane: the playing cover's colour (`ui/cover_tint.cpp`, the most vivid hue lifted to
the swatches' range: saturation 0.35-0.95, value 0.55-0.98) at 34 % under the pane's shade,
700 ms colour fades, gone with the window's focus (Win7's inactive glass is clear).

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
- DWM draws the frame 1:1 in device pixels (Win7 at 150 %, owner capture 2026-11-07): outer
  dark line `#071521` and inner light line `#E0EAF2` (top `#F1F8FF`) are one device pixel
  each, and the client edge is one light (`#C5DDEC`) plus one dark (`#21465E`) device pixel.
  These lines do not scale with DPI. The corner radius is kept at 6 DIP (owner's choice).
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

### Narrow library window (owner's WMP 12 pressed to its narrowest, 2026-11-13)

WMP 12 goes down to about 500 DIP, but there its time runs into the capsule. The owner asked
for the order of giving way without the overlap (2026-11-13).

Nothing in the band overlaps. Left to right it holds:

1. the song's text (at least 110 DIP; its title, artist, star and lyric laid out within the width it has)
2. the time, when it stands left of the capsule
3. the capsule
4. the time, when it stands right of the capsule
5. the stream block
6. the lyrics toggle

The gaps between them are 12 DIP. The capsule stays centred while some step allows it. Otherwise it moves off centre by as little as needed before a further step is taken.

The narrowest arrangement needs a 516 DIP client. The window's minimum is therefore 540 DIP, plus the lyrics pane when it is open. As the window narrows it gives way in this order:

| Step | What changes |
| --- | --- |
| 1 | Shuffle and repeat leave the capsule; its left end moves to 8 DIP before stop |
| 2 | The volume slider folds behind a 14 DIP arrow beside the speaker (WMP's split button art 4627 / 4628 when lit). The arrow opens WMP's flyout over the button, with its right edge on the arrow's: the 5 DIP glass frame of the mini player round the 80 x 23 slider, over a frosted copy of the band. A click elsewhere or deactivating the window closes it. The capsule ends 9 DIP after the arrow |
| 3 | Stop leaves; the capsule starts 8 DIP before previous |

**The shortened capsule.** The capsule bitmap (wmploc 2164) has two divider lines baked in,
at columns 162 - 164 and 398 - 400. A shortened half therefore cannot be the bitmap shifted
inwards: the lines would land inside the previous and next segments. It is drawn instead as:

- its end (columns 77 - 92 on the left, 463 - 480 on the right), moved in;
- one plain column, stretched from the end to the orb.

**The folded speaker.** It is the mini player's split button:

- each half lights by itself (4628), and is pressed in 4627;
- both halves sit over the shared plain plate 4630, which fades in (160 ms) while either half is under the pointer or held, or while the flyout is open, and fades out in 260 ms;
- the arrow is SplitArrow's white vector.

The slider maps the pointer from its own rect, so it stays under the pointer inside the flyout.

**The time box.** It is as wide as the time's text can get for the song ("4:56 / 4:56"; Segoe UI's digits are tabular), not a fixed 110 DIP. The stream block starts 12 DIP after it.

**A cut lyric line.** Its karaoke fill is measured on the whole line, not on the cut one. Once the sung part reaches the cut, the visible line, ellipsis included, stays blue.

- The capsule bitmap is cut at the orb's centre, and its two ends move in independently.
- The time stands right of the capsule while 110 DIP + 20 fit there, else right-aligned left of it.
- The stream format block hides below 60 DIP.

The command bar and the navigation pane also give way:

- The search box goes from 230 DIP down to 140 (32 % of the client width).
- Create playlist, Add folder and Add files then go, last first, behind a » after the buttons that stay. Its menu runs their main actions. Organize always stays.
- The navigation pane narrows from 190 DIP to 150 (30 % of the client width).

## Now-playing queue icons (owner, 2026-11-09)

- Shuffle, repeat-all and repeat-one in the sidebar capsules use 16 DIP Segoe Fluent Icons
  (Segoe MDL2 Assets fallback), requested at bold weight 700 for heavier strokes.

## Now-playing capsules and glass (owner photos, 2026-11-09)

- Shape references are the owner's `Photo 1` segmented control (1280 x 307 JPEG) and
  `Photo 2` Apple Music capsule (393 x 313 JPEG). The bright capsule in Photo 2 spans
  approximately x 55..300 and y 96..210. Its shallow transition into the flat edge is
  reconstructed as a continuous-curvature profile, shared by the queue buttons, tab
  track and selected thumb.
- For height H, the circular side radius is R = H / 2 and the horizontal transition
  extends 1.30 R. In top-left coordinates normalized to R, the cubic runs from
  (1.30, 0) through (0.94974747, 0), (0.58578644, 0) to (0.29289322, 0.29289322),
  then follows the circle to (0, 1). The cubic has zero curvature at the flat edge and
  matches the circle's tangent and curvature at 45 degrees. The other quarters mirror
  this profile. Narrow bounds reduce R to fit. These values are fitted to the JPEG
  contours, with the tangent and curvature constraints enforced analytically.
- The shared client edge is a hollow two-device-pixel frame. The opaque WMP content
  base `#F1F7FD` fills only the library client rectangle. The lyrics/queue pane exposes
  the window's Aero material and screen-anchored reflection streaks through its existing
  30% black tint, as specified in ADR 0010.

## Lyrics search window (new UI, 2026-11-09)

The lyric search uses native Win32 controls, as the existing text prompt and preferences
do. Its initial outer size is 640 x 540 DIP, minimum 520 x 400, with 16 DIP margins,
8 DIP gaps and 24 DIP fields / buttons. Text uses Segoe UI 9 pt. The result list and
read-only preview share the remaining height equally. Columns are source 130, title 180,
artist 130 and timing 130 DIP, with native horizontal scrolling when needed. These are
new application layout values, not measurements of an Apple or Win7 window.

## Reflection (R6)

Soft white bands leaning like "/", anchored to the monitor, intensity 50.

## Apple Music account window (new UI, 2026-11-09)

The native account window uses existing Win32 controls and Segoe UI 9 pt. Its client
area is 460 x 330 DIP with 16 DIP margins. Account, password and verification fields
are 24 DIP high; labels are 20 DIP high. The password and code fields are 310 DIP
wide, with a 12 DIP gap before their 106 DIP action buttons. The status region is
428 x 64 DIP and the Close button is 106 x 28 DIP. These are application layout
values, not measurements from Win7 or Apple Music.

## Provider group browsing (new UI, 2026-11-09)

Provider-defined genre/composer lists reuse the existing Details list, header and row
metrics. The Name column fills the remaining width; the right-aligned Songs column has
an initial width of 100 DIP. This column width is an application default, not a Win7
measurement. Navigation rows reuse the existing provider row metrics and icons.

## List status band (new UI, 2026-11-10)

The transient loading, cached, partial and error band reuses the measured row height
(22 DIP), text inset, typography and header background palette. In Details and grouped
layouts it sits directly below the measured 23 DIP column header; in tile layouts it
starts at the list top. The band occupies content height and introduces no new color or
icon.

## Artist overview (new UI, 2026-11-14)

Icon tiles keep WMP 12's measured album tile (132 x 160 DIP). Each group starts with a
30 DIP header line: the title in the grouped view's blue (`#2E64B4`) at 14 DIP, the group
rule (`#C9D7E8`, one device pixel) after it, and "Show all (N)" right-aligned 8 DIP from
the edge in the same blue, the link colour (`#0066CC`, underlined) under the pointer. Tile columns share the row's spare
width evenly (the pitch is the row width over the columns that fit).

Full-screen pane card: `OVERLAYS.OUTLINE.PNG` (ehres, 287 x 86, nine-grid 8) is a white plate
at alpha 0x46 with a two-pixel bright edge (0x74, 0x5D) and a corner about 3 px round; the
blue rim under it uses the same 3 DIP radius.

## Search drop-down and page tabs (2026-11-15)

The drop-down follows the owner's Windows 7 Explorer captures (search box drop-down with
"添加搜索筛选器", the size filter's value list, a hot filter button). Measured colours:

- Frame: 1 px `#646464`; list area white; a CS_DROPSHADOW shadow (Explorer: about 5 px fading
  from `#8C8C8C` to the background). Without earlier searches the list area is left out.
- Footer: background `#FCFCFC`, top rule `#DFDFDF`, then a fade from `#EDEDED` to the footer
  over 5 DIP; its label grey `#6D6D6D`.
- Scope buttons: blue text `#0070C0` with 3 DIP insets on 18 DIP plates, 20 DIP lines (Explorer's
  are tighter still); under the mouse (fading in, out slower) and for the chosen scope (black
  text) Explorer's filter button, measured on the owner's 150 % capture (28 px high): a `#B0B0B0`
  frame with about 2 px corners, a white line inside it (brightest over the upper half), and a
  glossy grey: `#FBFBFB` to `#F2F2F2` at 43 %, a step to `#E4E4E4`, `#ECECEC` at 65 %, `#FBFBFB`
  at the foot. The label line 18 DIP, 4 DIP footer padding.
- Hot row: Explorer's item plate, 1 px `#83ABDC` frame with 2 DIP corners over a vertical
  gradient `#EFF5FC` to `#CCE1FB`. Rows 21 DIP, text 8 DIP in, the system message font.
- An earlier search's row shows aero.msstyles' tooltip close button on the right under the mouse
  (IMAGE 878 / 879 / 880 by DPI: grey cross, red cross on a plate, pressed).
- The scope token in the box is the filter blue `#0070C0`, followed by the typed text.
- The box's magnifier (and its cross while it holds a search) are glass glyphs after the
  owner's capture of Explorer's magnifier: the strokes (1.8 / 2.0 DIP) filled `#6CC0FF` to
  `#2A7FE0` to `#13489E` (hot cross `#8FD3FF` to `#3A95EE` to `#1D5FC0`), a 1 DIP white rim, a
  18 % black shadow offset (0.4, 0.9) DIP.

The detail band's Play all on album and playlist pages: a 14 DIP green triangle (rounded
corners, `#8CE36A` / `#3DB52A` / `#1E7F14`, a white rim, a 22 % shadow; no green play glyph was
found in wmploc, aero.msstyles or ehres) before the label. The page's Collect stands at the
command bar's right where Refresh stood (Refresh left the bar; F5 and the page menu remain).

Page tabs (owner's choice of Media Center's large text tabs, 2026-11-15; new UI values): a 44
DIP band over the list on the library background, a `#D5DFEB` hairline at its bottom. Labels in
Segoe UI Light 20 DIP from 20 DIP in, 28 DIP apart, `#1E395B`, centred above the bar: the
current tab opaque, the others at 45 %, brightening to 85 % under the pointer with the
breadcrumb's fade. The current tab's bar is a square 3 DIP strip on the bottom rule (`#6BB7F5`
to `#2F7FD0`) as wide as its text, in its own visual: it slides to a newly chosen tab on the
compositor in 500 ms with Windows 8.1's fast-out curve, cubic-bezier (0.1, 0.9, 0.2, 1).

Mixed results mark each cover's bottom-right corner with the source's own app icon (18 DIP;
14 on song cards), read at run time from the Start menu (Apple Music, 网易云音乐) or Windows
Media Player's icon (wmploc 102) for the local library. A tile's play button takes the mark's
place while it shows.

Search page tiles (new UI values after Apple Music's search page): every overview group is a
shelf that pages sideways. Song cards (`TileStyle::Song`) 280 x 60 DIP, a 44 DIP cover 8 DIP in,
title and artist on two 18 DIP lines 10 DIP after it; top results and songs three rows high,
the others one row of WMP album tiles. A group's "show all" (19 DIP) is aero.msstyles' expando
button (IMAGE 693 / 694 / 695 at rest, 696 - 698 lit, 699 - 701 held; 96 / 120 / 144 DPI) in
vectors, measured on the 144-DPI bitmaps (a 29 px disc about (14.5, 15.5)): a metallic disc flat
`#F4F4F4` to row 11.5, then down to `#C1BFBF` at row 24.5 (lit `#EEF7FD` to `#9FCFE9`, held
`#DEF0F9` to `#61A8CD`), a 1.8 px light ring 1.75 px inside (rest `#FBFBFB` top to `#E1E0DF`
low; lit `#F9FCFE` to `#D0E8F4`; none held), a one-pixel rim (`#8C8C8C`, lit `#3C7FB1`, held
`#2C628B`), and a `#333333` chevron with a one-pixel white edge, turned to point right about
the disc's centre with the bitmap's one-pixel offset towards its point. Lit and held fade in
over the rest look; held, the chevron sinks half a DIP. A shelf's paging arrows (26 x 56 DIP, as
tall as Apple Music's) are a thin chevron (`#1E395B`, 2.2 DIP, round caps) that fades in while
the mouse is over the shelf, in the command bar's glass plate (aero Toolbar BUTTON 849, hot
state, nine-grid 3) only while the mouse is on the arrow; the shelf's tiles slide a page on the
compositor in 500 ms with Windows 8.1's fast-out curve, cubic-bezier (0.1, 0.9, 0.2, 1). Source
marks fade out as a tile's play button fades in.
Play buttons fade in under the mouse (0.2 per 16 ms tick, out at 0.15): on a cover, WMP 12's
transport play orb (wmploc 4153 cropped round its orb, 4155 faded in on the button, glyph 4162)
at 30 DIP, 4 DIP from the cover's bottom-right corner; on a song card, a 22 DIP triangle after
wmploc 5346 (rounded corners, `#5BB6FF` / `#1A84EC` / `#0A4FC0`, brighter under the mouse, a
white rim, a 28 % shadow) over a 22 % black veil on the cover.

## Downloads list (2026-11-15)

The Progress column draws Windows 7's progress bar at its bitmap height, 14 DIP, centred in the
row: the trough is aero Progress 648 (19 x 14; `#B2B2B2` top edge, `#8C8C8C` bottom edge, rows
`#FCFCFC` to `#BCBCBC` with a step at row 7) as a nine-grid with 3 px borders; the fill is 656
(green, 51 x 14, 7 px ends, a centre highlight `#CDFFCD` / `#00D328`), 658 while paused and 657
after a failure, nine-grid with 7 px ends and cut at the downloaded share. The navigation item
is WMP's download activity arrow at rest (wmploc 45221's last frame, 16 x 16 at x 288, y 9) redrawn
in vectors from its pixels (see `DownloadArrow` in `ui/nav_pane.cpp`).

The search box's cross is WMP 12's clear button (wmploc WW_CLEAR_144, 39 x 30 per state): a 25
DIP square plate at the box's right end between its edges. Hot: side borders `#3C7FB1`, an inner
light line `#FAFDFE` to `#E8F5FC`, fill `#E9F6FD` > `#D8EFFC` stepping at row 14 of 30 to `#BEE6FD`
> `#A6D8F4`. Pressed: borders `#2C628B`, an inner shadow (top `#9EB0BA`, sides `#A5BFCD` > `#5C97B9`),
fill `#E6F2FA` > `#C5E5F6` stepping at row 15 to `#9DD4F0` > `#6CB2D8`; the cross shrinks 15 % and
moves 0.5 / 0.75 DIP right and down. Hot fades in about 120 ms, out about 250 ms; it clears on release.
A running bar carries Windows 7's glow: aero 664 (127 x 18, the move overlay) drawn at the bar's
height, sweeping from before the bar's start to the fill's end in 1.7 s, resting 0.7 s, clipped
to the fill, repainted about 30 times a second only while a running bar is on screen.

Song lists' Download column (32 DIP, before the length, after Apple Music's mark): a 16 DIP disc
(both kinds the same outer size, no plate behind) with a white arrow of round 1.7 DIP strokes (a
stem and an open head). Downloaded: Apple's grey disc `#C4C4C4`, fading to `#A0A0A0` under the
mouse. Offer on the hovered row (shown at once with the row): a glass ball in WMP 12's play-orb
colours (wmploc 4153 sampled down its middle): body `#1A4E9E` at the top, `#0C3E90` at the
horizon, `#0C55A3`, `#0C85D8`, `#2BAAEA`, `#44CAFA` at the foot, a cyan glow at the foot, a white
gloss (78 % to 28 %) over the upper part ending in a sharp horizon, a `#0A3478` rim. Under the
mouse it lights as the orb does (wmploc 4155): the body azure (`#2A7CC4`, `#0060B1` at the
horizon, `#0A7ACA`, `#38ADE6`, `#5FD1F7`, `#7AF0FC` at the foot), the foot's glow nearly white
cyan, the rim `#0372C0`, and a `#23A4EF` halo (55 % at its peak) 3 DIP round the disc; fading in.
The arrow sits on a navy shadow. Downloading: a 13 DIP ring, 3 DIP wide, over a
`#DADADA` track, filling clockwise from the top with round caps in Windows 7's progress colours
(green `#7EEB86` to `#0AB526`, paused `#FFE27A` to `#D49B00`, failed `#F08080` to `#C81E1E`).

## Home cards (2026-11-15; new UI values)

After Apple Music's Top Picks (owner's screenshots at 200 %: cards about 228 x 305 DIP, about
10 DIP corners), without its grey hover veil; the hover is a Material card's lift instead (owner).
Windows 7 has no such cards, so these are new values (`ui/card_layer.h`, `ui/list_view.cpp`):

- Card 220 x 293 DIP (3:4). Corners of 12 DIP radius with continuous curvature (Figma's corner
  smoothing at 0.6: a short arc between two Bezier curves, so no joint shows where the curve leaves
  the straight edge; owner, 2026-11-15); the picture, the video's mask and both shadows share the
  outline. Cards stand 20 DIP apart at a fixed pitch (240 DIP) from 18 DIP in, the last one
  showing in part (owner: no spreading). A `TileStyle::Card` tile is the card plus 10 DIP each side
  and 12 DIP above, 18 below. A shelf is one strip, scrolled as a whole: paging slides it on the
  compositor (500 ms, Windows 8.1's curve).
- Picture: a tall picture fills the card (cropped about its centre); a square cover spans the top
  and melts into the card's colour over its lower 45 %. A hairline of white at 10 % runs round
  the edge. NetEase's daily card carries the date's day in 52 DIP semibold white at 16, 10 DIP.
- Words from the bottom left, 16 DIP in: label 12 DIP semibold at 62 %, title 15 DIP semibold
  (2 lines), subtitle 13 DIP at 88 % (2 lines), all 182 DIP wide (beside the play orb), over a
  scrim of the card's colour rising from 70 DIP above the label (0 %, 55 % / 35 % halfway for tall
  / square pictures, 92 % at the bottom). Colours are the source's (Apple: the artwork's `bgColor`,
  `textColor1`, `textColor2`); otherwise the cover's tint (ui/cover_tint.h) mixed 38 % towards
  black, with white words (dark words on a light colour).
- Elevation after Material Design 1's cards (owner: Material 1, about half the first try's lift):
  resting 2 dp, raised 8 dp under the mouse, 4 dp held down, each two black shadows in the card's
  shape - a key light (short, offset down) and an ambient light (wide, faint) - with the card
  rising and growing a little about its centre. DIP, opacity: rest key y 1.5 blur 3 26 %, ambient
  y 0.5 blur 6 12 %; raised rise 3, scale 1.015, key y 6 blur 10 24 %, ambient y 2.5 blur 16 16 %
  (240 ms); pressed rise 1, scale 1.005, key y 3 blur 5 25 %, ambient y 1 blur 9 14 % (100 ms);
  settling back in 320 ms; all on the curve (0.4, 0, 0.2, 1). The raised card is above its
  neighbours.
- Play: WMP's play orb (`wmp::transport::PlayOrb`) at 34 DIP, 12 DIP from the bottom right corner,
  fading in with the hover (160 ms, out 260 ms), lit under the mouse; only on cards that can play.
- Motion covers (Apple's `editorialVideo.motionTallVideo3x4`): the smallest H.264 rendition of
  the HLS playlist at least the card's width in pixels, muted and looping, fading in over the
  picture in 700 ms once open; only while the card is on screen, the window is not minimized and
  Windows' animation effects are on.

Radio mode (`media/radio.h`): while a station that takes "recommend less" plays (NetEase's personal
FM), the shuffle toggle's place shows a heart struck through. In the main window it is drawn as
its neighbours are (wmploc 4175 / 4177 measured: `#0066CC` ink 2 px wide, a 1 px white edge at
`FFFFFF/91` = 57 %, the ink about 12.6 px across about (12.0, 12.2) of the 25 px box): NetEase's
heart outline traced from its client, 11.2 DIP wide on its centre line, 2 DIP round strokes with
a 1 DIP white edge; the slash at 45 degrees, 6.2 DIP up-right and 5.4 DIP down-left of the middle
(past the right lobe a little, the lower left a little more), cutting a 0.8 DIP gap through the
heart as shuffle's arrows cross. Lit, a radial `#63CFFC` glow behind (92 % at the middle, 55 %
halfway, gone at 12.5 DIP). The mini player keeps its own glyph. On other stations shuffle is
drawn at 35 % and does nothing; repeat cycles off / one.

Window open and close (`MainWindow::AnimateWindow`): the window's content grows to its size from
92 % about its middle while it fades in (250 ms, ease-out `(0.1, 0.9, 0.2, 1)`), and on close
shrinks to 94 % as it fades out (180 ms, ease-in) before foobar2000 exits; the Win7 shadow window
joins once the window stands at its size. Off when Windows' animation effects are off. The values
are estimated from Windows 7's DWM animation, not measured: capture Win7's open and close at
60 fps to replace them.

Breadcrumb (`ui/breadcrumb.h`): every level but the last has an arrow with a menu; hovering lights
the level's name and arrow together (WMP_BREADCRUMB_MAINBUTTON / _SPLITBUTTON: the part under the
pointer hot, the other grey; pressed while its menu is open), 25 % a frame in, 10 % out. A path
wider than the bar hands its first levels to the root arrow's menu.

## Open items

- Blur strength: DWM host backdrop blur is stronger than Win7's (ADR 0002).
- Hover/pressed colors are extrapolated (no reference yet): capture Win7 hover states.
- WMC visuals (menus, focus glow, transitions) not measured yet.
