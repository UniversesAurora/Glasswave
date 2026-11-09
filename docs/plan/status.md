# Status

Updated: 2026-11-09

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
    sorted and gains a sortable Source column. The NetEase liked page re-reads /likelist on
    every entry (the authoritative like state) and uses the liked playlist detail only for
    its order and metadata, so likes and unlikes made in the official client appear without
    a restart; likes the detail has not picked up yet are appended. Liking or unliking in
    the app no longer rebuilds the open page: the two Favorites views drop the unliked rows
    in place (keeping the top song), and other song tables only refresh their hidden
    favorite flag.
  - Navigating away and back restores the page's previous top row: the window remembers
    the top row per view identity and replays it when the source is set, including after an
    asynchronous page replaces its loading placeholder.
  - The NetEase liked, playlist and album pages keep their last song list. Returning to
    them shows those rows immediately while the fresh fetch runs; when it arrives the rows
    are swapped in without losing the selection or the top song. The loading text appears
    only for a page that has never been loaded.
  - Song context menus keep View artist and View album together at the top; the extra
    divider between the artist group and the actions is gone, and Add to Favorites now sits
    with Add to playlist / Add to NetEase playlist. Page-specific removals moved below the
    add actions and a divider separates our commands from foobar2000's. Add to NetEase
    omits the playlist being viewed, and View album is hidden on that album's own page.
  - Song list pages also offer Play / Play next / Add to queue / Add to playlist for their
    whole (filtered) contents from the empty-area menu, alongside Select all; the queue
    keeps Clear Now Playing and the history pages keep their clears. Album tiles offer
    Collect/Uncollect and Go to artist. Navigation rows without their own actions no
    longer open the unrelated playlist management menu.
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
  - Single-song context menus in every list (local, NetEase, the queue and the play
    history) also offer Add to playlist, a submenu of the regular foobar2000 playlists;
    those accept local and ncm:// tracks alike. NetEase rows keep their separate Add to
    NetEase playlist submenu, and the playlist being viewed is omitted from its own copy
    target. The built-in Favorites playlist is not a copy target: songs use the top-level
    Add to Favorites action, which routes to the local playlist or the NetEase liked list,
    and whole playlists or albums are never added to Favorites.
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
    page is opened, not live while it is on screen. Their navigation icons are wmploc's
    clock-with-note (575) and pie chart (550); the Smart playlists header keeps the
    auto-playlist icon.
  - Every song list — the shared song tables, the local library and playlists — offers a
    Count column fed by our cross-source play history: hidden by default under Choose
    columns, visible by default on Most Played. The local sources read the store
    while painting instead of foobar2000's %play_count%, so the column works without
    Playback Statistics and also counts NetEase rows inside playlists. A track that
    starts while a list is on screen updates its count immediately; the row order changes
    only when the page is rebuilt.
  - Mixed-source lists show a Source column: All Music > Favorites and the two play-history
    pages already had one; the queue and every playlist now label each row Local Music or
    NetEase Cloud Music as well, and the column sorts by source.
  - The transport band's shuffle and repeat toggles load cfg at start-up after the buttons
    were first painted; they now repaint unconditionally, so the highlight matches the
    restored playback order instead of staying off.
  - Album, artist and playlist covers share one cache key everywhere: NetEase song rows,
    playlist/artist/album tiles, category grids and detail headers use
    `ncm:album:` / `ncm:artist:` / `ncm:playlist:` ids, local album tiles use the library
    album key, NetEase rows inside local playlists use the NetEase album key, and the local
    rows of Favorites / play history dedupe by album. ArtCache also reuses a finished larger
    decode when a smaller size is requested, so a cover already shown by a grid or header
    does not load again for a list column. If the larger entry is stale, its old bitmap is
    shown at once while the requested size reloads. Album and playlist detail covers only
    invalidate when the image URL changes (the `?param=` size suffix is ignored), so the
    grid tile's decode stays usable by the song rows.
  - The window remembers the page that was open when it closed, plus that page's top row,
    and restores both on the next start. A local playlist is matched by name first (so
    reordering playlists does not open the wrong one) with the saved index as fallback.
    Without a saved page it still starts on foobar2000's active playlist.
  - Covers live in two caches: the in-memory decoded-thumbnail cache (600 entries, LRU,
    gone on restart) and the NetEase disk cache (`<profile>\glasswave\netease\cache\covers`,
    default 256 MB, no time expiry; evicted only when over budget, by LFU with last use as
    the tie-breaker). Downloaded covers therefore survive restarts.
  - NetEase's personal radar (私人雷达) is marked specialType 100 in /user/playlist.
    It is now kept in the account lists, so collecting it from Recommended makes it appear
    under Collected playlists and survive account refreshes; the interested page still
    shows the service's daily title and cover. specialType 5 (the liked songs playlist) is
    still kept apart, and the other special types (yearly reviews, ...) stay excluded.
  - The personal radar (playlist 3136952023) carries a different cover in
    /recommend/resource (its recommended image) than in /user/playlist and
    /playlist/detail (the first song's cover). The recommended image is pinned in the
    catalog, so the navigation, recommended grid, collected grid and page header keep one
    cover instead of alternating with whichever list was fetched last. The pin is dropped
    on sign-out.
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

- 2026-11-08 (review of the NetEase work, owner request): reviewed with the Muxora review
  checklist and restructured (ADR 0009). Providers sit behind `stream::Provider`; NetEase is
  `netease::StreamProvider` over `netease::Session`; provider pages are `ui::Page` objects
  (`ui/remote_pages`), song tables and tile grids are provider-neutral; `MainWindow` went
  from 6300 to about 4600 lines. Fixed on the way: two pairs of config GUIDs used twice,
  Details header sort doing nothing in song tables, the description scroll resetting every
  second while playing, stale page results corrupting album paging, failed refreshes
  emptying lists, writes reloading the open page, a lock-order deadlock between sign-out and
  account refresh, shutdown waiting for network requests, the cover cache evicting what it
  had just written, truncated downloads being cached, plain-list covers not repainting.
  New Preferences > Display > Glasswave page: language, volume bubble delay, cover caches
  and threads, play history size and rows, and NetEase account, audio quality, cover disk
  cache size/clear and service address. The play history file and the saved last page use
  new formats (old ones are ignored once).
- 2026-11-08 (later): the provider's account row left the navigation pane (sign in/out on the
  preferences page; the provider root's menu offers Sign in while signed out). Playlist views
  and history follow playlists being created, deleted and reordered. Remove from Now Playing
  removes the selected rows only. NetEase stream URLs (signed for ~20 minutes) are renewed
  when they expire during a pause and the decoder seeks or reconnects.

- 2026-11-09: lyrics pane (ADR 0010, docs/design/lyrics-view.md). A button at the right of
  the transport band (and View > Lyrics) opens a pane on the glass to the right; a restored
  window grows by the pane. Apple Music's lyrics look and motion: springs with staggered
  starts, scaled and blurred inactive lines, word-by-word fill with float and emphasis,
  translations, interlude dots, hover plate, press-to-seek, drag / wheel browsing with
  inertia, header with cover. Frames follow the compositor clock (high refresh rates) and
  stop when idle. Lyrics come from the cache, tags or a .lrc next to the file, NetEase's
  own word-timed lyrics (YRC -> enhanced LRC with translation) for NetEase tracks, then
  registered searchers. Checked in the dev instance: pane, header, NetEase lyrics, blur and
  interlude dots while paused; animation during playback not yet seen by the owner.
  ESLyric-compatible script searchers are specified for the next implementer in
  docs/plan/lyrics-script-host.md.
- 2026-11-09 (owner feedback): the pane joins the client area (one edge, no glass gap); the
  focused line sits high (1.5 lines of sung lyrics above) and upcoming lines blur slowly;
  the transport band shows title and artist on one row and the current lyric below it,
  filling blue word by word, aligned with the format / device rows on the right; NetEase
  bitrates show in kbps.

- 2026-11-09 (later): files opened from outside go to Now Playing (the queue stays the active
  playlist). Lyric lines are compositor visuals (blur and fade on the GPU): about 3 % of a
  core while lyrics play instead of ~45 %. The pane gained Apple Music's queue side (bottom
  bar switch, History / song card / shuffle-repeat / Up Next, reorder by handle, rubber-band
  scrolling) and View > Up Next. Checked in the dev instance by capture; interaction not yet
  tested by the owner.

