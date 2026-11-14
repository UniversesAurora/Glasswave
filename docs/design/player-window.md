# Player window: mini player and full screen

`ui/player_window.*`, `ui/album_wall.*`, `ui/aero_light.*`. One window, two forms, both on
Windows Media Center's wall of album covers. The library window hides while it shows and lends
it its lyrics / queue pane (`NowPlayingPane::Reparent`), so lyrics, queue and history are the
same objects in all three places.

The owner's brief: the modes of Apple Music's mini player and full screen (strip, cover, cover
with lyrics or queue; full screen with lyrics or queue), in the Aero language, taking Media
Center as the starting point and going further where its motion is plain. Controls are the
original Windows 7 artwork (ADR 0007), upscaled 2x by `scripts/upscale-art.ps1`; flat or
frosted stand-ins are not wanted.

## Entering and leaving

| Action | Result |
| --- | --- |
| Click the cover in the transport band, the corner button beside the lyrics button, Ctrl+Shift+M, View > Mini player | mini player |
| F11, View > Full screen, the mini window's maximize button, double-click on the wall | full screen |
| Esc | full screen -> mini -> library |
| The back orb (full screen), the corner button (mini), the close buttons | library |
| L / Q, the two buttons at the right of the transport | lyrics / queue |

The player window gets the application icon and its own taskbar thumbnail buttons
(`PlayerHost::AdoptPlayerWindow`).

## Forms

**Mini** is WMP 12's now-playing window: the Win7 glass frame (`GlassMaterial`, frame outline,
client edge, `ShadowFrame`, the DWM caption buttons), the wall inside it, WMP's narrow
transport below: shuffle and repeat (where they leave 10 DIP after the volume), stop,
previous, play, next from the wmploc cluster. The speaker is followed by a volume slider where
the window is wide enough for it before the transport (about 560 DIP); narrower, the speaker's
button carries an arrow and opens WMP's volume flyout above it (see "Volume"). The stage
decides the layout: a strip (cover left, lines beside it) when it is lower than 200 DIP or at
least 1.5 times as wide as high, a cover with the lines centred under it otherwise, or the pane
(the window grows to 720 DIP for it and returns to 540). The smallest window is 344 DIP wide
(the volume's split button, the cluster, lyrics and queue; heart, shuffle and repeat leave
first) with a client 96 DIP high, a little above the control bar that lies over the picture
(owner, 2026-11-12). In the strip the cover takes the stage's height less 24 DIP, at most 45 %
of the width; the margin before it is 7 % of its side (at least 12 DIP) and the gap to the lines
13 % (at least 14 DIP), so they grow with it (owner, 2026-11-13); its lines (title 17, artist and album 13, the time 13, the sung line 14 DIP at
scale 1, the block centred on the cover) scale with the cover (side / 115) within their width
(column / 170), from 0.8 to 2, never higher than the stage. Under the cover the lines are title
19, artist, album, time and the sung line 14 at the window's text scale. In both, the gaps
between lines (4.4 DIP after the title, 3.2 after the others at scale 1) grow faster than the
type: x (1 + 0.9 (scale - 1)) above scale 1, so large lines in a roomy window do not crowd
while small ones stay close (owner, 2026-11-12).

