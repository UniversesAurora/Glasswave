# Status

Updated: 2026-11-11

## Current integration gate (2026-11-11)

- The R6 compositor-wake fix is owner-confirmed. The pane position-only geometry fix and
  gfx draw-clip fix are included in R7, but resize/residual-surface and auth-restoration
  integration remain manual and unverified. No further tests are planned in this phase.
- The pre-fix managed-runtime HTTP 503 for `/glasswave/account` was located as a cross-VFS
  reader issue. The obsolete reader was deleted and the native getter was promoted. No
  additional live online acceptance was run, so current live account/library integration
  remains unverified rather than accepted.
- Suggestions passes 4/4 with zero invalid/unexpected requests. Startup seed passes, but
  same-account restart times out and remains unverified. Revisit it only if the owner later
  requests that diagnostic phase.
- The owner requires only necessary, change-related validation. Accepted audio, list,
  scheduler, baseline and mutation runs were not repeated. Outstanding acceptance is listed
  below for handoff only; it does not authorize further tests in this phase.
- R7 combined Release x64 passes with zero warnings/errors in 22.25 s
  (`.local/tests/finalNov11-phase-handoff-build-r7.log`). Normal deployment and start
  succeeded (`.local/tests/finalNov11-phase-handoff-deploy-r7.log`).
- Native provider baseline 50, mutation reconciliation 62 and ListSession 30
  (73 primary / 20 enrichment requests) are previous accepted evidence and were not rerun.
- Standalone validation passes: list cache 99 checks over ten runs, current provider snapshots 32,
  lyric priority 19, Apple lyric protocol 14 and lyric request state 8. The script-host,
  eleven reference scripts and HTTP contracts also pass. Suggestion and startup seed cases
  pass; same-account restart remains timed out for later diagnosis.
- Strong AAC progressive acceptance passes with the reproducible default fixture:
  3,816,841 upstream bytes remain withheld when first PCM arrives in 26.9198 ms; all
  251.669333 seconds decode, near-EOF seek succeeds, and the same-version cache rerun
  passes six early seeks. Only one audio GET serves two sessions; both sessions are deleted.
  Evidence: `.local/tests/finalNov11-unified-audio-progressive-PASS-20261111T115934909Z.json`.
  The six-case native ALAC/AAC mapping and failure-cleanup matrix also passes:
  `.local/tests/finalNov11-unified-audio-mapping-PASS-20261111T120210Z.json`.
  Prior failures were inconsistent fixture edit-list timing, an unstable mock account and
  seek assertions applied to expected failures. Production duration/cache keys were not
  changed to hide them. No audio retest is needed for unrelated lyric/navigation changes.
- Public Apple reads verify all eight artist views, mixed best search and input suggestions.
  Some empty artist views return 404; an existing-artist check disambiguates this from a
  missing artist, authentication failures and failed continuations. Evidence:
  `.local/tests/apple-public-api-audit-20261111.json`.
- Account verification is separate from DRM/global library refresh, so verified readers
  do not wait for every album, artist and playlist page. Stable native DSID binds cached
  account state; country/display names are not identities. Go applied-tree tests and vet
  pass; the refreshed native Windows runtime is built and promoted. Evidence:
  `.local/tests/applemusic-stage-promotion-20261111T111401Z.md`.
- Favorites restores its provider-owned navigation target before global membership refresh.
  A generic bounded navigation-binding snapshot (sole current schema 3) and playlist-only
  readiness gate are implemented; targeted native acceptance is pending. Warm reads do not
  wait for unrelated album/artist collections or DRM; cold
  reads must distinguish pending discovery, confirmed absence and genuine request failure.
- The 20 affected old bootstrap entries were explicitly reset in the development cache.
  Audio, image and list cache entries, account scopes and owner configuration were outside
  the reset predicate. Evidence: `.local/tests/provider-bootstrap-schema3-reset-20261111.json`.
- Unified automatic lyric priority uses `source` for embedded/local-origin or current native
  provider lyrics and `sidecar` for adjacent same-name files. General Apple Music search is
  independent. Existing manual selections remain unconditional overrides, including when
  automatic caching is disabled or a refresh bypasses automatic cache.
- Account-bound automatic lyric delivery rechecks scope/revision on the main thread.
  The neutral/manual delivery helper owns its callback by value, avoiding a dangling
  reference after `Service::Start` returns. Both changes are compiled in R7.
- The latest owner-state capture is
  `.local/tests/playback-probe-state-20261111T131819599Z`. Its contents are not assumed here,
  no artifact consistency checks or hashes are performed, and no restoration is requested.
  Do not restore the historical 196-row, 35-row, `124857`, or `110846` states.
- Memory watcher/supervisor/GPU collector remain active. Current owner scope is continuous
  monitoring/protection; further allocator investigation waits for another captured fault.

## Earlier phase evidence (2026-11-11)

These entries retain earlier diagnostics. The integration gate above supersedes their
pending counts and owner-state backups.

- Startup cache restoration and uploaded-M4A streaming are in progress. This is a provisional
  acceptance snapshot; the remaining integrated checks and owner interaction checks are pending.
- Release x64 at 09:29:48 local has zero warnings/errors. Provider baseline passes 50/50,
  mutation reconciliation 61/61 and helper checks 29/29. The mutation scenario records
  exactly 20 empty-body 202/204 acknowledgments and two nonempty create-201 responses, with
  zero unexpected calls or contract errors. Both scenarios restore Japan 1 -> 0 -> 1 and
  restore the captured bridge bytes and ACL. This phase performed no real-account writes.
  Evidence: `.local/tests/finalNov11-provider-build.log` and
  `.local/tests/finalNov11-provider-summary.json`.
