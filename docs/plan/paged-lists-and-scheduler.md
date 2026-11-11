# Paged lists and predictable playback

Status: functional acceptance, 2026-11-11. Release R4 has zero warnings/errors; native
ListSession passes 30 cases (73 primary / 20 enrichment requests), scheduler 18, provider
baseline 50 and mutation reconciliation 62. Audio mapping and progressive cache reuse pass.
The remaining Favorites startup gate is tracked in `status.md`. Repeat only necessary
change-related tests. Visual and interaction acceptance remains manual, including `ALBUM_0`.

## Owner requirements

- Default server order loads streaming lists on demand, starting with the first page.
- Explicit sorting, local page search and expanded album tiles drain the complete logical
  list, publishing matching/sorted content after every page. A pinned top status reports
  loading; errors retain usable content and offer retry.
- Local page search works across the complete source regardless of its pagination support.
  Clearing search or reverting presentation releases full-list demand without discarding rows.
- Reusable list caching is optional per query/provider. Catalog searches default to no cache.
  Pagination and cache refresh must coexist without mixing old/new page generations.
- Replace native shuffle with one predictable schedule, shared by transport, menus, the
  Up Next pane, insertion commands and encoded-audio prefetch. Core decoding/output remain.
- Play Next is immediate priority; ordinary additions enter unplayed random positions.
- Turning shuffle off restores canonical order efficiently, including additions/removals.
  Replacing Now Playing invalidates restoration/feed state from the old source. Dragging
  Up Next is authoritative and preserves exact duplicate occurrences.
- Keep physical playlist projection independent of playback roles. Explicitly selecting a
  new current song must preserve user Play Next as the next track. Double-click and failed
  starts must not make rows jump with shuffle off or on. The current revision passed 17
  native behavior cases, but restoration failed; the corrected 18-case rerun is pending.
  Use one current schema with an explicit development-data upgrade, without compatibility readers.
- Metadata-region changes are display-only: preserve canonical raw metadata, cancel obsolete
  generations and coalesce visible-first work without lag or stale names until Refresh.
  The owner accepted Account -> Japan KAF -> 花譜; rapid China/Japan toggles remain open.

## Integration contracts

- `stream/list.h`: semantic `ListQuery`, provider-owned opaque `ListCursor`, typed `ListPage`.
  Cursor positions count source records, not records remaining after availability filtering.
  A request cancellation flag is transport-only and excluded from semantic identity.
- `Provider::FetchListPage` returns exactly one source page where supported. Legacy methods
  remain available through a one-final-page fallback. Explicit mutations call
  `NotifyListChanged`; metadata reads do not invalidate their own sessions.
- `stream::ListSession`: main-thread shared reads with independent consumer leases and
  OnDemand/All policies. Cached content stays visible until a fresh contiguous prefix
  reaches the displayed frontier. Request/data versions separate loading changes from data
  replacement. `View()` borrows the current prefix and `appendOffset` describes unchanged
  rows, allowing suffix-only conversion and batch sort/merge. Last-consumer cancellation
  prevents queued provider work and further draining.
- `cache/lists`: Data-budget chunks plus an atomic manifest. Each network page is written
  once rather than rewriting every earlier row. Schema/chain/generation/account checks
  reject corrupt, incomplete, mixed and late snapshots. List-only revocation leaves song
  and image generations alone.
- Remote pages consume sessions uniformly. Source identity includes duplicate occurrence
  ordinal; selection/focus/scroll anchors survive appends and replacements. A default
  unsorted Details activation starts the loaded prefix and feeds subsequent pages into
  Now Playing under a content-generation token. Sorted/filtered playback waits for the
  complete requested order.
- `media::PlaybackSchedule` owns canonical order, occurrence identity, priority blocks,
  unplayed order and repeat-cycle plans. `PlaybackScheduler` projects this into the owned
  protected Now Playing playlist; source/library playlists retain server order. Explicit
  SDK queue entries remain ahead of the physical future and are included in Up Next.
- Main UI owns native-order import; the scheduler owns SDK queue observation. Native
  shuffle is mapped to the custom scheduler, and own Default echoes are suppressed.

## Validation

- The November 11 Release build at 09:29:48 local reports zero warnings/errors. The accepted
  provider phase is recorded in `.local/tests/finalNov11-provider-summary.json`; native
  reruns and the controlled decode trace still need main's final evidence acceptance.