The time line, in all three forms (Media Center's #8CCBFF): the position and the song's length,
"1:23 / 4:56"; a click on it counts down instead ("-3:33 / 4:56") and back, remembered in the
settings.

The mini window's small buttons (shuffle, repeat, stop, heart, lyrics, queue) share the look
of WMP 12's speaker button (owner, 2026-11-12): nothing at rest, under the pointer WMP's square
plate 4725 (a navy rim, a light inner frame, a pale top over a sharp horizon, a cyan glow rising
to a bright foot), pressed 4724, stretched sideways to the button. On (shuffle, repeat, the
pane's side that shows) glows as WMP's toggles (4174 / 4176 against 4175 / 4177): a #63CFFC disc
behind the glyph, solid to 5 DIP from the centre, alpha 0.69 at 6.5, 0.42 at 8, 0.20 at 9.5,
0.07 at 11, gone at 12 (a pressed plate for "on" read as a hover left behind). The plates are drawn in
vectors from four of the bitmaps' columns (rim, frame, the column inside it, the middle), each a
gradient through its 25 rows, smooth within the bands and sharp between them (rows 1, 2, 12, 23,
24). Over it Media Center's glyphs at one pixel per DIP (TELETEXT, GUIDE, STOP, the heart), and
shuffle / repeat in the same glyph style in WMP's shapes. Shuffle, repeat and stop are 25x25,
heart, lyrics and queue 29x25, six apart. Every state change fades (the widgets' layers).

**Full screen** is Media Center's now-playing screen with its windowed-mode chrome: the back
orb top left, minimize / restore / exit top right, the bottom bar with the controls at the
right in WMC's order (lyrics, queue | stop, previous, play, next | mute, volume down, up) and
the seek bar in the space left of them. Chrome and pointer leave after 3 s without movement.
Without the pane the cover and the song's lines stand side by side as in WMC, the sung line of
synced lyrics as the last of those lines (flush left, the lines' size, shrinking to 70 % before
it is cut); with the pane the cover moves left and the pane lies on a card of Media Center's
overlay glass on the right (OUTLINE, a 32 DIP soft shadow), the wall around it left lit. Inside
the outline, in place of BACKGROUND.GRADIENT's deep opaque blue (owner, 2026-11-13: too dark),
clear glass: the wall behind blurred (14 DIP), its colours deepened (x 1.5) and mixed with
Media Center's navy #0B2C6B at 42 %, about as deep as the blue behind the song's lines and the
menu; like Win7's window frame but heavier, and not frosted. The card's own shadow is cut away
under the glass so that it blurs the wall, not the shadow. The clear rim between the outline
and the glass takes the same navy at 20 %, unblurred (owner, 2026-11-13).

When the chrome leaves or comes back the cover grows or shrinks and the group moves; the
song's lines and the sung line keep step with it, scaled from their top left on the cover's
curve (ease 0.16, 0.9, 0.2, 1) from their old place and size, instead of fading out and in
(owner, 2026-11-13). They are painted at the new size at once.

Between two songs the player never shows "nothing playing": while playback switches (from the
start or stop command to the new song decoding) it keeps the previous song's cover, lines and
lyrics, and shows the next song (its tags or the provider's record, its cover; no lyrics yet)
as soon as a provider input starts opening it, before its stream is resolved. A gapless
pre-open while a song still plays does not count. A real stop shows nothing playing.

## Artwork used (ehres.dll unless noted)

| Part | Resource |
| --- | --- |
| Full-screen transport, caption buttons, back orb | `TWOFOOT.CONTROL.<NAME>.<NOFOCUS/FOCUS/PRESSED/DISABLED>` (38x31, play 42x43, caption 29x24, back 42x42) |
| Lyrics / queue buttons | `TWOFOOT.CONTROL.TELETEXT` (a page of text; Windows 7 has no lyrics glyph), `TWOFOOT.CONTROL.GUIDE`; in the mini window their NOFOCUS glyphs on WMP's square plate |
| Mini small buttons' plate | wmploc 4725 (hot) / 4724 (pressed), 25x25, in vectors from their columns |
| Volume arrow (full screen without wmploc) | `TWOFOOT.CONTROL.DOWN` 16x16, on the speaker's plate |
| Plates of buttons without art (speaker + arrow in full screen) | `TWOFOOT.CONTROL.PLUS.FOCUS/PRESSED`: columns 0 - 9 and 28 - 37 kept, column 10 (frame and glow only) stretched |
| Favourite | a heart drawn in the TWOFOOT glyph style (rim #063E67, face #E6EFF3 -> #AFC8D7, shadow 1.4 DIP down), 38x31 like its neighbours |
| Bars | `TWOFOOT.FULLSCREEN.BOTTOMBAR.LEFT/RIGHT` 531x53, `TOPBAR.LEFT` 129x55, `TOPBAR.RIGHT` 210x64, `TWOFOOT.CONTROL.DIVIDER` |
| Volume overlay | `OVERLAYS.OUTLINE` 287x86 (nine-grid, margins 8), `OVERLAYS.BACKGROUND.DARK` 150x60, `OVERLAYS.VOLUME.BAR.OFF/ON` 150x10, `OVERLAYS.VOLUME.MUTE` 41x36 |
| Pane card (full screen) | `OVERLAYS.OUTLINE`, `OVERLAYS.BACKGROUND.GRADIENT` 500x300 |
| Seek bar | none: the library window's WMP 12 seek bar (`wmp::paint::SeekBar`) |
| Blue behind the song's lines | `NOWPLAYING.MUSIC.META.GRADIENT` 600x225 |
| No cover | `AUDIO.NOCOVER` (cover), `AUDIO.NOCOVER.1-6.LARGE` (wall) |
| Light | `COMMON.BACKGROUND`, `.GRAD1/2`, `.LAYER1-6`, `.SHARDA1-3`, `.SHARDB1-3`, `.GOODSHARD`, `.LITESHARD` |
| Mini transport | wmploc cluster 4153-4155 and glyphs (as the library window), speaker 5244-5247 |
| Switch glyphs | wmploc 4601 (to now playing, library window), 4603 (to library, mini) |

Each falls back to procedural painting when the user's files lack it.

Notes from the owner's review (2026-11-12):

- WMP's speaker and stop glyphs carry a pale halo made for its light capsule and showed white
  edges on the dark bar, as did the white-framed corner glyph 4603. The mini window therefore
  takes stop, speaker and the back orb from Media Center's set too, and its bar is a deep blue
  (0x051739 -> 0x030C24) rather than black, the ground those plates were drawn for.
- The lent pane is embedded (`NowPlayingPane::SetEmbedded`): no tint rectangle and no switch
  of its own. In the mini window the wall behind it sinks into deep blue with no edge
  anywhere: a vertical gradient over the whole window (0x061C46 at 58 % at the top, 80 - 84 %
  through the middle, 88 % into the bar) and a faint blue light (0x3070C8, 20 %) from above.
  Full screen it lies on the pane card.
- The seek bar is WMP 12's narrow transport's, drawn in vectors (sharp at any DPI) to the
  colours of its bitmaps, one bitmap pixel per DIP: the track 4159 (a translucent groove 6
  high: outer ring #DCD8E5 at 29 % above / #ECEAF0 at 32 % below, inner ring #373639 at 44 %
  / #3F3C41 at 25 %, clear inside), the played part 3881 (rows #7FC9FA / #014CB6 / #619FF2
  inside the groove, 2 DIP in from its ends), its head 3882 (each row a gradient through the
  bitmap's 16 columns) and, while the pointer is on the bar, the thumb (4058; 3884 with the
  pointer on it, 3883 while dragged), a 16 x 9 pill of glass drawn as the flyout's ball (see
  "Glass thumbs"). Compared on the owner's WMP with and without the pointer.
  The thumb's three states are visuals of their own riding the head: the thumb fades in in 160
  ms when the pointer comes onto the bar and out in 280 ms when it leaves, its lit state the
  same under the pointer, the dragged state in 60 ms.

## The wall (measured from the owner's WMC recordings, 3322x1832 at 30 fps)

| Quantity | WMC | Here |
| --- | --- | --- |
| Tile side | 297 px = 0.162 of the screen height | 0.162 of the monitor's height in both forms (at least 84 DIP) |
| Drift | upwards, about 4.4 px/s = one tile in 68 s | the same, one linear animation per tile height, then every tile shows the place below |
| Tile look | the picture edge to edge, no seams; each tile darker along its top (about 20 px of 295) and left (18 px) edge with a darker pixel or two at the edge: the shadow of its upper and left neighbours (change.mp4, PixPin_2026-11-11_23-23-39) | that shade as one shared bitmap per tile (top 7.5 % at 0.52, left 6.5 % at 0.44, seam 0.8 % at 0.30) |
| Entrance | tiles land one by one at random places over about 2.7 s; the wall itself does not move. Each appears about 6 % larger and faint (the blue through it), is at its size after 9 frames of 30 and solid after about 15 (enter.mp4) | random order over 2.6 s, a little faster at first; each falls straight down: scale 1.08 -> 1 accelerating (bezier 0.55, 0, 0.9, 0.55) in the first 42 % of 720 - 880 ms, dips to 99.4 % and comes up; opacity 18 % at once, solid at 70 %; a soft square shadow under it closes in from (3.5 %, 5 %) of the side and fades as it lands; later tiles lie above earlier ones |
| Dimming behind text | blue over the tiles, soft edges | WMC's own gradient bitmap, stretched; navy 0x0B2C6B at 22 % over the whole wall |
| Track change | cover cross-fades in about 0.3 s | cross-fade 380 ms, the new cover grows from 95.5 % |
| Idle layout | after about 22 s the menu leaves, the cover grows and moves right | not taken over |

The grid starts at the window's corner and moves with it (a wall held still on the screen
jittered behind a dragged mini window). The tiles have the full screen's size in the mini window
too, so the mini wall is the full screen's top left: a window that grows (resizing, or the mini
window becoming the full screen) keeps the tiles it showed and the places it uncovers fill with
falling tiles within 0.8 s; the entrance is not played again.

Owner's choice of covers (2026-11-13): the wall shows the playing playlist (else the active
one), each album once: a streamed track by its album's picture key, a local file by album
artist and album, or by folder without an album tag. The covers are dealt like cards: one
round holds every album once, made up to 24 cards with the six coloured no-cover tiles
(`AUDIO.NOCOVER.1-6.LARGE`, ehres.dll) when there are fewer albums; it is shuffled and dealt
onto the places in the order they appear (the grid row by row, then each row the drift brings
in). When a round runs out the next one is shuffled. A card showing the same picture as any of
the eight neighbours, or as the card dealt just before, is passed over for the next card that
does not and dealt later in its round; only when no card left fits is the next round searched,
and only coloured tiles alone can still meet. A cover is painted only while a tile shows it,
so a long playlist costs no more than the tiles on screen.

Owner's addition (2026-11-13): an album without a cover takes no part (it leaves the deck and
any place showing it as soon as that is known). A card whose cover has loaded is dealt before
one still loading; a loading card is dealt only when no loaded one fits, shows its colour and
turns into the cover when it arrives. The cards next in line, about two screens of places,
load ahead of the drift; the rest of a long deck loads only as it comes up.

