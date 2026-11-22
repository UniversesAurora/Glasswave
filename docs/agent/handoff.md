# Handoff for the next coding agent

Written 2026-11-22 when the owner moved the work to another agent. Read this after
[AGENTS.md](../../AGENTS.md); together they are enough to start. Keep this file current or
fold it into `docs/plan/status.md` once it is stale.

## Read first, in this order

1. `AGENTS.md`: non-negotiables, workflow, conventions (binding).
2. `docs/plan/status.md`: the top sections are the latest work and its open points.
3. `docs/plan/review-2026-11-21.md`: the full code review's work list (62 of 63 items done).
4. `docs/architecture.md`, then the ADRs in `docs/adr/` before any substantial change.
5. `docs/agent/commit-style.md` before the first commit.

## How the owner works (rules not spelled out in AGENTS.md)

- Talk to the owner in Simplified Chinese; everything in the repo is English.
- Fix detailed, uncontroversial findings directly; ask only real owner decisions
  (behaviour, design trade-offs, privacy). Verify a finding is real before fixing it.
- The bar is the most elegant, thorough, best-practice fix at the right layer, not the
  smallest one ("不要得过且过"). A generic mechanism beats a per-source special case.
- One commit per logical change, Linux-kernel style, without waiting for owner checks.
  Intermediate commits need not compile: build once when the whole task is done, then fix
  build errors in follow-up commits. Never push; rebase / fast-forward only.
- Edit source with editing tools; scripts only for genuine bulk edits.
- No screenshots or UI automation unless the owner asks. Build, start the dev instance
  (`scripts/run-dev.ps1`) and give the owner a short check list; they test in the app.
- Do **not** add mock-service tests (all mock servers and mock probes were removed at the
  owner's request on 2026-11-22: they go stale on every client change and risk polluting
  dev account state). Keep standalone unit tests and real-account probes instead.
- Never print the Apple bridge token, the NetEase cookie or API keys. Ask before downloads.
- Use only `.local/fb2k-dev`. From Git Bash pass foobar2000 switches with
  `MSYS_NO_PATHCONV=1` (`/stop` is otherwise rewritten into a path).
- Delegating to subagents: only when the owner asks; never a swarm; review their diffs.

## Where things stand (HEAD f659786 and the docs commit after it)

- The full review (2026-11-21/22) landed about 140 commits: shared core and stream layers,
  file splits, UI helpers, scheduler split. Most were written without being run in
  foobar2000; the first real start on 2026-11-22 exposed two fatal bugs, both fixed:
  - 27e08c0: the component failed to load (`ERROR_DLL_INIT_FAILED`): providers were built
    by global initializers and the executor's `abort_callback_impl` asked foobar2000 for a
    service during DLL initialization. Providers now register an accessor and are made on
    the first `stream::Providers()`.
  - 5bd509a: a crash at start-up: `SnapshotSaver::Queue` copied its `Flight` guard and the
    temporary's destructor re-locked the same mutex.
- Latest feature: songs are read when asked for (`stream::Catalog::Want`, a reader each
  provider starts with itself). It fills records that lack album or artist ids (Apple Music
  station songs) and songs unknown after a restart; the now playing artist is a link only
  when it leads somewhere.
- f659786: an Atmos seek destroys the old renderer before rewinding the shared source
  (a resumed Atmos song failed with "The Apple Music Atmos renderer failed.").
- Passing: build (Release x64, warning-free incrementally), ListSession and
  PlaybackScheduler probes, lyrics, list cache and provider snapshot tests.
- The dev instance runs; Apple Music's cache scope was restored by hand to the owner's real
  one (`scopes` table of `profile/glasswave/cache/v1/index.sqlite3`; backup of the index in
  the previous agent's scratchpad, not needed unless something looks wrong).

## Waiting on the owner

- Confirm an Atmos song resumes after quitting mid-song (f659786, not yet reproduced).
- The check list at the end of the status's Phase 5/6 section and the read-through section:
  transport, shuffle/repeat (also stations), lyrics in both windows, window frames and caption
  buttons, list cells (rating, favourite, download), menus, scroll bars and flings, play from
  a row everywhere, recent lists and stations across a restart, CD playback, sign-out during
  writes, artist links from the now playing bar and the queue's context menu.
- Decisions resolved (2026-11-22):
  - Artist links without an artist id now fall back to catalog search by artist name in the
    Now Playing bar (`BuildArtistLinks(..., true)`).
  - Cache account scopes are now stable per `(provider, accountId)` (`cache::SelectAccountScope`,
    SQLite index schema v3) so switching accounts or re-signing in reuses cached covers,
    audio, snapshots and lists.

## Open technical points

- (Resolved 2026-11-22) Startup false `"no picture"` cover log spam on `CacheOnlyScope` misses
  was fixed via `cache::IsCacheOnly()` guarding `LogArtFailure` in `stream/art_extractor.h`.
- (Resolved 2026-11-22) Extracted `PlaybackScheduler`'s `g_probeEnabled`/`g_probeCurrentIndex`
  branches into a `PlaybackDriver` seam (`LivePlaybackDriver` vs `ProbePlaybackDriver`), closing
  item 63 of 63 from `docs/plan/review-2026-11-21.md`.
- A full LTCG rebuild warns C4702 four times in the SDK's `input_impl.h` (after
  `input_stubs::decode_run_raw`, a throw). Older than the review; pragmas, a per-file
  `/wd4702` and an out-of-line stub did not silence it. Incremental builds do not show it.
- Cache formats changed during the review (Apple songs v7, NetEase songs v1, audio source
  memo v2) without converting the dev data; the records refill as they are used. AGENTS.md
  requires converting or removing affected dev data in the same change: do that next time.

## Where to look when something breaks

- Component log: `.local/fb2k-dev/profile/glasswave/glasswave.log` (older entries are in
  `profile/glasswave/netease/glasswave-netease.log`).
- Apple playback events: `profile/glasswave/applemusic/playback.log`.
- Crashes: `profile/crash reports/failure_*.txt` (symbolised stack, look for `glasswave::`).
- A DLL that will not load: load `shared.dll` and then the DLL with
  `[System.Runtime.InteropServices.NativeLibrary]::Load` in pwsh; the error names the cause.
- Bisecting across checkouts: delete `build/obj/foo_ui_glasswave/x64/Release/*.ipdb` and
  `*.iobj` before each build, or LTCG fails with C1354 (corrupt ipdb).
- In-process probes need the instance started from outside while the script waits
  (`scripts/test-playback-scheduler.ps1`); if foobar2000 shows "New user interface
  module(s) found", the owner must answer it (it blocks start-up).