- Isolated DLL load/unload passes. Startup exposed and fixed UI-before-initquit service
  ordering; the scheduler is explicitly initialized before UI startup queries its mode.
- The standalone list-cache regression passes 77 checks; the storage/list suites pass 745
  checks in total.
- Pure playback schedule regression passes 123639 checks. A 100000-row model benchmark
  measured 0.39 ms to enable shuffle and 7.46 ms to disable it; a separate workload measured
  16 ms to append another 100000 rows and 9 ms for 50000 removals. The 108 ms toggle workload
  means 100000 toggles on a 64-row model. All timings exclude SDK projection and rendering.
- Native ListSession/page-source baseline passes all 29 cases / 69 primary requests / 20
  enrichment requests, including deferred demand, per-consumer cancellation, generation
  replacement, repeated continuations, bounded empty pages, metadata-error retry cursor
  atomicity, out-of-order enrichment and UI presentation. Real remote page/source classes
  verify progressive filtering, All/OnDemand transitions, sorted appends, cross-page album
  merging, selection/focus preservation and page-destruction cancellation. This is the
  preceding baseline; the current 29-case native rerun is still in progress.
- Existing queue/motion/source regressions pass 144927 checks; album grouping passes 22.
- The real API rejects projected library playlist/album detail requests. Library paging
  now starts directly at the tracks collection; catalog resources retain embedded primary
  pages. A focused mock checks exactly one first page and three opaque continuations,
  359 rows including three duplicate occurrences, and query-bound cursor rejection. Apple
  mock acceptance now passes baseline 50/50, mutation 61/61 and helper 29/29, with exactly
  20 empty 202/204 ACKs, two nonempty create-201 responses and zero unexpected calls/contract
  errors. Both scenarios restore Japan 1 -> 0 -> 1 and captured bridge bytes/ACL; this phase
  made no real-account writes. The earlier real 359-row list published its first 100 rows
  in 1883.98 ms and completed four pages in 6830.10 ms. All
  original real-library acceptance checks pass: 210 songs, eight sections, three playlist
  covers, three artist portraits, two uploaded-song covers and library-only artist
  navigation.
- The earlier native PlaybackScheduler integration passed all 13 cases and restored the
  original queue/state. It exercises actual SDK playlist/explicit-queue operations with silent
  start/current-location test leaves, including foreign duplicate entries, priority drag,
  cross-boundary drag, stale snapshots, replacement/feed generations, native mode import,
  repeat priority/cycle visibility and checkpoint-role restoration. This earlier matrix does
  not close the newly reported row jumps, failed-start bounce or Play Next regression.
- The first new native scheduler run passed all 17 behavior cases, but restore failed:
  empty active/playing SDK state (`SIZE_MAX`) was skipped. Restore code is corrected, and a
  new SDK-empty case makes the pending rerun 18 cases, after Colors six and ListSession 29.
  The authoritative post-upgrade owner backup is 196 rows, current=1, priority=0, Japan=1;
  final selective restoration follows all tests. Seventeen behavior passes are not a fully
  accepted/restored native run.
- Prefetch regression passes 43 policy checks and 11 native host checks after schedule
  listener integration. DLL load/unload passes; that completed Release build has zero
  warnings/errors.
- At the acceptance run, the deployed DLL matched the tested build, and the production
  bridge configuration and retained sign-in were restored. This documentation pass did not
  rebuild or launch the UI.

Evidence: `.local/tests/list-session-result.json`, `list-cache-run.log`,
`playback-schedule-run.log`, `schedule-large-benchmark.log`, `playback-scheduler-result.json`,
`applemusic-provider-performance.json`, `applemusic-library-paged-run.log`,
`prefetch-host-result.json` and `paging-scheduler-build.log`. Current provider phase:
`.local/tests/finalNov11-provider-summary.json` and `finalNov11-provider-build.log`.

## Limits and follow-up

- Related metadata no longer blocks baseline publication. Native acceptance covers failures,
  cancellation, account changes, out-of-order pages, duplicate occurrences and UI
  presentation. Apple mock coverage includes baseline-before-enrichment and skipping fully
  known records. Current persistent formats are index v2 and list schema v3; payload hashing
  and compatibility readers have been removed. Chunk tokens identify immutable publications.
  The 100-row page size stays.