Covers are decoded at the tile's own size in pixels (and fetched from NetEase in the rendition
for it). Like the player's cover, a smaller thumbnail already decoded for a list may stand in,
scaled up, and the tile repaints with the sharp cover as soon as it arrives (owner, 2026-11-14:
tiles stayed blurry until they were dealt again). Neither change jumps (owner, 2026-11-14): a
cover arriving on a tile fades in over its colour in 600 ms, the sharp cover over its blurred
stand-in in 400 ms, both on one ease-out curve (0.4, 0, 0.2, 1).

Addition: the tiles float like boxes packed together on very slow water. They never move apart;
the water's height under each (two waves, periods of about 21 s and 33 s, crossing the wall at
different angles) sets how deep it lies in its neighbours' shadow: the shade's opacity is
0.72 + 1.1 x (mean height of the upper and left neighbour - own height), clamped, and a tile
lower than the water's mean is up to 10 % darker. Drift and swell run only while music plays;
paused, they go on at a quarter of their pace (as WMC's wall slows rather than stops); the
swell rises over 5 s after the entrance. The owner slowed the waves to periods of about 35 s and
55 s (2026-11-13).

Owner's change to the entrance (2026-11-13): WMC's 6 % looked too close to the wall and the
accelerating fall stiff, ending in a snap. Each tile now falls from 118 % on one curve that
leaves gently and eases onto the wall (cubic-bezier 0.38, 0, 0.22, 1, landing at 62 % of 900 -
1060 ms), dips to 99.3 % and rises again softly; its shadow starts proportionally farther away.

## The light

`AeroLight` rebuilds Media Center's moving background from its own layers: the ground bitmap,
two corner gradients, six sheets of blue light and eight shards. Each sways sideways (5 - 14 %
of the width, 31 - 59 s) and breathes in opacity (11 - 29 s) on its own period, eased at both
ends, so the pattern does not repeat for minutes. It lies under the wall (seen before the tiles
arrive) and once more, faint, over the covers (beams at up to 0.62,
sheets at 22 %). The owner asked for this light to move all the time: it runs while the window
is visible, as compositor animations only, and pauses when the window is minimized or hidden.

## Volume (`ui/wmc_volume.*`)

Media Center's own overlay, measured on volume.mp4 (2.23 px per DIP): the outline 160x86 DIP,
the plate inside it 150x74 from (5, 6.5), the bar its last 10 DIP, lit to the level and cut
square there; above it the level in figures (Segoe UI semi-light 35 DIP, the figures 25 DIP
high, #F2F2F2) or, muted, `OVERLAYS.VOLUME.MUTE` with the bar still showing the level. The
outline is a nine-grid whose 8-pixel borders keep one bitmap pixel per DIP. It neither moves nor
grows: it fades in in 250 ms and out in 300 ms where it stands. Full screen it shows top right
(57 DIP in from the right, 58 down) for 1.6 s after each change. Owner's additions
(2026-11-13): the lit bar does not jump to a new level, its end springs there (damping 0.72,
period 48 ms: quick, a hint of overshoot), so a run of steps reads as one motion; and under the
translucent outline lies clear glass, the content behind blurred (10 DIP), its colours deepened
(saturation 1.7) and mixed 16 % with #9CC8F0, as iOS's materials do.

The mini window's volume is WMP 12's throughout (Media Center's speaker and bar only without
wmploc). Wide enough, its speaker (25x25) and five DIP on its slider: the grey groove 2168, the
fill 5678 to the thumb and the thumb 5679 / 5681 hot / 5680 dragged, both 80x23, the thumb's
centre running from 11 to 66 DIP.

Narrower, the speaker and its arrow are WMP 12's split button (wmploc, 39x25, the speaker on the
left 25, the arrow on the right 14): at rest only the arrow; under the pointer the plate, the
half the pointer is on lit (4628, cyan from below) and the other plain (4630); pressed, that
half glows blue (4627); while the flyout is open the arrow stays pressed. The halves are two
widgets over one plain plate, so the plate fades in and out (160 / 260 ms) and each half's
light fades by itself as the pointer crosses from one to the other. The arrow baked into
those bitmaps came out jagged when enlarged, so it is cut out of them (its 9x7 patch at x 27, y
10 covered by the plate's plain column 26) and drawn in vectors as 4626 has it: white rows 11 -
14 of 7, 5, 3 and 1 pixels (#FFFFFF to #DFDFDF) in a one-pixel navy shadow.

The speaker's half mutes, the arrow's opens WMP 12's volume flyout (measured on the owner's WMP at
200 %): 92 x 32 DIP over the button, its right edge on the arrow's, a frame of 5 DIP all round
in half-DIP rings lit from two sides. Over black, from the outside:

| Side | Rings |
| --- | --- |
| top / left | #414142 #BFBFC0 #C5C5C6 #545455 #313132 #808081 #6D6D6E #333334 #4D4D4D #B9B9B9 |
| foot / right | #003340 #0099BF #07A0C5 #144753 #2F2F2F #595959 #575757 #282828 #494949 #B9B9B9 |

The cyan rim runs the whole foot and right edge; none of the wmploc, aero.msstyles or ehres
bitmaps holds it (scanned), so it is the window's own border. Over the light speaker plate the
rings read much lighter (#282828 as #626B70): the frame is glass. It is drawn over the window's
content blurred (4 DIP), each ring a lighter colour at 45 - 85 % so that over black it gives the
value above. Inside, WMP's flat #F2F2F2 becomes frosted glass at the owner's request: milky white
at 74 % to 56 % with a sheen over the upper half. On it WMP's small slider (2100 / 2101, 84x17)
drawn afresh at any resolution: its groove a hollow tube three rows high from x 8 to 76 (top
#ADADAE, ends #C0BFBF, foot #CCCBCD, the inside showing the glass), the fill those rows solid,
each row a gradient through the bitmap's columns (#95B6D4 - #84C5FE, #7F98DF - #1043CD, #9CB0E6
- #6F97FE from left to right) that the level uncovers; the thumb (2102, 2103 hot, 2104 pressed) a
ball of glass (see "Glass thumbs"). The whole flyout takes presses and drags. The thumbs of the
flyout and of the inline slider are visuals of their own: lit fades in in 140 ms and out in 260,
held in 60.

### Glass thumbs

The flyout's ball (2102 / 2103 / 2104: a circle of radius 6 about (6, 8.5) in 12x17) and the
seek thumb (4058 / 3884 / 3883: a 16x9 pill) in vectors, the owner having rejected both the
enlarged bitmaps and a hand-coloured imitation. Each is glass cut by a sharp horizon (y 8, y 4):
above it a pale dome whose light gathers in a crescent under the rim, below it deep blue lit by
a cyan glow from the foot. Each half is a vertical gradient (three stops) with two radial layers
over it, a crescent (transparent to a fitted radius, then rising to the rim) and a glow (fading
from its centre), radial about the shape's centre line so that the pill's ends curve as the
ball does (radial brushes on the ends, the same stops mirrored on a vertical gradient between).
The geometry and colours of each layer were fitted to the bitmaps' pixels by least squares over
4x4 subsamples, leaving the outermost ring to the rim (rms error 1.4 - 2.1 levels above the
horizon, 4.6 - 7.8 below); the rim is a one-unit stroke in the bitmaps' outermost colours at
85 %. The tables are in `ui/player_window.cpp`.

The bubble shows the level centred over the thumb of the inline slider or the open flyout:
after the pointer has rested there for the system's tooltip pause (the double-click time), at
once while dragging, and for 1.6 s after a wheel or key step (over the speaker when neither is
there). It is a 40 x 24 DIP pane of the window frame's glass, corners 4 DIP: the window behind
it blurred (2.5 DIP), Aero's tint (#C4D8EC at 24 % to #3A5E90 at 26 %, breaking at the middle),
DWM's own reflection streaks and top-left glare from its atlas, a gloss over the upper half, a
cyan glow rising from the foot, a light inner edge (white above, pale cyan below) and a dark outer
one; the figure Segoe UI semibold 13 DIP white on a soft dark glow. It fades in in 140 ms and out
in 220 ms.

## Menu (misc.mp4, 2.37 px per unit of a 768-unit high screen)

Buttons the 251x51 `COMMON.BUTTON.LEFT.FOCUS`, 55 units apart, 41 in from the left; type 28
units from 16 in, #9CDDFC at rest and #F2F2F2 under the pointer, no shadow; the check mark
(27x26) centred 229 units from the button's left. `NOWPLAYING.MUSIC.LEFTMENU.GRADIENT` once,
410 units wide from the screen's edge and 65 units beyond the menu above and below.

## Menu press (button.mp4, frame by frame at 30 fps, five presses)

Measured as the 50 % point of each front, in fractions of the type's width, the highlight's
bright head in fractions of the button's, time from the first changed frame:

| | 0 ms | 33 | 67 | 100 | 167 | 200 | 233 | 267 | 333 | 367 | 400 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| black front | -0.15 | 0.47 | 1.0 | past | | | | | | | |
| white front | | | 0.11 | 0.38 | 0.555 | 0.645 | 0.725 | 0.80 | 0.91 | 1.0 | gone |
| highlight head | 0.03 | 0.61 | 0.96 | 1.08 | gone | | | | | | |
| highlight opacity | 0.3 | 1 | 1 | 0.35 | 0 | | | | | | |

The type does not turn black at once: from white a black shadow covers it from the left, fast
and even (about 85 ms across), always behind the highlight, which runs ahead and faster across
the button and fades at its right edge. White follows from the left as soon as the black has
gone (55 ms): a leap, then slowing down to the end. Both fronts are soft over 44 units (about a
character and a half). The highlight (`COMMON.BUTTON.PRESSEDHIGHLIGHT`, 443x56 units, its head
at 330) stays inside the button: its host is clipped to the plate (inset 2 units, radius 5).
The afterglow (327x63, centred) is faint: 0.55 at 150 ms, gone by 750. While the press runs the
item does not draw its type, two layers do: black and #F2F2F2 under complementary four-stop
masks (white, ramp, black, ramp, white) whose stops follow the measured paths keyframe by
keyframe. Shuffle and repeat take their new state before the press is drawn, so repeat's three
labels (off, all, one) and the ink always agree.

The menu's shade (`NOWPLAYING.MUSIC.LEFTMENU.GRADIENT`) is still 30 - 40 % opaque along its top
and bottom rows, so stretched into a box it ended in straight edges; on the recordings it is a
soft pool over the left of the screen. It is drawn taller (190 units beyond the menu) under an
elliptical falloff from the middle of the left edge: whole to half its radius, 55 % at three
quarters, nothing at the edge.

The sung line is always the last of the song's lines and in their style, without a shadow: a
fourth line centred under the cover at the album line's size (mini), a third beside the cover
(mini strip), the last beside the cover (full screen). Word-timed lines fill word by word with a
soft 3 DIP front, redrawn every 16 ms while they fill; no room is kept without synced lyrics.
A new line rolls in: the old one moves up a line's height and fades (360 ms) while the new one
comes up from below (520 ms), both on cubic-bezier(0.3, 0, 0.1, 1), inside a clip of the line.
When the pane opens or closes the line does not travel with the cover (owner, 2026-11-12): it
goes at once and comes back in its new place, fading in with the song's other lines.
Right-clicking the line or the song's lines opens the library window's lyrics menu (search,
sources, a file, automatic lyrics, and Hide / Show lyric line). That switch
(`settings::LyricLineShown`) holds for the library window, the mini window and the full screen
alike; without the line, or without synced lyrics, the library window's band shows the title
with its star on the first row and the artist on the second.

## Not done yet

- WMC's idle layout and its left menu; browsing the library in full screen.
- Mini window position and form are remembered only for the session.