- The later 10:25 Release build has zero warnings/errors. Mutation reconciliation passes
  62 operations, including authoritative saved-state behavior. Baseline operations pass 50/50,
  but the runner rejects excess playlist page/hydration requests; performance acceptance is
  still pending. Evidence: `.local/tests/final-nov11-authority-*-evidence.json`.
- Colors/RAII native acceptance passes the ALAC mapping and four rejection guards (5/6).
  Uploaded AAC exposes a real tail-moov M4A streaming gap; Go container normalization and
  native first-PCM-before-final-byte verification are in progress. The generic ListSession
  rerun passes 29 cases / 69 primary / 20 enrichment requests. New restoration-gate cases
  have not yet run in the native instance.
- Scheduler native acceptance passes all 18 cases, including empty active/playing SDK
  state (`SIZE_MAX`), with installed/restored both true. Evidence:
  `.local/tests/playback-scheduler-result-nov11.json`.
- Current persistent formats are index v2 and list schema v3. Chunk tokens identify
  immutable publications; payload hashing and compatibility readers were removed.
- The shared audio-cache regression passes seven cases, including eight parallel readers
  over six passes / 97.8 MiB. The image-cache regression passes 11 cases.
- Generic no-cover placeholder painting and bounded negative caching are implemented. The
  owner reported gray right-side lyrics; the latest `pane_common` change reuses the existing
  `ALBUM_0` artwork and still needs the owner's manual check.
- The owner manually confirmed Account -> Japan changes KAF to 花譜. This validates that
  display-name example. Provider region/identity/reconciliation contracts now have the
  passing mock evidence above; live rapid-toggle responsiveness and appearance remain manual
  acceptance items. These settings select metadata presentation regions, not UI language.
- The watchdog captured a playback-chain memory fault at 18:02:59 local on 2026-11-10.
  Evidence is under `.local/tests/memory-watch-live` in `alert.json` and bounded minidumps.
  Stopping `engine.exe` and foobar2000 reduced global commit from about 85 GB to 48 GB, which
  is strong causal evidence for this Glasswave playback-chain fault. FOOBAR Shared reached
  39.646 GB. The exact allocation path and original seek trigger remain unconfirmed;
  the automatic-playback case is captured. See the investigation section for monitor status.
- Owner visual and interaction acceptance remains manual. No screenshots or UI automation
  are used unless the owner explicitly requests them.

## Startup caches and artist views (2026-11-11, in progress)

- Root cause: account/bootstrap observations rotated the persistent list-cache guard, and
  provider startup lacked a minimal navigation snapshot. The implementation separates
  in-memory publisher retirement from hard persistent invalidation and restores cached
  navigation before account verification. Cached visibility must not grant authentication.
- The standalone list-cache race suite passes ten consecutive runs of 89 checks. It covers
  preserved manifests, stale publishers, pending hard rotation, region round trips and account
  isolation. Provider bootstrap integration and native restart acceptance remain pending.
- Apple documents artist views for `top-songs`, `featured-albums`, `full-albums`, `singles`,
  `live-albums`, `appears-on-albums`, `compilation-albums` and `featured-playlists`.
  The current UI implements only a provider-labelled top-songs entry plus the ordinary
  mixed artist-albums relationship. The other dedicated sections are not implemented.
  Library-only uploaded artists use library tracks/albums rather than catalog rankings.
- The owner now requests all eight artist interfaces independently, retaining separate
  datasets/cursors/cache identities while deferring artist-page layout redesign.
- Catalog search currently implements songs only. The owner requests independent Best
  Results, Artists, Albums, Songs and Playlists interfaces, with no search UI work yet.
  Apple's catalog-search endpoint documents `with=topResults`; this is distinct from
  search suggestions. Mixed best results must preserve server ranking and resource type.
- Input suggestions are also requested: preserve `searchTerm` and `displayTerm` separately,
  plus typed top-result suggestions. UI scheduling and layout are deferred.
- Prefer documented Apple contracts when they provide the function. Use existing Adam/Go
  implementations for missing or unusable public functionality; the Go helper may still
  transport authenticated requests to documented Apple endpoints. Keep the existing sign-in.
- The owner explicitly accepts continuous memory monitoring/protection as the current scope.
  Further allocator investigation is deferred until another captured fault; it must not block
  the remaining cache, playback and catalog work.

## Owner reports and pending acceptance (since 21:40, 2026-11-10)

Current dispositions below include the accepted November 11 implementation/mock phase.

- **Source identity:** Colors Of Happiness (Yui Makino), previously catalog `1237064886`,
  now reports catalog `1865440142` / library `i.ZOMrromt4P51o6r`, ISRC `JPTE01701820`,
  ALAC 16/44.1. The authoritative library/reporting-ID mapping and RAII rejection guards
  pass native fixtures. Actual owner playback acceptance remains pending.
- **Navigation:** reported artist 400, song-row album 404 and romanized/unavailable
  multi-artist links led to authoritative identity/alias fixes. The provider/mock phase is
  accepted; native source acceptance and owner navigation checks remain separate.
- **Metadata regions:** the display-only revision preserves canonical raw metadata, cancels
  obsolete generations and coalesces visible-first work. Mock region contracts pass; the
  reported China/Japan lag and old names until Refresh still need owner interaction checks.
