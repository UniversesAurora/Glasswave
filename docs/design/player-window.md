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
transport below: stop, previous, play, next from the wmploc cluster, the speaker with an arrow
that opens the volume slider, no shuffle or repeat. Its height decides the layout: a strip
(cover left, two lines), a cover with the lines centred under it, or the pane above the
transport (the window grows to 720 DIP for it and returns to 540).

**Full screen** is Media Center's now-playing screen with its windowed-mode chrome: the back
orb top left, minimize / restore / exit top right, the bottom bar with the controls at the
right in WMC's order (lyrics, queue | stop, previous, play, next | mute, volume down, up) and
the seek bar in the space left of them. Chrome and pointer leave after 3 s without movement.
Without the pane the cover and the song's lines stand side by side as in WMC; with it the
cover moves left and the pane takes the right half.

## Artwork used (ehres.dll unless noted)

| Part | Resource |
| --- | --- |
| Full-screen transport, caption buttons, back orb | `TWOFOOT.CONTROL.<NAME>.<NOFOCUS/FOCUS/PRESSED/DISABLED>` (38x31, play 42x43, caption 29x24, back 42x42) |
| Lyrics / queue buttons | `TWOFOOT.CONTROL.TELETEXT` (a page of text; Windows 7 has no lyrics glyph), `TWOFOOT.CONTROL.GUIDE` |
| Volume arrow (mini) | `TWOFOOT.CONTROL.DOWN` 16x16 |
| Bars | `TWOFOOT.FULLSCREEN.BOTTOMBAR.LEFT/RIGHT` 531x53, `TOPBAR.LEFT` 129x55, `TOPBAR.RIGHT` 210x64, `TWOFOOT.CONTROL.DIVIDER` |
| Seek bar | `SEEKBAR.PROGRESS.BACKGROUND` 27x24, `SEEKBAR.PAST` / `FUTURE` 14x24, `SEEKBAR.POSITION.DORMANT` 26x26 / `ACTIVE` 28x28 |
| Volume slider / overlay | `OVERLAYS.VOLUME.BAR.OFF/ON` 150x10 on `CONTEXTMENU.BACKGROUND` (nine-grid, 12) |
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
- Seek bar pieces, measured in the bitmaps: the groove is solid in columns 9 - 17 of 27; the
  past is a 9-column glow tail followed by solid white (columns 9 - 13 of 14), the future its
  mirror image at alpha 102. Only the solid columns stretch (nine-grid margins 11 / 1).
- The lent pane is embedded (`NowPlayingPane::SetEmbedded`): no tint rectangle and no switch
  of its own. Full screen, the wall behind it sinks into blue with no edge: a horizontal
  gradient that starts left of the pane and reaches 86 % at its text.

## The wall (measured from the owner's WMC recordings, 3322x1832 at 30 fps)

| Quantity | WMC | Here |
| --- | --- | --- |
| Tile side | 297 px = 0.162 of the screen height | the same; at least 84 DIP |
| Drift | upwards, about 4.4 px/s = one tile in 68 s | the same, one linear animation per tile height, then the rows rotate |
| Tile look | pillowed: light top and left edge, soft shadow bottom and right | light 7 % of the side (alpha 0.22 / 0.16), shadow 11 % (0.42 / 0.36), a sheen across the face, hairline seams |
| Entrance | tiles fade in in random order over about 2.5 s while the wall slides up | a ring from the middle (85 ms per tile of distance + up to 160 ms), each tile from scale 0.58 with an overshoot (bezier 0.22, 1.28, 0.36, 1; 760 ms); the wall settles upwards by 0.9 tile in 2.6 s |
| Dimming behind text | blue over the tiles, soft edges | WMC's own gradient bitmap, stretched; navy 0x0B2C6B at 22 % over the whole wall |
| Track change | cover cross-fades in about 0.3 s | cross-fade 380 ms, the new cover grows from 95.5 % |
| Idle layout | after about 22 s the menu leaves, the cover grows and moves right | not taken over |

Additions: a swell crosses the wall every 11 s (each tile sinks to 92.5 % and 80 % opacity
for about 2 s, later the further right and down it is), which lets the light behind show
through the seams. Wall drift and swell run only while music plays.

## The light

`AeroLight` rebuilds Media Center's moving background from its own layers: the ground bitmap,
two corner gradients, six sheets of blue light and eight shards. Each sways sideways (5 - 14 %
of the width, 31 - 59 s) and breathes in opacity (11 - 29 s) on its own period, eased at both
ends, so the pattern does not repeat for minutes. It lies under the wall (seen before the tiles
arrive and through the seams) and once more, faint, over the covers (beams at up to 0.62,
sheets at 22 %). The owner asked for this light to move all the time: it runs while the window
is visible, as compositor animations only, and pauses when the window is minimized or hidden.

## Not done yet

- WMC's idle layout and its left menu; browsing the library in full screen.
- Mini window position and form are remembered only for the session.