## Development handover (2026-11-09)

- Reviewed the requirements, architecture, ADRs 0001-0010, visual specifications, plans,
  recent commits and the main rendering, library, provider, lyrics and queue code paths.
- The README's M0 status and several roadmap checkboxes lag the implementation. Album
  grouping, cover grids, column editing and a preferences page already exist. The planned
  colourization controls on that page are still separate work. The pane's queue and history
  also exist and need interaction testing. The ESLyric-compatible script host remains
  unimplemented, with its extension point in `lyrics::Searcher` and its work order in
  `docs/plan/lyrics-script-host.md`.
- Removed duplicate source and header entries from `foo_ui_glasswave.vcxproj`. The repeated
  source entries produced four MSB8027 warnings and concurrent writes to `frame_clock.obj`
  that failed a full rebuild. All 63 component source files remain included exactly once.
- Verified a full Release x64 rebuild through `scripts/build.ps1 -Rebuild` with 0 errors.
  Two C4702 warnings remain in the fetched foobar2000 SDK's `input_impl.h` (lines 249 and
  418) during link-time code generation. MSBuild/PDB communication failed in restricted
  execution on this host; the completed rebuild ran in the normal host environment.
- Dev-instance interaction and visual comparisons were not repeated in this handover.
  Existing owner-test items below still apply.