- **Now Playing:** physical projection is independent of playback roles and user Play Next
  is preserved after explicit selection. The first priority-clearing revision was rejected;
  the current revision passes all 18 native cases with SDK restoration verified.
  The development upgrade uses one current schema, without compatibility readers.
- **Favorite Artists:** keep this separate from all library artists. Official `inFavorites`
  reads are validated; missing values remain unknown. POST/DELETE and stale-read reconciliation
  pass the mock phase. Page/real-account acceptance remains pending; this phase made no real writes.
- **Library mutations:** label album actions Add to Library / Remove from Library. A remote
  Remove was acknowledged successfully, but a subsequent Refresh error rolled back the UI
  or left stale Saved state. The generic mutation epoch now separates acknowledged state
  from eventual reconciliation for albums, playlists and artist favorites in the accepted
  61-operation mock scenario. The original owner-facing failure still needs live acceptance.
- **Owner state:** the authoritative post-upgrade backup records 196 rows, current=1,
  priority=0 and Japan=1. Main will perform the final selective restore after all tests;
  an intermediate probe restoration is not the final owner-state acceptance.

## Graphics lifetime and idle policy (2026-11-11)

- Accepted implementation: `RetainedSurface` keeps Widget's six layers and MainWindow's two
  layers stable. Hidden/minimized paint coalescing and FrameClock suspend/resume add no timers;
  `WM_SHOWWINDOW` / `WM_WINDOWPOSCHANGED` handling follows default window processing.
- `test-composition-surface-policy.ps1` passes four logical policy cases. This does not
  measure actual GPU allocations or COM reference counts. The earlier FrameClock occluded-spin
  fix separately passes four cases, with no new FrameClock source change in this phase.
  Neither result establishes the allocation root of the captured 39.646 GB fault.
- The GPU idle preflight and 18-hook helper smoke pass. The instrumented real-song attempt
  times out with incomplete allocation coverage and is not an acceptance pass. A subsequent
  uninstrumented `1479232203` run decodes all 194 seconds as ALAC 24/44.1, passes six early
  seeks and the near-EOF seek, and emits first PCM in 2.171 seconds. Different cache/start
  state prevents attributing the trace timeout to instrumentation. Neither run reproduces
  the original 146-second real-time fault. Real playback and screen-off checks remain manual.
- `scripts/run-dev.ps1` now always copies ordinary artwork, without source/destination length
  comparison. The current no-redundant-hash and single-current-format rules remain in force.

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

## Apple Music integration (2026-11-09)

- Added a native provider, catalog/artwork adapters, local account dialog and
  `apple://song/<id>` input. The helper is a pinned, patched Adam Linux build in the
  existing WSL Ubuntu 24.04 distribution; it contains neither Electron nor VLC.
- The owner completed native Apple Account authentication. The real subscription and
  US storefront were verified; all seven library playlists were read. The session
  survived a helper restart without asking for the password again.
- Real full-song proof passed for Eagles' Hotel California, catalog id `635770202`:
  the source master offered ALAC 24/192 and 24/48; the selected encoded source was
  decoded by foobar2000 as 24-bit, 192000 Hz stereo, 75140520 sample frames,
  391.356875 seconds. A completed-cache run verified duration and seeking. Evidence:
  `.local/tests/applemusic-real-192-result.json`. No helper audio decoder or resampler
  was used. An earlier real track also decoded completely as ALAC 24/48.
- Synthetic normal/fragmented MP4 and authenticated HTTP-to-`apple://` probes each
  passed full 24/192 decoding and seeking. Release x64 compiled with zero warnings
  and errors; the existing 144927 queue/motion/source assertions passed.
- The input now spools original encoded bytes into a core-owned temporary file before
  opening the decoder and releases its helper session after transfer. Unknown-length
  HTTP fixture decoding, full duration, near-EOF seek position and session cleanup pass.
  The current DLL also passed real first-play ALAC 24/96 (`1624173757`, 17880126 frames,
  186.251313 seconds) and ALAC 24/192 (`635770202`, 75140520 frames, 391.356875 seconds)
  with full duration and seek-position verification. First playback waits for transfer.
  The rebuilt helper is `c2c11e770c183ce9`; restart retained the authenticated account.
- A complete three-second AAC/48 kHz fixture also passed the same native input, core
  decoding, duration, near-EOF seek and session cleanup checks. A real AAC-only catalog
  source has not yet been identified; no ALAC failure is converted into AAC playback.
- Source-defined navigation now supports arbitrary section ids/labels and playlist group
  counts. Apple supplies eight sections: Favorites, Songs, Artists, Albums, Genres,
  Composers, My playlists and Saved playlists. Child playlist dragging is disabled;
  section ordering remains local. NetEase retains its terminology and legacy order.
- The real library probe read three created and three saved playlists, 196 favorite songs
  and 210 library songs. Favorite Songs is identified by its `favorited` tag and is not
  in either ordinary playlist group. Chinese section labels and curated playlist names
  passed, including New Music Mix as "音乐新发现". Merging catalog composer metadata
  fixed the empty Composer view: the final read found six genres and nine composer groups;
  sampled drilldowns returned 85 and one songs respectively.
- Provider-defined playlist vocabulary covers source menus, dialogs and empty states:
  Apple uses playback-list/library terms while NetEase keeps its song-list terms.
