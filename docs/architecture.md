# Architecture

```
foobar2000.exe (core: decoding, playlists, library, DSP, output, components)
 └─ foo_ui_glasswave.dll  ── user_interface service (owns the main window)
     ├─ ui::MainWindow        Win32 window, WS_EX_NOREDIRECTIONBITMAP, custom non-client area
     │   ├─ layout (DIP → px), hit testing (resize edges, caption, Snap Layouts via HTMAXBUTTON)
     │   ├─ widgets           pre-painted layers; state = compositor opacity animations
     │   └─ core callbacks    play_callback, playlist_callback → repaint only what changed
     ├─ aero::GlassMaterial   HostBackdropBrush → ColorMatrix (Win7 recipe) + reflection
     ├─ aero::ShadowFrame     4 layered click-through windows with the measured Win7 shadow
     ├─ aero::paint           Direct2D painters for Win7 frame / caption / transport elements
     ├─ wmp::paint            Direct2D painters for WMP 12 client chrome (lists, nav, bars)
     ├─ ui::ListView          virtualized list over media::ListSource (header, rows, scrollbar)
     ├─ ui::NavPane           Now Playing, All Music, Local Music, one section per provider
     ├─ ui::Page              the asynchronous page on screen (provider pages, All Favorites)
     ├─ ui settings page      Preferences > Display > Glasswave (app/settings, settings::Field)
     ├─ ui::SearchBox         self-drawn search field (WM_CHAR + IME)
     ├─ media::LibraryModel   library snapshot: dedup, sort, artist/album/genre groups
     ├─ media::*Source        PlaylistSource / TrackSource / GroupSource behind the list
     ├─ ui::SongTableSource   song tables over local and provider tracks; tile grids for providers
     ├─ stream::Provider      a streaming provider (ADR 0009); netease::StreamProvider is the first
     └─ gfx::CompositionHost  DispatcherQueue, Compositor, D3D11/D2D devices, surfaces, animations
```

## Content model

- The list shows one `media::ListSource` at a time. Playlist sources keep selection and focus
  in the foobar2000 playlist itself (so foobar2000 shortcuts and context menus agree); library
  sources keep their own selection.
- Library views follow WMP: activating a track rebuilds the "Now Playing" playlist from the
  view and plays from that track; groups (artist/album/genre) drill down.
- `MainWindow` owns navigation history (`View` records) and rebuilds the source on
  navigation, library changes (debounced 400 ms) and playlist callbacks.

## Streaming providers (ADR 0009)

- `stream/` is the provider-neutral model and interface; nothing in `ui/` or `media/`
  includes a provider's own headers (only its sign-in dialog lives under `ui/`).
- A provider registers itself while the component loads and is started by the window (or
  by its `initquit` service under another UI). Its tracks are metadb items handled by its
  own input, so playlists, the queue and play history mix them with local files.
- Provider pages are `ui::Page` objects created by `MakeRemotePage`. The window keeps the
  one on screen; a page's callbacks are wrapped with `Page::Guard`, so results for a page
  that was left are dropped. Shared `stream::ListSession` leases retain bounded browsing
  state for Back/Forward and coalesce consumers of the same logical list.
- Provider notifications: account changes re-open the provider's page; library changes
  update the navigation lists and the page in place; favorite changes refresh the stars.
- Favorites are optimistic in `stream::Provider` itself, so every star and heart of the UI
  behaves alike: `SetFavorite` records the new state, notifies at once and runs the provider's
  `WriteFavorite` on its own queue (Apple Music's write queue), which pages, playback and list
  changes do not cancel. `IsFavorite` reads the pending state until the answer; the latest
  click on a song decides, an earlier write's answer is ignored. A failure drops the pending
  state and notifies again, so whatever still shows that song shows the service's (old) state;
  it is reported on the console, not in a dialog. An answer that never comes stops counting
  after two minutes.
- Providers supply stable navigation section ids, labels, kinds and child-ordering flags.
  The UI does not prescribe the number of playlist groups or a shared service vocabulary.
  Generic track/group pages support songs, genres and composers; section ordering is local,
  while playlist child dragging additionally requires the provider's reorder capability.
- **Capabilities.** `Provider::Offers()` returns a `stream::Capabilities` set of
  `stream::Capability` values; the UI asks `provider.Can(...)` and shows nothing a source
  did not declare. Reads and writes are separate enumerators and none implies another
  (favorites may be readable without being writable; appending to a playlist does not imply
  removing from it). A new ability is one more enumerator, a new source one list.
- **Wording.** The UI writes every source-facing string once, in the common English wording,
  and shows `provider.Text(english)`: the source's own English term (`Wording`, e.g. Apple
  Music maps "Collect album" to "Add to Library") translated in the source's context
  (`TextContext`, language-file keys `"<context> | <english>"`, e.g. 播放列表 for Apple Music
  where the common translation of "playlist" is 歌单), falling back to the common
  translation. A new term needs no interface change.
