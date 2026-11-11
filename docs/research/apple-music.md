# Research: Apple Music integration

Status: native Windows helper, owner authentication, real full-song ALAC, progressive
encoded delivery, restart, native library browsing and artwork all have current passing
evidence as of 2026-11-10, including current native 24/48 full decode and seek. Real
AAC-only playback, real Apple writes and release licensing remain pending. The library-only
artist fallback is implemented for every unmapped artist and has one passing real target
sample; the 25 affected artists are not claimed as individually sampled. The Moon
mid-playback key-rotation fix now has one real post-fix full-song pass. Uploaded MP3 and
AAC samples now play to EOF with seek, and a read-only audit verified MZ plus bounded
HTTP Range headers/containers for all 66 unique no-catalog uploads. Full PCM/seek
coverage is not claimed for all 66. Four Atmos JOC core layouts now pass the final native
tail fix, and the slow-EOF and cancellation acceptance cases also pass.
The Account/Japan/China display-region test expansion is still in progress; its final counts
are pending after the next build.

Last reviewed: 2026-11-10.

## Acceptance boundary

Glasswave must:

- play complete songs and never use a preview as a successful result;
- choose the highest complete encoded variant available to the signed-in subscription and
  storefront, preferring native ALAC up to 24-bit/192 kHz;
- permit complete AAC when no complete ALAC variant exists for that track;
- deliver original encoded audio to foobar2000 without helper-side decode, PCM transfer,
  resampling, transcoding, DSP or output;
- keep artwork extraction and WIC decoding on background workers;
- expose only library and playlist writes whose remote result semantics are truthful;
- preserve localized library display names and system-playlist identity.

Evidence labels:

- **Verified native:** exercised through the Windows helper and foobar2000 input.
- **Verified local read:** exercised against the owner account without a write.
- **Verified fixture:** an executable loopback or synthetic test, not evidence about a real
  Apple catalog entitlement.
- **Historical:** useful implementation evidence superseded by a newer path.
- **Pending:** required behavior without a passing current result.

## Evidence chronology

- **Historical WSL proof:** an earlier Linux helper proved owner login, encoded ALAC,
  restart authentication and the provider read/write shape. It is no longer the normal
  runtime. The old WSL process PID `33462` was reported stopped on 2026-11-10.
- **Native Windows runtime:** the isolated Windows helper now starts on
  `127.0.0.1:20025`, imports the preserved authenticated session and serves both library
  reads and full encoded songs.
- **Native restart and production repro:** the helper restarted to PID `62296` with the
  pinned engine and remained signed in. The final native 24/192 playback and native library
  probe then passed.
- **Progressive encoded delivery:** the foobar2000 input can begin decoding a growing
  encoded MP4 cache before the network reaches EOS. It does not expose a guessed length.
- **Real native playback:** native Windows delivered real 24/96 and 24/192 ALAC with
  declared duration, full sample count and seek.
- **Library/artwork:** the final real-read probe resolved localized Sections, playlist
  details, artist data, covers and avatars, all without a cloud mutation.
- **Write contract:** the latest loopback fixture passes 19 operations and exactly 12
  expected writes. No real Apple mutation has been executed.
- **Moon mid-play repair:** the pre-fix native cache invalidated Moon tracks
  `396652147` and `391590320985` as ALAC after 29.907 seconds. The updated intermediate
  helper now passes a real strict-cache repro through the former 29.9-second boundary and
  to EOF.
- **Provider/UI hardening:** duplicate-album grouping, relationship hydration and
  `ArtCache` retry behavior have targeted checks. Uploaded-source header/metadata coverage
  is separate. These checks do not replace per-upload full-PCM coverage.
- **Regional display metadata:** the Account/Japan/China test expansion is in progress.
  The 29-operation provider mock baseline predates these cases; no final coverage count is
  claimed yet.
- **Atmos decoded-buffer drain:** the renderer previously returned at compressed EOF while
  its decoded buffer still contained valid samples. The final native renderer explicitly
  drains that buffer. The four layouts now produce exact PCM under the final renderer.