- Private Web contracts for favorite add/remove, playlist create/rename/delete, track
  append/removal, album save/removal and playlist save/removal are implemented. The actual
  native Provider passed 19 sequential fixture operations, including failed-rename rollback,
  successful rename and authoritative readback, with 12 expected writes and no unexpected
  calls. These tests only used a nonce-authenticated mock bridge. No real Apple library
  writes have been exercised. An extra executable production-bridge guard check was blocked
  by automatic approval review without a stated reason; the command did not execute.
- Final Release x64 reports zero warnings/errors; all 144927 queue/motion/source assertions
  pass. The deployed dev DLL matches the build, and the bridge still has its restricted DACL.
- Verification remains program-only. The owner handled the native login and the
  foobar2000 interface-selection prompt; no UI automation was performed.

## Apple Music follow-up (2026-11-10)

- Native Windows now replaces the temporary WSL helper, whose owned process was stopped.
  Adam nightly is pinned at `72d835ebf4c73f3b5f8b4e2b2496a2042a4a214c`, libhybris-win at
  `606bae7cdb49eaf8be79828de6cda0ce9d414295`. Build scripts preserve upstream Go/source
  structure and use adapter patches; no Electron/VLC output or helper decoder is included.
  `setup-applemusic.ps1` defaults to Windows, and `run-dev.ps1` starts its managed native
  endpoint without overriding fixture or alternate bridge endpoints.
- The complete isolated build/test/promotion path passed. Go external linking plus
  `editbin /STACK:67108864,2097152` commits sufficient Windows stack for guest ELF frames;
  this fixed initialization's stack-guard access violation without altering Apple binaries.
  At that acceptance point, runtime manifests recorded artifact hashes and the accepted
  engine SHA-256 was
  `d1c8dfed4f4baea8ddbb336c13477372b59a43881dd190a6b9c6f5d180f249c1`. Those digests
  are historical run receipts, not the current verification contract.
- Existing authorization survived migration and a native restart. The production dev
  endpoint is `127.0.0.1:20025`, served by the owned Windows `engine.exe`.
- Real Windows ALAC 24/192 passed full decoding: 75140520 frames, 391.356875 seconds,
  declared 391.357 seconds, precise seek, first audio at 2140.843 ms on the final build.
  A fresh Windows ALAC 24/96 source passed 17880126 frames, 186.251313 seconds and seek;
  its cold first audio took 10081.516 ms. Current Windows ALAC 24/48 also passed 9514880
  frames, 198.226667 seconds and seek, with first audio at 2891.818 ms. First-audio timing
  includes input initialization.
- The input now reads progressive encoded MP4 while a background transfer fills a private
  core-owned cache. Multiple-fragment ALAC/AAC ten-second fixture transfers decoded audio
  before network EOF, preserved exact provider tags and passed full duration/seek checks.
  Cancellation after the first chunk shut down in 109.795 ms. Seeking ahead may wait for
  outstanding transfer; starting playback no longer waits for the complete song.
- Fixed dynamic decoder info replacing catalog titles with untagged-container fallbacks.
  Both now-playing surfaces also update when provider metadata arrives asynchronously.
- Application-token discovery uses an expiry-aware, single-flight in-memory cache and
  bounded refresh after authentication failure. Browsing no longer downloads Apple's
  homepage/script for every request. Library refreshes coalesce, parallelize independent
  collections and serve cached navigation/details without repeated account validation.
- Final native real-read acceptance passed: three own playlists contain 35, 14 and 359
  songs; repeat detail reads took 0.3/0.0/5.3 ms and the cached shelf took 1.9 ms. All three
  playlist covers and all three sampled portraits decoded through the core/WIC path.
  The sampled catalog artist returned 19 albums, an accurate count of 19, and 10 top songs.
  Artists without catalog IDs now read their library albums; one real target returned
  one album with a matching count. This does not independently validate all 25 unmapped
  artists. Legitimately missing portraits no longer prevent artist-detail cache hits.
  Unknown counts are no longer represented as zero. Eight localized sections, 196
  favorites, 210 songs, six genres/nine composers and Favorite Songs exclusion also passed.
- Native private-write adapter regression passed all 19 mock operations with 12 expected
  writes and zero unexpected calls. These tests did not modify the real Apple library.
- At that acceptance point, the final Release component built with zero warnings/errors and
  its deployed DLL hash matched the build output. Routine build and deployment hashing was
  later removed. Queue regression passed 144927 assertions. Current real-read and audio
  evidence is saved under `.local/tests`.

## Apple Music correctness and Atmos (2026-11-10)

- Reproduced corrupt ALAC for `moon (feat. 初音ミク)` and `39` in the previous cache.
  Their first content-key transition occurs at segment 2, around 29.907 seconds;
  upstream retained only segment-0/1 key tags. The fix preserves the complete schedule
  and owns key state per DRM connection for playback, seek and reconnect. Handshake
  failures propagate, and cache version v10 invalidates bytes from the old producer.
  Both real songs now pass full foobar2000 decoding and seeking (335.435465/221.573333 s).
- Personal library IDs can resolve their authenticated original upload without a catalog
  match. Real MP3 and AAC uploads pass full decode and seek. All 66 no-catalog items
  pass bounded source-header probes: 26 MP3 and 40 AAC. The first 16 probes were only
  the initial-page subset; the audit now covers the complete paginated library.