- **Covers.** `Provider::ArtKeyOf(item)` names the picture a track shows: its album's when
  the album is known, else its own. The provider's art extractor stores the image under the
  same identity, so the playing track, the queue, list rows, the album page and the grids
  share one download and one decode per size.
- **Profiles and details.** `stream::Artist`, `stream::Album` and `stream::Playlist` carry
  every detail of the music itself a page can show (artist: biography, tagline,
  born/formed, origin, area, kind, genres, aliases, roles, video count; album: release date,
  label, copyright, type, edition, UPC, genres, aliases, audio traits; playlist: tagline,
  type, created, updated, update frequency, tags, aliases). Social figures (followers, plays,
  saves, comments, shares) and the services' badges are left out on purpose (owner,
  2026-11-14). A source fills what it knows and leaves the rest empty; type, area and trait
  values are untranslated UI keys. `Artist::profile` marks the artist's own record (a list
  row only has a summary), so caches and merges never let a row replace a profile
  (`stream::CopyArtistProfile`); the list cache stores every field (page schema 4). Pages
  turn the fields into `DetailInfo::Fact`s, which the details band lays out like Windows
  7's details pane, a value cut short showing whole in a tooltip; a field the source lacks
  is not shown. Apple Music's profile attributes (`artistBio`, `bornOrFormed`, `origin`,
  `isGroup`, `audioTraits`) exist only on the web API surface; NetEase adds its artist wiki
  (`/ugc/artist/get`: area, type) to the artist detail.