## Lyrics anchor and queue follow-up (2026-11-09)

- The lyric anchor now uses a fixed one-line original plus one-line translation reference
  block. Wrapping and missing translations no longer change the focused line's height.
- The sidebar still reads Now Playing. It excludes the song card's current item, follows
  its current position and wraps the tail to the beginning only for repeat-all. Stopping
  leaves the last cursor in the open window. Queue items remain in the underlying playlist.
- After owner clarification, the implementation keeps foobar2000's native shuffle and
  the stored Now Playing order. No additional shuffle state is persisted.
- Rows move up or down after native shuffle jumps, and skipped rows keep their visuals
  until their exit fade finishes. Clicking a song springs back to the card without a
  rubber-band discontinuity when the list shrinks. Growing history translates existing
  springs together, and dragging across the wrapped section maps the actual playlist slots.
  Motion tuning follows the supplied AMLL cover and song-information transitions. Long
  jumps cross-fade within one row of travel, avoiding a large blank gap between lists.
- Queue notifications are coalesced onto the next main-thread dispatch. This reads the
  completed core transaction after replacement/add/remove/reorder and the settled playing
  index after a track callback. A first sidebar click now follows the new song immediately.
  A stopped cursor is reused only while its original item remains in that slot.
- `scripts/test-queue.ps1` passes 144927 assertions covering empty and singleton queues,
  changed queue sizes, exhausted tails, repeat wrapping, invalid reorder input, wrapped
  drag permutations, forward/backward jumps and spring translation/settling. The test uses
  MSVC with /W4 /WX. Source checks also cover empty/malformed/valid URLs, quality fallback
  precedence, provider identities, explicit retry and stale worker results. The Release
  x64 build passes with 0 warnings and 0 errors.
- Dev-instance screenshots and interaction checks confirmed the fixed lyric anchor,
  whole-list replacement, addition/removal, first-click follow, repeat wrapping and native
  shuffle motion.

