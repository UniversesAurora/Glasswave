# ADR 0012: Native Windows service and progressive Apple Music playback

Status: accepted 2026-11-10. Core native Windows, authentication, full-song ALAC and
progressive encoded playback are verified. Real AAC-only, real Apple writes and release
hardening remain open.

## Context

ADR 0011 established the Apple Music provider/helper boundary and an encoded WSL proof. The
owner now requires a native Windows runtime and playback that can start before the whole
file is downloaded.

The first native implementation exposed three additional constraints:

- an Apple guest function could use a frame near 125 KiB, faulting below Windows' default
  4 KiB stack guard;
- an early token design rediscovered the Apple web application token and repeatedly
  refreshed account state;
- whole-file spooling delayed first audio and was unnecessary once a valid MP4 prefix was
  available incrementally.

Library and artwork probes also exposed missing playlist counts, missing artist
albums/hot songs, incomplete covers and metadata loss from raw-decoder `DynamicInfo`.

## Decision

### Native runtime

Glasswave uses a separately managed Windows `engine.exe` built from pinned Adam nightly and
libhybris-win sources:

- Adam: `72d835ebf4c73f3b5f8b4e2b2496a2042a4a214c`;
- libhybris-win: `606bae7cdb49eaf8be79828de6cda0ce9d414295`;
- canonical pins:
  [`scripts/applemusic/windows/source-pins.json`](../../scripts/applemusic/windows/source-pins.json);
- current provenance: pinned source revisions and toolchain versions in that file.

The pin file and Glasswave patch files are reviewed build inputs. The isolated build checks
the pinned checkouts and tool versions, applies the patches, builds the Windows DRM/libhybris
layer, runs Go tests, compiles the engine, verifies PE stack headers and writes a runtime
manifest. That manifest uses schema 2 and records the platform and generation time. It does
not record patch, artifact, deployment or payload digests.

No Electron UI, VLC output, helper PCM decoder or helper audio device is part of the
runtime. The helper remains outside `foo_ui_glasswave.dll`.

### Stack handling

The Windows executable uses:

```text
linker:  -extldflags=-Wl,--stack,67108864
post-link: editbin /STACK:67108864,2097152
```

The first value is virtual-address reserve; the second is eager commit. This addresses the
guest 125 KiB-frame fault at `RSP-8` without modifying an Apple binary or globally changing
the host stack defaults. `llvm-readobj` must verify both PE header values after link.

### Startup and normal development flow

- The default managed endpoint is `http://127.0.0.1:20025`.
- `setup-applemusic.ps1` dispatches to the Windows helper scripts.
- `run-dev.ps1` prepares a `managed.runtimeDirectory` bridge descriptor when the bridge is
  absent or already points at port `20025`. It does not launch a detached runtime.
- The component starts the native helper from a worker before account, login, catalog or
  playback requests, including a direct launch of `foobar2000.exe`. Concurrent requests
  share startup and wait for authenticated readiness. Credentials remain in the existing
  isolated session directory; restarting the player does not sign the owner out.
- The child starts suspended, with restricted handle inheritance and its own environment.
  A kill-on-close Windows Job Object is assigned atomically during process creation via
  `PROC_THREAD_ATTRIBUTE_JOB_LIST`, avoiding even a suspended orphan if the player exits
  between creation and resume. Job assignment is mandatory before any child code executes.
  Normal quit cancels pending startup and closes the owned job. An abnormal parent exit
  has the same job cleanup guarantee, including descendants.
- Existing healthy services are borrowed, never adopted or terminated. Missing `managed`
  metadata means an explicitly external bridge. A later request can relaunch an owned
  engine that has exited; user operations are not blindly replayed after transport errors.
- Explicit fixture or alternate bridges are not auto-replaced.
- Normal Windows start/status/restart does not invoke WSL.
- The old WSL PID `33462` is stopped.
- PID file, executable path, process start time, command-line port, API token and ACLs are
  checked before the helper is treated as owned.

### Authentication and token cache

The preserved native owner session is imported and remains usable after restart. The
production restart moved from PID `71036` to PID `62296` and remained `signedIn`. The
current private token flow:

- reads the Apple appcache JWT;
- caches it only to an unknown-lifetime safety bound or known JWT expiry;
- shares one discovery flight among concurrent callers;
- invalidates once on `401` and retries with a refreshed token;
- does not reload the homepage or execute the token script for every API call.

Public Apple API semantics remain documented separately: developer token plus Music User
Token for personalized requests. Music Assistant's shared application token is forbidden.

### Progressive encoded cache

