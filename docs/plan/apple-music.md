# Apple Music implementation and acceptance plan

Status: the native Windows helper, retained owner authentication, real native 24/48, 24/96
and 24/192 ALAC, progressive encoded delivery, native library browsing, playlist details
and artwork have current passing evidence. Native restart and the final production repro
also pass. Real AAC-only playback, real Apple writes and release licensing remain pending.
The shared library-only artist fallback is implemented for every unmapped artist; one real
target sample passes, without claiming that all 25 were sampled. Moon key-rotation repair
has a real post-fix full-song pass. Uploaded MP3 and AAC samples now pass full-length
playback with seek; a separate read-only audit verified MZ plus bounded HTTP Range
headers/containers for all 66 unique no-catalog uploads without claiming full PCM for all
66. Atmos JOC rendering now passes the final decoded-buffer-drain build for stereo,
headphones, 5.1 and 7.1 core layouts, including slow-EOF and pre-EOF cancellation.
Current November 11 integration evidence: Release R4 has zero warnings/errors; provider
baseline 50, mutation 62, native ListSession 30 and scheduler 18 cases pass. The six-case
ALAC/AAC mapping and cleanup matrix passes. Reproducible uploaded-AAC progressive playback
and cache reuse pass, including six seeks and first PCM before the held tail is released.
Favorites bootstrap readiness and the corrected suggestion matcher remain the scoped
integration gate; see [current status](status.md). Do not repeat accepted unrelated tests.
The owner manually confirmed Account -> Japan KAF -> 花譜; live interaction acceptance remains separate.

Last updated: 2026-11-11.

Architecture: [ADR 0011](../adr/0011-apple-music.md) and
[ADR 0012](../adr/0012-native-apple-music-streaming.md).

Research and evidence: [Apple Music research](../research/apple-music.md).

## Current owner reports and acceptance gaps (since 21:40, 2026-11-10)

Dispositions updated for the accepted November 11 implementation/mock phase:

- Colors Of Happiness (Yui Makino) was catalog `1237064886` and now reports `1865440142` /
  library `i.ZOMrromt4P51o6r`, ISRC `JPTE01701820`, ALAC 16/44.1. Authoritative mapping and
  early-session RAII cleanup pass the six-case native fixture matrix. This does not replace
  owner listening/navigation acceptance for this real track.
- Artist pages return 400, song-row album links return 404, and multi-artist credits can be
  romanized or lack links in the owner's report. Authoritative identity/alias fixes have
  accepted provider/mock evidence; live navigation acceptance is still pending.
- China/Japan selects metadata presentation, not UI language. The implemented display-only
  revision preserves canonical raw metadata, cancels obsolete generations and coalesces
  visible-first work. Mock region tests pass; reported lag/stale names need owner checks.
- Physical scheduler projection is independent of playback roles and preserves user Play
  Next after explicit selection, with one current schema and an explicit development upgrade.
  All 18 native cases pass, including restoration of empty active/playing SDK state
  (`SIZE_MAX`). The restored owner queue remains separate from fixture playback state.
- Favorite Artists stays separate from all library artists. Official optional `inFavorites`
  reads and mock POST/DELETE/stale-read behavior are validated. Separate-page and real-account
  acceptance remains pending; this phase performed no real favorite-artist writes.
- Album actions must read Add to Library / Remove from Library. The owner reports remote
  Remove success followed by Refresh error and UI rollback/stale Saved state. Zeno/Pascal's
  generic mutation epoch preserves acknowledged state through later reconciliation in the
  accepted 62-operation mock scenario, including playlists. The owner-facing path remains
  subject to live acceptance.

Evidence: `.local/tests/finalNov11-provider-summary.json` records exactly 20 empty-body
202/204 acknowledgments, two nonempty create-201 responses, zero unexpected calls/contract
errors, Japan 1 -> 0 -> 1 and captured bridge bytes/ACL restoration in both scenarios.
No real-account writes were performed. The authoritative owner backup is
`.local/tests/playback-probe-state-20261111T131819599Z`, captured after the owner's latest
use before R7 deployment. It supersedes all earlier captures. The live profile remains
authoritative; capture fresh state before future probes. A necessary restoration must
touch only Now Playing files and the scheduler checkpoint, never the whole configuration
database.

