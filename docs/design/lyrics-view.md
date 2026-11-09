# Lyrics view

## Source selection (owner clarification, 2026-11-09)

An explicit selection is remembered per track, including across restarts. Automatic
selection first checks embedded lyrics and the adjacent `.lrc`, then the streaming
provider's native lyric endpoint. Fallback source results are cached by track identity,
subsong, metadata and duration. Searchers execute concurrently, but a lower-priority source
cannot replace a successful higher-priority source. Local files and each streaming
provider have separate fallback orders in Preferences > Display > Glasswave > Lyrics.
NetEase tracks retain their native endpoint ahead of every fallback script.

Right-clicking either the sidebar lyric body or the transport lyric row opens the same
menu. It offers source information, search / result selection, a specific-source search,
loading an LRC or text file, a fresh automatic search and restoring automatic selection.
Manual search permits edited title / artist, shows source and timing type, and previews
the candidate before loading. Closing the search or changing tracks cancels the search.
Clearing automatic caches preserves explicit selections. Re-searching resets the selected
override for that track. Script enablement or priority changes invalidate automatic caches.

The lyrics pane recreates Apple Music's lyrics (iOS 17+, macOS 14+) on the window's Aero
glass. Code: `ui/lyrics_view.{h,cpp}` (rendering, motion, input), `ui/spring.h`,
`gfx/frame_clock.{h,cpp}`. Owner references: `.local/refs` captures of Apple Music on iOS
(2026-11-09). Numbers marked AMLL come from the documented behaviour of the AMLL project
(applemusic-like-lyrics, AGPL-3.0), which measured Apple Music; only values were taken.

## Layout (DIP; F = lyric font size)

| What | Value | Source |
| --- | --- | --- |
| Pane width | 440, at most half the window | new UI |
| Join to the client area | none: one client edge runs around both | owner, 2026-11-09 |
| Pane tint over the glass | black 30 % | new UI, legibility of white text |
| Header | 84 high: cover 52 (radius 6), title 16 semibold 95 %, artist 14 regular 60 % | AM iOS header |
| F | round(pane width x 0.075), 20..42 (33 at 440) | AM iOS: 30 pt on a 390 pt screen |
| Font | Segoe UI Variable Display (else Segoe UI), bold; translation semibold 0.55 F | AM: SF Pro bold |
| Hover plate | from 12 to width - 12, radius 0.35 F; text inset 0.45 F | AM iOS capture |
| Line block | padding 0.4 F above and below; translation 0.3 F below the line | AMLL |
| Focused line | 1.5 fixed reference blocks below the header. Each reference block has one unwrapped original line and one unwrapped translation line, using the current fonts and padding. The anchor is independent of the song's wrapping and translations | owner, 2026-11-09 |
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
- **Blur**: 0 for the active line. Sung lines above: (distance + 1) x 0.8 DIP (AMLL).
  Upcoming lines below grow slower so the next few stay readable: 0.3 + 0.5 x (distance - 1)
  DIP (owner, 2026-11-09; AMLL: (1 + distance) x 0.8). At most 5. Eased over 0.4 s. A hovered
  line is sharp.
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

## Rendering and frame pacing