## Streaming source availability (2026-11-09)

- Restored the dev instance's global playback error popups after an incorrect attempt to
  hide the missing-source error through Preferences. No core error setting is changed by
  the component.
- `stream/playback_source` supplies provider-neutral URL checks, availability results and
  a per-playback unavailable-source set. Empty addresses never reach HTTP or an audio
  decoder. An unavailable input ends without audio, and the core chooses its next track.
  The core's 2.26 `skipTrack` predicate excludes confirmed unavailable items on later
  selections, including native shuffle and repeat. Queue items are retained.
- NetEase distinguishes a completed empty-URL response from transport, authentication,
  malformed-response and server failures. Quality fallbacks can recover to a playable
  source, while a failed request retains normal error reporting. Conversion and integrity
  testing also keep their error reporting. Skipped inputs do not count as played songs.
- Availability is temporary. Playback commands, stop, account/session changes and quality
  changes clear it. An epoch prevents an old worker from poisoning a newer attempt.
  Another provider can use the same checks and skip handling from its input adapter.
- The reported item was Jolin Tsai's "不一样又怎样" (`ncm://song/29732695`). The current
  session returned item code 404 and an empty URL. The owner can play it elsewhere and
  requested treating this special source state as unplayable, without diagnosing rights.
- With global error popups enabled, dev-instance validation skipped this item to a local
  track without a popup. A list containing only this item stopped normally with shuffle
  and repeat-all enabled, without repeatedly requesting the missing source.

## Queue capsule icon weight (2026-11-09)

- Queue capsule shuffle, repeat-all and repeat-one icons now request bold weight 700
  at 16 DIP, following the owner's request for heavier strokes (2026-11-09).
- Release x64 builds with 0 warnings and 0 errors. Dev-instance screenshots verified
  the heavier glyphs in enabled, disabled and repeat-one states.

## Continuous capsules and restored pane glass (2026-11-09)

- Rebuilt queue capsules and both parts of the bottom segmented control from the owner's
  iOS photo contours. A shared path eases from a straight edge with zero curvature into
  a circular side with matching tangent and curvature. All corners are mirrored. The
  thumb remains a compositor visual, repainted only on layout changes.
- Traced the missing glass to commit `326ee57`: joining the client edge around library
  and pane also stretched the edge painter's opaque content fill across the pane.
  Separated the library base from the hollow edge. Lyrics and queue now reveal the
  existing Aero material and screen-anchored reflection streaks through their dark tint.
- Release x64 builds with 0 warnings and 0 errors. Existing playback checks pass 144927
  assertions. The curve's diagonal join was checked analytically at curvature 1/R.
- Dev-instance screenshots confirmed the restored glass and streaks on both pane modes,
  the capsule/track/thumb contours, thumb dragging and maximized/restored layouts.
- Capsule geometry is a fit to the owner's photographs. Apple's exact construction and
  pixel-for-pixel equivalence have not been established.

## Lyrics sources and manual selection (2026-11-09)

- Added pinned QuickJS-ng 0.18.0 and miniz 3.1.2 amalgamations, verified by bootstrap.
  The host implements ES modules, synchronous request callbacks, persistent per-source
  strings, Mini-XML traversal, base64, UTF-8 buffers, decompression, parser dispatch and
  promise jobs. Each search has a 64 MB runtime and 20 s budget.
- Discovers profile ESLyric scripts and Glasswave overrides. Third-party scripts remain
  untracked. The original QRC parser delegates through a compatibility module to the user's
  unchanged community JavaScript decryptor. Added our MIT LRCLIB source.
- Priority follows the owner's clarification. Manual selections are remembered, followed
  by tags / adjacent LRC, the streaming provider's own endpoint, then fallback cache /
  sources in per-provider order. Sources run concurrently with deterministic selection.
  Automatic-cache keys include metadata and duration.