- Album relationships are hydrated for library, playlist, artist and search songs. Shared
  expanded tiles group by provider/album identity, with a conservative missing-ID fallback;
  22 grouping checks preserve song activation and duplicate references. Library-only artist
  songs use actual credits and album IDs, without inventing popularity rankings.
- Uploaded artwork uses Apple's blobstore rather than mzstatic. The image-origin policy
  now accepts both legitimate HTTPS origins; 35 URL tests pass. Two uploaded album covers
  pass the real core/ArtCache/WIC path. Existing 3/3 playlist and 3/3 artist samples pass.
  Missing image entries can retry on library changes without clearing successful images.
- Optional Atmos delivery now preserves original EC-3/JOC sample bytes, verified against
  an independent copy-only remux. Cavern is loaded as a NativeAOT decoder/object renderer
  inside the input component; the helper does no audio decoding. Existing provider settings
  expose Atmos off/on and stereo-speaker, headphone, 5.1 and 7.1 rendering. The default
  remains highest lossless with stereo speakers. See ADR 0013.
- Real Atmos full-song/seek checks pass for all four output targets with 15 dynamic objects:
  8962560 PCM frames, 186.720 seconds, matching all 5835 encoded EC-3 frames. The renderer
  drains decoded data after compressed EOF and trims only an incomplete output block.
  Short-stream regression returns all 144384 frames (3.008 seconds). A ten-second media
  response produces its first decoded audio after 152.723 ms, 9868 ms before network EOF;
  cancel-after-first-chunk shutdown takes 80.706 ms. Metadata and session cleanup pass.
  Independent, interleaved and mixed-layout PCM hashes agree; headphone filter state is
  per context. Headphones add 6 dB of headroom, while speaker calibration stays unchanged.
- At that acceptance point, the deployed renderer SHA-256 was
  `5b2ab3e80d3ecc1621eec887fdff53084d908f45a7be4d8f8a4f9344bb1c515a`; both component
  and renderer development DLLs matched their tested build products.
- The then-accepted native Go helper was healthy at the managed endpoint, with engine
  SHA-256 `5c1220c3e16282e5c9a814ec9eb33d2f60be03d19c982e131daf189961af94ed`. The digest
  is historical run evidence, not a current runtime verification requirement.
  A new real 24/192 ALAC regression still passes 75140520 frames and precise seeking.
  Component builds remain warning-free. Queue regression passes 144927 checks; all 19
  mock provider operations pass with 12 expected writes and zero unexpected calls. No
  real Apple library writes or UI automation were performed.

## Apple Music app-owned service lifetime (2026-11-10)

- Fixed direct player restarts reporting an unreachable bridge: native startup belonged
  only to `run-dev.ps1`. Bridge configuration now opts into component ownership through
  an absolute `managed.runtimeDirectory`; deployment prepares it without starting a
  detached engine. Explicit fixture and alternate bridges remain independent.
- Account, sign-in, library and playback requests share a worker-side startup gate and
  authenticated readiness check. The child has an isolated environment and restricted
  handle inheritance. A kill-on-close Windows Job Object is assigned atomically during
  process creation, so even a parent exit before resume cannot strand a suspended child.
  Quit cancels pending startup and closes the owned job; there are no idle health timers.
- `test-applemusic-lifecycle.ps1 -IncludeExternalPreservation` passes direct cold launch,
  normal quit (actual process and listener disappear), another direct launch with the
  retained signed-in account, forced-parent Job cleanup, and external-service preservation.
  A stale diagnostic PID record after forced exit does not represent a live process and
  is safely replaced by the next owned launch. The test restores a managed dev instance.
- Full native library acceptance still passes: 210 songs, 8 sections, all 3 own playlist
  covers, all 3 sampled artist avatars and both sampled uploaded album covers; zero
  acceptance failures. Evidence: `.local/tests/applemusic-lifecycle-library-result.json`.
  Runtime evidence: `.local/applemusic/lifecycle-regression.log`. At that acceptance point,
  the component built with zero warnings/errors and deployed/build SHA-256 values matched;
  routine deployment hashing was later removed.
- Bridge descriptor contract tests pass without a live engine. No real Apple library
  writes, system service installation, WSL process or UI automation is involved.

## Memory-fault investigation (2026-11-10)

- Investigated system logs, resource-exhaustion records, existing application reports
  and a real WATCHDOG kernel live dump without changing Windows settings or application
  code. The recorded unexpected-shutdown time is 06:19:50; the next boot at 09:04 retains
  kernel stop 0x154. Its crash dump failed to write, preventing an exact stack diagnosis.
- The same stop category existed on October 20/23, before this repository. Separate
  Event 2004 records show approximately 255.7 GiB system commit exhaustion on November 9
  at 11:27/11:32, followed by an 11:34 reboot. They are not the overnight failure's
  immediate memory state and do not identify the source of that separate exhaustion.
- A 03:30 WATCHDOG capture is diagnostic 0x1A8/display-path activation failure in dwm,
  not the later 0x154 system stop. No causal chain between these events is established.
  The exact cause of that separate 03:30 capture remains undetermined. Private evidence and
  limits: `.local/diagnostics/system-freeze-20261110.md`.

### Playback-chain memory fault at 18:02:59

- The watchdog triggered while playback was automatic and the owner took no action. At the
  trigger, global commit was 87.36% (about 85.1 GB), the target process private memory was
  about 898 MB, kernel pools were stable and system cache grew from about 13.7 GB to 26.9 GB
  in 12 seconds. Other visible process private sizes were not large.
