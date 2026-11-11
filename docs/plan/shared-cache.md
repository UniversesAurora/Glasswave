# Shared cache and responsiveness

Status: implementation and functional regression verified, 2026-11-10. The paused lyrics
output-switch visual check remains with the owner; no UI automation was used.

## Owner requirements

- Diagnose slow playlist loading with actual timings and remove unnecessary blocking work.
- Paused lyrics must retain their document, position and highlight through output-device
  reloads, playlist loading and navigation.
- Both provider root context menus offer sign-in/sign-out according to account state.
- One provider-neutral persistent cache first covers original encoded audio and all art.
  Songs remain progressively playable while downloading and are reusable on later plays.
- Images show last-known bytes first and revalidate in the background; no blanking or
  rollback to an older late result. Page/account lifetimes and concurrent writes are guarded.
- Reserve a typed data category so future network list caching can use the same store.
- Prefetch the actual next song before the current one ends, with correct queue, shuffle,
  repeat, cancellation and quality/account-change behavior. Do not advance playback state
  or invent a random next track to warm the cache.

## Chosen foundation

`cache/store` uses Windows' bundled SQLite API for atomic WAL metadata and immutable
generation payloads under `<profile>/glasswave/cache/v1`. No separately installed database
or service is required. Keys contain kind/provider/account generation/stable identity/
actual encoding or rendition version; signed URLs and authorization data are excluded.
Per-write monotonically increasing tickets reject late writes; persistent scope rotation
rejects old-account lookup and publication. Snapshot leases protect active readers.
Payload reads and commits do not calculate integrity hashes. Atomic publication and
generation ownership prevent partial files from publishing. Startup removes owned orphans
and can quarantine a corrupt index. Only the current storage format is supported.
Audio/image/data budgets are independent, with LRU eviction and bounded decoded thumbnails.

Default audio budget: 10240 MiB; images: 512 MiB; future data: 128 MiB.
Common preferences replace NetEase-only cover-cache controls. Shared audio and image
adapters keep transport and presentation independent. Decoder/output ownership stays
with foobar2000.

## Working validation

- Standalone store: 735 assertions pass, including ticket races, rotation/restart,
  damaged/missing/truncated payload, abort, LRU and pinned-reader clear/eviction behavior.
- Original 359-row playlist: primary 867 ms, duplicated relationship pagination 5791 ms,
  serial album hydration 23891 ms, full callback 30849 ms. The implementation removes
  duplicate first-page fetches, requests nested song relationships, bounds fallback hydration
  to four calls and separates detail queue work. A fresh real read now returns all 359 rows
  in 5199-6313 ms; subsequent reads take 0.5-0.6 ms. Instrumentation on one real run:
  primary 941 ms, remaining pages 4283 ms, hydration 0 ms, main-thread commit 18 ms.
  The 35/14-row lists took 766/1042 ms in the first cold sample.
- Provider regression: 19 operations against a loopback mock pass. The 359-row projection
  requires no hydration; the deliberately incomplete response uses 18 batches with maximum
  concurrency four. Duplicate rows and album/artist credits remain intact. Two concurrent
  callers share one request; explicit refresh causes one new request, not a stale callback.
- Native audio regression passes all six cases for ALAC 24-bit/192 kHz, AAC 48 kHz,
  FLAC 24-bit/48 kHz and MP3 48 kHz:
  first decode before producer EOF, cached second read without fetch, independent concurrent
  positions and cancellation, invalidation, scope rotation and final-reader staging cleanup.
  The concurrent case starts inside a prefetch scope, then opens formal playback: the same
  single producer is promoted to playback priority and remains alive after the prime releases.
- Native image regression passes ten cases: cached-first 304, changed 200, incorrect MIME,
  malformed HTML, truncation, no-store, request coalescing, invalidation during a request,
  late old scope and final shutdown. The last case also exercises three concurrent finish/wait
  rounds, in-flight shutdown, repeated shutdown and rejection of late calls.
- Real NetEase artwork can contain PNG bytes declared as JPG. Recognized encoded magic is
  authoritative; each format's structural completion check and WIC decode still apply.
  A read-only index audit confirms both providers publish images in the shared store.
- Lyrics request/document tests pass eight checks; prefetch policy passes 43 checks.
  Component load/unload succeeds in an isolated child process; Release builds warning-free.
- Actual library browsing passes with 210 songs, eight localized sections, 3/3 playlist
  covers, 3/3 portraits and 2/2 uploaded album covers through core/ArtCache/WIC.
- Actual Apple inputs pass complete decode, metadata and near-EOF seeking for ALAC, AAC
  and Atmos. Each controlled two-play run creates/releases two authorized sessions but
  performs exactly one media GET. Slow transport proves the first decode precedes EOF.
  Atmos stereo speakers, headphones, 5.1 and 7.1 outputs retain their expected channel masks.
- The native prefetch-host probe passes 11 callback scheduling/generation/cancellation
  checks without touching playback, output or playlists. Its snapshot and prime I/O leaves
  are injected only after a dev-profile marker is claimed; they are restored afterwards.
- Ordered/repeat/core-queue next-track preparation is connected to core callbacks. The
  public SDK has no read-only random-next or early-random-selection API. Without an explicit
  queue, Random/Shuffle skips proactive preparation and retains native core behavior.

## Deployment and limits

- Release builds with zero warnings/errors and the isolated DLL load/unload check passes.
- The native lifecycle regression passes cold start, retained account on restart, normal
  exit and forced-parent Job cleanup. The final dev instance is restored with its real account.
- The paused output-switch visual behavior needs direct owner verification. Stable logical
  track identity, request generations and equal-document retention are implemented and tested;
  this does not claim a pixel-level UI test.
- Network list snapshots remain future work. The typed Data category supplies their storage
  foundation; this change does not persist every provider list.

Private sanitized evidence lives in `.local/tests/`: `audio-cache-*-result.json`,
`applemusic-*-cache-reuse.json`, `applemusic-provider-performance.json`,
`applemusic-shared-cache-browse-result.json`, `prefetch-host-result.json`,
`imagecache-*/result.json` and `cache-lifecycle-run.log`.
