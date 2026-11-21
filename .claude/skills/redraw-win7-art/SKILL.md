---
name: redraw-win7-art
description: Redraw a piece of Windows 7 / WMP 12 / Media Center artwork (a thumb, orb, button plate, glyph, frame) in crisp vectors from the original bitmap - how to observe its pixels, read its shape, colour, light and edges, turn that into a Direct2D model, fit the model to the pixels, preview it and wire it in with fading states. Use when a bitmap looks blurry, frayed or jagged at high DPI, or the owner asks for an HD / vector / 1:1 redraw of Win7 art.
---

# Redrawing Win7 art in vectors

The owner's bar is 1:1 with Windows 7, but sharper than the 96-DPI originals. What failed
before, and why this process exists:

- **Enlarged bitmaps** (waifu2x, cubic) look soft and frayed at the edge. Fine for large soft
  gradients, never for outlines, horizons or small glyphs.
- **Hand-picked colours** (a gradient through one column of the bitmap, "rows as bands") look
  banded and miss the light: the owner called the flyout ball "too blurry" and then "not the
  same colour, not crystal clear".
- **What worked**: a small layered model that Direct2D can draw exactly, fitted to every pixel
  of the bitmap by least squares, previewed enlarged next to the original, then drawn with
  sharp edges where the original has sharp edges. The flyout ball and the seek thumb redrawn
  this way were accepted ("这次重绘的滑块不错").

## 1. Find the original

- The extracted artwork is in `.local/catalog/<module>/<type>/<id>.png` (wmploc 257, ehres 10,
  aero) and indexed in `.local/catalog/index.tsv`; the map of what is where is
  `docs/design/win7-asset-map.md`. Re-extract with `scripts/catalog-art.ps1`.
- Find every state: normal / hot / pressed / disabled are separate ids (for example 2102,
  2103, 2104), often consecutive. A toggle has on / off glyphs (4174 / 4175).
- If the owner says the look comes from somewhere specific (a cyan rim, a glow), look for it in
  the bitmaps before inventing it: scan the catalog for the colour. If no bitmap holds it, it
  is drawn by the window (DWM border, a code-drawn frame), and the owner's screenshot is the
  reference.
- Never commit these bitmaps (ADR 0005). Tables of measured colours in code are fine.

## 2. Look at it

Run both, side by side:

```
uv run --no-project --with pillow python .claude/skills/redraw-win7-art/scripts/zoom_sheet.py out.png 12 a.png b.png c.png
uv run --no-project --with pillow python .claude/skills/redraw-win7-art/scripts/dump_pixels.py a.png
```

The zoom sheet (nearest neighbour on a checkerboard) shows the structure; the dump gives the
numbers (`RRGGBB` opaque, `RRGGBB/aa` translucent, `.` empty). Read, in this order:

1. **Outline and size.** Where alpha becomes partial: that is the antialiased edge. Estimate
   the true geometry from the partial alphas (an alpha of 0x71 on the top row of a disc means
   the edge crosses about 56 % into that row). Decide the shape: circle, stadium (pill),
   rounded rect, and its centre and radius in bitmap pixels.