- The watchdog then stopped `engine.exe` and foobar2000. Global commit fell to about 48 GB
  (about 49%). This is strong causal evidence that Glasswave's playback chain caused the
  pressure; it is not treated as an unrelated whole-machine event.
- Evidence: `.local/tests/memory-watch-live/alert.json` and the adjacent bounded minidumps.
  FOOBAR Shared reached 39.646 GB. The exact allocation path remains unconfirmed. The original
  seek trigger has not been confirmed, but the automatic-playback case is captured.
- The old watcher died at 18:57 from an `Export-Csv` null-reference failure. CSV fault
  isolation, null-process handling, supervision and an atomic current-monitor pointer are
  now in place. Resolve `.local/tests/memory-watch-live.json` for current ownership rather
  than relying on a checkpoint PID. Protection still passed a four-second test with logging
  blocked; forced watcher-child restart also passed. Private/shared safety limits are 8 GiB;
  a separate GPU sidecar is active.
- A live Frida baseline at 14:08--14:12 UTC ran 235 seconds with 14 hooks and zero errors.
  Shared stayed about 1.037 GB and no Nt section calls were observed. This is a stable
  observation window, not an allocation-root diagnosis or a memory-fault fix.
- Planned, not executed: after batch reload, run `1479232203`, Late Night Obsession
  (feat. Nathan Nicholson) by Sebastian Davidson, through controlled ALAC decode plus six
  seeks under bounded spawn tracing. Its 194 s media duration is decoded faster than real
  time; this is an allocator test, not real-time playback or the original 146-second repro.
  The 18-hook/3.297 s smoke result validates the helper only. Keep protection active and
  retain separate private/shared/GPU observations; owner continuous/screen-off checks remain pending.

## Shared cache, responsiveness and prefetch (2026-11-10)

This earlier checkpoint's counts are retained as session evidence; the current baseline
and new owner reports are recorded at the top of this file.

- Added the provider-neutral `cache/` store (ADR 0014): Windows SQLite WAL, immutable
  payload generations, account-scope revocation, reader leases, crash/orphan recovery and
  independent LRU budgets. Common preferences expose song/image capacities and clear
  actions. Defaults are 10 GiB encoded audio and 512 MiB images; a Data category reserves the
  foundation for future list caching.
- Both provider inputs progressively stage original encoded audio and reuse completed
  generations. Shared flights isolate reader offsets/cancellation, reserve capacity for
  playback and let playback adopt speculative work. Cache I/O failure remains transient
  playback; non-cancel decoder rejection invalidates the exact source.
- Artwork uses cached-first/background validation with conditional GET, unchanged-byte
  suppression, old-pixel retention on failure, valid-format/WIC checks and account/request
  epochs. A real NetEase PNG response labelled JPG exposed a compatibility bug; recognized
  bytes now determine format. Old-account results and invalidated flights cannot republish.
  Shutdown retains service object lifetime while canceling/joining workers, avoiding a
  reviewed use-after-free and preventing late worker recreation.
- Apple playlist detail requests reuse embedded rows and include nested song relationships.
  Fallback hydration is batched with at most four parallel requests; a dedicated detail
  queue and single-flight callbacks avoid waiting behind library refreshes. The real
  359-row list fell from 30.849 s to 5.199-6.313 s cold and 0.5-0.6 ms warm. One timed run
  spent 4.283 s in remaining pages and zero in hydration; pagination now dominates.
- Provider root menus now offer sign-in or sign-out for both services. Lyrics keep stable
  track identity/document/position through transient unloads and unrelated settings/list
  updates. Eight request/document checks pass; paused output-switch appearance awaits the
  owner's direct check, without UI automation.
- The next-track host follows core callbacks and primes one input within the last 30 s.
  Core queue, ordered/repeat playback, skip filters, pause, stop-after-current, candidate
  edits and account/quality changes are handled without an idle timer. Public SDK inspection
  found no early random-next selection API. Random/Shuffle without a core queue skips
  proactive prefetch, as the owner permitted; native random selection is unchanged.
- Validation: 745 storage checks, 77 list-cache checks, 43 prefetch policy checks, 11
  in-process host checks, seven shared audio-cache cases, 11 image-cache cases and 19 mocked
  provider operations pass. Actual Apple input ALAC/AAC/
  Atmos two-play tests use one media GET with complete decode/seek; four Atmos targets pass.
  Each audio format also verifies promotion from a live prefetch to formal playback with
  one producer, independent offsets and an unaffected reader after the prime cancels.
  Real library acceptance passes all 210 songs, eight sections, three playlist covers,
  three portraits and two uploaded covers. Release is warning-free and DLL load/unload passes.
- Native lifecycle checks pass cold start, retained login, graceful quit and forced-parent
  cleanup. See `docs/plan/shared-cache.md` for evidence locations and remaining limits.

## Paged lists and predictable shuffle (2026-11-10)

The owner subsequently replaced the earlier native-shuffle requirement. ADR 0016
supersedes the native-random restriction recorded in the previous cache session.
The validation below is an earlier checkpoint, not acceptance of the post-21:40 revisions.

- Added shared typed list sessions, true provider paging and optional Memory/Persistent/None
  cache policies. Apple Music and NetEase supply one page per request where the service
  supports paging. NetEase playlist ID snapshots are immutable and bounded to 32/16 MiB.
- Default server order reads on demand. Local page search, explicit sorting and expanded
  album presentation request the whole source, with progressive updates and a pinned status
  band. Data/status versions avoid rebuilding content just to update loading state.
