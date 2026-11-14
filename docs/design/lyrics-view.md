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
| F | round(pane width x 0.075), 20..42 (33 at 440), times the user's lyrics size (50 - 200 %, default 100) | AM iOS: 30 pt on a 390 pt screen; owner, 2026-11-12 |
| Font | Segoe UI Variable Display (else Segoe UI), bold; translation semibold at the user's translation size (25 - 150 % of the same automatic size, default 55) | AM: SF Pro bold |
| Font list | optional, preferences: family names separated by commas; each character comes from the first listed family that has it, then from Windows' fallback (DirectWrite font fallback). All heights are the laid-out metrics of the fonts actually used | owner, 2026-11-12 |
| Hover plate | from 12 to width - 12, radius 0.35 F; text inset 0.45 F | AM iOS capture |
| Line block | padding 0.4 F above and below; translation 0.3 F below the line | AMLL |
| Swap original and translation | a check item of the lyrics' context menu and a choice in the preferences: only the texts change places, everywhere a lyric shows (pane, transport band, player). The translation takes the lyric's size and, for a word-timed line, word timing of its own: its CJK characters or words (weight 1 + 0.3 per further letter) spread over the time the original's words are sung, the gaps between them kept, so it fills word by word | owner, 2026-11-13 |
| Focused line | 1.5 fixed reference blocks below the header. Each reference block has one unwrapped original line and one unwrapped translation line ("Ag" and a CJK character, so fallback fonts count), using the current fonts and padding. The anchor is independent of the song's wrapping and translations | owner, 2026-11-09 |
| Low view | the room goes in turn: below the focused line until it equals the room above; then above it, until none is left; then below again. The focused line (wrapped, with its translation) always stays whole: a window keeps the lyrics at least as high as their tallest line | owner, 2026-11-12 |
| Edge fades | lyrics fade out over 36 below the header and 72 at the bottom, each at most the room left above / below the focused line | AM; owner, 2026-11-12 |

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
  line is sharp. A line that has just been sung starts blurring as it rises and shrinks, while
  its glow still fades (owner, 2026-11-13; it used to stay sharp until the glow was gone).
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
- **Right-click**: the lyrics menu (below). View > Lyrics toggles the pane.

### The lyrics menu (pane, transport band, player; owner 2026-11-13, after ESLyric)

| Item | What it does |
| --- | --- |
| Save lyrics | next to a local track (its same-name .lrc; a cue song as "Artist - Title.lrc"), else to `<profile>\lyrics\Artist - Title.lrc`; UTF-8 with a byte order mark |
| Save lyrics to | Tag (SYNCEDLYRICS if the file has it, else LYRICS; local files), Profile folder, Track's folder, Browse... |
| Select lyrics | the candidates this session's searches for the song found (the automatic lookup's and the search dialog's), the shown one checked; Search lyrics... at the end |
| Copy lyrics | Copy lyrics (with the remembered options) and Copy... (metadata, timestamps, merge identical lines, reference lines, standard lyrics: word times left out; the last two only with timestamps, standard only for word-timed lyrics). Times are the shown ones, offset applied |
| Lyrics encoding | lyrics read from a file (a same-name .lrc, a loaded file) read again in Auto detect or one of ESLyric's ten code pages; the result becomes the song's selection and remembers file and code page |
| Lyric info | source, file, encoding and offset over the document as found, read-only, with Copy |
| Adjust offset | +100 ms (earlier), -100 ms (later), +1 s (earlier), -1 s (later), Custom offset... (a signed number), Reset; kept per lyric of a song (owner, 2026-11-14: each source's or version's lyric is timed differently), named by the lyric's parsed lines and times, in the lyrics cache folder and applied before anything shows; saving writes it into [offset:]. Lines are retimed in place, not flown in again |
| Reload lyrics | the song's lyrics read again (selection, cache, same-name file), without a new search |
| Hide translation / Hide original | a check item among the display items: hides the small line under each lyric, the translation, or the original when the two are swapped; remembered |

The hover plate belongs to the line under the pointer: when lines move under a resting
pointer (the song advancing, the wheel, inertia) the plate passes to the line now under it.
The queue does the same, and finds its row again after a refresh (play counts written,
the schedule changing) instead of dropping the highlight.

Searching (Search lyrics..., Search lyric source, Load lyrics from file..., Search for lyrics
again, Use automatic lyrics) and the display items follow.

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
"Lyrics" and "Up Next" (icon and 14 DIP semibold label); a white 66 % thumb inset 3 DIP sits
under the selected half, whose icon and label turn dark. The thumb slides on the switching
spring, and can be dragged: the content follows it and settles on the nearer side. View >
Lyrics opens the pane, View > Up Next its queue side.

The tab track, thumb and queue capsules use the same continuous-curvature outline from
the owner's two iOS photos. Cubic transitions join the flat edges to circular sides with
matching tangent and curvature. The profile is specified in `visual-spec.md`. These
surfaces are painted at layout/DPI changes and slide as compositor visuals.

In the library window's sidebar Media Center's moving light (`AeroLight`, the veil kind, at
85 %, mirrored left to right) lies over the pane's tint and under its content, as behind the
mini player and the full screen (owner, 2026-11-13); lent to the player the pane shows none of
its own.