- First-party artwork absence is persisted as a bounded, five-minute negative result.
  Fresh negatives coalesce across thumbnail sizes; first access after process restart
  revalidates in the background. Failed metadata requests are not stored as absence.
  Generic missing-cover painting and bounded negative caching are implemented, and all 11
  native image-cache cases pass. The owner reported gray right-side lyrics. The latest
  `pane_common` change reuses the existing `ALBUM_0` artwork and still awaits the owner's
  direct visual check.
- Historical latency investigation before asynchronous enrichment: the shared session
  requested 100 rows for every page, including the first. A saved 94-song Apple playlist
  therefore fit in one page. Read-only measurements returned 94 rows in 1.00--1.42 s; all
  rows omitted direct album/artist relationships despite the requested projections. At the
  time, the provider waited for fallback metadata hydration before publishing the page. One
  equivalent read measured 998 ms for the collection plus 885 ms for five 20-song hydration
  batches with concurrency four (1.894 s total). A 25-row request measured 0.74--1.18 s in
  the same run. These measurements explain service latency; they no longer describe the
  current publication barrier. Evidence:
  `.local/tests/saved-playlist-first-page-timing.json` and
  `.local/tests/saved-playlist-hydration-timing.json`. No page-size behavior was changed.
  A subsequent interleaved 50/100 comparison (three requests each against the same
  94-song list) averaged 1028.5 ms for 50 rows and 1229.5 ms for 94 rows requested at
  limit 100. Individual samples overlapped; this is a small transport sample, not an
  end-to-end guarantee. Evidence: `.local/tests/saved-playlist-50-vs100-timing.json`.
- Source-aware visual placement and interactive drag feel still need the owner's direct
  check; no mouse/keyboard automation was used. Native page-source tests cover behavior.
- The owner should check missing-cover transitions, the right-side lyrics pane after the
  `ALBUM_0` change, and paged-list visual placement after the next build.
- The owner manually confirmed Account -> Japan KAF -> 花譜. Metadata-region China/Japan
  lag/stale names until Refresh remain owner reports needing live checks. Display-only
  generation cancellation and visible-first coalescing are implemented with passing mock
  region contracts; these are metadata settings, not UI language.
- The scheduler's projection/playback-role separation and Play Next preservation have 17
  native behavior passes. Empty-state restoration and the complete 18-case rerun remain
  pending; owner double-click/failure interaction checks are separate.
- Provider identity/alias fixes for artist 400, song-row album 404 and multi-artist credits
  have accepted mock evidence; live navigation remains pending. Favorite Artists stays
  separate from all library artists; optional `inFavorites` reads and mock POST/DELETE/
  stale-read contracts pass, with no real-account writes in this phase.
- Album and playlist mutations must retain acknowledged success through Refresh errors and
  stale responses, using mutation epochs and separate eventual reconciliation. The owner
  reports successful remote album Remove followed by UI rollback/stale Saved state. The
  mutation-epoch fix passes the 61-operation mock scenario; owner/live acceptance remains
  pending. See [current status](status.md#owner-reports-and-pending-acceptance-since-2140-2026-11-10)
  for the source-authority report and the complete acceptance list.
- The automatic-playback memory fault is captured and attributed to our playback chain by
  the 85 GB -> 48 GB stop/restore result; the exact allocation path and original seek trigger
  remain unconfirmed. Resolve the repaired monitor via `.local/tests/memory-watch-live.json`.
  RetainedSurface, six Widget/two MainWindow stable layers and hidden/minimized paint
  coalescing are implemented. Four logical composition-policy passes and the earlier four
  FrameClock passes do not measure GPU allocations/refcounts or prove the 39 GB root fixed.
  The GPU idle preflight and 18-hook/3.297 s helper smoke pass; the real 147 decode plus six
  seeks is pending. Its 194 seconds is media duration decoded faster than real time in an
  allocator test, not a real-time playthrough or original 146-second repro. Continuous
  playback and screen-off remain owner checks. Details:
  [current status](status.md#graphics-lifetime-and-idle-policy-2026-11-11).
- Large ordinary track pages use suffix conversion and stable batch sort/merge. Grid,
  multi-album and combined-provider favorites retain the correct progressive replacement
  fallback; very large mixed lists still rebuild their displayed prefix per data page.
- NetEase adapter changes pass the full build and source checks; no live NetEase account
  mutation was performed. Provider APIs that expose no pagination return one final page.
- The consumed `scheduler-probe/request.claimed.json` marker was retained when automatic
  review blocked its cleanup without a detailed reason. Startup only consumes `request.json`,
  so this retained diagnostic marker cannot rerun the probe or affect normal playback.