- Both lyric regions expose search, specific-source search, file loading, re-search and
  automatic reset. Search supports edited title / artist, source / timing columns and
  preview. Track changes invalidate old results. Automatic-cache clearing preserves manual
  selections. Workers and HTTP requests cancel and join before component unload.
- Release x64 builds warning-free. Offline host checks cover modules, XML, DTD rejection,
  buffers, base64, zlib, HTTP ordering, parser dispatch, copy-on-add, promises and CPU-bound
  cancellation, plus 11 unmodified reference configurations. Existing queue / motion /
  source checks pass all 144927 assertions.
- The shared HTTP transport keeps its blocking public interface over asynchronous WinHTTP.
  Abort events interrupt waits, and the request context and buffers survive until the final
  handle-closing callback. Loopback-only program tests cover normal / 404 bodies, blocked
  header / body scenarios, repeated cancellation and timeout. After the owner's instruction,
  verification uses programs and code review exclusively, with no further UI automation.
- Live dev-instance search returned NetEase, Kugou word timing and LRCLIB results. Loading
  a Kugou candidate updated both lyric views. A copied local FLAC used its adjacent LRC,
  manual loading overrode it, restart retained the selection, and automatic reset restored
  the adjacent LRC. Musixmatch successful retrieval remains unverified. Apple Music and
  Spotify are deferred at the owner's request.

## Original source compatibility follow-up (2026-11-09)

- Verified the original ESLyric package's 49 script / dependency files byte-for-byte against
  the dev profile, including QQ, NetEase, Kugou and Musixmatch. Community QQ Ex / NetEase Ex
  and QRC / YRC / KRC parsers and libraries are unchanged too. `run-dev.ps1` now copies these
  local reference trees automatically without tracking or bundling third-party code.
- Fixed the host's Unicode Base64 contract, which blocked QQ Ex before its lyric request.
  Original sources pass Unicode metadata to `btoa`, while KRC uses `atob` on UTF-8 JSON.
  Added offline tests that execute the original QQ Ex script through the mocked search /
  lyric request and validate the binary QRC parser adapter.
- Program-only live probes of Jay Chou's Qing Tian returned 10 word-timed candidates from
  original `qqmusic.js` and 10 from unchanged `qqmusic_ex.js`. The native-module adapter
  delegates to the external community decryptor's byte API. No GPL algorithm was copied.
- The same original-source program probe returned 20 Kugou word-timed candidates and five
  NetEase candidates. Musixmatch's original source remains installed and enabled, but its
  live Hello / Adele request returned no lyric body. Apple Music and Spotify remain deferred.
- Commit preparation reused the completed program checks. Release x64 reports zero
  warnings and errors, and queue / motion / source checks pass all 144927 assertions.
  No further UI automation was performed.

## Next

1. Owner tests lyric source priorities and manual search. Complete the remaining endpoint
   and account acceptance in docs/plan/lyrics-script-host.md.
2. Owner tests the restructured NetEase pages and the preferences page (list in the session
   reply); fix what they report.
3. Open review items the owner chose to leave: the helper's loopback binding is not verified
   after start (ADR 0008 says it is); an external service address over plain http sends
   the session cookie in clear text.
4. Glass blur stays the host backdrop + color matrix (legacy accent tried, no gain).
5. Remaining tuning from the Win7 captures.
6. Stacked covers for artists.
7. Playlist editing: drag to reorder, drag from library to playlists, rename inline.
8. Split main_window.cpp further (detail band, breadcrumb, menus) and list_view.cpp
   (1700 lines) by responsibility.

## Known issues

- Library rows are formatted once per reload; very large libraries (100k+) not yet profiled.
- No device-lost handling yet; hover/pressed colors still extrapolated.
- DWM host backdrop blur is stronger than Win7's blur (no API to tune it).
- Side highlight length (H / 4 + 20 DIP) is fitted to two captures, not taken from uDWM.