## Pinned sources and build provenance

Canonical pins are in
[`scripts/applemusic/windows/source-pins.json`](../../scripts/applemusic/windows/source-pins.json).

| Component | Pin | Evidence |
| --- | --- | --- |
| Adam nightly | `https://github.com/silentone12725/apple-music-linux/tree/72d835ebf4c73f3b5f8b4e2b2496a2042a4a214c` | Native Go engine and DRM integration |
| Glasswave Adam patch | `scripts/applemusic/glasswave.patch` | Reviewed input; SHA-256 `72690913df2218efbbcab5744c1e2f84687f578dcd459d75dd57f7b62053ab57` is a historical run measurement |
| Adam FairPlay lock | `drm/fairplay-libs.lock` | Historical SHA-256 measurement `79eda95709aa8ee24ca8619a629f85e49717550c7ae5d40ad5671c75537f7236`; not a current cache/artifact contract |
| libhybris-win | `https://github.com/silentone12725/libhybris-win/tree/606bae7cdb49eaf8be79828de6cda0ce9d414295` | Windows guest-runtime bridge |
| Glasswave libhybris patch | `scripts/applemusic/windows/libhybris-win.patch` | Reviewed input; SHA-256 `0b5aa324330b63d17c3d47f7d835a45f8643d555c62a06c2afa1d516d351b038` is a historical run measurement |
| Native runtime manifest | `.local/applemusic/windows/runtime/manifest.json` | Current schema 2/platform/generation marker; no patch or artifact hashes |
| Historical native engine | `.local/applemusic/windows/runtime/engine.exe` | Historical SHA-256 `5c1220c3e16282e5c9a814ec9eb33d2f60be03d19c982e131daf189961af94ed`; PID `66528` |
| Previous native engine | Historical `d1c8dfed…` | Retained only as pre-Moon repair proof |
| Final native 24/192 result | `.local/tests/applemusic-final-192-result.json` | Historical accepted-engine ALAC full-duration/seek result |
| Native 24/96 result | `.local/tests/applemusic-native-windows-96-result.json` | Fresh final-repro track result |
| Native 24/48 result | `.local/tests/applemusic-native-windows-48-result.json` | Current-input real ALAC full-duration/seek result |
| Additional 16/44.1 result | `.local/tests/applemusic-39-fixed-result.json` | Real ALAC full-duration/seek result |
| Uploaded MP3 result | `.local/tests/applemusic-uploaded-mp3-result.json` | Real owner-upload full-duration/seek result |
| Uploaded AAC result | `.local/tests/applemusic-uploaded-aac-result.json` | Real owner-upload full-duration/seek result |
| Uploaded source audit | `.local/tests/applemusic-uploaded-source-audit.json` | 210 library rows scanned; 66 unique no-catalog songs pass MZ plus bounded HTTP Range header/container inspection |
| Progressive ALAC | `.local/tests/applemusic-progressive-alac-result.json` | 24/192 encoded-cache fixture |
| Progressive AAC | `.local/tests/applemusic-progressive-aac-result.json` | AAC 48 encoded-cache fixture |
| Progressive cancellation | `.local/tests/applemusic-progressive-cancel-result.json` | Cancel-after-first-chunk cleanup |
| Final native library probe | `.local/tests/applemusic-final-library-result.json` | Latest localized library, timing, artwork and library-only track proof |
| Moon post-fix result | `.local/tests/applemusic-moon-fixed-result.json` | Real strict-cache repro past the former 29.9-second failure |
| Received Atmos resource | `.local/tests/applemusic-atmos-source.mp4` | E-AC-3/JOC resource proof, not rendered audio |
| Original Atmos bitstream fixture | `.local/tests/applemusic-atmos-source.ec3` | Copy fixture of the original encoded E-AC-3 source |
| Historical Atmos renderer DLL | SHA-256 `5b2ab3e80d3ecc1621eec887fdff53084d908f45a7be4d8f8a4f9344bb1c515a` | Historical build identity for the decoded-buffer-drain and headphone-headroom acceptance run |
| Atmos core results | `.local/tests/applemusic-atmos-{stereospeakers,headphones,speakers51,speakers71}-core-result.json` | Four final native Cavern JOC output layouts |
| Atmos transport result | `.local/tests/applemusic-atmos-transport-result.json` | Byte-exact encoded transport review |
| Atmos slow result | `.local/tests/applemusic-atmos-progressive-result.json` | Three-second exact-frame slow-EOF result |
| Atmos slow timing | `.local/tests/applemusic-atmos-slow-timing.json` | First audio before an actual ten-second transfer EOF |
| Atmos cancel result | `.local/tests/applemusic-atmos-cancel-result.json` | Pre-EOF cancel, shutdown and session cleanup |
| Atmos cancel timing | `.local/tests/applemusic-atmos-cancel-timing.json` | Cancel before transfer EOF with DELETE evidence |
| Uploaded artwork result | `.local/tests/applemusic-uploaded-artwork-result.json` | Two authenticated Apple artwork blobs and WIC decodes |
| Provider write contract | `.local/tests/applemusic-provider-contract-result.json` | 19 synthetic operations |
| Provider contract state | `.local/tests/applemusic-provider-contract-state.json` | 12 expected writes, no unexpected calls |