- **Song pages.** A source with `Capability::SongDetails` answers `FetchSongDetails` with a
  `stream::SongDetails`: the song's details (position, release, genres, tags, language, BPM,
  version, audio, lyrics, ISRC, aliases), the credits in the service's own groups and the
  similar songs. The page (`View::Kind::RemoteSong`, "Song info" in a song's context menu)
  shows the details in the details band with Play, and the credits and similar songs as WMP
  12's details grouped under header lines (`SongInfoSource`). Apple Music reads the song and
  `/songs/{id}/credits` (both on the web API surface); NetEase reads `/song/detail` and the
  song wiki (`/song/wiki/summary`: style, tags, language, BPM, similar songs).
- **Checking pages with real data.** `foobar2000.exe /glasswave:open:<source>:<artist|album|playlist|song>:<id>`
  opens that page in the running window (`ui/dev_commands.cpp`), and the details band of the
  page on screen is written to the log when it changes ("page: ..."), so a page can be
  checked against the service without constructed data or screenshots.
- Deferred page requests use the same page lifetime guard as their responses. Account
  changes purge the provider's list sessions and saved navigation state; language and display
  changes only retire the in-memory reads, so the persisted snapshots still give the first
  paint. Localized group identities return to their group section after a language change.
- **Loading is shown once.** Anything that has been loaded before is shown from its cache at
  once and refreshed behind what is on screen: a list keeps its rows while a new generation
  catches up (the "Loading..." row appears only while there are no rows), a picture keeps its
  bitmap until the replacement is decoded, a cached song plays without asking the service
  first. Writes (favorite, playlist edits) call `NotifyListChanged`, which revalidates the
  provider's lists in place (`stream::RevalidateLists`) and never drops their snapshots.

## Shared provider cache (ADR 0014)

Shared provider caching lives in `cache/`: typed stable resource keys, opaque
account generations, a Windows SQLite WAL metadata index, immutable payload generations
and reader leases. Separate budgets cover encoded audio, raw images and typed list data.
Audio adapters reuse one progressive transfer; original bytes still reach core decoders.
See `docs/plan/shared-cache.md` for current acceptance status.

- **Store.** The index runs in WAL mode with `synchronous=NORMAL` (payloads are flushed
  before they are published; the index is disposable). A lookup does not write: recency is
  kept in memory and flushed with the next commit or trim. Byte totals per kind are kept in
  memory, so a commit does not rescan the index. `Store::Revalidate` records a 304 by
  updating validators only; payloads are never copied or hashed.
- **Images.** `cache::Images` downloads one identity once. A cached image is returned without
  any request while it is younger than its freshness window (7 days); after that, or after
  `Invalidate`, it is revalidated in the background with a conditional request and listeners
  are told only when the bytes differ. There is no per-process or periodic revalidation. A
  download is checked by its container structure and header, and decoded once, by its
  consumer.
- **Thumbnails.** `ui::ArtCache` holds one record per picture (source key) with its decoded
  sizes; a smaller request is served from a larger decode. Lookups are O(1) per paint. An
  old bitmap stays visible while its replacement loads; a failed provider picture is asked
  for again only after a growing pause (30 s up to 10 min), driven by painting.
- **Audio.** `cache::audio::RememberSource` keeps, per song and requested quality, what the
  service delivered last time (the fields that name the cache entry). An input first tries
  that entry with `Request::cachedOnly`; only a miss asks the service (NetEase: the song URL,
  Apple Music: a playback session). Trial clips are never remembered, and a source whose
  bytes the decoder rejects is forgotten.
- **Cover lanes.** `ui::ArtCache` decodes in two lanes. The local lane reads files and the
  image cache under `cache::CacheOnlyScope`, so it never waits on a service; a provider
  picture that is not cached moves to the network lane. A starting bridge or a slow fetch
  therefore never holds up covers already on disk. A cover the service confirmed absent is
  remembered for a day, across restarts.
- **HTTP.** `net::SendHttp` (`net/http.*`, used by every source, the image cache and lyric
  scripts) uses one WinHTTP session per proxy mode for the process, so requests to one host
  reuse a pooled connection instead of a new TLS handshake each.

The event-driven prefetch host primes one input within 30 seconds of track end, following
the explicit core queue or the shared Glasswave schedule and the core's skip filters.
It cancels stale candidates on playback, schedule, queue, account and encoding changes.
Formal playback can adopt the same encoded transfer. No idle timer is added.

## Paged lists and playback scheduling (ADRs 0015, 0016)

`stream/list.h` describes typed pages and opaque provider cursors. `ListSession` shares a
logical read between independently cancellable consumers. Default server order uses
OnDemand; explicit sorting, album expansion and local filtering use All. Status and data
versions are separate, so loading changes do not rebuild rows. Cached prefixes stay on
screen while a new contiguous generation catches up. `cache/lists` writes each page once
and atomically updates a manifest, under the independent list budget and account guard.

A list confirmed within the last five minutes is shown without a request, also right after a
restart when its persisted snapshot is complete. An older one re-reads behind its rows; when
the first page of that re-read carries the revision of the rows on screen (NetEase playlist
and favorites pages do), the re-read ends there instead of fetching every page again.
Reading the disk snapshot is not reported as loading; only a miss is.

A list read only in part (nobody scrolled to its end) does the same: an unchanged first page
keeps the rows, and further pages continue from the persisted cursor into the snapshot that
holds them (`cache::lists::Record::base` names the loaded snapshot's generation, so a later
run appends to it instead of starting from page one). The first page of a read is small;
rows that will be read anyway (a re-read catching up, a list wanted whole) are asked for up
to 1000 at a time, which NetEase serves with parallel 200-song detail requests. Apple Music
pages follow the service's own `next` links and stay at its maximum of 100.

Optional related metadata is fetched after baseline rows publish. A shared generation token
and occurrence-range validation prevent stale updates; two enrichment requests may be active
without blocking subsequent primary pages. Tables update the changed range in place, while
album-dependent views wait for a contiguous metadata-ready prefix. Completion without field
changes only advances readiness. Current cache formats are index v2 and list schema v3;
payload reads/commits do not calculate integrity hashes. Random immutable page tokens and
atomic manifests provide publication ownership without backward-compatible readers.

`media::PlaybackSchedule` is the occurrence-based model; `PlaybackScheduler` is its
main-thread SDK host. Canonical order, played history, unplayed random order, priority
blocks and the next repeat cycle have separate roles. The host projects this schedule
into the protected Now Playing playlist while core playback order remains Default.
Source playlists retain their original order. The same plan supplies Up Next and prefetch.
Shuffle-off preserves surviving canonical order and manual drag intent. Replace/Clear
changes the content generation, rejecting stale restoration and paged-source appends.
Checkpoint loading validates queue identity and complete occurrence roles before use.

Native order commands are imported into this scheduler, and the main Playback menu uses
its current modes directly. Explicit third-party SDK queue entries retain priority and
duplicate occurrences. See `docs/plan/paged-lists-and-scheduler.md` for acceptance evidence.

## Apple Music (ADR 0011)

- `applemusic/` implements the shared provider and `apple://song/<id>` input. The
  authenticated loopback helper owns account sessions and encoded media transport;
  `input_entry::g_open_for_decoding` delegates ALAC/AAC/MP3 decoding to foobar2000.
- A shared background transfer stages original encoded bytes and publishes a persistent
  cache generation only after successful completion.
  foobar2000's MP4 input reads sequentially before the transfer completes. A complete
  cache view supplies truthful size/random access when seeking reopens the decoder;
  seeking ahead can wait for transfer, while starting playback does not. Actual codec,
  known source rate/depth and full-song EOF remain checked without helper audio decoding.
- WSL was the initial proof host. The runtime now uses pinned Adam nightly Go source and
  libhybris-win on Windows, with a maintained adapter patch and without Electron or VLC.
  The default setup/build/start scripts and dev launch use native executables; WSL is
  not needed for ordinary operation. Source/artifact pins and manifests are under
  `scripts/applemusic/windows/` and the private runtime directory.
- The native account window accepts credentials and verification responses; passwords
  are never stored by Glasswave. Persistent runtime sessions remain in the isolated
  Windows runtime account directory. The component reads only the loopback endpoint/token from
  `<profile>/glasswave/applemusic/bridge.json`.
- The native RequestContext owns the current account and exposes its stable DSID together
  with native authorization data. The Go bridge consumes that account snapshot; it must not
  independently open Apple's internal account database through a second SQLite VFS. The
  component's shared SQLite cache remains an independent store for disposable derived data.
- An explicit `managed.runtimeDirectory` in that bridge opts into native process ownership.
  The component serializes startup on request workers, launches the engine suspended with
  an isolated child environment and an atomically assigned kill-on-close Windows Job Object, then waits
  for authenticated readiness before issuing the requested operation. The player owns the
  process handle, not a PID discovered by name. Quit cancels startup and closes its job;
  abnormal player exit also closes the job. An existing healthy bridge is borrowed and
  remains independent; external/fixture bridges without `managed` never launch a process.
- Provider features distinguish library reads from playlist create/append/remove/
  rename/delete capabilities. Catalog searches use the same song tables and queue.
- Apple library requests follow the UI language. Library playlist names take precedence
  over catalog names; `attributes.tags` containing `favorited` identifies the system
  Favorite Songs playlist, which is excluded from ordinary created/saved playlists.
- Real-account full-song 24/192 decoding and cached-stream seeking have been verified;
  the detailed acceptance evidence and remaining cases are in `docs/plan/apple-music.md`.
- Personal uploads retain their `i.*` library identity when no catalog match exists.
  The helper resolves their authenticated original asset, validates its actual encoding
  and transports MP3/AAC/ALAC without conversion. Library album/artist browsing does not
  require an online catalog identity.
- Optional Atmos uses byte-preserving EC-3 remux in the helper and a Cavern NativeAOT
  object renderer loaded inside the input component. Rendered PCM enters foobar2000's
  ordinary DSP/output chain. Settings distinguish stereo speakers, headphones, 5.1 and
  7.1. See `docs/adr/0013-atmos-object-rendering.md` for module/context lifetime and scope.

## Lyrics (ADR 0010)

- `lyrics/` is the model (`Lyrics`, `ParseLrc`) and `lyrics::Service`. Existing manual
  per-track selections remain unconditional overrides. Every automatic source participates
  in one ordered pipeline: `source` selects embedded lyrics for local files or exact-ID
  native lyrics for the current provider; `sidecar` independently selects an adjacent
  same-name file. General search plugins retain independent ranks. Automatic cache hits
  participate at their source's current rank and account scope. See ADR 0017 and
  `docs/lyrics-sources.md`. Searchers run concurrently, and results reach the window on
  the main thread. `ScriptHost` implements
  ESLyric's JavaScript contract with QuickJS, while `scripts` discovers and registers
  sources from the profile's ESLyric and Glasswave folders. Worker threads are cancelled
  and joined on shutdown. `ui::ShowLyricsSearch` provides manual search and preview.
- `ui::LyricsView` draws the pane with Direct2D through its own device context (effects
  and command lists persist across frames); `gfx::FrameClock` paces frames to the
  compositor clock while something moves. Behaviour: docs/design/lyrics-view.md.
- The pane (`ui::NowPlayingPane`) sits on the glass right of the client area, joined to it;
  a restored window grows by its width. It shows the header and either `ui::LyricsView` or
  `ui::QueueView` (history, the song card, Up Next), switched from its bottom bar.
- The queue stays foobar2000's active playlist, so files opened from outside land in it.

## Window

- Registered through `user_interface_factory` (`user_interface_v2`). The user selects
  Glasswave in foobar2000's UI module prompt; Default UI stays installed as a fallback.
- `WM_NCCALCSIZE` returns 0 so the whole window is client area; the Win7 frame, caption,
  caption buttons and resize borders are drawn and hit-tested by us. Caption buttons
  report `HTMINBUTTON/HTMAXBUTTON/HTCLOSE` so Windows 11 Snap Layouts still work.
- DWM attributes: `DWMWA_USE_HOSTBACKDROPBRUSH`, no corner rounding, no accent border,
  no system backdrop, NC rendering disabled (removes the Win11 shadow, see ShadowFrame).

## Glass (ADR 0002)

`Compositor.CreateHostBackdropBrush()` gives the blurred content behind *this* window.
`aero_colorization.h` folds the Win7 DWM formula
(`base*colorBalance + luma*afterglow*afterglowBalance + backdrop*blurBalance`) into one
5x4 matrix, applied by a D2D ColorMatrix effect inside a `CompositionEffectBrush`. The
effect description is implemented without Win2D (`gfx/color_matrix_effect.cpp`).
Inactive windows use a second brush with weaker tint. When "Transparency effects" is off
the material falls back to an opaque color computed from the same recipe.

Reflection streaks are painted once per monitor at quarter resolution and anchored to
screen coordinates; moving the window only changes an offset.

## Rendering and motion (ADR 0003)

- All painting is Direct2D/DirectWrite into `CompositionDrawingSurface`s, in DIP with
  hairlines snapped to whole device pixels (`floor(scale)`), so 1-DIP lines stay crisp.
- A widget owns normal / inactive / hover / pressed layers (+ optional halo). Interaction
  animates layer opacity in the compositor; nothing is repainted on hover.
- Resizing never repaints large surfaces: frame, client edge, bars and the transport
  capsule are nine-grid or horizontally stretched strips.
- Looping animations (WMC breathing halo) run only while hovered.

## Layout (WMP 12 library mode)

Glass caption (30 DIP) and 8-DIP frame; client area: navigation bar (29), command bar (28),
content, transport band (45) with the capsule control group centered on the play orb.
All numbers in `aero/aero_metrics.h`, sources in `docs/design/visual-spec.md`.

## foobar2000 integration

- Playback: `playback_control` (play/pause/stop/prev/next, volume in dB, mute).
- Shuffle/repeat map onto playback orders (`playlist_manager::playback_order_*`).
- Menus: `mainmenu_manager` builds the full File/Edit/View/Playback/Library/Help menu
  (right-click content, Alt/F10), so preferences, DSP and output stay reachable.
- Keyboard: `keyboard_shortcut_manager::on_keydown_auto`.
- Planned: hosting `ui_element` panels from other components (lyrics, visualizations) as
  child windows below/above the composition tree (ADR 0006).

## Threading

Everything UI runs on foobar2000's main thread. The DispatcherQueue is created (or reused)
on that thread; composition animations run on the compositor thread.
