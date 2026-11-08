# Status

Updated: 2026-11-08

## Where things stand

- M0 (glass window, Win7 frame/shadow/caption buttons, WMP 12 layout, transport) is done.
- **Functional pass (M1 core) is done and tested in the dev instance** with a real library
  (E:\music\CDs added as a music folder in the dev instance only):
  - Navigation pane: Now Playing; All Music with Favorites, the cross-source Recently
    Played / Most Played lists and the playlists; Local Music with the Smart playlists
    section and its categories; NetEase Cloud Music.
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

- 2026-11-08 (resume / rows / translations):
  - The custom playback resume was removed. foobar2000's own Playback > Other option
    ("Save playback state when closing foobar2000 and resume on next startup") was
    confirmed by the owner to restore the track, position and play/pause state; keeping
    our cfg_resume* state, UI timer and shutdown hook duplicated it and could race with
    the core. Resume is now core-owned.
  - The Album art column makes Details rows 40 DIP tall (42 DIP wide), including grouped
    Details; Expanded tile keeps WMP's 22 DIP because its album column already shows the
    cover.
  - Chinese UI: the search placeholder, view menu (Icon / Expanded tile / Details), column
    menu (Choose columns / Reset columns) and every empty-list message are translated.
  - Auto playlists restored after the UI could stay under Playlists when their lock event
    arrived before `autoplaylist_manager` reported the client ("已分级 5 星"). The nav pane
    now classifies an auto playlist by its lock: the autoplaylist lock blocks item edits
    (add / remove / reorder / replace), while our queue / Favorites lock does not. The
    rebuild that `on_playlist_locked` triggers is therefore already correct; no retry
    timers or paint-time rebuilding.
  - Album art in list rows is cached per album (album name + folder), not per track: a CUE
    album's dozens of subsongs share one fetch/decode, and all rows draw the same thumbnail.
    Library rows key on the library album group. Three worker threads decode the visible
    covers in parallel, and ListView keeps up to 128 decoded D2D bitmaps across paints so a
    newly arrived cover does not recreate all the others. Cover decoding uses an image's
    embedded thumbnail when it is at least the requested size, skipping the full-size scan.
  - Win7 menus draw the submenu arrow and the radio bullet as GDI+ vectors: the 9x9 / 16x16
    theme bitmaps were soft at 200 %. The screenshots' blue ball keeps its gradient and
    highlight; the bitmap remains the fallback if GDI+ is unavailable.
  - The volume bubble works again: it was removed and reinserted into the composition tree
    on every update, which kept it from rendering. It is now inserted once on the unclipped
    window root, its text is centred, dragging shows it immediately and hovering the slider
    shows it after an 800 ms delay. The wheel changes volume only inside the slider, not
    across the whole transport band.  - Details song lists now regroup by the clicked sort column as WMP does: rating groups
    ("5 Stars", ...), length in whole minutes, year, genre, artist, title initial, etc.
    Non-album sorts are global (not just within albums); the third header click resets to
    the album grouping. Expanded tile keeps album grouping for its album column.
  - Group header bars (album headers and sort groups) are clickable like WMP: clicking one
    selects every row under it without highlighting the bar itself; hovering draws only a
    one-pixel outline (no fill); Ctrl-click keeps the existing selection, and right-click
    selects the group before opening its menu.
  - Details lists show sections only while sorted; a header context-menu item (Show groups)
    turns them off. Section rows keep the plain row height when the cover column makes the
    track rows taller.