- List cache pages use immutable chunks and atomic manifests under an independent 128 MiB
  default budget. Old displayed prefixes survive background revalidation until a contiguous
  fresh generation catches up; account/query generations reject stale results.
- Added an occurrence-based shuffle/repeat scheduler and main-thread SDK host. The protected
  Now Playing playlist is the physical projection; core playback remains Default and still
  owns decoding/DSP/output. Up Next and encoded-audio prefetch consume the same schedule.
- Canonical ranks survive shuffle toggles, edits and duplicate tracks. Manual drag updates
  canonical slot intent. Replace/Clear invalidates old restoration and paged playback feeds.
  Priority insertion and third-party explicit queue entries are included in the actual order.
- Validation passes 27 native session/page-source cases, 13 native scheduler cases, 29
  mocked Apple provider operations before the metadata-region expansion, 77 list-cache
  checks, 123639 schedule assertions and the existing queue/grouping/prefetch regressions.
  The real 359-song Apple playlist first publishes 100 rows in 1.884 s, completing four
  pages in 6.830 s. The last completed Release build had zero warnings or errors, isolated
  DLL loading passed, and the tested build was deployed with retained Apple sign-in. See
  `docs/plan/paged-lists-and-scheduler.md` for evidence and limits.

## Async related metadata and guarded seek investigation (2026-11-10)

- Codex delegation now uses the default invocation without model/reasoning overrides.
  Verification requiring the owner's original file, official client or listening evidence
  must be explicitly requested; conflicting observations are not proof of an upstream bug.
- Primary list rows publish before optional relation enrichment. Album-dependent views
  gate on the metadata-ready prefix; ordinary tables patch changed ranges without clearing
  selection/focus. At this November 10 checkpoint, the baseline was 29 native cases / 69
  primary / 20 enrichment requests and 50 provider operations / 12 mock writes / zero errors.
  The November 11 accepted provider phase and pending native reruns are recorded above.
- Removed payload hashes and old-schema readers from shared storage. Index v2 and list
  schema v3 are the sole supported formats; chunk tokens identify immutable publications.
  Standalone storage/list suites pass 745/77 checks. Removed ordinary bootstrap, patch,
  build and deployment hashes; functional identity/image-change/PCM-render checks remain.
- Fixed shared progressive-file cursor races with positioned overlapped reads. Seven native
  audio-cache cases pass, including eight parallel readers over six passes / 97.8 MiB.
- First seek reopens completed, sized encoded media, validates the real range, and rejects
  invalid targets before entering the core decoder. ALAC and AAC each pass six early seeks.
- Build 2021 BGM1 has an MP3 Info control frame at 64 kbps followed by 128 kbps audio.
  The API declares 470.952 s; 9811 audio frames at 48000 Hz imply 235.464 s. The owner
  confirmed the official client initially flashes 7:51 but actual playback is about 3:55.
  The Go adapter now parses the Info/Xing duration and actual audio bitrate, and rejects
  truncated upstream responses. Native Windows helper tests/build/promotion pass. The real
  uploaded song decodes 235.434667 s, passes six early seeks and the near-EOF seek.
- The memory watchdog records global commit, available RAM, system cache, kernel pools,
  and repository audio process private/working bytes. It captures bounded diagnostic dumps
  and stops only repository audio processes on pressure. The current monitor is identified
  by `.local/tests/memory-watch-live.json`; its CSV/supervision recovery checks pass. The
  captured 18:02:59 playback-chain fault and remaining allocation unknowns are detailed above.
  System file-cache bytes must not be confused with per-process private memory.
- Generic no-cover placeholder painting and bounded negative caching are implemented; all 11
  native image-cache cases pass. The owner reported gray right-side lyrics. The newest
  `pane_common` uses the existing `ALBUM_0` resource; direct owner acceptance remains pending.
- The owner manually accepted the Account -> Japan KAF -> 花譜 example. China/Japan toggle
  responsiveness and immediate display refresh still need owner checks. The subsequent
  display/generation implementation and mock acceptance are recorded in the November 11 snapshot.

## Next

Primary rows now publish before optional album/artist enrichment. The earlier hydration
measurements are historical transport evidence, and first-page sizing remains a separate
latency follow-up.

1. Keep production frozen. Finish the in-flight six Colors/RAII native cases and 29-case
   ListSession rerun, then the corrected 18-case native scheduler run, then the controlled
   147 decode/six-seek trace. Main supplies final results and performs selective restoration
   from the authoritative post-upgrade owner backup after all tests.
2. The owner manually checks missing-cover transitions, right-side lyrics `ALBUM_0`, paged-list
   placement/drag feel, rapid metadata-region toggles, Now Playing double-click/failure
   behavior and Play Next preservation. No proactive UI automation or screenshots.
3. Owner tests lyric source priorities and manual search. Complete the remaining endpoint
   and account acceptance in docs/plan/lyrics-script-host.md.
4. Owner tests the restructured NetEase pages and the preferences page (list in the session
   reply); fix what they report.
5. Open review items the owner chose to leave: the helper's loopback binding is not verified
   after start (ADR 0008 says it is); an external service address over plain http sends
   the session cookie in clear text.
6. Keep the repository memory watchdog running while the captured 18:02:59 playback-chain
   fault is analyzed. Preserve the alert/minidumps and old seek evidence. Treat the
   85 GB to 48 GB stop/restore result as strong causal evidence, while keeping the exact
   allocation path and original seek trigger explicitly unconfirmed. The controlled decode/
   six-seek spawn trace is pending; real continuous playback and screen-off acceptance remain
   owner checks. Logical graphics and FrameClock test passes do not prove the memory root fixed.