Build tools pinned in the source manifest are Go `1.26.0`, MinGW `16.1.0`, LLVM
`23.1.3` and Visual Studio `editbin`.

The isolated build performs the following sequence:

1. verify pinned upstream checkouts and tool versions;
2. apply the Glasswave patches to isolated worktrees;
3. build the Windows DRM DLL and libhybris runtime;
4. run the Go tests with the `adamplay_backend glasswave` tags;
5. build `engine.exe`;
6. set PE stack reserve/commit to 64 MiB/2 MiB with `editbin`;
7. verify the PE headers and write a schema/platform runtime manifest;
8. promote only the tested artifact set.

The current isolated build checks pinned revisions and tool versions, then runs Go tests and
PE stack-header assertions before promotion. Patch, artifact, deployment and cache-payload
hashes are not part of the current runtime contract. Current persisted formats are runtime
manifest schema 2, shared-store index schema v2 and list schema v3. Each data kind has one
current format; development-format changes update or delete the affected derived data without
compatibility readers. Functional identity, image-change and PCM hashes remain test-only
evidence.

Historical acceptance measurement: the native helper was PID `66528`, and its engine SHA-256
`5c1220c3e16282e5c9a814ec9eb33d2f60be03d19c982e131daf189961af94ed` matches the updated
runtime files from that run. The recorded patch SHA-256
`72690913df2218efbbcab5744c1e2f84687f578dcd459d75dd57f7b62053ab57` is also historical.
The older `d1c8dfed…` artifact remains historical only.

### Windows stack requirement

The initial native crash occurred on an Apple guest function with an approximately 125 KiB
frame. Control jumped to `RSP-8`, below Windows' default 4 KiB guard, and the guest faulted
before the Apple logic could execute.

Glasswave fixed the executable rather than patching an Apple binary:

- linker flag: `-extldflags=-Wl,--stack,67108864`;
- post-link PE header: `/STACK:67108864,2097152`;
- validation: llvm-readobj must report 64 MiB reserve and 2 MiB commit.

This allocates Windows virtual address reserve without eagerly committing 64 MiB, and the
patch remains outside the Apple runtime payload.

## Default development flow

The accepted flow uses the Windows runtime only:

- `scripts/setup-applemusic.ps1` dispatches to
  [`scripts/applemusic/windows`](../../scripts/applemusic/windows);
- the default helper endpoint is `http://127.0.0.1:20025`;
- `scripts/run-dev.ps1` prepares a managed bridge descriptor when the bridge is absent or
  already points at port `20025`; the component itself starts the helper on a request
  worker, including when `foobar2000.exe` is launched directly;
