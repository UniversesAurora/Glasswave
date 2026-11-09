# Lyrics view

The lyrics pane recreates Apple Music's lyrics (iOS 17+, macOS 14+) on the window's Aero
glass. Code: `ui/lyrics_view.{h,cpp}` (rendering, motion, input), `ui/spring.h`,
`gfx/frame_clock.{h,cpp}`. Owner references: `.local/refs` captures of Apple Music on iOS
(2026-11-09). Numbers marked AMLL come from the documented behaviour of the AMLL project
(applemusic-like-lyrics, AGPL-3.0), which measured Apple Music; only values were taken.

## Layout (DIP; F = lyric font size)

| What | Value | Source |
| --- | --- | --- |
| Pane width | 440, at most half the window | new UI |
| Gap to the client edge | 8 (glass) | new UI |
| Pane tint over the glass | black 30 % | new UI, legibility of white text |
| Header | 84 high: cover 52 (radius 6), title 16 semibold 95 %, artist 14 regular 60 % | AM iOS header |
| F | round(pane width x 0.075), 20..42 (33 at 440) | AM iOS: 30 pt on a 390 pt screen |
| Font | Segoe UI Variable Display (else Segoe UI), bold; translation semibold 0.55 F | AM: SF Pro bold |
| Hover plate | from 12 to width - 12, radius 0.35 F; text inset 0.45 F | AM iOS capture |
| Line block | padding 0.4 F above and below; translation 0.3 F below the line | AMLL |
| Focused line | its top at 35 % of the lyrics area | AMLL `alignPosition` |
| Edge fades | lyrics fade out over 36 below the header and 72 at the bottom | AM |

## Colour (white with alpha)

| State | Alpha | Source |
| --- | --- | --- |
| Inactive line | 0.24 (0.45 while the user browses) | AMLL dim layer 0.2 |
| Active line, sung part | 0.92 | AMLL bright 1 x line 0.85 |
| Active line, not yet sung | 0.36 | AMLL 0.4 x 0.85 |
| Translation | 0.32 | AMLL sub line 0.3 |
| Hover plate / pressed | 0.09 / 0.05 | AMLL #fff1 / #ffffff05, raised for the glass |
| Unsynced lyrics | 0.85, no blur, no focus | |

## Motion

- **Line position**: spring per line (mass 0.9). Playback: stiffness 170..220 depending on
  the time since the previous line (100..800 ms -> ratio^0.2), damping sqrt(k) x 2.2.
  Seeks, interludes, the first line: stiffness 90, damping 15 (AMLL).
- **Stagger**: when the focus moves, visible lines start 50 ms apart from the top; below the
  focus the step shrinks by 1/1.05 per line (AMLL). Seeks, scrolling and resizing move
  without stagger.
- **New lyrics** fly in from 60 % of the pane's height below, staggered.
- **Scale**: lines other than the active one shrink to 97 % while playing (left-centre pivot),
  spring mass 2, stiffness 100, damping 25 (AMLL).
- **Blur**: 0 for the active line; others (1 + distance) x 0.8 DIP, at most 5; lines above
  the focus count one line further (AMLL). Eased over 0.4 s. A hovered line is sharp.
- **Word fill** (word-timed lines): a soft edge 0.5 F wide sweeps across each word over its
  duration, bright to the left, dim to the right; a wrapped word fills across its runs (AMLL
  `wordFadeWidth`).
- **Float**: a sung word rises 0.05 F, ease-out over max(1 s, its duration), and settles
  back when the line ends (AMLL).
- **Emphasis**: a word held 1 s or longer (any CJK word; Latin words of 2..7 letters) grows
  up to 1 + 0.1 x amount, rises 0.025 x amount F and glows (white, radius up to 0.3 F) on a
  rise-and-fall curve; amount = (d / 2 s) cubed below 1, sqrt above, x 0.6, x 1.6 for the
  line's last word, at most 1.2 (AMLL emphasize).
- **Activeness** (bright layer) fades in over 0.3 s, out over 0.45 s.
- **Interlude**: a gap of 7 s or more shows three dots (0.3 F, gap 0.18 F) in the line's
  place; they light up one after another through the gap, the group breathes (+-8 %, 4 s),
  swells to 125 % over the last second and vanishes in the last 250 ms (AMLL interlude dots).

## Interaction

- **Hover**: plate fades in over 0.25 s; the line sharpens.
- **Press**: the line (plate and text) sinks to 95 % on a stiff spring (k 420, c 32); release
  on the same line seeks to its start and resumes playback if paused.
- **Drag**: more than 4 DIP of movement scrolls the lyrics 1:1 (no spring) with inertia
  (decay 0.95 per 60 Hz frame). Wheel: 90 DIP per notch on a fast spring. While browsing all
  lines are sharp and brighter.
- **Return**: after 0.5 s without input the view realigns at the next line change, or after
  3 s once the mouse has left the pane; seeking realigns at once (AMLL resume delay 0.5 s).
- **Right-click**: Search for lyrics again, Hide lyrics. View > Lyrics toggles the pane.

## Frame pacing

`gfx::FrameClock` waits on the compositor clock (`DCompositionWaitForCompositorClock`) and
posts one frame message at a time, so animation runs at the display's refresh rate. It runs
while lyrics play or anything moves and stops otherwise.
