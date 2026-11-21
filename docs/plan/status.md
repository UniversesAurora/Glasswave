# Status

Updated: 2026-11-21

## Duplicate GUIDs, wall's coloured tiles, a disabled item's glow, components (2026-11-21, latest); owner checks pending, uncommitted

- The mini player came back at its default place: settings' cfg_miniPlayerRect had the GUID of
  the search history (0x8d3a51d2, search_scope.cpp) and cfg_songTags that of the recent lists
  (0x8d3a51d3, recent_lists.cpp); each pair shared one stored value, and whichever wrote last
  won ("color capsule" then four numbers did not parse). Both settings have fresh random GUIDs.
  scripts/build.ps1 now fails on any GUID literal written twice in src. Dev data updated by hand:
  the last mini place (208 384 2138 1982) moved to the new key, the search history set back to
  its one entry.
- Album wall: coloured tiles counted as "still loading" in PickSlot, so any loaded cover went
  first and with a handful of covers the coloured tiles left the wall at once. They now count as
  ready and are dealt at their share of the deck (kMinDeck 24), giving way one by one.
- Full screen: a disabled menu item (a station's shuffle) no longer starts the breathing focus
  glow on hover.
- Installed in the dev instance: foo_playcount 3.1.11 (foobar2000.org; star ratings, %rating%,
  %play_count%), foo_input_dvda 0.8.2 (SourceForge dvdadecoder), the Free Encoder Pack 2026-03-13
  (foobar2000.org, signed by Illustrate Ltd; unpacked into fb2k-dev\encoders instead of running
  the installer). Topping E50 II: native DSD to DSD512, DoP to DSD256.

## Menus that keep clear of what opened them; clipped song starts (2026-11-21); owner checks pending, uncommitted

- The output device menu picked a device by itself: it opens on the press, anchored under the
  device name at the window's foot; with the ASIO devices the list no longer fit below, Windows
  moved it over the pointer and the release chose the item there. `ShowActionMenu(owner, RECT
  anchor, ...)` passes the anchor as TPMPARAMS::rcExclude (TPM_VERTICAL), so the menu opens below
  or above it, never over it; used for the device menu, the now-playing artist links and the
  breadcrumb's drop-downs (`TrackWin7Menu` takes the exclude rectangle).
- Song openings not heard: no Glasswave code seeks at a start. The dev instance's output is now
  ASIO+DSD (core.output.mode 3CC07629..., foo_out_asio+dsd); an ASIO device restarts on a new
  sample rate and the DAC needs time to lock, which a seek back (device running) does not
  repeat. To confirm with the owner: whether it happens on the earlier output too.
- Fades on pause / resume: foobar2000's own Preferences > Playback > Output > Fading. They stop
  once an output device's Type is DSD: foo_input_sacd 2.0.27 clears outputCoreConfig_t::flagUseFades
  whenever its DSD path is on (autoproxy_output_manager::getCoreConfig), for PCM songs too, so a
  fade cannot corrupt DoP. Proposed, waiting for the owner: Glasswave ramps the volume itself on
  pause / resume (harmless for DSD: the plugin holds DoP at 0 dB).

## Rows of cards as the service shows them; output components (2026-11-21); owner checks pending, uncommitted

- Owner: rows that are not mixed may be cards, as Apple Music's 专属推荐歌单 and 音乐回忆 rows are,
  with motion. Apple's listen-now response says how each row is shown: `display.kind`
  `MusicNotesHeroShelf` (cards: Top Picks, 专属推荐歌单, 音乐回忆：你的热门音乐) or `MusicCoverShelf`
  (tiles; also `MusicConcertsEmptyShelf`, left out). Their items all carry superHeroTall art and
  motionTallVideo3x4. `stream::HomeShelf` now holds cards or items; the first hero shelf stays
  the top cards, every other one is a card row (`TileStyle::Card`, cards keyed per row). Home
  cache format 6. Log: "home: apple 19 cards (14 with motion), 19 rows (2 of cards)". 月度音乐回忆
  is an editorial item with no record behind it but a link (`attributes.link.url`,
  replay.music.apple.com): `HomeCard::link` (https only) opens in the default browser on click,
  and its menu has Open in browser.
- Installed into the dev instance (profile\user-components-x64, x64 DLLs and their helper
  executables; all four load): foo_out_asio 2.2.4 (foobar2000.org), foo_out_asio+dsd 0.4.7,
  foo_input_sacd 2.0.27 and foo_dsd_processor from the same package (SourceForge sacddecoder;
  foo_input_sacd's DoP output needs foo_dsd_processor since 1.6.1).

## Stations as tiles, compact tag and its setting (2026-11-21); owner checks pending, uncommitted

- Two 最近播放 on Home and 浮枕的电台 vanishing: Apple's 最近播放 row mixes 3 playlists with 8
  stations; the stations went to a card row of the same title (two 最近播放), and a station that
  was also a top card (same card key in two rows) had one visual moved between rows, leaving a
  gap. Now a station is `stream::Station`, a home row item (`HomeShelfItem`) shown as a tile like
  the others in the service's order (as Apple Music shows them), with its cover
  (`ArtKind::Station`, apple://cover/station/, catalog /stations/{id}), playing as radio, with a
  menu (Play station, Refresh). Live stations stay out. Card visuals are keyed per row. Home
  cache format 4. Apple Home now has 20 rows.
- The 原唱 tag hugs its glyphs' ink (DirectWrite overhang metrics) with a 2.5 / 1.5 DIP margin and
  is centred on the line (it sat low and right in a loose box); one painter (`ui::DrawTag`) for
  lists and the pane's queue. Preferences > Song lists: show or hide song tags (default shown).
- Output components researched (installed in the next session):
  foo_out_asio 2.2.4 (foobar2000.org, official, x64 since 2.2); foo_out_asio+dsd 0.4.7
  (SourceForge sacddecoder, 2025-05-29, native DSD) and foo_input_sacd 2.0.27 (same project,
  2026-11-01, DSD/SACD decoding); foobar2000 has no official DSD output. WASAPI event/push came
  from the old foo_out_wasapi; v2's built-in [独占] outputs replace it.

## Original-singer tag, artist search, station rows (2026-11-21); owner checks pending, uncommitted

- Owner confirmed: the home scroll flash is gone (absolute scroll coordinates, below).
- 原唱: NetEase's song objects carry `originCoverType` (1 original, 2 cover) on /song/detail,
  playlist, album and search songs; `ParseSong` reads it into `Song::original` and the track
  gets the badge "Original". `stream::Track::badges` is generic (untranslated keys); every view
  draws them before the artist, as NetEase does, never as a column: the song table's artist
  cell (`ListSource::CellBadges`), song tiles (`TileInfo::badges`: Home, search, artist page),
  foobar2000 playlists such as Now Playing (`stream::TrackBadges(item)`), and the pane's queue.
  Red outline #E5484D (owner's capture of NetEase). Persisted: list cache schema 6, home cache 3,
  NetEase songs.tsv 16 fields (the owner's 3486 lines converted in place).
- Song menu: Search for artist "<name>" for each credited artist, beside song and album.
- Apple Music home rows of stations (心情主场): station items were dropped, so a row of them had
  no items and was left out. A row's stations are now `HomeShelf::cards` with the square cover
  (HomeCardOf without the tall editorial art), shown as a card row that plays as radio, with a
  menu (Play station, Refresh); live stations stay out. Home went from 11 to 14 Apple rows.
- Output devices: the dev instance is a clean foobar2000 v2 with only its own components. The
  owner's other build lists ASIO (foo_out_asio), ASIO+DSD (the SACD component) and WASAPI
  event/push (the old foo_out_wasapi, which v2 replaced with the built-in [独占] outputs).

## List scrolling in absolute coordinates (2026-11-21); owner confirmed the flash is gone, uncommitted

Owner: the double-buffered rows surface did not stop the flash while scrolling the home cards up
and down. The scroll was built on coordinates relative to the painted range, whose origin moved
at every repaint, so every new glide had to start from this thread's estimate of where the
running one was (`VisualTopDip`, from its own clock). The compositor starts a glide only after
the surface is drawn and committed (tens of ms for a multi-screen surface), so the estimate ran
ahead and the content jumped forward at every notch of a running glide, most where a glide is
fastest. Now content coordinates are absolute: m_content's offset is minus the scroll position,
the painted range (rows sprite, card layer, shelf slide strip) stands at its own place in it, and
a glide starts from `this.StartingValue` (where the compositor shows it), its painted range with a
quarter-view margin each side. Settling also stops a glide the compositor still runs (a jump to a
position left the old animation driving the offset). Two earlier changes on this path were
guesses (double buffering, which stays as a sound practice); this one removes a definite flaw.
If it still flashes, the next step needs a frame capture of it (asking the owner first).

## A song that failed mid-play (2026-11-21); owner checks pending, uncommitted

Owner: a song failed before the last restart. Fact (applemusic/playback.log, 02:53 local): Apple
song 906238175, on the station 浮枕的电台, failed after 12.6 s decoded with "Unsupported format or
corrupted file"; the next song started 3 s later. Fact: Apple's transfer (`AppleEncodedFetch`)
read until the connection closed and called that a whole song, never checking the bridge's
Content-Length; NetEase's did the same after its first bytes. Hypothesis, not confirmed: the
bridge's connection closed early and the cut stream was decoded as a whole song. Now both check
the length: Apple's short transfer fails as "ended early (received of expected bytes)" and is
not kept; NetEase's continues once from where it stopped (renewed URL), then fails the same way.
A recurrence now says which it was. Not done: playing on through a failed Apple transfer (the
bridge's /audio may not take a Range).

## Menus, places and the scroll flash (2026-11-21); owner checks pending, uncommitted

- Context menus of records: one builder (`ui/item_menu.h`, RecordActions / ShowRecordMenu) for
  an album, an artist or a playlist of any source, used by Home, search, recently played, the
  artist page, saved albums, followed artists and the playlist menus (ShowRemotePlaylistMenu now
  wraps it). Same groups everywhere: View; Play (PlayRemoteList: what plays stops at once), Play
  next, Add to queue, Add to playlist, Download, Add to <source> playlist; Collect / Follow /
  Rename / Delete; Go to artist; Search for "<name>"; then the page's own (Remove from Recently
  Played, Refresh). Local results get View, the song commands and the search. Before, a recently
  played album on Home offered View and Refresh only.
- The song menu follows the same groups: Go to album, Go to artist, Song info (opened from the
  page: the path goes on from it), then favorites / playlists / download, then Search for song
  "<title>" and Search for album "<album>" (it offered only 搜索此唱片集). Wording: "Go to" is
  转到, Album is 唱片集 everywhere (WMP 12), searches name what they look for.
- A page's own place (SectionForView) follows real membership: an album is under Albums only
  when saved, an artist under Artists only when followed, a playlist under a shelf only when the
  source places it there; otherwise right under its source (an artist page read 唱片集 > ...).
  Item pages no longer need a section to be restored. The log writes the breadcrumb as it
  changes ("path: ..."). Checked with dev switches: Home > album > artist > playlist, back to Home.
- Home scroll flash ("what is further down flashed up over the cards"): the rows surface on
  screen was drawn into at each glide's start, so its new rows could reach the screen a frame
  before the glide's new offset. Now two surfaces are drawn in turn and the swap lands with the
  offset. Owner check pending.

## Technical info across restarts (2026-11-21); owner checks pending, uncommitted

Owner: after a restart the resumed NetEase song showed no sample rate. Its kept info was only
`channels=2`: NetEase's input reported technical info only from its catalog record (filled only
when the song's stream was asked for), never from its decoder, so nothing better was ever kept.
Both inputs now report the decoder's technical info while decoding (NetEase once more through
dynamic info as decoding begins) and keep it with `stream::NoteTechInfo` (written to metadb on
the main thread, so the snapshot keeps it); before decoding they fill in the last kept
(`KnownTechInfo`). Checked: played 6 s, restarted; paused and undecoded, the playback info reads
FLAC 1010 kbps 44100 Hz 16 bit (`/glasswave:queue` now logs it). Apple's path is the same code,
not yet checked with an Apple song.

## Breadcrumb, window animation and the remaining queue (2026-11-21); owner checks pending, uncommitted

- Breadcrumb is a component (`ui/breadcrumb.{h,cpp}`) that owns layout, painting, hover fades,
  hit tests and menus; MainWindow only gives it the path. Every level but the last has an arrow
  with a menu (at least the next level), so name and arrow always light together (before, levels
  without children - Home, Now Playing, search results - lit only the name). Too wide, the first
  levels go to the root arrow's menu.
- The path is the chain of pages one came down through: `View::from` is the page another was
  opened from inside (`MainWindow::Open`, the PageHost call every page uses); the breadcrumb is
  that page's path plus the page's own level. Sections of the navigation pane, the pane, the
  breadcrumb's menus and start-up show a page's own place. A link back up to a page already on the
  path cuts it there. Replaces the search-only origin fields and the album's artist parent; a list
  opened from Home now reads Home > list, a search result Search results (term) > it. The root
  menu is the pane's top level (Home, Now Playing, All Music, Local Music, the sources). Last
  view record is format 4 (no parent fields; older records fall back to the default page).
- Window opening and closing animate (visual-spec "Window open and close"; values estimated).
- Apple Music: an artist only in the library (uploaded songs) lists "Songs", not 热门歌曲. A
  removed album stays out of every page of the saved albums while Apple still lists it (the list
  read now applies the removals made here, as the artist album read did, now one helper), and its
  page says "This album is no longer in your library." instead of HTTP 404.
- Home scroll flash: not reproduced or explained. Two defects in that path fixed: the list's
  painted-range surface was re-created at every wheel step (up to 50 MB each; now one surface for
  the longest glide, kept while the view keeps its size), and motion covers faded in before
  their first frame (now once the video moves).
- Pane ghost: left until it shows again (memory: run `/glasswave:visuals` then).

## Restored queue keeps its songs' info (2026-11-21); owner checks pending, uncommitted

Owner: after a restart the resumed song in Now Playing showed "Apple Music 1888098957" and no bit
rate. Causes found (log, the snapshot file and `/glasswave:queue`), all fixed for both sources:
- Apple's restart catalog was installed from a worker after start-up; the paused song foobar2000
  resumed was opened before it, got the input's stand-in title, and the player wrote that into
  metadb (which foobar2000 keeps), so it stayed. It is installed on the main thread in `Start`
  now (0.5 MB). Checked: after two restarts the playing title is 感情グラス.
- `Catalog::Put` (Apple 30+ callers, NetEase 32) changed only the in-memory record; only
  `PutAndHint` wrote metadb, so a song put any other way never replaced stale tags (the restart
  install and unknown-song reads skip songs already known). Both catalogs now queue every record
  that is new or shown differently and write them to metadb in one batch on the main thread.
- Unknown songs asked for before the Apple provider started were dropped (and not asked again
  for 2 minutes); `Start` reads them now.
- Tag hints were "fresh" and wiped the technical info playback had measured (bit rate, codec).
  Providers write tags with `stream::HintTags`, which keeps it.
- New `stream/track_snapshot`: every streaming song in a playlist (tags, technical info, length)
  is saved (3 s after changes, and at exit) to `<profile>\glasswave\tracks\snapshot.txt` and
  given back to metadb in `on_init`, before playback resumes. Inputs use these last known tags
  before a stand-in; stand-ins carry `glasswave_placeholder` and are never kept, and the input
  reports the record once it arrives. Checked: 636 songs, no stand-ins, 399 with a bit rate (the
  others have not been played since; a song's bit rate is kept once it has played).

## Playing service lists, mini player place (2026-11-20); owner checks pending, uncommitted

- One read per list: every "play this service list" (home cards and shelves, a list page's Play,
  recently played, artist and search tiles; any provider, any song list kind) goes through
  `PlayRemoteList` (ui/remote_grids.h), which joins the same shared `ListSession` as the page. A
  cache confirmed within 5 minutes plays at once; otherwise the service's answer plays, at the
  moment the page shows it (the settled state is looked at after the notification, so the refresh
  a cache hit starts is seen). Before, the card used `FetchPlaylist` on its own: 15 s for Apple's
  喜爱歌曲 (log 22:52:27-41, fallback_batches 10) while the page read the list twice, the two
  fighting over its header. Open: Apple's `/tracks` read at 22:52:39-44 still gave 203 rows while
  `?include=tracks` gave 204 (hypothesis: the endpoints lag each other; check on the next test).
- Pressing play on such a list stops what plays at once; until the list is read, Play (main,
  mini, full screen, space, taskbar) does not resume the old queue; Stop or any new queue cancels
  it (`media::BeginPendingPlay`).
- The mini player's rectangle is kept across restarts (`settings::MiniPlayerRect`).
- Next, owner's queue: the home scroll flash (not found yet; the rows surface path clears before
  drawing), a navigation bar redesign (one breadcrumb component whose roots follow the nav pane,
  pages may only add nodes below theirs, uniform hover; lists opened from Home / search inherit the
  current path), whether the pane-ghost fixes hold for the latest changes, Apple uploaded songs'
  artists filed under top songs, removing an uploaded album (it comes back with 0 songs and a 404
  page), and a Win7 window open / close animation.

## Owner's sixth review (2026-11-20); owner checks pending, uncommitted

- 不感兴趣: the mini player's glyph is back as it was (the owner was happy with it; only the
  main window's was to change). The main window's keeps NetEase's heart outline but is drawn at
  the weight of its neighbours, shuffle and repeat (2 DIP ink, 1 DIP white edge at 57 %, about
  13 px across), with the slash cutting a white gap through the heart and its lower end shortened
  (owner: the 1.3 DIP hairline at 16 DIP was thin and out of place, the lower end too long;
  visual-spec "Radio mode"). Schedule tests updated for repeat one keeping the list order
  (123717 checks pass).
- The ghost above the now-playing pane's header is back. No `surface:` failures are logged and the
  pane sits beside the list, so it is not the row cover of 2026-11-14. Owner: it came after
  closing the mini player (its width dragged first), at random, not reproducible. The owner's
  capture at 200 %: a square-cornered band 670 x 48 px (the mini pane's width) at the pane's top
  left over the queue's song card, holding the cover large and sharp. Not the queue's hover plate
  (rounded, 12 DIP in). Static reading found no undrawn surface, so it is to be caught live:
  `foobar2000.exe /glasswave:visuals` logs every visible visual of the main window (tree path,
  window rectangle, opacity, scale, clip, brush, the surface's real size; pane parts named via
  `Visual.Comment`). Run it while the ghost shows; a visual of about 670 x 48 near the pane's top
  left names it, and none at all points at the system compositor.
- Station restore compares the saved paths with Now Playing only as a guard against a queue the
  station did not leave (a crash before foobar2000 saved it, another component editing it);
  owner asked why, decision pending.

## Repeat one, owner's fifth review (2026-11-20); owner checks pending, uncommitted

- Repeat one was built on moving the playing song to the end of Now Playing (the songs still to
  come before it, the core's order left at Default so it stopped at the song's end and the host
  restarted it). Next and Previous therefore fell off the list's end first: Next replayed the
  song, the list looked emptied, switching repeat off left nothing to play. Now the list keeps its
  order in every mode and the core's own order is Repeat (track) while repeating one
  (`PlaybackScheduler::Impl::ApplyCoreOrder`): the core replays the song at its end, Next and
  Previous walk the list. Checked on a station: repeat one sets "Repeat (track)"; Next went on to
  the next song twice (one song more each time, two ahead), Previous back; repeat off went back
  to Default with the list (3 ahead) and playback intact.
- Previous with nothing before (a station's first song; quick presses that outran the greyed
  button) starts the song over instead of letting the core stop. Checked: three quick presses
  past the first song left it playing.
- Station restore at start-up needs no service: the record is compared with the queue the
  scheduler restored (paths only); the songs to come are asked for as it plays, and a source still
  restoring its session does not count as refusing.
- A wheel step at a shelf's end no longer blanks it (the running strip was hidden before finding
  there was nothing to move).
- 不感兴趣's slash crosses the heart at 50 degrees through its middle (superseded by the sixth
  review).

## Owner's fourth review (2026-11-20); owner checks pending, uncommitted

- Radio across restarts: the station playing is kept (`cfg_station`: source, id, name, the paths of
  the songs it queued). At start-up, if the queue the scheduler restored holds exactly those
  songs, it goes on as the station (more songs as it plays); otherwise the record goes and the
  queue is an ordinary list. A station that answers nothing three times becomes an ordinary list.
  Checked: personal FM restored after a restart ("fm restored with 3 songs").
- No station goes back from its first song any more (heartbeat's "back" there stopped playback:
  nothing precedes it in the queue); `StationBacksAtStart` removed.
- Stopping clears the now-playing area whole (text, cover, lyrics) instead of a placeholder
  under the last song's name. A song's text and cover now change together (`MainWindow::ShowTrack`):
  a new song whose cover is loading waits up to 1.5 s with the last song shown whole; between
  songs the last one keeps its cover; and a starting playback shows its target at once
  (`PlaybackScheduler::StartingTrack`: Next / Previous / a list started), before its audio opens.
- The queue pane's repeat icons are vectors (`pane::DrawRepeat`): a loop of two arrows, the "1"
  alone in its open middle (the icon font's badge "1" sat on the arrows).
- Every sideways shelf keeps its tiles' width (the last one shows in part), not only the cards;
  a horizontal wheel or Shift+wheel over a shelf moves it 100 DIP a notch, gliding from where it
  shows (`ListView::SlideShelf`); paging slides the same way.
- Back restores each page's shelves where they stood (`ListView::ShelfPositions`, kept with the
  vertical position per view; a shelf that loads later takes its place when it comes).
- Home: a visit reads a source again only after 20 minutes; Refresh always reads
  (`Page::RefreshRequested`); the page's own second "Refresh" item is gone.
- Full screen menu: quick presses on two items no longer leave the first without its text (the
  first redraws its own type when the press's ink moves on).
- Play orbs on covers and cards show WMP's pressed look (wmploc 4154) while held, and play on
  release over the orb.

## Radio, scrolling and hit areas, owner's third review (2026-11-20); owner checks pending, uncommitted

- Previous on a station's first song: NetEase's PC client decides it by its play mode
  (`playAi` heartbeat, `playFm` personal FM; FM mode `FAMILIAR` is labelled 相似歌曲), no server
  field. `Provider::StationBacksAtStart`: heartbeat yes; personal FM, similar mixes and Apple's
  stations no. `radio::CanGoBack` greys Previous in the main window, the mini bar and the full
  screen (and the widget takes no hover).
- 相似歌曲 / 相似艺人 are stations now: the PC client's `/api/v1/radio/get` with `mode: FAMILIAR` and
  the card's `sourceIds` (`similar:<ids>`), 不感兴趣 like the personal FM.
- Apple station songs showed addresses in the queue: their records were stored but not hinted
  to foobar2000 (`Catalog::PutAndHint` on arrival now).
- Repeat one made a station queue songs without end: with repeat one the queue puts the songs
  still to come before the repeating song, so neither the schedule nor the queue's places tell
  how many are ahead. The station now keeps its own account (the songs it queued, in order, and
  which of them plays, followed through Next and Previous). Checked: personal FM with repeat one
  for 20 s asked for one batch ahead and stopped; switching to repeat one keeps the list (Up Next 2,
  queue rows 3 before and after; dev switches `/glasswave:repeat:<off|all|one>`, `/glasswave:queue`).
- Radio limits everywhere: one repeat cycle (`MainWindow::CycleRepeat`, `PlayerWindow::NextRepeat`;
  off / one on a station, and on a station the button shows repeat-one's glyph lit or not, the
  full screen item reads "Repeat one") for the transport, the queue pane's capsules, the mini bar
  and the full screen menu; shuffle shows 不感兴趣 where the station takes it (mini bar: a heart struck through
  in the player's Media Center glyph style; full screen menu: "Not interested"), else greyed (main
  window 35 %, the pane's capsule glyph 32 %, the player's glyph grey and the menu type grey, none
  with hover feedback).
- Album wall: `AlbumWall::UpdateAlbums` (called as the playing list changes) adds new albums to a
  playing wall: among the next cards at random, and onto coloured placeholder tiles (fading) once
  their covers load; albums already shown stay. A different list still starts the wall again.
- Covers: 每日推荐 / style daily lists get a made cover in the band (`DetailInfo::artBadge` /
  `artCaption`: the first song's cover, shaded, the day of the month and the list's name; a
  gradient without a cover). Home's song rows: each NetEase song's album is registered with its
  cover (the album cover lookup had no address until the song was played).
- Hit areas: a tile is what it draws (its plate box; a card its card), not its column's whole
  pitch, so the space between tiles is empty space for the page menu (every tile view).
- Scrolling by pixels in every list: the thumb is the view's share of the content; the wheel
  glides 100 DIP a notch (Windows' 3 lines; fractions for fine wheels; added up while gliding)
  over 220 ms on ease-out cubic, run by the compositor: the rows surface is painted for the whole
  glide and `m_content` (rows, cards, a sliding shelf) moves inside the clipping viewport, so the
  motion keeps the display's refresh rate with no repaint. Arrows step 48 DIP, the track a view.
  The scroll bar's thumb is a visual of its own placed by a compositor expression over
  `m_content`'s offset, so it glides with the rows (owner: the thumb stepped while the rows glided).

## Home page, second round; daily lists; radio mode (2026-11-15); owner checks pending, uncommitted

- Owner's review of the first cut: corners now continuous-curvature (no joint at the straight
  edge); elevation follows Material Design 1 (2 dp resting, 8 dp raised, two shadows each, rising
  3 DIP instead of 6); cards keep a fixed 20 DIP spacing and the last one shows in part; a shelf is
  one strip that slides as a whole (the jump to the next page before the slide is gone). Values in
  visual-spec "Home cards".
- Home at start-up: sources still restoring their session count (they used to be left out, so a
  restored Home said "Sign in..."); the page listens for account events and reads a source once its
  account is known. Each source's last home page is kept in `<profile>\glasswave\home\<source>.json`
  (`ui/home_store.cpp`, with its account; another account's is not shown) and shown at once,
  before the source is read again.
- Daily lists (owner: 每日推荐 and 华语流行日推 are fixed lists, not radio): `HomeCard::list` opens a
  `View::Kind::RemoteHomeList` page (a paged track page over `ListKind::HomeList`, cached like a
  playlist, "Home > 每日推荐" in the breadcrumb, Play and Download all in the band); the orb plays
  it. NetEase: `daily` (`/recommend/songs`) and `style:<category>:<tag>:<songs>` (the PC client's
  `/api/homepage/category/daily/song/list`, `source=homepage` plus the card's ids from its
  `targetUrl`; returns that style's 30 songs). Checked: 每日推荐 33 songs with its tagline.
- Radio mode (`media/radio.cpp`, `stream::Capability::Stations`, `HomeCard::station`): a station
  plays in the queue with two songs queued ahead; more are asked for as the schedule moves
  (`PlaybackScheduler::AddListener`, after the scheduler has moved, not a raw playback callback);
  a stall at the end restarts once songs arrive; playing anything else ends it. Shuffle and repeat
  all are off while it plays (the user's own come back after); repeat one works. Previous goes
  back through what played. NetEase: personal FM (`fm`, `/personal_fm`, 不感兴趣 through
  `/fm_trash` in the shuffle button's place, then the next song) and heartbeat mode (`heart`,
  `/playmode/intelligence/list` seeded with a liked song, then with the last song sent). Apple:
  every non-live station card (`POST /v1/me/stations/next-tracks/{id}?limit=5`; no "suggest less":
  its effect on the account is unverified). Checked: personal FM started and topped up on skips;
  an Apple station started with 5 songs.
- Not yet: NetEase's 相似歌曲 / 相似艺人 mixes (their endpoint is not in the PC bundle's plain
  routes), Apple's station shelves below the cards (rows hold songs, albums and playlists only),
  a "radio" mark in the Now Playing pane.

## Home page, first cut (2026-11-15)

- New `stream::Capability::Home` with `Provider::FetchHome` (`stream::Home`: the source's featured
  cards with words, picture, motion video and colours, and its rows of songs, albums, playlists)
  and `FetchHomeCardTracks` (a card that plays a song list). Apple Music: the Top Picks shelf of
  `/v1/me/recommendations` (with `extend=editorialVideo,editorialArtwork` and `l=`) become cards
  (tall `superHeroTall` art, `motionTallVideo3x4`), its album / playlist shelves rows. NetEase:
  the PC client's cards (`/api/pc/daily/rcmd/block`; podcasts left out), its rows
  (`/api/homepage/block/page` with the client's block codes, cursor and extInfo as JSON texts,
  plus the radar rows of `/api/pc/customize/block/page`); the daily-songs card plays
  `/recommend/songs`.
- Home in the navigation pane, above Now Playing, with WMP's online globe (wmploc 29635); a
  `View::Kind::Home` page (`ui/home_page.cpp`): Apple Music's cards, then NetEase's, each a shelf of
  `TileStyle::Card`, then the sources' rows in turn (song cards or covers with source marks). A
  visit reuses the session's last read for 5 minutes; Refresh reads again.
- The cards (`ui/card_layer.cpp`): composition visuals under the list's rows surface, lifted on
  hover as Material cards (compositor animations, no repaint), WMP's play orb, Apple's motion
  covers through Windows.Media.Playback (an H.264 rendition chosen from the HLS master; system
  media controls off; closed when off screen or minimized). Values in visual-spec "Home cards".
- Not yet: stations, personal FM, heartbeat mode and the similar-song / similar-artist mixes have no
  target (they show, but do not open or play) until radio mode; NetEase's style daily songs
  (华语流行日推) has no endpoint yet. Dev switch `/glasswave:home` opens Home and logs what each
  source sent; checked live: Apple 9 cards (4 with motion, two playing on screen), 16 rows; NetEase
  10 cards, 7 rows.

## Home page and radio: API survey (2026-11-15)

Probed with the dev instance's own sessions (read-only calls, plus Apple station next-tracks).

- NetEase, `/homepage/block/page` (the mobile home), paged by `cursor` (`hasMore`). Blocks seen:
  `PLAYLIST_RCMD` (playlists), `STYLE_RCMD` (songs "made for you", play all), `OLD_SUBSCRIBE_ARTIST_NEW`
  (followed artists' new songs / albums), `MGC_PLAYLIST` (radar playlists), `NEW_ALBUM_NEW_SONG`,
  `OFFICIAL_PLAYLIST` (scene playlists); `VIP_RCMD` and `TOPLIST` are listed in the order but were
  not returned on the first two pages. Skipped: podcasts / voice lists / voice books, music
  calendar (concerts), GPT news. `/homepage/dragon/ball` is empty.
- NetEase PC home (found in the PC client's own bundle, `orpheus.ntpk`, a zip from offset 0x64;
  called through api-enhanced's generic `/api?uri=...&data=...&crypto=eapi`):
  - Top cards: `/api/pc/daily/rcmd/block` `{clientTime: "YYYY-MM-DD HH:mm:ss"}`, fully dynamic.
    `data.items[]`: `coverText` (the card's label: "11-15" for daily songs, 心动模式, 私人漫游, 私人雷达,
    相似歌曲, 相似艺人, 华语流行日推, 听·<artist>, mood / trend playlists, music podcast), `title`
    ("从「…」听起"), `simplifiedTitle`, `coverUrl`, `resourceType` / `subResourceType` /
    `moduleType`, `resourceId`, `targetUrl` (orpheus:// action). `extInfo.pcRcmdDemoteItems` are
    fallbacks. The client keeps only known resource / module types and drops duplicates.
  - Rows below: `/api/homepage/block/page` with `cursor = {offset: 0, blockCodeOrderList: [...]}`,
    `extInfo = {abInfo: {"hp-new-homepageV3.1": "t3"}}`, `newStyle: true` returns the named
    blocks, e.g. `HOMPAGE_BLOCK_VIP_RCMD`, `HOMEPAGE_BLOCK_RED_SIMILAR_SONG`,
    `HOMEPAGE_BLOCK_STYLE_RCMD`, `HOMEPAGE_BLOCK_PLAYLIST_RCMD`. Radar rows:
    `/api/pc/customize/block/page` (`CUSTOMIZE_PLAYLIST_MGC`, `CUSTOMIZE_PLAYLIST_OFFICIAL`).
    `/api/pc/page/rcmd/resource/show` (`pageCode: PC_RECOMMEND_HOME`) gives the first screen
    (daily mix, banners, podcasts, toplist).
  - Other PC endpoints seen in the bundle: `/api/pc/page/rcmd/block/resource/refresh`,
    `/api/homepage/category/daily/song/list`, `/api/tag/tab/fm/recommends`,
    `/api/content/scene/fm/play/list`, `/api/radio/trash/add`.
- NetEase radio: `/personal_fm` and `/personal/fm/mode` (`DEFAULT`, `FAMILIAR`, `EXPLORE`, `aidj`,
  `SCENE_RCMD` + `submode`) return 3 songs per call, endless, each with `alg` and sometimes a
  `reason`. "Recommend less" is `/fm_trash` (exists in api-enhanced 4.41.1, not called: it changes
  the account's recommendations). Heartbeat `/playmode/intelligence/list?id=<seed>&pid=<liked list>`
  returns ~150 songs (`songInfo`) at once.
- Apple, `/v1/me/recommendations?name=listen-now&platform=web&timezone=...&with=friendsMix,library,social`
  (without `platform` it answers 400): the Home shelves, each `personal-recommendation` with a
  title, `display.kind` (`MusicNotesHeroShelf` for the large cards, `MusicCoverShelf`) and contents
  (albums, playlists, library playlists, stations, editorial items). Seen: Top Picks, Recently
  Played, genre and decade shelves, Find Your Mood (mood stations), Stations for You, Playlists
  Made for You, New Releases for You, More Like X, X Fans Like, Favorite Artists (artist
  stations), Replay, Concerts (skip). The top cards are the first shelf (Top Picks,
  `MusicNotesHeroShelf`, decoration `explanation`), dynamic: each item's `meta.reason` is the
  small label ("We Recommend", "Made By You", "Mood for You", "Featuring CIEL", "New Release"),
  `meta.subtitle` / `editorialNotes.short` the line under the title, `meta.editorialCard`
  ("vcard" or an "emc." editorial card) the card style, `artwork.bgColor` / `textColor1-4` the
  colours. `/v1/me/recent/played` and `/v1/me/recent/radio-stations`
  work. `/v1/editorial/...` (Browse) is outside the bridge's allowlist.
- Apple radio: `POST /v1/me/stations/next-tracks/{id}?limit=n` returns catalog songs for user
  (`ra.u-`), artist (`ra.a-`, `ra.<artist>`), mood / query (`ra.q-`) and discovery stations.
  Live stations (`isLive`, e.g. Apple Music Hits) and radio show episodes are HLS streams: skip.
  "Suggest less" would be a song rating of -1 under `/v1/me/ratings/` (not called).

## Download marks and smaller fixes (2026-11-15, later); owner checks pending, uncommitted

- Download marks redrawn after Apple Music (round strokes, a fading plate under the mouse; the
  offer an Aero glass ball fading in with the row); the Downloaded tab shows covers and the
  playing marker; the Downloads navigation arrow is a vector redraw of wmploc 45221; the search
  box's cross is WMP 12's clear button with its hot and pressed plates (clears on release).
- Artist page Tiles -> Icons at the top: the rebuild restored the old layout's first-row offset
  (`ScrollPosition::atTop` now brings it back at the top).

## NetEase title notes (2026-11-15); owner checks pending, uncommitted

- NetEase's grey note after a title (its `alia`, else `tns`; older endpoints `alias` /
  `transNames`) is `stream::Track::note` / `netease::Song::note`. Song lists draw it after the
  title as "(note)" in `#8C8C8C` where the title leaves room (`ListSource::CellNote`); the list's
  search matches it. Titles stay the title alone everywhere else: file names and tags of downloads,
  sorting, lyrics searches, now playing, matching.
- Formats: the NetEase catalog file has a fifteenth field (lines of the old 14-field format are
  dropped and fetched again); list cache pages are schema 5 (older pages read as misses).

## Downloads (2026-11-15); owner checks pending, uncommitted

- Follow-ups the same day: the band's "Play all" is "Play", its text no longer sits low (the
  layout already centres vertically); local album and playlist pages have Play on the command bar
  (plays the list as shown); a search page's breadcrumb is "Search results (term)" like the pages
  opened from it; a page switched to another layout while at the top stays at the top (the first
  row's offset from the old layout's header left a sliver); shelf arrows show the pressed plate
  (849 state 3) while held and page on release.

- Generic: a source with `Capability::Download` implements `Provider::ResolveDownload` (worker
  thread): the file's extension and format, its tags (title, artists, album, album artist, disc,
  number, date, genres, cover URL) and a `link(renew)` giving the HTTP address and headers (renew:
  the old link expired). Nothing else is source-specific.
- Engine: aria2 1.37.0 (`download/aria2.h`), the official 64-bit Windows build, downloaded on
  first use into `<profile>\glasswave\aria2` and `aria2c.exe` extracted with miniz (no hash: a
  newer aria2c.exe or release simply replaces it); it runs in a kill-on-close job with JSON-RPC on 127.0.0.1 and
  a random secret, and stops with foobar2000. Each song is split over "Connections per song"
  (1-16, default 8); "Songs downloaded at once" (1-10, default 3). An aria2 error renews the link
  and continues from its control file (three times). When aria2 cannot be installed or started,
  the song fails with the reason; the next download command (Download, Start, Retry) tries again. Options and
  RPC calls were checked against the real aria2c 1.37.0 (getVersion, addUri with 8 connections,
  a header, out/dir, tellStatus, shutdown). NetEase implements it at its own "Download quality" setting (a preview clip
  is refused, an expired link is renewed at the same level and refused if the file changed).
  Apple Music does not offer it (its audio is DRM-protected; decrypted exports are not built).
- `download/downloads.h`: a queue, files written as `<file>.part` and resumed
  from their size (pause, failure, restart), renamed when complete, then tagged and given the
  front cover through foobar2000's own writers. The list lives in
  `<profile>\glasswave\downloads.txt` (unfinished songs continue at the next start). Names follow
  Preferences > Downloads: the folder (default `<Music>\Glasswave`), Title / Artist - Title /
  Title - Artist, and no folders / by artist / by artist and album; a name in use gets " (2)".
- Playback: the NetEase input plays a downloaded song from its file while the file exists, and
  reads its tags when the catalog has not seen the song this run.
- UI: All Music > Downloads (`ui/downloads_page.h`), tabs Downloading (Windows 7's progress bar,
  aero 648 / 656 / 658 / 657, status with percent and speed, size) and Downloaded (format, size,
  date, folder); Pause all / Start all or Open download folder at the command bar's right; menus
  to start, pause, retry, cancel, open the file's location, remove or recycle the file. Song menus
  offer Download and Save as... (one song) or Download to... (several); album and playlist tile
  menus Download and Download to...; album and playlist pages Download all beside Collect.
- Lyrics (Preferences > Downloads > Lyrics in downloaded files: don't save / embed / a same-name
  .lrc beside the file / both; default embed): the lyrics the lyrics panel shows for the song (the
  owner's selection, else the automatic answer, `lyrics::Service::Lookup`, which does not disturb
  the panel's request), as found, with the song's offset as [offset:]. Embedded through
  foobar2000's tag updater (MP3: UNSYNCED LYRICS, i.e. USLT; others: LYRICS). Another selection,
  "no lyrics", automatic again, a new search or another offset (`Service::SubscribeChanges`)
  rewrites the song's downloaded files 1.5 s after the last change; a new setting rewrites every
  downloaded file; deleting a file recycles its .lrc too.
- Song lists of a downloading source (and mixed search results) have a Download column before
  the length: downloaded songs show a grey round mark (a click asks to remove the download and
  recycles the file), downloading ones a progress ring, other songs a blue mark on the hovered row
  (a click downloads). Downloaded tab: the songs' own menu (View album, Go to artist, Song info,
  playlists) with Open file location / Remove from list / Delete file, and a Source column.
  Running bars carry Windows 7's moving glow.
- Owner checks: a song, an album and a playlist downloaded; pause / resume / restart in the
  middle; names and folders of each setting; Save as...; playback from the file (also after the
  file is deleted: it streams again); the progress bar's look.

## Unified search (2026-11-15); owner checks pending, uncommitted

- NetEase search API (`netease/client.h`): songs, albums, artists, playlists and lyrics
  (`/cloudsearch` types 1, 10, 100, 1000, 1006), best matches (`/search/multimatch`, empty
  when not signed in) and suggestions (`/search/suggest`, mobile completions plus top items).
  The provider now offers `CatalogSearch` and `SearchSuggestions`; `ListKind::CatalogSearchLyrics`
  is new, its tracks carry `lyricMatch` (the matched line and two after it) and `lyricText`.
- One set of terms for every source: All results / Top results / Songs / Artists / Albums /
  Playlists / Lyrics, in the app's WMP wording (歌曲, 艺术家, 唱片集, 播放列表).
- Search box scopes (`ui/search_scope.h`): Current list (the old filter, default), All sources
  (the local library and each provider), Local Music, each provider. Shown as a blue
  "网易云音乐:" token, Backspace in an empty box removes it, the cross clears text then scope.
  History of the last 20 searches (`cfg_searchHistory`), clearable from the drop-down.
- Drop-down (`ui/search_popup.h`): history when empty, suggestions while typing (200 ms pause;
  the current list's titles/artists/albums in the list scope; the library's names plus each
  provider's terms and top items otherwise), the scope links in its footer. Up/Down/Enter/Esc.
- Search page (`ui/search_page.h`, `View::Kind::Search`, replaces RemoteSearch and the
  "Search ..." prompt): WMC text tabs over the list; All results groups Top results, Artists,
  Albums, Songs, Playlists with "Show all (N)" opening the tab. Several sources mix by rank,
  songs with a Source column, covers with a source badge. Lyrics tab: a Lyrics column and
  "Copy lyrics" on one selected song; shown only where a source can search lyrics (NetEase).
- Second round (owner feedback the same day): Show all crashed (the tab navigation destroyed
  the list source inside its click; now deferred); history rows delete with aero's red cross;
  glass magnifier and cross; scope buttons as Explorer's filter buttons; the list area hides
  without history; suggestions follow the IME's uncommitted text; the tab bar sits on the rule
  and slides; source marks are the installed apps' own icons (`Provider::AppNames`); the
  category tabs offer Expanded tile; the overview is shelves of song cards and covers paging
  sideways (`RowGroup::carousel`, `TileStyle::Song`) with round-chevron arrows and "show all";
  covers and song cards show a play button on hover (`ListSource::CanPlayTile/PlayTile`).
- Third round: the scope buttons use Explorer's measured filter button, tighter, with a fade;
  the tab bar slides on the compositor (Windows 8.1 curve); "show all" is a vector redraw of the
  aero expand button; shelf arrows are tall, shown over the hovered shelf, plated on hover; source
  marks fade with the play button; play buttons also on provider album / playlist tiles, the artist
  overview, local album tiles and the local Playlists page.
- Apple Music: a library-only album (l.gypM54e "Stargazer") had its library id mapped as its
  catalog id and its page asked the catalog (404). Only all-digit ids go to the catalog now.
- Fourth round: the drop-down's highlight leaves with the mouse; the search box edits like an
  edit box (caret, selection, word moves, clipboard, mouse); "show all" is the expando button
  (aero 693 - 701) in vectors; shelves slide on the compositor (Windows 8.1 curve); pages opened
  from search results carry the search (`View::searchTerm`), and the breadcrumb shows "Search
  results (term)" above them; "View album" / "Go to album" select and scroll to the song
  (`View::select`); a double click on Expanded tile's album column plays only that album
  (`ListSource::ActivateGroup`); "Search for album" in song and album menus searches every
  source; Recently Played has Songs / Albums / Playlists tabs (`ui/recent_page.h`; playlists
  played are kept in `media/recent_lists.h`); album and playlist pages show Play all in the band
  and Collect at the command bar's right, Refresh left the bar; scroll positions keep the top
  row's offset, so a restored page no longer hides the group header above its first row.
- Not done: NetEase-style expanding lyric blocks under each song (needs variable-height list
  rows); restoring a mixed/local search page after a restart (only provider searches restore);
  Play all on
  local album / playlist pages (they have no detail band).
- Owner checks: drop-down look and keys, token, tabs, overview, mixed results and badges,
  lyrics tab, suggestions of each scope, history clearing.

## Player and lyrics follow-ups (2026-11-14)

Committed after the owner's requests of 2026-11-13/14; built warning-free and deployed, the
items below not yet checked by the owner:

- Lyrics offsets are kept per lyric (a different lyric of the same song has its own), with
  +-1 s steps beside +-100 ms. The two per-song offset files of the old format were deleted
  from the dev profile.
- Album wall: covers decoded at the tile's size, the sharp one replacing the interim one;
  covers cross-fade in (600 ms over the placeholder, 400 ms over the blurred stand-in).
- Apple list cursors no longer carry the process generation ("The list cursor does not
  belong to this query" after a restart); a refused saved cursor reads the list again.
- The full-screen sung line runs on the frame clock (WM_TIMER starved to once a second).
- Full screen: the back orb slides down from above with the lyrics or the queue and back up.
- Every seek bar (library window, mini and full player) follows the position on the frame
  clock, a device pixel at a time, instead of the once-a-second callback.
- The pointer stayed hidden over the mini window after leaving the full screen with its
  controls away (Escape or a double click); the mini window now never hides it.
- Open: whether per-song "Add to Library" is wanted for Apple Music (album-level today).
- Later the same day: the blurry player cover after a restart (ArtCache reported a stand-in as
  Ready), the album wall drifts downwards, and artist, album and playlist pages show every
  detail of the music the services have (details pane under the stats line, tooltips for cut
  values; docs/architecture.md, "Profiles and details"). `foobar2000.exe
  /glasswave:open:<source>:<artist|album|playlist>:<id>` opens a page with real data and the
  log records what its band shows.
- Next: the artist page as a grouped overview (owner's choice, 2026-11-14): one Windows 7
  group header per artist view the source declares (Apple Music's views come with localized
  titles: top songs, latest release, albums, singles and EPs, live, compilations, appears on,
  playlists, music videos, similar artists), a row of tiles under each and "Show all (N)";
  NetEase has top songs and albums.
- Song page done (details band, credits, similar songs); not planned: curators, record labels,
  music videos, radio / podcasts (owner).
- Memory incident, 2026-11-14 13:45 (owner: the player vanished while playing full screen):
  the repository memory watchdog stopped engine.exe and foobar2000 after system commit jumped
  from about 79 to 98.9 GB (decimal) within two seconds (`.local/tests/memory-watch-pointer-*/
  incidents/20261114T054529355Z`). Six incidents today share the pattern: commit flat, then a
  sudden 14-19 GB rise; stopping engine.exe releases 13-16 GB, foobar2000 1-4 GB. engine.exe at
  the moment (its dump): 596 MB private, 5 MB mapped, no Section handles, 2207 semaphores (a
  stable count); foobar2000: 359 MB private, 615 MB shared commit, GPU about 700 MB and flat.
  So the commit is charged outside both processes' address spaces yet released when engine.exe
  exits (fits the 2026-11-09 report: 255 GB commit, 39 GB process commit). Not yet known: who
  owns it.
- The memory watchdog now records that evidence (running elevated since 14:17,
  `.local/tests/memory-watch-forensics-20261114-141743`): every sample adds the sum of all
  processes' commit and the unattributed rest (normal now: about 46 GB and 28 GB); a rise of
  4 GB within 10 s writes an `incidents/*-rise` folder without stopping anything; an incident
  writes, before and after the stop, a census of every Section object (size, SEC_COMMIT or
  reserve, holders), kernel pools by tag, and the tails of the engine, Glasswave and playback
  logs. Next spike: compare `sections.txt` / `pool-tags.txt` with `sections-after.txt` and the
  unattributed column. (14:06 the old watchdog stopped the player because a 2 GB test
  allocation of mine pushed commit to 85.0 %; not a fault of the player.)
- 14:25 and 14:32 the watchdog stopped the player again with commit at 85.6 / 85.0 % and the
  repository processes at 0.8 GB: ordinary fluctuation over an 83-85 % idle level, not a
  leak. Stops now come at 95 %, or at 88 % while commit rises 4 GB within 10 s (the real
  incidents rise 14-19 GB in seconds). Running since 14:34 in
  `.local/tests/memory-watch-forensics-20261114-143451`.
- Build 2021 BGM1 (uploaded MP3, `i.1YBNXgQSqJaz4dR`): Apple's library says 470.952 s, the
  delivered audio is 235.435 s (ffprobe of the stream; the session declares 235.464 s). The
  preview check added on 11-11 rejected it before it started, and the decoder's estimate kept
  7:51 on the seek bar, so seeking past 3:55 failed. Now uploaded songs skip the preview
  check, the input reports the session's length when the decoder disagrees, and the
  delivered length is kept in `applemusic/cache/lengths.tsv` and replaces the library length
  in the catalog, metadb and Apple lists. Lists restored from the list cache keep the old
  length until they are read again. Owner check pending.
- Library items Apple refuses to play (bridge 500 "webPlayback refused the library item";
  owner: a region restriction) open as empty tracks and are logged as `playback_skipped`, so
  playback moves on without an error. Meeting the same song again before anything plays, or
  20 in a row, fails as before. Owner check pending.

## Owner request batch (2026-11-14), third part; owner checks pending, uncommitted

- CD drives: a drive found empty no longer reads itself again on every refresh of its page
  (it showed "Reading the disc..." forever); a click on its row looks again.
- Navigation pane: scrolling to the end leaves the last row whole (the 6 DIP top inset was
  not counted).
- A link to the page on screen (the playing song's artist while on that artist) does nothing;
  Refresh still reloads (`m_forcePageRefresh`).
- Taskbar thumbnail: every answer to DWM is one square bitmap with the cover fitted in it. The
  art cache's 1x1 clear stand-in (a cover reloading) had been scaled to DWM's whole box, so
  DWM took the window's shape for the frame and stretched the real cover into it.
- Breadcrumb plates fade in (about 65 ms) and out (about 160 ms) like Explorer's.
- Control bar: a title, artist, lyric line, stream format or output device cut short shows
  whole in the tooltip (above it) after the hover delay.
- Apple Music: Audio quality (High Quality AAC 256 / Lossless up to 24-bit 48 kHz / Hi-Res
  Lossless up to 192 kHz), sent as the playback capabilities. The helper (glasswave.patch)
  now honours `hiRes: false` by picking the best ALAC variant at or below 48 kHz (a master with
  only hi-res keeps its best); pre-warmed sessions carry the best ALAC and are skipped for a
  capped request. Helper rebuilt and promoted; Go tests pass.
- NetEase artist pages: EPs and singles in their own groups when NetEase types them "EP" or
  "Single"; "EP/Single" stays in Singles and EPs. Apple Music keeps its own sections.
- The image cache logs a transport error's text and tries once more.
- CDs: a MusicBrainz 503 (its rate limit) is asked again after 1.5 / 3 / 6 s, and a tagging
  whose lookup failed tries again every 20 s (4 attempts); before, the cover lookup got the
  answer and the tags were never written. Tracks the loader left without info (path as title,
  0:00) are read once more on the worker. Verified here: the second disc was tagged on start.

## Opened files and audio CDs (2026-11-14); owner checks pending, uncommitted

- A file opened from Explorer ("Open with"): foobar2000 replaces the active playlist (the
  queue) with it and plays it. The queue then becomes every file foobar2000 reads in that
  folder (not its subfolders), the opened song playing on without a restart
  (`PlaybackScheduler::AdoptPlaying`, media/outside_play.cpp). foobar2000 hands plain file
  arguments to no component, so this watches for the single foreign song in the queue.
- Audio CDs (cd/): every CD drive has a row under Local Music > Discs (WMP's audio-CD icon,
  wmploc 29560), named after the disc's album once read. With "When an audio CD is inserted:
  Read it at once" (Preferences > Glasswave > Audio CDs, the default) a disc is read on
  insertion; otherwise only when its row is opened, or when File > Open audio CD plays it.
  The drive is read as a folder (`cdda://<id>,<nn>`) through `playlist_loader` on a worker,
  so foobar2000's progress dialog never shows. No playlist, no autoplay; playing a row makes
  the queue the disc; right-click offers Play and Eject. File > Open audio CD still works:
  its playlist is taken over by the queue and removed.
- Discs are identified on MusicBrainz by disc id (or the TOC when not submitted) and tagged
  (title, artists, album, date, disc, MusicBrainz ids) through `metadb_io_v2`; the cover is
  the release's Cover Art Archive front, cached under the disc. foobar2000 names untagged
  tracks "CD Track 01", so "tagged" means having an album; lyrics wait for that.
- A disc taken out leaves the queue (playback stops if it was playing), and its songs leave
  Recently Played / Most Played (refreshed if on screen) / the pane's history until it is
  back (counts are kept).
- The image cache logs a transport error's text and tries once more: the CD's 1200 px cover
  failed twice (archive.org redirects are slow at times), leaving the 500 px one stretched in
  full screen. This disc's original on the Cover Art Archive is 1280 px.
- Verified here: the disc in G: was identified and its 25 tracks tagged. Not verified: the
  pane, the setting, the cover in full screen, lyrics, eject handling, Explorer folder
  expansion.

## Owner request batch (2026-11-14), first part; all owner checks pending

- NetEase playlists show `trackNumberUpdateTime` (songs added or removed) as "Updated";
  `trackUpdateTime` also moves when NetEase changes any listed song on its side.
- NetEase song tables have a Popularity column (`pop`, 0-100) drawn as a bar
  (`Capability::TrackPopularity`; its own saved column layout).
- The search box filters the view only: activating a row plays the whole list from it.
- The search box draws the IME composition in place; the candidate list sits under the box.
- A list's loading band pushes the rows down instead of scrolling them under it.
- Taskbar thumbnail: the playing song's cover (iconic bitmap), the window on Aero Peek.
- Full screen: no shadow frame (its corners showed as rounded lines); the chrome layers fade
  ahead of a shorter drift toward their edges.
- One song's menu offers "Save album art..." (the provider's original: Apple 9999 px PNG,
  capped by Apple at the upload's size; NetEase without `param`). The playing song's cover and
  title open its menu without the play / queue commands. Covers drawn above 1200 px use the
  original rendition.
- Second part (same day, owner checks pending):
  - Taskbar thumbnail: answered at once with a clear square while the cover loads (DWM sized
    the first frame from the window and stretched the cover into it).
  - Compact mini player (narrower than 344 DIP, down to 250): square client, the cover fills
    it; title on a shade at its top and only stop / previous / play / next / volume at its
    foot, both with the pointer; growing, the margin opens first (the cover's glass edge and
    shadow fading in with it), then the cover grows. The old cover-only state is gone; the
    song's lines no longer stay behind.
  - Mini / full screen: right click on title, artist or album opens the song's menu; on the
    time, the lyrics menu.
  - Column layouts are kept per provider and kind of list (`stream-<provider>-<kind>`); the
    Popularity column is offered on every NetEase list, on by default on albums only.
  - Artist / saved-album pages in the expanded (tile) view load one album's songs at a time,
    only as the list nears its end (or for sort, filter, play all), 600 ms apart; the band
    shows only while something loads.
  - Preferences > Window color: Win7's 16 colours (from its `themecpl.dll`) or the playing
    cover's, and Win7's intensity slider mapped as themecpl does (visual-spec.md); colour
    changes cross-fade. The lyrics pane takes the cover's colour under its shade.
  - `scripts/build.ps1` caps parallel compilers by free commit: on 2026-11-14 at 14:52 a
    32-compiler build took system commit from 85 % to 99.8 % and the memory watchdog
    (correctly) stopped the dev instance.
  - Old column layouts under `stream-songs*` are no longer read.
- Third part (same day, owner checks pending):
  - The list keeps its anchor row at the same distance from the top across a change (it
    was put at the very top, hiding the album header above it on every load).
  - Apple Music artist lists (albums, top songs, playlists) are persisted like the others;
    the expanded view's 600 ms gap counts only after reads from the service.
  - A double click in the expanded artist view plays that album only, at once.
  - Narrowest library window with a lyric line: the artist leaves the title's row.
  - Mini window restored from full screen: a pane opened there closes when the window is
    lower than the pane's height or compact.
  - Full-screen pane card: glass laps the rim by a device pixel (pale corner arcs); rim
    `#2F64B4` at 36 %.
  - Collapsed artist description: blank lines closed, last line ellipsized, Show more inline.
  - Select lyrics > No lyrics (kept as a selection, `[glasswave-none:1]`).
  - The full-screen chrome does not hide (nor the pointer) while a menu is open.
- Artist page overview (same day, owner check pending): the icon view of an artist page is
  now one group per artist section of the source (docs/architecture.md, "Streaming
  providers"), one row of tiles each and "Show all (N)" opening the group in place.
  Apple Music: latest release, hot songs, albums, singles and EPs, live, compilations,
  appears on, artist playlists, similar artists (`/view/latest-release`,
  `/view/similar-artists` added). NetEase: latest release and the album parts derived from
  its album list's types, hot songs, similar artists (`/simi/artist`, signed in). The
  ListView draws icon tiles in groups (header lines among tile rows). Not verified against
  the services yet: the two new Apple views and NetEase's similar artists.
- Follow-ups (same day): Show more sits on the description's last line (its top from the
  line metrics; the caret's y was a line low); tile columns share the row's spare width and
  respread on resize; text cut short in tiles and cells shows whole in the Win7 tooltip;
  the full-screen card's rim takes OVERLAYS.OUTLINE's ~3 px corner (a 6 DIP rim left a pale
  crescent of the outline's white plate in each corner).
- Later still: the collapsed description's third line was not drawn (three lines of 16.004
  DIP in a 48 DIP box; DirectWrite drops a line that does not fit), so Show more seemed to
  stand alone; Apple's `latest-release` view answers `limit` with HTTP 400 and is now
  requested without it; the expanded artist view keeps one loading band from the first
  album read from the service until the screen is full (it came and went per album).
- Covers that stayed away until a restart: NetEase album covers whose address the catalog
  did not know (albums seen only through their songs) failed for good; the album's own
  record is now read for it, as a song's details are for a song (verified in the log: the
  failing albums load after one `/album` request). The cover cache gives a delivered picture
  to the same cover's other sizes that have none, and tells the views when a failed
  picture's retry pause ends (a view asked once never asked again). The NetEase cover
  failure log line now says which cover and why.
- Page header: an artist's counts survive the profile read after the album list (the stats
  line showed, then vanished); without a stats line the facts close up under the name.
- Next: owner checks of this batch; commits per logical change once approved.

## Backend review: caches, lists, requests, provider model (2026-11-13)

Built (Release x64, zero warnings) and started in the dev instance; nothing below has been
exercised by a probe or by the owner yet. Design notes are in `docs/architecture.md`.

- Lists no longer reload after a restart: a write (`NotifyListChanged`) revalidates lists in
  place instead of purging every persisted list of the provider; the UI no longer purges
  them on a language or display change; `InvalidateListCache` has no default mode. The
  "Loading..." row shows only for a list without rows; reading the disk snapshot is not a
  load. Freshness window 60 s -> 5 min; a fresh complete snapshot needs no request; an
  unchanged NetEase playlist ends its re-read on the first page.
- Images: one download per picture (a track's cover is its album's), no per-process and no
  5-minute revalidation (7-day window, explicit `Invalidate`), no SHA-256 on read or write,
  no second decode after download, 304 updates validators instead of copying the payload.
  `ui::ArtCache` is indexed per picture; failed pictures retry with a backoff. Refresh
  revalidates only the refreshed source's art. The unused `netease::CoverCache` is deleted.
- Store: `synchronous=NORMAL`, no write per lookup, in-memory byte totals, cached digest
  provider. HTTP: one shared WinHTTP session per proxy mode (connection reuse).
- Audio: a cached song opens from the cache without asking the service first (NetEase song
  URL, Apple Music playback session).
- Provider model: `stream::Capability` set replaces the `Features` flags; `Provider::Text`
  / `Wording` / `TextContext` replace `PlaylistText`, `LibraryActionText`,
  `LibraryStateText` and `ArtistPreferenceActionText`.
- Second pass, found with new diagnostics in the dev instance (`list cache:` / `list:` lines
  in `glasswave-netease.log`):
  - An Apple Music playlist was never persisted: a track credited an artist without an id
    and the list cache rejected the whole page ("artist has no stable identity"). A credit
    now needs a name or an id. Verified: miss on the first start, hit on the second.
  - Every catalog request carrying `platform=web` has been answered with HTTP 400
    ("Platform web is not supported") since 2026-11-11, so regional display names and
    catalog song enrichment failed and were repeated for every list page (over a thousand
    logged failures). `applemusic::Apple` retries a refused catalog request once without
    the parameter and then leaves it out for the run; a 400 is also recorded as a definite
    absence. Verified: no such failure logged after the fix. This is an observation of the
    service through the local bridge; the owner may want to confirm it against the official
    client.
  - A re-read replaces the rows on screen only when it reaches as far and its related
    records have arrived, and is written to disk only then, so album tiles no longer empty
    out during a refresh and closing mid-refresh cannot shorten the persisted list.
  - A cache read no longer overtakes queued writes of the same list (caught by
    `test-list-cache.ps1`).
- Also done: the queue checkpoint stores a count and 64-bit digest instead of the hex of
  every path (checkpoint version 3); `bridge.json` is parsed once per change; album-track and
  all-favorites pages borrow the session state instead of copying it; the log file stays
  open; the per-cover log lines are gone.
- Tests run after these changes, all passing: `test-cache-store` (745 checks),
  `test-list-cache` (99), `test-image-cache`, `test-list-session` (30 cases),
  `test-playback-scheduler` (18 cases). Not run: the Apple Music provider/startup probes,
  audio cache and prefetch tests.
- Third pass, driven by a request log (`apple GET ... -> status (ms)` lines are now written
  for every Apple Music request). Requests at start with one playlist open: about 80 before,
  17 on a repeated start after.
  - The album count of every library artist was requested again after every library
    refresh (one or two requests per artist). Counts are now kept for a week in the cache
    store and only unknown ones are asked for.
  - Every library refresh dropped the favorite songs and downloaded the whole Favorite
    Songs playlist again (all pages, related records, regional names) to draw the stars;
    every star click did the same to confirm the write. The ids are now kept across
    refreshes and runs (re-read after six hours) and a successful write updates them in
    place.
  - The account notification restarted list reads that were already in flight, sending the
    open list's first page twice. `stream::ResumeLists` starts only the reads that waited.
  - An automatic re-read of a complete list stops at its first page when the total and the
    first page's rows are the ones on screen (any source; a source revision is still exact,
    a refresh the owner asks for reads everything).
  - Lyrics: a track with cached lyrics, an owner selection or a recent "nothing found" is
    answered at once without a search and without the "searching" state; before, every
    play queried every source again even on a cache hit, and "nothing found" was never
    written. Asking for the same track while its search runs joins that search.
- Tests after the third pass, all passing: `test-list-session` (30), `test-list-cache` (99),
  `test-lyrics*` (19 + 8 + HTTP), `test-applemusic-provider` against its mock (favorite and
  playlist writes, regional names, list paging). The mock now answers the lyrics route and
  the test no longer requires artist album requests on every run. Not run: audio cache,
  prefetch and Apple startup tests.
- Fourth pass (the items left open above, and what the request log showed next):
  - A list cached only in part resumes: an unchanged first page keeps the rows and further
    pages continue from the persisted cursor into the loaded snapshot
    (`cache::lists::Record::base`) instead of re-reading from page one.
  - Rows that will be read anyway are asked for up to 1000 per page; NetEase playlist and
    favorites pages fetch their song details in parallel batches of 200 (offset-paged NetEase
    lists stay at 100, Apple Music follows its own `next` links at 100).
  - HTTP helpers moved to `net/` (`glasswave::net`).
  - Apple Music: opening a catalog album or catalog playlist failed with 400 ("Limit may not
    be supplied on this request", confirmed through the bridge); the resource request no longer
    carries `limit`. An artist whose albums span two pages failed its count because the
    service's `next` link writes the storefront in lower case; paths now compare without case.
    An artist the catalog does not know (404) is remembered for the week instead of being
    asked at every start.
  - Album wall: the covers are the playing playlist's albums, each once, dealt like shuffled
    cards with Media Center's coloured no-cover tiles making up small decks; the same picture
    is kept off neighbouring places. Covers are painted only while shown. Details in
    `docs/design/player-window.md`.
  - Tests: `test-list-cache` (106, with a new continuation case), `test-list-session` (30),
    `test-lyrics` (HTTP), `test-applemusic-provider` against its mock (which now refuses
    `limit` on catalog resource requests, as the service does). Built with zero warnings.
- Fifth pass (owner report: covers and artist photos late after a restart, genre/composer
  lists not cached), measured with temporary per-cover timing in the dev instance:
  - Cached covers read in 0-1 ms and decode in 4-8 ms (a 540 KB 1200 px JPEG included); the
    size of the pictures is not the delay. At start the Apple bridge needs 5-7 s, and every
    uncached cover waited for it while holding one of the three cover workers, so covers
    already on disk queued behind them and then appeared all at once. `ui::ArtCache` now
    has a local lane that answers only from disk (`cache::CacheOnlyScope`) and a network
    lane for misses. Measured after: 60 of 64 startup covers at once, 4 misses on the
    network lane.
  - Two library albums without artwork were asked for at every start (absence was kept
    5 minutes and ignored after a restart); now a day, across restarts.
  - Apple genres and composers (and a genre's songs) downloaded the whole library song list
    on every open and were not kept on disk. Their lists are persisted now, and the derived
    views share one copy of the library songs read within the library window (5 min).
  - Album wall: coverless albums leave the deck; loaded covers are dealt first; the next two
    screens of cards load ahead.
  - Tests: `test-image-cache` (absence case updated), `test-applemusic-provider` (mock).
  - Not changed: Apple track lists whose details were still loading when the program closed
    come back undescribed, and the album-tile view waits for their details again (seen in
    the log mostly after the forced restarts of testing).
- Sixth pass (2026-11-14, owner reports):
  - Ghost over the pane after leaving the mini player: captured live (Windows.Graphics.Capture,
    the window untouched). No stray window; the pane's header held a magnified slice of the
    cover, and its header and switch were painted for the player's width (790 px) while the
    pane had the main window's (881 px). The pane decided to repaint by comparing new bounds
    with its last bounds, and a header whose draw failed kept its old surface. Now it repaints
    whenever what was painted differs from its bounds (retrying for a while after a failed
    draw), an undrawn header shows nothing, surfaces whose clear failed are cleared again
    later instead of being given up, and surface failures go to the log file. Where the cover
    pixels came from is not proven; the next occurrence will leave `surface:` lines.
  - Covers named by song in one run and by album in another (regional names skip restoring
    songs to the catalog) missed the disk cache and were fetched again: the catalog now keeps
    every song's album (also from restored lists), the UI and the extractor ask the same, and
    a cover lookup also tries the song's name in the cache.
  - "Apple Music <id>" titles: songs from restored lists are installed into the catalog (when
    unknown), and any song still unknown is read in batches and published to the metadata of
    every view (asked again after 2 minutes when a read fails).
  - Up Next: repeat-one keeps the list; repeat-all ends with "Repeats N songs"; motion as
    measured from the owner's iOS recording (see docs/design/lyrics-view.md).
  - Album wall: the entrance keeps the full screen's pace in the mini window.
  - Composer section: WMP's composer icon (wmploc 29515, as the local Composer category).
  - NetEase covers: 1200 px for views drawing larger than 500 px, 500 px elsewhere; the
    largest smaller picture already decoded stands in while the large one loads.
  - Tests: `test-playback-schedule` (123716), `test-queue` (144927), `test-applemusic-provider`.
  - Open, owner to confirm: Apple Music's Songs section. The service lists 222 library songs
    in 43 albums (72 in albums with no catalog match, 16 single-song albums); nothing in the
    data marks the 26 songs the app shows, so the section is unchanged.
- Owner checks for the fourth pass: open a catalog album and a catalog playlist in Apple
  Music; a long NetEase playlist (loads in fewer, larger steps); scroll a long list, restart,
  scroll further (continues without reloading the top); the album wall with a short and a long
  playing playlist.
- Owner checks: reopen lists after a restart (no "Loading..." for lists opened before);
  favorite a song, restart, reopen lists; covers of one album in list / queue / now playing;
  replay a cached NetEase and Apple Music song (starts without a wait, also offline);
  playlist wording per source; menus per source capability.

## Player window (2026-11-12)

- New: the mini player and the full screen (`ui/player_window.*`), Media Center's album
  wall (`ui/album_wall.*`) and its moving light (`ui/aero_light.*`); design and measurements
  in [docs/design/player-window.md](../design/player-window.md). The library window lends
  the player its lyrics / queue pane.
- Controls are the original ehres.dll / wmploc.dll artwork; `scripts/upscale-art.ps1 -Source
  ehres` makes their @2x versions (72 written in the dev profile).
- Release x64 builds with zero warnings. Checked in the dev instance by passive window
  captures only: mini (cover, lyrics, queue), full screen (with and without lyrics). Not
  checked: mouse interaction, the entrance and swell in motion, taskbar buttons, other DPIs,
  a profile without the Win7 files. Owner acceptance is open.
- Next: the owner's review of look and motion; WMC's idle layout; persisting the mini
  window's place; measuring the seek bar pieces.

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

The owner subsequently authorized only a delegated, read-only investigation of
white main-list residual surfaces covering the right sidebar. The worker reports
to `.local/tests/sidebar-residual-investigation-20261112.md`; no product edits,
builds, tests or runtime/UI actions are authorized for that investigation. The
coordinator is not waiting on its result. Startup sidebar blankness specifically
means streaming navigation is absent until account restoration completes; this
remains unconfirmed as fixed. List-opening latency is a separate issue. Memory
root-cause work remains paused; keep the existing watchdog and supervisor running.

The owner confirmed that horizontal-resize tint/tab misalignment is resolved;
close that issue. The residual-surface worker must use only the screenshot ending
`c4cf715f-6556-47ab-bc58-cff0f3c1a306.png`. The other two screenshots show the closed
resize issue and are excluded from investigation and causal reasoning.

The owner then manually confirmed streaming navigation now appears at startup;
close the left-sidebar blankness issue. The remaining cache symptom is precise:
after fully loading an Apple Music playlist, quitting and restarting restores
that same page, but tracks remain hidden behind loading until the network read
finishes. The cache worker is investigating only this last-page restoration path
read-only, without tests or runtime actions. Existing bootstrap/list-cache code
does not establish acceptance of this remaining symptom. Keep memory monitoring
active without resuming memory root-cause investigation.

Player window, 2026-11-12 (uncommitted, awaiting the owner's visual acceptance): back to
Media Center after the owner rejected the drawn "crystal" seek bar and volume. The seek
bar is the library window's; the volume is WMC's own overlay (`ui/wmc_volume.*`) full
screen and, in narrow mini windows, behind the speaker's arrow; wide mini windows get WMC's
bar as a slider with a level bubble. Speaker and arrow light as one button; the favourite
is a heart in WMC's control style; menu presses darken the type and bring white back from
the left (misc.mp4); the full-screen sung line is the last of the song's lines; the
full-screen pane lies on a WMC glass card and the mini pane on an edgeless gradient; the
mini frame has DWM's side highlights. The album wall is laid on the monitor: tiles fall
straight in at random places, a growing window fills by falling tiles, and the swell shows
only in the tiles' shadows (docs/design/player-window.md). Stills checked; hover, drag,
menu presses and all motion still need the owner's eyes. Open: the random ghosting in the
lyrics view. scripts/upscale-art.ps1 now defaults to waifu2x tile 64 (128 garbled the
outline on the RTX 5070 Ti); hd\wmploc was made before the straight-colour split and may
be worth regenerating.

2026-11-13 (uncommitted, built, not yet checked by the owner): favorites are optimistic for
every star and heart (`stream::Provider`, docs/architecture.md); the player keeps the previous
song or shows the one being opened while playback switches (provider inputs announce the
opening); Swap original and translation replaces "translations above" (texts change places,
translations get spread word timing); a sung line blurs while it rises; the full-screen
song lines scale with the cover when the chrome comes and goes; the card's rim takes a pale
navy; the sidebar light is mirrored, its switch, capsules and hover plates lie on iOS-like
glass and the header no longer dips while switching sides; a queue drag survives list
refreshes, and under shuffle a drop reorders only the playing order (it used to rewrite the
playlist and turn most of the future into history, emptying the list). The runtime test
`test-playback-scheduler.ps1` was not run (it needs the dev instance closed).

2026-11-13, later (uncommitted, built, not yet checked by the owner): the lyrics menu gained
ESLyric's tools (docs/design/lyrics-view.md, "The lyrics menu"): save / save to, select,
copy and Copy..., encoding, lyric info, adjust offset (per song, retimed in place) and reload.
Lyrics now carry their document, file and code page; manual selections remember the file.