- authenticated readiness is awaited before account/login/library/playback operations;
- a mandatory kill-on-close Windows Job Object owns newly launched native processes.
  Normal and abnormal player exit both clean up that process tree. Existing healthy
  services are borrowed and are not terminated when the player closes;
- an explicit fixture or alternate bridge remains independent and is not replaced;
- the normal build/start/status/restart path requires no WSL process.

The helper owns a private runtime root under `.local/applemusic/windows/runtime`, a random
API token, protected ACLs and isolated `APPDATA`, `LOCALAPPDATA` and temporary directories.

## Authentication and tokens

The Windows native session import preserved the owner's existing authentication. Library
reads and full-song audio both work after restart without an owner Apple Developer Program
key.

Historical authentication measurement: an earlier production repro restarted the helper
from PID `71036` to PID `62296`; status remained `signedIn`, and the then-current
`d1c8dfed…` engine served playback and the library probe.

The historical post-fix engine subsequently ran as PID `66528` with SHA-256
`5c1220c3e16282e5c9a814ec9eb33d2f60be03d19c982e131daf189961af94ed`.

The Apple public documentation remains the compatibility baseline for a future
documented-API mode:

- [Generating Developer Tokens](https://developer.apple.com/documentation/applemusicapi/generating-developer-tokens)
- [User Authentication for MusicKit](https://developer.apple.com/documentation/applemusicapi/user-authentication-for-musickit)
- [Apple Music API](https://developer.apple.com/documentation/applemusicapi)

Current private-document-token behavior is narrower than scraping the web page:

- discovery uses the Apple `appcache`;
- a JWT is held in memory and cached only until its safe expiry;
- concurrent callers share one token request;
- HTTP `401` invalidates the cached token and retries with one refreshed token;
- ordinary pages and Glasswave API calls do not reload the homepage or rerun the per-API
  web token script.

Music Assistant's bundled application credential is forbidden. No shared app token,
cookie, Music User Token, native session file or API token is copied into research
artifacts, diagnostics or the repository.

## Native full-song playback

### Real ALAC evidence

| Run | Source/result | Decoder result |
| --- | --- | --- |
| Final native 24/192 | Track `635770202`; `.local/tests/applemusic-final-192-result.json` | ALAC stereo 24/192, 75140520 frames, 391.356875 s, full decode and seek pass; first audio `4966.673 ms` |
| Native Windows 24/96 | Fresh asset `1624173757`; `.local/tests/applemusic-native-windows-96-result.json` | ALAC stereo 24/96, 17880126 frames, 186.2513125 s decoded, full decode and seek pass |
| Native Windows 24/48 | Track `1625058484`; `.local/tests/applemusic-native-windows-48-result.json` | ALAC stereo 24/48, 9514880 frames, 198.2266666667 s decoded, 198.227 declared, full decode and seek pass |
| Additional native 16/44.1 | `.local/tests/applemusic-39-fixed-result.json` | ALAC stereo 16/44100, 9771384 frames, 221.5733333 s decoded, 221.573 declared, full decode and seek pass; first audio `2750.8975 ms` |
| Historical 24/48 | Track `1625058484` under the old input | Earlier codec/sample proof only; duration unavailable and no seek |

The diagnostic native 24/192 run reached first decoded audio after 3.624 seconds. The
saved native result records `firstDecodedMs: 2140.8434`. The final fresh-asset 24/96 repro
records `firstDecodedMs: 10081.5162`. These are observed timings, not fixed latency
promises.

The earlier repeated native 24/192 playback and native library browse passed on PID
`62296`. The accepted post-fix engine then repeated the 24/192 full-song decode and seek on
PID `66528`; that current result is
`.local/tests/applemusic-final-192-result.json`.

The current native 24/48 input now passes on track `1625058484`: 9514880 frames,
198.2266666667 seconds decoded versus 198.227 declared, `seekVerified:true`, and first
decoded audio at `2891.8178 ms`. This replaces the historical old-input result as the
current end-to-end proof.

### Moon key rotation: real post-fix repro passed

The pre-fix cache diagnosed tracks `396652147` and `391590320985` as invalid ALAC after
29.907 seconds. The real HLS manifest changes content keys at segment 2, while the previous
upstream path dropped `EXT-X-KEY` URIs after segment 1.

The staged Go repair:

- preserves the complete segment-to-key schedule instead of retaining only one URI;
- keeps key-selection state on one DRM connection so play, seek and reconnect use the same
  schedule;
- propagates key-negotiation failures rather than treating a missing later key as valid;
- versions incompatible ALAC cache records to v10.

Go tests pass for delayed first content, multiple rotations, seek after transition,
reconnect after rotation, reusable sources and unknown later keys.

The updated intermediate native helper and foobar2000 then passed a real strict-cache repro:

- ALAC stereo 16-bit/44100 Hz;
- 14792704 frames and 335.43546485 seconds decoded versus 335.435 declared;
- play continued past the former 29.907-second failure to EOF;
- seek passed;
- first decoded audio was `9581.3081 ms`.

The result is `.local/tests/applemusic-moon-fixed-result.json`. This proves the shared key
schedule repair on one real post-fix source; it does not separately enumerate both
originally failing assets.

### Progressive encoded cache

The native helper writes unchanged encoded MP4 bytes into a background cache. The
foobar2000 input exposes an already valid `ftyp`/`moov` prefix and can start sequential
decoding before the final network byte arrives.

- No guessed final length is reported.
- The helper performs no decode, PCM transfer or resampling.
- ALAC and AAC containers reach foobar2000 as encoded data.
- A seek into data not yet cached can wait for the required transfer or completed cache.
- Explicit EndSession DELETE cancels outstanding work and releases the session.
- The native input writes a persistent, bounded private playback diagnostic suitable for
  correlating cache invalidation, codec handoff, seek and session teardown. It contains no
  credential values.

Fixtures with deliberate 10-second final-byte delay passed for:

- multi-`moof` ALAC stereo 24/192;
- AAC 48 kHz.

Saved results show first decoded audio before EOS:

- progressive ALAC: `firstDecodedMs: 1484.7745`, full three-second duration and seek pass;
- progressive AAC: `firstDecodedMs: 0.0825`, three-second declared duration and seek
  pass, with `bitDepth:null` expected for AAC;
- cancel-after-first-chunk: shutdown `109.7952 ms`; a subsequent observed repeat was
  approximately 110 ms.

The cancellation result ends with 4096 ALAC frames and intentionally does not claim seek.
The assertion is that cancellation closes the transfer and session without external PCM
decode.

### Metadata boundary

The raw MP4 decoder's `DynamicInfo` update previously replaced provider tags with an
untagged container fallback. The input now preserves the provider title and artist fields
when publishing decoder technical information. Exact fixture title/artist assertions pass
in both progressive ALAC and AAC results.

### Uploaded library songs

The first read-only MZ probe covered only the first 100 library rows and found 16
no-catalog candidates: seven MP3 and nine AAC. That was a subset, not the account total.
The final all-page library read scanned all 210 Songs using page sizes
`100, 25, 25, 25, 25, 10` and found 66 unique genuine no-catalog, owner-uploaded
candidates with no duplicate IDs.

The read-only source audit in `.local/tests/applemusic-uploaded-source-audit.json`
successfully inspected all 66/66 candidates with MZ metadata plus bounded HTTP Range
header/container inspection:

| Source class | Audited count |
| --- | --- |
| MP3 | 26 total: 25 at 44.1 kHz and 1 at 48 kHz |
| AAC | 40 total: all 44.1 kHz, 256 kbps, stereo |
| ALAC | 0 |
| Failures | 0 |

The audit reports `fullSongDownloads:false`, so this proves source header and container
metadata availability, not complete PCM playback of every upload. Two real MP3/AAC
samples independently passed full playback through foobar2000:

| Upload | Result |
| --- | --- |
| `i.1YBN0qVHqJaz4dR` — カラー | MP3 stereo 44.1 kHz, 10857600 samples, 246.2040816327 s decoded / 246.204 declared, EOF and seek pass, first audio `3490.9562 ms` |
| `i.1YBN07rCqJaz4dR` — あいぞめ piano version | AAC stereo 44.1 kHz, 5459032 samples, 123.7875736961 s decoded / 123.787 declared, EOF and seek pass, first audio `5917.0946 ms`; `bitDepth:null` is expected |

An initial MP3 probe returned `unexpected_codec` only because the synthetic fixture
whitelist excluded MP3; the real input had already decoded full samples. The whitelist was
corrected and both MP3/AAC probes were rerun successfully.

This establishes 66/66 source-header/metadata availability plus full PCM/seek for two
independently sampled real uploads. It does not claim that the other 64 received an
independent full-PCM test, does not claim every future upload format is playable, and does
not close the separate real-Apple-catalog AAC-only gate.

Direct authenticated API reads followed by image decode passed for both uploaded-cover
resources in `.local/tests/applemusic-uploaded-artwork-result.json`:

- `i.1YBN0qVHqJaz4dR`, album `l.ENk1r4y`: real Apple artwork blob decoded at 1120x1120;
- `i.1YBN07rCqJaz4dR`, album `l.NHW0iQ6`: real Apple artwork blob decoded at 514x514.

The result records sizes and decode status only; no artwork URLs are logged.

The actual blob-store defect was fixed in the C++ component rather than only in the probe.
The component plus WIC path passes 2/2 uploaded artwork decodes, and the new URI validator
passes 35 security tests.

### Atmos: decoded tail, four layouts and cancellation verified

The historical renderer build used for final Atmos acceptance has SHA-256
`5b2ab3e80d3ecc1621eec887fdff53084d908f45a7be4d8f8a4f9344bb1c515a`.
The owner explicitly requires Atmos implementation. The Cavern/Native-AOT path renders the
real E-AC-3 + JOC resource to all four final native core layouts:

| Layout | Output | Channel mask | First decoded | Final frames |
| --- | --- | --- | --- | --- |
| Stereo speakers | 2ch float PCM32/48000 | `3` | `4063.8684 ms` | 8,962,560 / 186.720 s |
| Headphones | 2ch float PCM32/48000 | `3` | `578.6696 ms` | 8,962,560 / 186.720 s |
| 5.1 speakers | 6ch float PCM32/48000 | `1551` | `996.5026 ms` | 8,962,560 / 186.720 s |
| 7.1 speakers | 8ch float PCM32/48000 | `1599` | `1075.5412 ms` | 8,962,560 / 186.720 s |

Each final result reports Cavern `atmosRendered:true`, 15 dynamic objects, and verified
seek. The exact raw EC-3 source contains `5835 * 1536 = 8,962,560` frames, or 186.720
seconds at 48 kHz. The earlier 8,961,024-frame/186.688-second result omitted one decoded
tail quantum. The final fix drains the decoded buffer when compressed EOF arrives, avoiding
the null reference, dropped tail and zero fill. All four native contexts pass PCM hashes
and mixed independent matches after the fix.

The catalog value is 186.251 seconds. The longer decoded tail is truthful source
padding/mix duration; it is not renderer duration drift. This proves Glasswave's Cavern
render path and does not claim bit-exact output from Apple's private official renderer.

The `32-bit` value is the float PCM render output width, not the encoded ALAC source bit
depth. Native source codec/facts must continue to report E-AC-3/JOC separately from the
decoder output.

Headphone HRIR/HRTF processing is isolated in its listener context. Only the headphone
render applies `CavernListener.Volume` at -6 dB because the raw HRTF peak is about 1.53 and
would otherwise clip. Speaker paths remain at unity with an observed peak of about 0.571.
This is headroom, not a limiter, and it introduces no ABI change.

The received resource remains:

- `.local/tests/applemusic-atmos-source.mp4`;
- six base channels with JOC carrying additional objects;
- `.local/tests/applemusic-atmos-source.ec3` as the original encoded remux-copy fixture.

The one-off encoded-transport equivalence test is byte-exact:

- byte count `17925120`;
- SHA-256
  `D7DF58DBDF5016564E36867CFE9D3E07F9E08B0093E3A5AA81665AEC3B314B2F`;
- independent FFmpeg copy-remux comparison matches, and the raw EC-3 frame count produces
  the same 186.720-second native output rather than relying only on a duration label.

The raw EC-3 count and native output agree exactly. The 0.469-second difference from the
catalog duration is metadata versus decoded source reality, not encoder drift.

Settings expose four modes; Off/On selections persist, the default remains Off, and the
owner-selected baseline is highest ALAC plus stereo speakers. Missing Atmos preference
metadata is the only condition that falls back to that ALAC/stereo baseline; other Atmos
errors fail explicitly.

The final slow-transfer case in
`.local/tests/applemusic-atmos-progressive-result.json` produced first audio at
`152.7226 ms`, decoded exactly 144,384 frames / 3.008 seconds, preserved metadata and
verified that playback began before the actual ten-second transfer EOF. The timing record
is `.local/tests/applemusic-atmos-slow-timing.json`.

The final cancellation case in `.local/tests/applemusic-atmos-cancel-result.json` stopped
after 1,024 rendered frames, with first audio at `152.9717 ms` and shutdown in `80.706 ms`.
Its timing record has no transfer EOS and confirms disconnect before EOF. Two expected
session DELETEs completed. The timing record is
`.local/tests/applemusic-atmos-cancel-timing.json`.

## Library read evidence

The final real-read probe passes with eight Chinese Sections:

```text
喜爱 / 歌曲 / 艺术家 / 唱片集 / 流派 / 作曲者 /
我的播放列表 / 收藏的播放列表
```

Observed owner-library counts:

- 3 Created and 3 Saved playlists;
- created playlist track counts 35, 14 and 359, each matching its detail and declared
  count;
- 196 Favorite songs and 210 library Songs;
- 6 Genres and 11 Composer groups from richer credits;
- first Genre drill-down 85 Songs and first Composer drill-down 1 Song;
- 45 followed artists; one representative catalog-resolvable artist was exercised.

Cached behavior is no longer a whole-account refresh:

- cached Created shelf callback: `0.0496 ms`;
- repeated playlist detail callbacks: `2.821`, `0.0475` and `0.5913 ms`;
- initial uncached playlist-detail reads in the same probe took `5.6293782`,
  `3.4952492` and `40.3882937 s`; cached reads avoid a full-account refresh but do not
  erase first-load latency;
- complete read snapshots and playlist order match before/after the cached shelf call.

Artwork and artist navigation:

- 3 of 3 Created playlist covers passed core `album_art_manager_v2` extraction and WIC
  decode;
- 3 of 3 sampled artist avatars decoded with non-zero dimensions;
- the first resolved public artist reports 19 declared albums, 19 on the complete first
  page, and 10 hot songs;
- no UI network work is required for WIC decode.
- shared UI album fallback grouping now passes 22 targeted checks, covering duplicate
  album tiles without changing provider IDs into a false single album;
- direct provider relationship hydration returns album data for library `210/210`, artist
  top tracks `10/10`, search `10/10`, and the sampled playlist tracks `35/35`;
- `ArtCache` retries only `Missing` entries for the four provider art kinds on a library
  event, while account and language changes perform full invalidation;
- unsigned library album art uses the `editorialArt` fallback at parity with other art
  sources.

The provider now applies the library-album path to every unmapped artist. One real
library-only target passed as `requested:true`, `resolved:true`, one declared album, one
album on a complete first page, `countMatch:true`, `accepted:true`, with empty fetch/read
errors. Its real album read also returned one track with real album IDs, artist/credit
fields, an unranked flag and accepted track state. This verifies the shared fallback path
with one sampled target. All-artist coverage is not claimed, and the 25 affected artists
are not described as individually tested.

Favorite Songs remains identified by the account-scoped `attributes.tags` projection
containing `favorited`, not by localized name or a hard-coded library ID. The field
selection regression is fixed by explicitly requesting
`fields[library-playlists]=name,artwork,editorialArtwork,tags,...`; the final probe records
`favoriteTagDetected:true` and `favoriteLeak:false`.

## Public versus private mutation contracts

The documented public API supports library reads, playlist creation, appending tracks,
adding resources to the library and ratings. Personalized requests still require a Music
User Token in documented mode.

Current Apple Web/private behavior additionally exposes Favorite and playlist edits that
are not represented as stable public delete/rename contracts:

```http
POST   /v1/me/favorites?ids[songs]=<catalog-id>&platform=web
DELETE /v1/me/favorites?ids[songs]=<catalog-id>&platform=web

POST   /v1/me/library/playlists
PATCH  /v1/me/library/playlists/<library-playlist-id>
DELETE /v1/me/library/playlists/<library-playlist-id>/tracks?ids[library-songs]=...&mode=all
DELETE /v1/me/library/playlists/<library-playlist-id>
POST   /v1/me/library?ids[albums|playlists]=...
DELETE /v1/me/library/albums/<library-album-id>
DELETE /v1/me/library/playlists/<library-playlist-id>
```

The provider keeps private write support behind capability flags until owner-account
acceptance exists. Ratings remain separate from Favorites.

The helper source check and latest loopback write regression pass 19 operations:

- Favorite add/remove;
- playlist create;
- injected rename failure with unchanged state, then successful rename;
- append and readback;
- remove and readback;
- delete;
- album save/unsave;
- playlist save/unsave;
- final refresh.

The fixture records exactly 12 expected writes, zero unexpected calls, a random nonce and
a clean final state. This validates the native client's request shape and local state
handling only. **Zero real Apple writes have occurred.**

## Remaining unverified matrix

- one complete real Apple-catalog AAC-only song, including proof that no ALAC variant
  existed;
- real Favorite add/remove persistence and failure rollback;
- real album/playlist add/remove;
- real playlist create, rename, append, remove, delete and any reorder support;
- follow/unfollow artist or an explicit unsupported decision;
- expired/revoked tokens, account switching and forced re-login;
- network loss, malformed/truncated transfer, helper crash and recovery on Windows;
- cancellation cleanup beyond the current controlled fixture;
- language-change and account-change category rebuild;
- redistribution/licensing disposition for every runtime artifact.

## Reusable source and licensing

| Component | Reuse status |
| --- | --- |
| Adam nightly at `72d835eb...` | MIT repository, with Glasswave patch and notice obligations |
| libhybris-win at `606bae7c...` | Retain the component licenses shipped in the upstream tree |
| Glasswave patches | Project-controlled integration and transport changes |
| Apple web bundle/private API behavior | Behavioral evidence only; proprietary source is not reused |
| Apple Android/FairPlay runtime files | Redistribution rights still require release review |
| Music Assistant bundled application token | Forbidden for Glasswave |

The native runtime must remain isolated from `foo_ui_glasswave.dll`. Build provenance,
private ACLs, cleanup behavior and artifact licensing are part of release completion.

## Current blockers

1. No real AAC-only full-song result.
2. No real Apple write acceptance.
3. Expiry, account-switch, network-loss, crash and recovery semantics need broader native
   coverage.
4. Runtime redistribution/licensing is unresolved.

Atmos JOC now has final native 15-object output for stereo speakers, headphones, 5.1 and
7.1, with the decoded tail, slow EOF and cancellation paths verified. E-AC-3 core decode
alone is still not equivalent to object rendering, and Glasswave does not claim equivalence
with Apple's private official renderer.

The earlier WSL runtime, whole-file spooling and “native Windows unknown” blockers are
obsolete. See [the implementation plan](../plan/apple-music.md) and
[ADR 0012](../adr/0012-native-apple-music-streaming.md).