Each line is a composition visual (a container with the hover plate and a sprite with the
line's own surface). The surface is drawn once with Direct2D in white; the compositor applies
position, scale, opacity, the blur (a GaussianBlur effect brush, swapped for the plain
surface brush when sharp) and the edge fade (the lines sit in a LayerVisual whose alpha is
multiplied by a gradient mask). Only a line that is active or fading out is redrawn, with
its word fill, float and glow. Lines more than a screen away release their visuals and
surfaces.

`gfx::FrameClock` waits on the compositor clock (`DCompositionWaitForCompositorClock`) and
posts one frame message at a time, so animation runs at the display's refresh rate. It runs
while lyrics play or anything moves and stops otherwise. Measured in the dev instance while
lyrics play: about 3 % of one core (the earlier full-surface Direct2D redraw used ~45 %).

## Transport band

Two 12 DIP rows of 16 DIP centred on the band on both sides: title and artist (then the
Favorite star) over the current lyric line on the left, stream format over the output device
on the right. A word-timed line turns blue (#1F5FB8) from grey (#6E6E6E) as it is sung; a
line-timed one is blue. Without synced lyrics the single left row is centred.

## Pane and queue (`ui/now_playing_pane`, `ui/queue_view`)

The pane holds the header, the lyrics or the queue, and a 64 DIP bottom bar with a segmented
control (owner, 2026-11-09: an iOS tab switch instead of Apple Music's round buttons): a
track as wide and tall as the queue's capsules (20 DIP margins, 34 high, white 14 %) holds
"Lyrics" and "Up Next" (icon and 14 DIP semibold label); a white 78 % thumb inset 3 DIP sits
under the selected half, whose icon and label turn dark. The thumb slides on the switching
spring, and can be dragged: the content follows it and settles on the nearer side. View >
Lyrics opens the pane, View > Up Next its queue side.

The tab track, thumb and queue capsules use the same continuous-curvature outline from
the owner's two iOS photos. Cubic transitions join the flat edges to circular sides with
matching tangent and curvature. The profile is specified in `visual-spec.md`. These
surfaces are painted at layout/DPI changes and slide as compositor visuals.

The pane exposes the window's original Aero material and screen-anchored light streaks
under its 30% black tint. The common client edge is hollow, and the opaque library base
is a separate visual confined to the library rectangle.

Switching (spring k 260, c 30): the lyrics fade out over the first two thirds and rise
24 DIP; the queue fades in from 15 % on and rises 48 DIP into place; the header stays and,
in the queue, follows the song card.

Queue, from Apple Music's "Playing Next" on a 390 pt screen scaled to the pane:

| What | Value |
| --- | --- |
| Order | History (oldest first, Clear on the right), the song card (the header), shuffle / repeat capsules, Continue Playing with the song count, the rest of Now Playing |
| Opening position | the card at the top; scrolling up reveals the history |
| Row | 56 high, cover 46 (radius 5), title 16 at 95 %, artist 14 at 60 %, reorder handle (3 lines, 18 wide, 40 %) |
| Section titles | 20 bold; subtitle 14 at 55 % |
| Capsules | 34 high, two halves; off: white 14 % with a white glyph, on: white 78 % with a dark glyph; shuffle/repeat glyphs are 16 DIP bold (700), owner 2026-11-09 |
| Hover / pressed plate | white 8 % / 4 %, radius 10 |
| Rows | ordinary reordering uses springs (k 300, c 30). Track changes use mass 0.9, k 220, c 26, following AMLL's cover swap. Departing rows move with the list and fade over about 0.22 s, following AMLL's song-information exit. Growing history preserves the card's screen position and existing spring velocities |
| Reorder | drag by the handle: the row lifts (103 %, plate 14 %), the others make room, the queue playlist is reordered on release |
| Scroll | wheel 80 DIP per notch on a spring; drag 1:1 with inertia (0.95 per 60 Hz frame); past the ends iOS's rubber band (0.55) and a spring back |
| Edge fade | 16 DIP at the top, 40 at the bottom |
| Actions | click: play (history: right after the current song); right-click a queued song: Remove from Now Playing |

The sidebar reads the real Now Playing playlist. It starts after the current song, which
appears in the card. Repeat-all appends the beginning of the playlist up to the current
song, excluding that song. Without repeat-all only the remaining tail is shown. Finishing
playback keeps the last cursor in the open window, so an exhausted tail stays empty.

Shuffle remains foobar2000's native playback order. The sidebar follows the playlist's
stored order and animates to the new playing position after each track starts. A forward
jump moves the retained rows up, and a backward jump moves them down. A clicked row returns
the view to the song card on the same spring. Programmatic return scrolling stays continuous
when the list shrinks, while mouse scrolling keeps its rubber band. Wrapped drag reordering
maps the visible slots back to the underlying playlist and keeps the playing slot fixed.

Animation references are the locally supplied AMLL project's
`packages/react-full/src/components/Cover/index.tsx` and
`packages/react-full/src/components/MusicInfo/index.tsx`. The native implementation uses
their numeric tuning and transition behaviour. No AMLL source or artwork is included.