- 2026-11-08 (NetEase Cloud Music; see docs/adr/0008-netease-provider.md):
  - NeteaseCloudMusicApi Enhanced runs as a loopback helper managed by the component; QR
    login, DPAPI session, likes, created/collected/recommended playlists, playlist pages and
    lossless playback through an `ncm://song/<id>` input all work.
  - Library tree: All Music / Local Music / NetEase Cloud Music; All Music shows Favorites,
    Recently Played, Most Played and the regular playlists, Local Music keeps a Smart
    playlists section and lists its categories directly, and NetEase shows Favorites,
    Created playlists, Collected playlists and Recommended. All Music > Favorites combines
    the local Favorites playlist with the NetEase liked list; the Details list is flat until
    sorted and gains a sortable Source column.
  - NetEase song lists and the combined favorites share SongTableSource (columns, sorting,
    filtering, selection, expanded tiles, context menus); both offer Details and expanded
    tiles, with a bounded LFU cover cache.
  - Artist pages: right-clicking a NetEase song offers Go to artist (submenu for several
    artists) and a local song opens the existing artist group view; the artist line in a
    NetEase expanded tile links to the page. The page has a fixed 104 DIP header (80 DIP
    avatar, name, album/song counts, brief description, Follow/Following) above a tile grid
    whose first tile opens the hot songs and the rest open albums. These header values are
    new UI, not Win7 measurements. The description wraps and grows the header instead of
    being ellipsized.
  - Navigation uses one breadcrumb model for every library: the root lists all libraries,
    each library owns its section menu, and each section lists its own content. NetEase
    gained Artists (followed artists) and Albums (collected albums) sections; playlist
    pages keep their Created/Collected/Recommended parent, artist pages sit under Artists,
    and an album opened from an artist keeps the artist in the path. Every level is
    separated by an arrow; only levels with children open a menu. The local library is
    labelled Local Music, and the Chinese terminology follows WMP (艺术家 / 唱片集). The
    account refresh prefetches followed artists, collected albums and recommended
    playlists so the menus are populated.
  - The collected albums page offers Icon and Expanded tile. Expanded loads the saved
    albums' tracks in pages of 12 as the list nears its end (six workers) and lists them
    grouped by album, like a playlist's expanded tile; the choice is remembered per view.
    Icon views no longer draw a column header (it did not line up with the tiles).
  - Hot songs are regrouped by album in Expanded tile, ordered by each album's hottest
    song: the album containing the #1 song comes first, and extra songs in an album never
    move it, so the order stays stable while /artist/songs?order=hot pages in 100 songs at
    a time. Details keeps the raw popularity order. Album names in NetEase song tiles link
    to the album page.
  - The artist page offers Icon and Expanded tile. Album metadata is fetched 24 albums at
    a time; the Icon grid appends the next page as the list nears its end. Expanded tile
    loads 12 albums' tracks per page (six workers per track page) and appends them without
    moving existing rows or clearing the selection; the header still shows the official
    album/song counts from the artist profile, so a 5000-album artist shows the first
    albums quickly instead of waiting for the whole catalogue. Loaded pages are cached
    per artist. Browse pages offer only their real layouts in the view menu, with no
    grayed entries. In expanded album views, sorting applies inside each album only and
    never reorders the album groups. Icon views no longer draw a column header. The
    maximum scroll position aligns the last line with the bottom of the list instead of
    letting the transport band cut it.
  - The detail band is shared by artist, album and playlist pages: cover or avatar, name,
    counts/creator, a three-line description with Show more / Show less, and the page's
    action button (Follow, or Collect/Collected). Album pages now carry the album
    introduction and collect state; playlist pages carry the creator, description and
    subscribe state. The actions are also on the header's and the empty list's context
    menus. Playlist tiles and navigation rows can be collected or uncollected, and the
    Created playlists node's menu creates a NetEase playlist through a small name prompt.
  - Collected albums: the Icon grid reads `/album/sublist` 100 albums per page and appends
    the next page near the end of the list; Expanded tile reads the same metadata pages and
    loads their tracks 12 albums at a time. Entering the Artists, Albums, Created/Collected
    playlists sections re-reads the first page, so items added in the official client show
    up without a restart. The command bar has a Refresh button (F5) that drops the decoded
    NetEase covers, refreshes the account lists and re-reads the current page.
  - Playlist covers follow song edits: after adding or removing songs the provider fetches
    one lightweight playlist summary (cache-busted), updates the catalog and the cached
    lists, and the window drops the playlist thumbnails so the regenerated cover loads.
  - Expanded tile collects an album's songs into one column on every song table, ordered by
    the album's first appearance; this also applies to local playlists (only the displayed
    order changes). Details keeps the view's own order.
  - Batch actions use every selected NetEase song. Right-clicking an album column or group
    header whose rows are already fully selected keeps the rest of the selection instead of
    replacing it. Removing songs updates the playlist source in place and keeps the same
    top song, and collecting an album/playlist or following an artist no longer reloads the
    song list. Jump-to-playing now decides visibility from the track's row, not its group
    header. Playlist tiles drop both decoded art copies when a cover changes.
  - The Refresh command appears only on NetEase pages and is hidden on the recommended
    section (a daily set), where re-reading changes nothing; F5 follows the same rule. Its
    label is centred and the button is sized to the text. Empty-list and detail-header
    context menus also offer Refresh. Created playlists can be renamed and deleted from the
    playlist page, its tiles and the navigation. New collected playlists and albums are
    inserted at the top of the cached lists, matching the service order. After a removal a
    single delayed summary read catches the server's regenerated cover and count without
    reloading the page.
  - Refreshing no longer blanks covers: invalidating an entry keeps its previous bitmap
    visible while the replacement loads, and a version counter lets the list swap in the
    new bitmap only once it has been decoded. Images that did not change therefore stay
    visually identical instead of flashing.
  - Expanded descriptions are capped at 260 DIP and get their own scroll area (wheel,
    arrows, thumb drag and track paging). Navigation rows can be dragged within their
    sibling group: root libraries and their subtrees, the All/Local/NetEase subsections,
    the local categories and smart playlists, and NetEase created/collected playlists
    (through /playlist/order/update). The hierarchy never changes; the static section order
    is persisted and the service order is reloaded after a playlist drag. The drag preview
    is a translucent copy of the whole row under the cursor plus an insertion line.
  - The list and description scrollbars use the original Aero parts from aero.msstyles:
    track 728, arrow plates 717, glyphs 718/719/720 by DPI, thumb 733 and gripper
    724/725/726. The track, plate and thumb only exist at 96 dpi; they are nine-gridded
    with the theme's fixed margins (and the glyph/gripper use their 120/144 dpi variants)
    so borders stay sharp when the window scale is not 100 %. Optional locally generated
    2x replacements for the arrow glyphs and/or the thumb gripper can be made with
    scripts/upscale-scrollbar.ps1 (waifu2x; never committed); the component loads
    hd\aero\<id>@2x.png automatically. The thumb background, plate and track stay the
    original theme art. Scrollbar hot/pressed states cross-fade over about 130 ms on a
    16 ms timer that stops once settled; holding an arrow repeats line steps and holding
    the track keeps paging toward the pointer until the thumb reaches it (350 ms delay,
    then 50 ms), in both the list and the expanded description bar. The locally generated
    arrow-glyph upscales are installed in the dev profile; the thumb-gripper upscales were
    rejected by the owner and are not installed. The upscaler stays optional.
  - Right-clicking a NetEase playlist or album (tile, page header or empty list) offers
    Play, Play next, Add to queue, Add to playlist (the local playlists) and Add to NetEase
    playlist for the whole contents; local playlist tiles get the same queue and copy
    actions. NetEase contents are fetched on demand and become ncm:// items.
  - Playlists and smart playlists separated: a foobar2000 playlist holds local and ncm://
    tracks alike, so regular playlists live under All Music; smart playlists are auto
    playlists over the Media Library (which never contains NetEase tracks), so they live
    in the Smart playlists section under Local Music. Local Music lists its categories
    directly, without the old Music level; breadcrumbs, section menus and drag ordering
    follow the tree, and a smart playlist's crumb starts at Local Music. The Smart
    playlists section is itself draggable among the categories; its slot is saved in
    cfg_navSmartAt and restored on the next run.
  - All Music gained cross-source Recently Played and Most Played, built on our own store
    because foobar2000 auto playlists cannot see NetEase tracks: each track that starts
    playing is recorded with its display metadata, source and NetEase ids in
    `<profile>\glasswave\play-stats.txt` (newest 2000 kept, written atomically), and the
    two pages mix local and NetEase rows with a Source column. The lists refresh when the
    page is opened, not live while it is on screen.
  - Every song list — the shared song tables, the local library and playlists — offers a
    Count column fed by our cross-source play history: hidden by default under Choose
    columns, visible by default on Most Played. The local sources read the store
    while painting instead of foobar2000's %play_count%, so the column works without
    Playback Statistics and also counts NetEase rows inside playlists. A track that
    starts while a list is on screen updates its count immediately; the row order changes
    only when the page is rebuilt.
  - Clicking the NetEase Cloud Music library row (and its breadcrumb) opens the liked
    Favorites page instead of the Created playlists section.
  - The Now Playing, Recently Played and Most Played pages add removal actions to the
    song context menu. Removing from Now Playing deletes the rows; when the playing song
    goes, playback continues with the song that moved into its place, or stops if the
    queue is empty. Removing from Recently Played clears the last-played time (the song
    stays in Most Played), and removing from Most Played resets the count (the song stays
    in Recently Played); both rebuild the page and keep the scroll anchor.
  - Right-clicking the empty area of any list page offers Select all. Now Playing also
    offers Clear Now Playing, and the two play-history pages offer Clear Recently Played /
    Clear Most Played; clearing the queue stops playback when the queue supplied the
    playing song, and the list clears reset the same fields as the per-song removals.
    Regular playlists offer no clear action.
  - Menu check marks are now drawn as vectors (shape and colours sampled from Aero IMAGE
    976) instead of scaling the 16 px theme bitmap, so column and sort checks stay crisp
    at 200 % like the submenu arrow and radio bullet.
  - Not implemented yet: NetEase full-catalog search UI and a combined All Music browse
    beyond Favorites.

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