iOS's material lies under the pane's light controls, on and off alike (owner, 2026-11-13): the
switch's track and thumb, the shuffle / repeat capsules and the hover plates of lyric lines and
queue rows. What lies behind is blurred (9 DIP), its colours deepened (saturation 1.8) and
mixed with 6 % white; the control's own white is painted over it as before (the light thumb
and the capsules' on state 66 % instead of 78 %). Capsules take their exact outline through an
alpha mask; plates are rounded rectangles that follow the plate showing most (its offset,
scale and opacity). The glass lies outside the lines' layer, whose content cannot see behind
the pane.

Ghost pictures in the pane (pieces of the library's list header, a playlist card, a cover over
the header; owner reports 2026-11-12/13) were surfaces shown without ever being drawn: a new
surface comes from a shared atlas and holds other surfaces' old pixels until it is drawn, and
when its BeginDraw failed most callers attached it anyway. `CompositionHost` now clears such a
surface as soon as the UI thread is free (unless it was drawn since), reports the failure on the
console, and reports draws begun off the UI thread or inside another draw; the pane's header,
message, queue rows and lyric lines no longer keep an undrawn surface and draw again.

The pane exposes the window's original Aero material and screen-anchored light streaks
under its 30% black tint. The common client edge is hollow, and the opaque library base
is a separate visual confined to the library rectangle.

Switching (spring k 260, c 30): the lyrics fade out over the first two thirds and rise
24 DIP; the queue fades in from 15 % on and rises 48 DIP into place; the header stays still
(it no longer rides the queue's rise, owner 2026-11-13) and, in the queue, follows the song
card as the list scrolls.

Queue, from Apple Music's "Playing Next" on a 390 pt screen scaled to the pane:

| What | Value |
| --- | --- |
| Order | History (oldest first, Clear on the right), the song card (the header), shuffle / repeat capsules, Continue Playing with the song count, the rest of Now Playing |
| Opening position | the card at the top; scrolling up reveals the history |
| Row | 56 high, cover 46 (radius 5), title 16 at 95 %, artist 14 at 60 %, reorder handle (3 lines, 18 wide, 40 %) |
| Section titles | 20 bold; subtitle 14 at 55 % |
| Capsules | 34 high, two halves; off: white 14 % with a white glyph, on: white 78 % with a dark glyph; shuffle/repeat glyphs are 16 DIP bold (700), owner 2026-11-09 |
| Hover / pressed plate | white 8 % / 4 %, radius 10 |
| Rows | measured frame by frame from the owner's iOS 26 recording (`.local/refs/ios/ios-menu.mp4`, 60 fps): every move (a tapped row, Next, Previous, shuffle on or off) is one critically damped spring of about 20.5 rad/s (mass 1, k 420, c 41; 15 / 41 / 62 / 77 / 92 / 98.5 % of the way at 33 / 67 / 100 / 134 / 200 / 300 ms). The list travels the whole distance (a song tapped two rows down: the rows under it rise two rows). Rows that leave (the tapped one and those above it, the next song on Next, rows a reorder pushes out of the list) fade where they are, linearly, in 125 ms; rows that come back (Previous, shuffle) fade in at their place as fast; rows rising from below arrive solid with the list. Under shuffle the rows still listed cross to their new places. Growing history preserves the card's screen position and existing spring velocities |
| Footer | repeat-all: the repeat glyph and "Repeats N songs" (N: the songs of one round), 16 semibold at 55 %, centred, 64 high, under the last row and moving with it |
| Repeat-one | the list stays as without repeat (Next and Previous still go through it); only the song repeats |
| Reorder | drag by the handle: the row lifts (103 %, plate 14 %), the others make room, the queue playlist is reordered on release. A list refresh during the drag (a playlist still loading, the next song starting) keeps the row in the hand when the row is still listed. Under shuffle a drop reorders only the playing order, not the playlist (writing it into the playlist turned every song physically before the current one into history, and the list emptied); a list that became shorter than its scroll springs back to its end |
| Scroll | wheel 80 DIP per notch on a spring; drag 1:1 with inertia (0.95 per 60 Hz frame); past the ends iOS's rubber band (0.55) and a spring back |
| Edge fade | 16 DIP at the top, 40 at the bottom |
| Actions | click: play (history: right after the current song); right-click a queued song: Remove from Now Playing |

The sidebar reads the real Now Playing playlist (`PlaybackScheduler::DisplayUpcomingSnapshot`).
It starts after the current song, which appears in the card, and lists the rest of the round;
the next round of repeat-all is the footer, not rows. Finishing
playback keeps the last cursor in the open window, so an exhausted tail stays empty.

Shuffle remains foobar2000's native playback order. The sidebar follows the playlist's
stored order and animates to the new playing position after each track starts. A forward
jump moves the retained rows up, and a backward jump moves them down. A clicked row returns
the view to the song card on the same spring. Programmatic return scrolling stays continuous
when the list shrinks, while mouse scrolling keeps its rubber band. Wrapped drag reordering
maps the visible slots back to the underlying playlist and keeps the playing slot fixed.

AMLL has no queue or history list of its own (its prebuilt player only offers a playlist
button for the host to answer), so the queue's behaviour and motion follow Apple Music's
Playing Next on iOS, measured from the owner's recording (owner, 2026-11-14; AMLL's springs
were used before).