2. **Sharp breaks.** Walk down the middle column: where neighbouring rows jump (#4674C8 then
   #053AAE) there is a horizon, a frame line or a band edge. Those must stay sharp at any
   scale; everything else is a smooth gradient.
3. **Bands along the edge.** Compare the outermost columns / rows with the middle: a rim
   (darker, translucent), an inner frame (lighter), a column inside it. Check the right side
   mirrors the left and that the middle columns are all equal (then the middle can stretch).
4. **Light.** Where is it brighter than the gradient alone would be: a crescent under the upper
   rim (the sides of the upper half brighter than its middle at the same height), a glow rising
   from the foot (a bright spot low in the middle, the sides staying dark), a gloss band.
   Compare the same row at the middle and near the edge, and the same distance from the centre
   along different directions: if they differ, it is a radial layer, not a vertical gradient.
5. **Translucency.** Partial alpha inside the shape means glass over whatever is behind; draw it
   translucent too. A colour seen over black is colour x opacity: from two backgrounds (the
   owner's screenshots over dark and over light) solve for both.
6. **States.** Diff normal / hot / pressed: usually the same structure with shifted colours, so
   one model with three colour sets.
7. **The owner's screenshots.** Captures at 200 % show how the original renders on screen
   (`.local/refs`, or the images in the chat). Measure there what the bitmaps do not hold, and
   compare the final result there.

## 3. Choose a model Direct2D can draw exactly

Build the look from primitives whose maths you can reproduce in Python:

- linear gradients (vertical bands, row by row), with extra stops 0.001 apart at a sharp break;
- radial gradients (`CreateRadialGradientBrush`, clamped beyond the last stop);
- radial "about a segment" for pills: radial brushes on the ends and the same stops mirrored on
  a vertical gradient between them, the parts split by `ALIASED` axis-aligned clips (they then
  share edges with no seam);
- a geometry mask (`PushLayer` with the outline) for the shape's antialiased edge;
- `ALIASED` clips for the sharp breaks inside (the horizon), so both sides snap to the same
  device pixel;
- strokes for rims (`DrawGeometry` of the outline inset half the stroke width).

For glossy thumbs and orbs the model in `scripts/fit_glass.py` covers it: per half a three-stop
vertical gradient, a crescent (radial, transparent to a radius, rising to the rim) and a glow
(radial, fading from its centre), and a rim stroke in the bitmap's outermost colour per row.
For button plates: one vertical gradient per distinct column (rim, frame, inner column, middle;
`scripts/plate_columns.py`), each through all rows with sharp stops at the band breaks, the
middle stretched. For glyphs: paths (Bezier, arcs), widened strokes (`ID2D1Geometry::Widen`)
united with filled heads (`CombineWithGeometry`, union). A closed figure that is widened becomes
a ring: arrowheads must be filled paths, not widened ones.

## 4. Fit it to the pixels

```
uv run --no-project --with pillow --with numpy --with scipy python .claude/skills/redraw-win7-art/scripts/fit_glass.py \
    .local/catalog/wmploc/257/2102.png --shape 6,6,8.5,6,8 --out <scratchpad>/fit --name ball
```

- Model each pixel as the mean of 4x4 subsamples (that is what the bitmap's antialiasing is).
- Leave out the outermost ring (within about 1.3 px of the outline): it mixes rim, inside and
  background, and the rim stroke covers it. Fitting it makes the model fake a rim with a hard
  inner ring.
- Bound the parameters to physical ranges (centres near the shape, radii 0.3 - 1.6 R, a crescent
  starting before 0.9), and pull the base colours weakly towards the half's mean. Unbounded fits
  reach the data with nonsense (pure white extrapolated under a layer) that shows at high DPI.
- Many random starts (60), keep the best. Read the rms per half: about 1 - 2 levels above a
  horizon and 4 - 8 below were good enough for the owner. More is a sign the model is missing a
  layer; look at a per-pixel residual map before adding one.

## 5. Preview before writing C++

`fit_glass.py` writes `<name>_compare.png` (original enlarged | model enlarged) and
`<name>_model.png`. Read them: the horizon sharp, the crescent and glow where the original has
them, no rings, no banding, the rim neither too thick nor too light. Compare with the owner's
screenshot of the original on screen as well. Only then generate the tables (`fit_glass.py`
prints a `GlassLook` initializer) and paste them into the code with a comment saying which
bitmaps they were fitted to and the rms.

## 6. Wire it in

- Draw in DIP with one bitmap pixel = one DIP; place sprites on whole device pixels.
- Each state is its own sprite (`PlayerWindow::FadingThumb`, `Widget` layers), painted once per
  scale, crossfaded on hover / press (in about 140 ms, out about 260 ms, held 60 ms). Never repaint
  a surface to change state: the owner wants every state change to fade.
- Things that appear and disappear (a seek thumb on hover) fade the container.
- A toggle's "on" is what Windows 7 shows (WMP: a cyan #63CFFC glow behind the glyph), not a
  pressed plate, which reads as a leftover hover.
- Keep `/W4` clean; remove the bitmap path it replaces and any upscale entries in
  `scripts/upscale-art.ps1` that are no longer used.

## 7. Record it

Add the measurements, the model and the rms to the matching `docs/design/*.md` section
(`player-window.md` "Glass thumbs" is the example), so the next change starts from numbers, not
from a fresh look. Then build, start the dev instance and let the owner judge: they compare on
their screen at 200 %.