7. Glass blur stays the host backdrop + color matrix (legacy accent tried, no gain).
8. Remaining tuning from the Win7 captures.
9. Stacked covers for artists.
10. Playlist editing: drag to reorder, drag from library to playlists, rename inline.
11. Split main_window.cpp further (detail band, breadcrumb, menus) and list_view.cpp
   (1700 lines) by responsibility.

## Known issues

- Library rows are formatted once per reload; very large libraries (100k+) not yet profiled.
- No device-lost handling yet; hover/pressed colors still extrapolated.
- DWM host backdrop blur is stronger than Win7's blur (no API to tune it).
- Side highlight length (H / 4 + 20 DIP) is fitted to two captures, not taken from uDWM.

## Phase handoff (2026-11-11)

The R6 FrameClock compositor-wake fix is owner-confirmed. The NowPlayingPane
position-only geometry fix and the gfx draw-clip fix are included in the R7 combined
build. R7 Release passed with zero warnings and errors in 22.25 s
(`.local/tests/finalNov11-phase-handoff-build-r7.log`), and normal deployment/start
succeeded (`.local/tests/finalNov11-phase-handoff-deploy-r7.log`). The native DSID
promotion-only step also succeeded without repeating the build or tests. No additional
live online verification was run, as requested by the owner.

Manual resize and residual-surface acceptance, plus the auth-restoration integration,
remain pending. The restoration hooks have no new tests, but they compiled in R7. Per
the owner, stop further tests and do not continue investigation in this phase. The
`startup-cache-same` run timed out and remains unverified. Keep the memory watchdog
running and preserve its current evidence. The latest owner capture,
`.local/tests/playback-probe-state-20261111T131819599Z`, remains valid. Commit recorded
in repository history; no artifact consistency checks or hashes are used. Never restore
the historical 196-row, 35-row, `124857`, or `110846` backup states.

The live profile remains authoritative after normal startup. Capture fresh owner state
before any future destructive probe; this recorded backup does not authorize overwriting
later owner changes.

Pending handoff only, with no work implied by this list:

- Complete the owner/manual checks for resize, residual surfaces, and auth restoration.
- Revisit `startup-cache-same` diagnostics only if a later owner explicitly requests it.
- Keep memory protection active and preserve the current watchdog alert/evidence path.
- The recorded backup is historical evidence. Any resumed probe must capture fresh
  owner state; it must not restore this snapshot over subsequent owner changes.

## Resumed streaming repairs (2026-11-12)

The owner resumed three bounded tasks: Apple Music 503/playback, startup cache and
list loading, and sidebar residual surfaces/resize geometry. This supersedes the
phase pause above for those tasks only. All three were delegated; no exhaustive
test sweep is authorized. See `paused-ui-work.md` for the other agent's restored
interface work and retained stash backup.

- Native account repair is committed as `ce9e416`. The prior const-mangled DSID
  getter did not exist in the bundled ELF and could abort the helper. Use the exact
  exported non-const getter, resolve absence without aborting, and check the ELF
  contract during builds. Account requests now await the existing native session
  initialization signal outside the backend lock, with cancellation and a 20 s cap.
- Two focused wait tests and the Windows staging build passed. After promotion,
  the first cold account request waited 8.030 s and returned 200 with a verified
  account identity. A real ALAC playback session returned 201, audio returned 206,
  and the open stream remained readable across another account request. Session
  deletion returned 204 and the helper stayed alive. Evidence is in
  `.local/tests/apple-cold-account-lazy-init-20261111T164209357Z.json` and
  `.local/tests/apple-cold-playback-validation-20261111T164252506Z.json`.
- Only the native runtime was promoted for that check; the installed interface
  was retained. Current owner state was captured after graceful exit in
  `.local/tests/playback-probe-state-20261111T164127564Z`. The live profile remains
  authoritative; this backup is not permission to overwrite subsequent changes.
- Cache diagnostics now distinguish list completion from actual page/manifest
  persistence. The checkpoint is committed as `746ae80`. The focused same-account
  restart check was not run. The runner still shares the owner's cache/profile
  and needs affected-store protection before fixture use. Product startup-path
  review has not established a new root cause, and startup latency is not claimed
  fixed. Release compilation passed with zero warnings/errors before the final
  narrow Cancel correction; that correction has not been rebuilt.
- Lyrics/queue animations now have fixed clipped viewports and relative content
  offsets. This closes an animation-boundary gap; the unrelated white main-list
  residual surfaces still need owner reproduction before assigning a root cause.
  This change is committed as `92aa18a`. Combined component compilation passed
  with zero warnings/errors, but deployment and owner visual acceptance are pending.
- The repository memory watcher, supervisor and GPU sampler remain running.
  No new pressure alarm occurred during the native account/playback validation;
  the earlier allocation fault is not claimed fixed.

At the owner's subsequent stop instruction, all further investigation and tests
were halted. Claude's six tracked and four new UI files were restored from the
stash without conflicts after the repair commits. They remain uncommitted for
Claude to continue. The stash backup remains; do not apply it again. No component
rebuild, deployment or test was performed after restoration. Native 503 fixes are
live; the newly committed component changes are not yet deployed. Resume only
the work the owner next requests.