The captured playback-chain memory fault remains unresolved: stopping playback restored
about 85 GB to 48 GB global commit, with FOOBAR Shared at 39.646 GB. The exact allocation
path and original seek trigger are unconfirmed. The repaired watchdog's current pointer is
`.local/tests/memory-watch-live.json`; its protection checks and a stable 235-second Frida
baseline do not establish a root fix. The graphics retained-surface implementation is
accepted with four logical policy passes; the earlier FrameClock occluded-spin fix has
four passes and no new source change. These do not measure actual GPU allocations/refcounts
or explain the 39 GB fault. GPU idle preflight and an 18-hook/3.297 s helper smoke pass;
the real 147 decode plus six-seek trace passed. Its 194-second media was decoded
faster than real time as a controlled allocator test, not a real-time playthrough or the
original 146-second repro. The owner now requests continuous monitoring/protection; further
allocator diagnosis waits for another captured fault. Continuous playback, screen-off and
right-side lyrics `ALBUM_0`
remain owner checks, without proactive UI automation/screenshots. See
[current status](status.md#graphics-lifetime-and-idle-policy-2026-11-11).

## Acceptance rule

An Apple Music capability is complete only when:

1. the owner-account operation succeeds against Apple;
2. the intended complete encoded song, not a preview, reaches foobar2000;
3. foobar2000 owns decode, DSP and output;
4. the helper does not decode to PCM, transcode or expose false metadata;
5. failures are explicit and never silently change codec, account or operation;
6. restart, cache, cancellation and cleanup behavior is covered where relevant;
7. no credential, user token or native session material is recorded.

A source review, build, synthetic fixture or successful library read does not satisfy a
real write or real AAC-only row.

Current build provenance pins source revisions and toolchain versions in
`scripts/applemusic/windows/source-pins.json`, applies the reviewed patch files, runs Go
tests and verifies the PE stack headers. The runtime manifest records only schema, platform
and generation time. The current runtime manifest is schema 2; shared-store/list formats are
index schema v2 and list schema v3. Patch, artifact, deployment and cache-payload digests are
not a current acceptance requirement. Functional identity, image-change and PCM hashes remain
valid test evidence. Development-format changes update or delete the affected derived data
without adding compatibility readers.

## Evidence snapshot

| Area | Current evidence | State |
| --- | --- | --- |
| Native platform | Windows `engine.exe` serves `127.0.0.1:20025`; WSL PID `33462` stopped | Verified |
| Default setup | Windows scripts prepare the bridge; component owns startup/readiness/Job cleanup on direct player launch | Verified |
| Upstream provenance | Adam `72d835ebf4c73f3b5f8b4e2b2496a2042a4a214c`; libhybris-win `606bae7cdb49eaf8be79828de6cda0ce9d414295` | Pinned |
| Build isolation | Pinned source + reviewed patches -> DRM/libhybris -> Go tests -> engine -> PE stack edit -> runtime manifest | Verified |
| Build validation | Pinned revisions/tool versions, Go tests and 64 MiB/2 MiB PE headers pass; schema-2 manifest records platform/generation only | Verified |
| Historical runtime identity | Engine SHA-256 `5c1220c3…61af94ed`; patch SHA-256 `72690913…53ab57`; PID is per player lifetime | Historical measurement |
| Stack safety | 64 MiB reserve / 2 MiB commit fixed the guest 125 KiB-frame guard fault without patching Apple binaries | Verified |
| Authentication | Restart to PID `62296` kept `signedIn`; no owner Developer key needed | Verified |
| Token acquisition | Apple appcache JWT cached to expiry/known JWT expiry; singleflight; 401 invalidates and retries once | Verified by code/tests |
| Native 24/192 | Real ALAC 75140520 frames, 391.356875 s, full decode and seek | Verified |
| Native 24/96 | Fresh asset `1624173757`, 17880126 frames, 186.2513125 s, full decode and seek | Verified |
| Native 24/48 | Track `1625058484`, 9514880 frames, 198.2266666667 s decoded, 198.227 declared, full decode and seek | Verified |
| Native 16/44.1 | `39-fixed-result`, 9771384 frames, 221.5733333 s decoded, 221.573 declared, full decode and seek | Verified |
| Final native 24/192 | `.local/tests/applemusic-final-192-result.json` on accepted engine; 75140520 frames, 391.356875 s, seek pass, first `4966.673 ms` | Verified |
| Progressive ALAC | Multi-`moof` 24/192 with delayed final byte starts decode before EOS | Fixture pass |
| Progressive AAC | AAC 48 with delayed final byte starts decode before EOS | Fixture pass |
| Cancellation | Cancel after first chunk shuts down in 109.7952 ms; no external PCM decode | Fixture pass |
| Metadata | Exact fixture title/artist survives decoder `DynamicInfo` | Verified |
| Moon key rotation | Pre-fix failure after 29.907 s; updated helper strict repro reaches EOF with ALAC 16/44100, seek pass | Verified |
| Playback diagnostics | Persistent private bounded log covers cache, codec, seek and session teardown | Implemented |
| Duplicate albums | Shared UI album-key fallback grouping passes 22 checks | Targeted pass |
| Album relationships | Direct hydration: library 210/210, top 10/10, search 10/10, playlist 35/35 | Direct API pass |
| Real library reads | 8 Sections, 3 Created, 3 Saved, 196 Favorites, 210 Songs, 6 Genres, 11 Composers | Verified |
| Playlist details | Created counts 35/14/359 match; cached reads 2.821/0.0475/0.5913 ms | Verified |
| Cached shelf | Full snapshot/order match; callback 0.0496 ms | Verified |
| Artwork | 3/3 playlist covers and 3/3 sampled avatars decode through WIC | Verified |
| Art retry | `Missing` only retries four provider art kinds per library event; account/language fully invalidate; editorial art parity | Implemented |
| Artist navigation | Earlier 45-artist/library sample, public 19/19 albums and 10 hot songs, library-only 1/1; identity/alias fixes now have accepted mock evidence | Live 400/404/multi-artist checks pending |
| Favorite Artists | Official optional `inFavorites` reads and mock POST/DELETE/stale-read checks pass; separate from library membership | Page/real-account acceptance pending |
| Metadata region | Owner KAF -> 花譜 example accepted; both mock scenarios restore Japan 1 -> 0 -> 1 | Mock verified; rapid live toggles pending |
| Provider contract | Baseline 50/50, mutation 61/61, helper 29/29; 20 empty 202/204 ACKs + two nonempty create-201 responses; zero unexpected calls/contract errors | November 11 phase accepted |
| Real Apple writes | Owner reports acknowledged album removal followed by Refresh/UI reconciliation failure | No end-to-end write acceptance |
| Real AAC-only | No complete real AAC-only result | Pending |
| Uploaded MP3 | `i.1YBN0qVHqJaz4dR` カラー, 44.1 kHz, 10857600 samples, 246.2040816327 s, EOF and seek | Verified |
| Uploaded AAC | `i.1YBN07rCqJaz4dR` あいぞめ piano version, 44.1 kHz, 5459032 samples, 123.7875736961 s, EOF and seek | Verified |
| Uploaded candidate set | First 100 rows found 16 as a subset; all 210 rows produced 66 unique no-catalog sources. MZ + bounded Range header/container audit passed 66/66: 26 MP3 and 40 AAC, 0 ALAC, 0 failures. Full PCM/seek independently passed for two real samples | Header audit and sampled decode verified |
| Uploaded artwork | C++ blob-store fix + WIC decoded 2/2 at 1120x1120 and 514x514; URI validator 35 security tests pass | Verified |
| Atmos renderer identity | Historical DLL SHA-256 `5b2ab3e80d3ecc1621eec887fdff53084d908f45a7be4d8f8a4f9344bb1c515a`; decoded-buffer drain and headphone headroom applied | Historical measurement; functional cases verified below |
| Atmos stereo speakers | 2ch PCM32 mask `3`, 15 objects, 8,962,560 frames/186.720 s, full/seek pass | Verified |
| Atmos headphones | 2ch PCM32 mask `3`, 15 objects, 8,962,560 frames/186.720 s, full/seek pass | Verified |
| Atmos 5.1 | 6ch PCM32 mask `1551`, 15 objects, 8,962,560 frames/186.720 s, full/seek pass | Verified |
| Atmos 7.1 | 8ch PCM32 mask `1599`, 15 objects, 8,962,560 frames/186.720 s, full/seek pass | Verified |
| Atmos slow/cancel | 144,384 frames/3.008 s before 10 s EOF; cancel at 1,024 frames, shutdown 80.706 ms, DELETE pass | Verified |
| Atmos transport | Byte-exact 17925120-byte EC-3 copy-remux comparison | Verified |
| Runtime licensing | Apple runtime redistribution review incomplete | Pending |

Machine-readable artifacts:

- `.local/tests/finalNov11-provider-summary.json` (current provider phase);
- `.local/tests/finalNov11-provider-build.log` (09:29:48 local Release, zero warnings/errors);
- `.local/tests/applemusic-native-windows-96-result.json`;
- `.local/tests/applemusic-final-192-result.json`;
- `.local/tests/applemusic-progressive-alac-result.json`;
- `.local/tests/applemusic-progressive-aac-result.json`;
- `.local/tests/applemusic-progressive-cancel-result.json`;
- `.local/tests/applemusic-final-library-result.json`;
- `.local/tests/applemusic-uploaded-source-audit.json`;
- `.local/tests/applemusic-uploaded-artwork-result.json`;
- `.local/tests/applemusic-atmos-stereospeakers-core-result.json`;
- `.local/tests/applemusic-atmos-headphones-core-result.json`;
- `.local/tests/applemusic-atmos-speakers51-core-result.json`;
- `.local/tests/applemusic-atmos-speakers71-core-result.json`;
- `.local/tests/applemusic-atmos-progressive-result.json`;
- `.local/tests/applemusic-atmos-slow-timing.json`;
- `.local/tests/applemusic-atmos-cancel-result.json`;
- `.local/tests/applemusic-atmos-cancel-timing.json`;
- `.local/tests/applemusic-atmos-transport-result.json`;
- `.local/tests/applemusic-provider-contract-result.json`;
- `.local/tests/applemusic-provider-contract-state.json`;
- `.local/applemusic/windows/runtime/manifest.json`.

## Stage 0: pinned native provenance

Completed:

- [x] Pin Adam nightly `72d835ebf4c73f3b5f8b4e2b2496a2042a4a214c`.
- [x] Pin libhybris-win `606bae7cdb49eaf8be79828de6cda0ce9d414295`.
- [x] Record the reviewed Adam and libhybris patch paths in
  [`source-pins.json`](../../scripts/applemusic/windows/source-pins.json).
- [x] Pin Go `1.26.0`, MinGW `16.1.0`, LLVM `23.1.3` and `editbin`.
- [x] Build in isolated worktrees rather than the source checkouts.
- [x] Run Go tests before the native engine build.
- [x] Apply PE reserve/commit `64 MiB`/`2 MiB` and verify both headers.
- [x] Promote only the tested artifact set.
- [x] Check pinned revisions and tool versions before building; rely on Go/PCM tests and PE
  headers instead of ordinary artifact hashes.
- [x] Write the schema-2 runtime manifest with platform and generation time only.

Pending:

- [ ] Complete the redistribution/license matrix for Adam, libhybris-win, MinGW runtime
  DLLs and Apple guest libraries.
- [ ] Record final packaging disposition for every runtime artifact.

## Stage 1: Windows-native process and startup

Completed:

- [x] Stop using WSL as the normal helper host; old WSL PID `33462` is stopped.
- [x] Start the native helper from `scripts/setup-applemusic.ps1 -Action Start`.
- [x] Use `127.0.0.1:20025` as the default endpoint.
- [x] Auto-start the managed native helper from `scripts/run-dev.ps1`.
- [x] Skip auto-start when a fixture or explicit alternate bridge is configured.
- [x] Keep PID ownership checks, process path/start-time checks and private ACLs.
- [x] Keep native state, logs and temp files under the isolated runtime root.
- [x] Confirm Windows `run-dev` normal flow needs no running WSL distribution.

Pending:

- [ ] Exercise helper crash, stale PID, occupied port and forced restart on Windows.
- [ ] Verify cleanup after an active playback is interrupted by restart or shutdown.
- [ ] Confirm the managed helper does not outlive the dev instance when that is not
  explicitly selected.

## Stage 2: authentication and token lifecycle

The 2026-11-10 app-owned runtime regression also passes direct cold start, normal quit,
direct restart with preserved sign-in, forced-parent cleanup and externally started
service preservation. `scripts/test-applemusic-lifecycle.ps1 -IncludeExternalPreservation`
verifies exact executable/parent identity and actual process/listener exit, then restores
the dev player. Bridge configuration opts in through `managed.runtimeDirectory`;
startup belongs to the component, not the development launcher. The full library probe
still passes with 210 songs and zero acceptance failures after this change.

Completed:

- [x] Import the preserved native owner session.
- [x] Read the owner library after native startup.
- [x] Create full-song native playback sessions.
- [x] Restart the helper to PID `62296`; `signedIn` remained true.
- [x] Confirm no owner Apple Developer Program key is required on this path.
- [x] Cache the Apple appcache-derived JWT only to a safe expiry.
- [x] Coalesce concurrent token discovery into one request.
- [x] On HTTP `401`, invalidate once and retry with a refreshed JWT.
- [x] Avoid a homepage/script rerun for every Glasswave API call.
- [x] Keep the Music Assistant bundled credential absent.

Pending:

- [ ] Force JWT expiry and expired/revoked session behavior end to end.
- [ ] Verify account switching clears prior library, art, Favorite and audio state.
- [ ] Verify re-login and verification-code cancellation paths.
- [ ] Audit every log and error path for token/session redaction.

## Stage 3: full-song native encoded delivery

Completed:

- [x] Select the highest complete tested ALAC variant.
- [x] Deliver encoded ALAC rather than helper-decoded PCM.
- [x] Decode real native ALAC 24/192 for the complete 391.356875 seconds.
- [x] Decode real native ALAC 24/96 for the complete 186.2513125 seconds.
- [x] Decode real native ALAC 24/48 for 9514880 frames and 198.2266666667 seconds.
- [x] Decode additional real ALAC 16/44100 for 9771384 frames and 221.5733333 seconds.
- [x] Verify declared duration, sample frames, EOF and seek for all four native results.
- [x] Preserve exact fixture title/artist through raw decoder `DynamicInfo`.
- [x] Pass multi-`moof` ALAC 24/192 progressive delivery.
- [x] Pass AAC 48 progressive delivery with `bitDepth:null`.
- [x] Start foobar2000 decode before EOS in progressive fixtures.
- [x] Keep encoded bytes in a background cache and avoid a guessed final length.
- [x] Permit a seek to wait for a complete truthful cache view.
- [x] Cancel after the first chunk and close within approximately 110 ms in the fixture.
- [x] Add a persistent bounded private playback diagnostic without credentials.
- [x] Preserve the full Moon segment-to-key schedule in staged Go code.
- [x] Keep key-selection state per DRM connection across playback, seek and reconnect.
- [x] Propagate key-handshake errors and version the incompatible ALAC cache as v10.
- [x] Pass Go tests for delayed content, multiple key rotations, seek after transition,
  reconnect after rotation, reusable sources and unknown later keys.
- [x] Pass a real post-fix strict-cache repro: ALAC 16/44100 stereo, 14792704 frames,
  335.43546485 s decoded versus 335.435 declared, EOF and seek, first audio `9581.3081 ms`.
- [x] First read-only MZ probe found 16 no-catalog candidates in the first 100 rows:
  seven MP3 and nine AAC; this was a subset, not the account total.
- [x] Read all 210 Songs using page sizes `100, 25, 25, 25, 25, 10`; found 66 unique
  genuine no-catalog owner uploads with no duplicate IDs.
- [x] Complete the read-only MZ plus bounded HTTP Range header/container audit for 66/66
  sources: 26 MP3 and 40 AAC, 0 ALAC, 0 failures. The audit performed no full-song
  downloads.
- [x] Record the source split: 25 MP3 at 44.1 kHz, one MP3 at 48 kHz, and 40 AAC at
  44.1 kHz/256 kbps/stereo.
- [x] Keep coverage truthful: only the two real samples below passed independent full PCM
  and seek; the other 64 are not claimed as full-PCM tested.
- [x] Pass real uploaded MP3 `i.1YBN0qVHqJaz4dR` full-length with EOF and seek.
- [x] Pass real uploaded AAC `i.1YBN07rCqJaz4dR` full-length with EOF and seek.
- [x] Correct the synthetic MP3 fixture whitelist after an `unexpected_codec` false
  negative; the unchanged real input had already decoded full samples.

Pending:

- [x] Historical acceptance: pass final native 24/192 through PID `66528` and engine
  `5c1220c3…61af94ed`.
- [ ] Separately replay both originally failing Moon assets, if needed for per-track
  coverage; the shared-key repair already has one real full-song pass.
- [ ] Prove one real AAC-only complete song and that no complete ALAC existed.
- [ ] Verify network loss, truncated input, cache corruption and helper crash cleanup.
- [x] Verify pre-EOF cancellation during an actual paced transfer, including shutdown and
  expected DELETE cleanup.
- [ ] Verify preview rejection and short promotional asset rejection.

## Stage 4: quality and selection matrix

Completed:

- [x] Real ALAC 24/192 on native Windows.
- [x] Real ALAC 24/96 on native Windows.
- [x] Real ALAC 24/48 on native Windows with declared duration and seek.
- [x] Additional real ALAC 16/44.1 with declared duration and seek.
- [x] Progressive encoded ALAC 24/192 fixture.
- [x] Progressive encoded AAC 48 fixture.
- [x] Explicit `alac-max: 192000`, `get-m3u8-mode: hires` configuration.
- [x] Render real E-AC-3/JOC to stereo-speaker float PCM with 15 dynamic objects.
- [x] Render the same JOC source to headphones, 5.1 and 7.1 with correct channel
  counts/masks and seek.
- [x] Freeze final render counts at 8,962,560 frames/186.720 s for all four native
  layouts after decoded-buffer drain.
- [x] Pass slow EOF at exactly 144,384 frames/3.008 s with first audio at `152.7226 ms`
  before an actual ten-second transfer EOF.
- [x] Pass pre-EOF cancellation at 1,024 frames, first audio `152.9717 ms`, shutdown
  `80.706 ms`, and expected DELETE cleanup.

Pending:

- [ ] Additional ALAC 16/48, if available and useful.
- [ ] Real AAC-only complete song.
- [ ] Storefront-unavailable, ineligible subscription, DRM refusal and license failure.
- [ ] Quality changes do not mutate an in-flight playback session.
- [ ] Codec/rate/depth shown in the UI always comes from foobar2000's decoder.

Required behavior:

- ALAC failure never silently falls back to AAC.
- AAC is acceptable only when source metadata proves no complete ALAC exists.
- A lower-quality result is not used to mask preview, DRM or network failures.
- Atmos is an active owner requirement. The Cavern/Native-AOT JOC renderer now passes the
  final native stereo-speaker, headphone, 5.1 and 7.1 outputs, including slow EOF and
  cancellation.
- The 32-bit result is rendered float PCM output, not the encoded ALAC source bit depth.
  Source codec/facts remain E-AC-3 + JOC.
- `.local/tests/applemusic-atmos-source.mp4` proves receipt of E-AC-3 at 48000 Hz with
  Dolby Atmos/JOC signalling and six base channels; `.local/tests/applemusic-atmos-source.ec3`
  is the original encoded remux-copy fixture.
- The four `applemusic-atmos-*-core-result.json` files prove 15-object rendering to stereo,
  headphones, 5.1 and 7.1; `.local/tests/applemusic-atmos-transport-result.json` proves the
  encoded transport is byte-exact.
- The final decoded-buffer-drain build returns exactly `5835 * 1536 = 8,962,560` frames
  / 186.720 seconds in every native layout. The old 8,961,024/186.688 result was a dropped
  tail.
- PCM hashes and mixed independent matches pass for all four native contexts after the
  tail fix.
- The 186.251-second catalog value is metadata; the longer 186.720-second result is
  truthful source padding/mix duration. Glasswave does not claim exact output from Apple's
  private official renderer.
- Headphone HRIR/HRTF listener context is isolated and applies `CavernListener.Volume` at
  -6 dB only. The raw HRTF peak is about 1.53, while speakers stay at unity with a safe
  observed peak of about 0.571. This is headroom, not a limiter, and introduces no ABI
  change.
- Four Atmos modes are present. Off/On selections persist, the default is Off, and the
  owner baseline is highest ALAC plus stereo speakers.
- Missing Atmos preference metadata alone falls back to the ALAC/stereo baseline; other
  Atmos errors must fail explicitly.
- The final slow case decoded exactly 3.008 seconds with first audio at `152.7226 ms`, and
  started before the actual ten-second network EOF. Metadata and timing assertions pass.
- The final cancellation case decoded 1,024 frames, produced first audio at `152.9717 ms`,
  disconnected before transfer EOF, shut down in `80.706 ms`, and completed expected
  DELETE cleanup.

## Stage 5: library, metadata and artwork

Completed:

- [x] Read all eight localized Chinese Sections from the owner account.
- [x] Read 3 Created and 3 Saved playlists.
- [x] Resolve created playlist detail counts `35`, `14` and `359`.
- [x] Match each list count, detail declared count and actual fetched track count.
- [x] Return cached shelf data in `0.0496 ms`.
- [x] Return cached playlist details in `2.821`, `0.0475` and `0.5913 ms`.
- [x] Record uncached detail latencies `5.6293782`, `3.4952492` and `40.3882937 s`; the
  cached path avoids a full refresh but first-load latency remains unresolved.
- [x] Read 196 Favorites and 210 Songs.
- [x] Count 66 genuine no-catalog owner-uploaded Songs in the full library read.
- [x] Derive 6 Genres and 11 Composers from richer credits; first drill-downs return 85
  and 1 Songs.
- [x] Keep the Favorite system playlist out of ordinary Created/Saved shelves.
- [x] Identify Favorite Songs using explicit `tags` projection and the `favorited` value.
- [x] Decode 3/3 created playlist covers through
  `album_art_manager_v2` and background WIC.
- [x] Decode 3/3 sampled artist avatars with non-zero dimensions.
- [x] Resolve the first public artist with 19 declared albums, 19 on the complete first page
  and 10 hot songs.
- [x] Apply the library-albums fallback to every unmapped artist; one real target returned
  one declared album, one complete-page album, matching count and accepted state with no
  errors. Its real album read returned one track with album IDs, artist credit, an unranked
  flag and accepted state. No all-25 individual coverage is claimed.
- [x] Preserve exact fixture title/artist across decoder technical updates.
- [x] Pass 22 shared UI checks for the album-key fallback grouping, avoiding duplicate
  album tiles without collapsing distinct provider IDs.
- [x] Hydrate album relationships for library `210/210`, artist top `10/10`, search
  `10/10`, and sampled playlist tracks `35/35` by direct API check.
- [x] Retry only `ArtCache::Missing` entries for the four provider art kinds on a library
  event; account and language changes still perform full invalidation.
- [x] Match editorial-art fallback behavior for unsigned library album artwork.
- [x] Authenticated direct API plus image decode passed for both uploaded covers: 1120x1120
  and 514x514; no URLs were logged.
- [x] Fix the actual blob-store path in the C++ component; component plus WIC passes 2/2,
  and the new URI validator passes 35 security tests.

Pending:

- [ ] Accept authoritative catalog/library mapping and early-session RAII for the reported
  Colors Of Happiness identity change.
- [ ] Verify live artist 400, song-row album 404 and multi-artist navigation after the
  identity/alias fixes; their mock contracts pass.
- [ ] Verify metadata-region toggles rebuild visible names without lag or stale generations;
  preserve raw identities and playback storefront. The KAF -> 花譜 example alone is accepted.
- [ ] Accept live Favorite Artists separately from library artists; preserve unknown optional
  `inFavorites` values. Mock write/stale-read contracts pass; real-write acceptance is separate.
- [ ] Verify account changes clear library metadata, Favorite identity and WIC state.
- [ ] Exercise long-list pagination, duplicate tracks and collaborative playlists.
- [ ] Verify the duplicate-album and `Missing`-retry behavior in the live UI after the next
  native build.
- [ ] Handle missing/multiple `favorited` tags without guessing by name or user ID.

## Stage 6: mutations

Implemented and exercised only against the loopback contract:

- [x] Favorite add/remove;
- [x] playlist create;
- [x] failed rename rollback;
- [x] successful rename;
- [x] append and readback;
- [x] remove tracks and readback;
- [x] delete playlist;
- [x] album Add to Library / Remove from Library;
- [x] playlist Add to Library / Remove from Library;
- [x] final refresh.

The earlier 19-operation mutation run is historical evidence. The November 11 accepted
phase passes baseline 50/50, mutation 61/61 and helper 29/29. The mutation scenario has
20 true empty-body 202/204 acknowledgments and two nonempty create-201 responses, zero
unexpected calls/contract errors, and Japan 1 -> 0 -> 1 restoration in both scenarios.
The bridge's captured bytes/ACL are restored; real-account writes are zero for this phase.
See `.local/tests/finalNov11-provider-summary.json`. PATCH sends only the playlist name;
library DELETE omits irrelevant locale/format parameters. The owner's original remote
Remove/Refresh UI failure still needs live end-to-end acceptance.

Accepted against the mock contract:

- [x] Mutation epochs preserve acknowledgments through Refresh failure, stale reads and
  rapid album/playlist changes, with eventual reconciliation kept separate.
- [x] Favorite-artist POST/DELETE and stale-read reconciliation; optional `inFavorites`
  remains distinct from all library-artist membership.

Pending:

- [ ] Favorite add/remove on the owner account, with restart persistence.
- [ ] Failed real write, server rejection and rollback.
- [ ] Real create, rename, delete, append and track removal.
- [ ] Real album and playlist library membership.
- [ ] Owner/live acceptance of mutation acknowledgment and eventual reconciliation for
  albums and playlists, including the original Refresh failure.
- [ ] Real favorite-artist behavior and explicit capability state; no real-account writes
  were performed in the accepted provider phase.
- [ ] Playlist track reorder if a truthful current contract exists.
- [ ] Capability flags expose no action lacking applicable real acceptance.
- [ ] Listener and page refresh behavior after every real mutation.

## Stage 7: release hardening

- [x] Regenerate the schema/platform runtime manifest for the selected tested build.
- [ ] Complete runtime redistribution/licensing review.
- [ ] Verify no secret appears in logs, crash output or artifacts.
- [ ] Verify private ACLs on runtime, token, session, logs and temp files.
- [ ] Verify update/rollback keeps the two patches and source pins identifiable.
- [ ] Verify helper remains unlinked from `foo_ui_glasswave.dll`.
- [ ] Confirm installer/dev setup does not require WSL.
- [ ] Confirm fixture bridges are never promoted as production endpoints.
- [ ] Document supported Windows architectures; current artifact is `windows-amd64`.

## Release blockers

- [ ] No complete real AAC-only song or no-ALAC proof.
- [ ] No real Apple write has been accepted.
- [ ] Expired/revoked token and account-switch behavior incomplete.
- [ ] Native crash/network/truncation/recovery matrix incomplete.
- [ ] Runtime redistribution/licensing incomplete.
- [ ] Owner/native metadata-region responsiveness, identity/navigation and account category
  rebuild acceptance incomplete.
- [ ] Playback-chain memory fault allocation root unresolved; stable tracing is not a fix.
- [ ] Now Playing row/failure interaction and complete scheduler restoration acceptance pending.
- [ ] Owner/live mutation acknowledgment and eventual Refresh reconciliation not accepted.

Earlier resolved blockers (these do not close the new owner reports above):

- [x] WSL runtime required;
- [x] native Windows helper unavailable;
- [x] native authenticated session unavailable;
- [x] native full-song ALAC unavailable;
- [x] native 24/96 or 24/192 seek unavailable;
- [x] progressive playback required a full pre-EOS spool;
- [x] metadata wiped by raw decoder `DynamicInfo`;
- [x] library/playlist loading required repeat full-account refresh;
- [x] Favorite identity lost when `tags` were omitted from field selection;
- [x] playlist counts and artwork were unavailable;
- [x] artist albums/hot songs/avatar paths were empty.
- [x] library-only artists without a catalog ID use the shared library-albums fallback; one
  real target passed.
- [x] Atmos decoded tail, exact final frame counts, slow EOF and cancellation were
  unavailable or merely provisional.

## Final completion checklist

- [x] native Windows helper builds from pinned sources;
- [x] native session import and owner login work;
- [x] real native ALAC 16/44.1, 24/48, 24/96 and 24/192 full decode/seek work;
- [x] progressive encoded ALAC and AAC fixtures work without EOS spooling;
- [ ] new catalog/library authority reports pass native acceptance;
- [x] real localized library/playlist/artist/artwork reads work;
- [x] cached reads avoid full-account refresh;
- [x] November 11 provider baseline 50/50, mutation 61/61 and helper 29/29 pass;
- [x] final native 24/192+native-browse repro passes;
- [x] current native 24/48 passes with duration and seek;
- [x] one real Moon strict-cache repro passes the former 29.9-second boundary to EOF with
  seek;
- [x] all 66 unique owner uploads pass source header/container inspection, and real MP3
  and AAC samples pass the full PCM/seek boundary;
- [x] Atmos JOC rendering passes to stereo, headphones, 5.1 and 7.1 core layouts with
  15 objects;
- [x] Atmos final decoded-buffer drain returns 8,962,560 frames/186.720 s in all four native
  layouts, with PCM hashes, slow EOF and cancellation passing;
- [ ] real AAC-only passes;
- [ ] real Apple writes pass or are explicitly unsupported;
- [ ] owner/native identity, navigation, metadata-toggle and mutation reconciliation cases pass;
- [ ] owner scheduler interaction preserves rows and Play Next through selection/failure;
- [ ] corrected scheduler restoration passes all 18 native cases, including empty SDK state;
- [ ] captured playback-chain memory fault has an accepted fix;
- [ ] failure/recovery matrix passes;
- [ ] licensing is release-ready.

Until this checklist is complete, describe the system as "native Windows playback,
progressive encoded streaming, real library reads, 66/66 uploaded-source header audit,
sampled uploaded MP3/AAC full PCM and provider baseline/mutation/helper contracts verified;
current native Colors/ListSession/scheduler reruns, controlled decode trace, owner interaction
and continuous/screen-off checks, real catalog AAC-only, real Apple writes, playback-chain
memory-fault resolution and broader recovery coverage pending", never merely "complete".