The helper transfers unchanged encoded MP4 bytes into a background cache.

- A valid `ftyp`/`moov` prefix can be read before EOS.
- foobar2000 starts sequential MP4 decoding as cache data arrives.
- The input does not expose a guessed final length.
- A forward seek may wait for the requested transfer range or a complete truthful cache.
- ALAC and AAC remain encoded; no external decoder or PCM handoff is introduced.
- EndSession DELETE cancels active production and releases its session.
- Cancellation after the first chunk must close promptly and discard incomplete work.

### Metadata

Raw decoder `DynamicInfo` may report only container technical facts. Publishing that update
must preserve provider title, artists and other tags already installed in foobar2000's
metadata service. Exact synthetic title/artist assertions are required for ALAC and AAC.

### Library performance and artwork

- `FetchPlaylists` and `FetchPlaylist` use cached snapshots after the initial read.
- A page-level section/detail read must not trigger a complete account refresh.
- Playlist list, detail and declared counts must agree; an empty playlist accepts zero.
- Album art extraction uses `album_art_manager_v2`; WIC decoding runs on background cache
  workers.
- Favorite playlist field selection must explicitly project `tags`.
- Artist resolution must provide avatar, non-zero album count, first-page albums and hot
  songs when the account/catalog exposes them.

## Acceptance evidence

Native Windows:

- real ALAC 24/192, 75140520 frames, 391.356875 s, full decode and seek pass;
- real ALAC 24/96, 17880126 frames, 186.2513125 s, full decode and seek pass;
- real ALAC 24/48, 9514880 frames, 198.2266666667 s decoded versus 198.227 declared,
  full decode and seek pass, first decoded audio at `2891.8178 ms`;
- diagnostic native 24/192 first audio observed at 3.624 s; current saved 24/192 JSON
  records `firstDecodedMs: 2140.8434`;
- final 24/96 fresh-asset run records `firstDecodedMs: 10081.5162`.

Progressive fixtures:

- ALAC 24/192 with final byte delayed ten seconds: first decode `1484.7745 ms`, full
  duration and seek pass;
- AAC 48 with final byte delayed ten seconds: first decode `0.0825 ms`, full duration and
  seek pass, `bitDepth:null`;
- cancel after first chunk: shutdown `109.7952 ms`, followed by an approximately 110 ms
  observed repeat, no PCM transfer.

Historical acceptance measurement: the final repeated native 24/192 playback and native
library browse passed on PID `62296` with engine SHA-256
`d1c8dfed4f4baea8ddbb336c13477372b59a43881dd190a6b9c6f5d180f249c1`. That digest
identifies the recorded run only and is not a current artifact-verification contract.

Real library read:

- 8 Chinese Sections, 3 Created, 3 Saved, 196 Favorites, 210 Songs;
- created playlist counts `35/14/359`, with cached reads
  `0.3074/0.0473/5.3257 ms`;
- cached shelf read `1.9107 ms`, with full snapshot/order match;
- 6 Genres, 9 Composers; first drill-downs `85/1`;
- 3/3 playlist covers and 3/3 sampled avatars decoded;
- first public artist 19 declared albums, 19 on the complete first page, 10 hot songs;
- one real library-only artist with one declared and one first-page album, complete page,
  matching count and accepted state. The fallback is implemented for every unmapped artist,
  but this one target does not establish all-25 individual coverage.

Mutation contract:

- helper source check and 19 loopback operations pass;
- exactly 12 expected writes and zero unexpected calls;
- no real Apple write has occurred.

## Remaining gates

- real AAC-only full song and no-ALAC metadata proof;
- real Favorite, album, playlist and track mutation acceptance;
- expired/revoked token, account switch and forced re-login;
- network loss, truncated MP4, helper crash, cancellation and temp/cache cleanup;
- language/account category rebuild;
- runtime licensing and packaging disposition;
- confirmation that no secret enters logs, crash output or packaged artifacts.

## Rejected alternatives

- keeping WSL as the shipped/helper-default path;
- requiring complete transfer before first decode;
- decoding to PCM in the helper;
- treating AAC as a fallback for an ALAC or non-codec failure;
- scraping the web homepage or running token JavaScript on each API call;
- using Music Assistant's bundled credential;
- globally enlarging every Windows thread stack;
- patching an Apple runtime binary for the stack fault;
- reporting guessed file length or metadata from an untagged MP4 container.

See [ADR 0011](0011-apple-music.md),
[the research report](../research/apple-music.md), and
[the acceptance plan](../plan/apple-music.md).
