# Status

Updated: 2026-11-08

## Where things stand

- M0 (glass window, Win7 frame/shadow/caption buttons, WMP 12 layout, transport) is done.
- **Functional pass (M1 core) is done and tested in the dev instance** with a real library
  (E:\music\CDs added as a music folder in the dev instance only):
  - Navigation pane: foobar2000 playlists; Music -> All music / Artist / Album / Genre.
  - Virtualized list: hover/selection (Ctrl/Shift), keyboard (arrows, PgUp/PgDn, Home/End,
    Enter, Delete, Ctrl+A), double-click to play, foobar2000 context menu on right-click,
    scrollbar drag/page.
  - Library views play like WMP: activating a track replaces the "Now Playing" playlist with
    the current (filtered) view and starts at that track. Groups drill down.
  - Back/forward history (buttons, Alt+Left/Right, mouse X buttons, Backspace) and breadcrumb.
  - Search box (Ctrl+E/F3): filters the current view, IME positioned, paste, Esc clears.
  - Command bar: Organize (manage library folders, rescan, add files/folder/location,
    preferences, full foobar2000 menu), Add files, Add folder, Create playlist.
  - Transport band: seek bar (click/drag), now-playing art + title + time, shuffle/repeat
    mapped to playback orders, volume slider/mute, wheel = volume.
  - Drag and drop files from Explorer into the shown (or active) playlist.
  - Library model: de-duplicates image+cue rips exposed twice, keeps albums with partial
    ALBUM ARTIST tags together, groups untagged compilations by folder ("Various Artists").
- Visual fine-tuning is **paused** until the owner provides Win7 reference captures.
- 32-bit builds were dropped (owner decision).

- Library de-duplication now works on the *audio source*: entries are merged only when they
  play the same file and track (embedded cue sheet vs external .cue, duplicate registrations);
  an image split by a cue is hidden as a whole-file entry; BOM-less cue sheets get their
  encoding detected (ICU + per-line validity + script heuristics, see media/cue_encoding.h)
  and mis-decoded text is repaired for display in library, playlist and now-playing views.
  Checked offline against every cue in the owner's E:\music\CDs; UI test by the owner pending.
- Owner supplied Win7 wmploc.dll / ehres.dll / aero.msstyles; ADR 0007 adopts original artwork
  loaded at runtime. Inventory started in docs/design/win7-assets.md.
- Original artwork in use (2026-11-07): the whole WMP 12 transport (capsule, cluster, orb with
  its glow ring fading in, glyphs, shuffle / repeat / speaker icons, volume slider) and the
  back / forward orbs come from wmploc.dll, placed by pixel matching against a native capture.
  Procedural painting remains the fallback when the files are absent.
- Library song views are grouped by album like WMP 12 (header line, album column with the
  cover and album / artist / genre / year); album, artist and genre views are cover tiles.
  Covers load on a background thread (ui/art_cache.*); WMP's generic cover and drop shadow
  are used when available.
- Glass reflection anchor is applied in WM_WINDOWPOSCHANGING (before the move) to stop the
  streaks shaking while the window is dragged. Owner to confirm.
- Owner's Win7 VM runs at 150 %; WMP 12 draws its bitmaps 1:1 there. The dev machine is 4K
  at 200 %.

- Caption buttons (with hover glow), the top-left glare and the reflection streaks now come
  from the Win7 DWM atlas in aero.msstyles (STREAM 971); uDWM.dll turned out to hold no
  images. Caption glyphs use the 192-DPI set at 200 %.
- Library: album, artist and genre lines in the album column are links; a view options button
  (Icon / Expanded tile / Details) next to the search box, remembered per view; list columns
  can be resized and reordered by dragging the header, remembered per column set.
- Artwork can be overridden by locally made 2x PNGs in `<profile>\glasswave\hd\<module>\`
  (`<id>@2x.png`). `scripts/upscale-art.ps1` makes them for the WMP bitmaps with
  waifu2x-ncnn-vulkan (cunet, no denoise, TTA; colour and alpha upscaled separately); the
  dev profile has them (2026-11-08).
- 2026-11-08: named wmploc resources are `NAME.PNG` (dot), not `NAME_PNG`; the WMP list
  header (normal / hot / pressed cells), the generic cover and its drop shadow now actually
  load. Header is 23 DIP like the art; a translucent copy of a dragged header follows the
  cursor with an insertion line. Header clicks cycle ascending / descending / unsorted.
  DWM side highlights (atlas part 43) on the active frame. The view options button no longer
  shows at the window origin on views without view styles.
- Frame and client edge lines are one device pixel at any DPI, as DWM draws them (they were
  floor(scale) px, i.e. 2 px thick at 200 %). The corner radius stays 6 DIP.
- Tried and removed (2026-11-08): DWM legacy blur-behind accent with a fitted overlay as a
  lighter glass. The owner saw no difference from the host backdrop, and the blur filled the
  window corners square despite a window region.

- 2026-11-08 (owner's large request): asset index docs/design/win7-asset-map.md (every image
  in wmploc / aero.msstyles / ehres with IDs and uses; scripts/catalog-art.ps1 exports them);
  disabled widgets without hover; WMP seek bar from measurements; column lines; Expanded tile
  gutter; shuffle / repeat (off / all / one) independent; volume tooltip; free column widths,
  Shift+wheel sideways scroll, rubber band, hit areas ending at the last column; breadcrumb
  levels with child menus; split buttons (Create playlist / auto playlist, view options);
  original art for nav orbs + groove, command bar, breadcrumb, view icons, speaker plate,
  tooltip, library background, nav pane icons, WMP app icon; now-playing links, stream
  format, output device menu, synced lyrics; star ratings (Playback Statistics) and a
  Favorites playlist; taskbar thumbnail buttons and jump list tasks; Win7 popup menus.
  waifu2x needs an explicit tile size (auto garbled wide bitmaps).
- 2026-11-08 (later): Music categories are configurable like WMP's "Customize navigation
  pane" (right-click Music): Artist, Album, Genre, Year, Rating, Contributing artist,
  Composer, Folder; saved in cfg, default Artist/Album/Genre. Smart playlists header opens
  its own page (Favorites + auto playlists); the Playlists page lists plain playlists only.
  foobar2000 restores auto playlists after the UI is created, so the nav pane re-sorts on
  `on_playlist_locked` (each auto playlist arrives as a lock). wmploc has no 192 px auto
  playlist icon (29572 stops at 32 px; 29772 does not exist), smart lists use 29722.
  translate-deepseek.ps1 also scans `k...[] = {L"..."}` label tables and ternaries.
- Playlists offer Expanded tile (album column over runs of consecutive songs from one album;
  the playlist order is kept) besides Details; the choice is saved. New "Album art" column
  (Apple Music style cover per row, row height stays WMP's 22 DIP): on by default in
  playlist Details, hidden in library Details (album headers already show the cover).

## Next

1. Owner tests everything from 2026-11-08 (list in the session reply); fix what they report.
2. Glass blur stays the host backdrop + color matrix (legacy accent tried, no gain).
3. Remaining tuning from the Win7 captures.
4. Stacked covers for artists.
5. Playlist editing: drag to reorder, drag from library to playlists, rename inline.

## Known issues

- Library rows are formatted once per reload; very large libraries (100k+) not yet profiled.
- No device-lost handling yet; hover/pressed colors still extrapolated.
- DWM host backdrop blur is stronger than Win7's blur (no API to tune it).
- Side highlight length (H / 4 + 20 DIP) is fitted to two captures, not taken from uDWM.
