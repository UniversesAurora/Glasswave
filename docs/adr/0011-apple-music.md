# ADR 0011: Apple Music through an isolated encoded-stream helper

Status: accepted 2026-11-09; amended 2026-11-10 after native Windows acceptance and runtime
provenance cleanup.

Runtime and transport details are refined by
[ADR 0012](0012-native-apple-music-streaming.md). ADR 0011 keeps the provider boundary,
library semantics and release policy.

## Context

Glasswave needs Apple Music browsing and full-song playback while foobar2000 remains the
only audio decoder, DSP and output owner. The required quality policy is:

- use the highest complete variant available to the signed-in subscription/storefront;
- prefer native ALAC up to 24-bit/192 kHz;
- allow AAC only when no complete ALAC exists for the track;
- never count a preview as success;
- never decode, transcode, resample or output audio in the helper;
- expose only library and playlist operations whose remote behavior is truthful.

Early implementation used a WSL helper and completed encoded spooling to obtain truthful
length and seek. Those results were valid historical proof, but are not the delivered
platform or final transport.

## Current evidence

The current Windows-native path has passed:

- authenticated owner library and full-song requests with no owner Developer key;
- native session import and production restart to PID `62296`, which remained `signedIn`;
- real ALAC stereo 24/192, 75140520 frames and 391.356875 seconds, with EOF and seek;
- real ALAC stereo 24/96, 17880126 frames and 186.2513125 seconds, with EOF and seek;
- real ALAC stereo 24/48, 9514880 frames and 198.2266666667 seconds (`198.227` declared),
  with EOF and seek on the current input;
- progressive multi-`moof` ALAC 24/192 and AAC 48 fixtures whose delayed final byte arrives
  ten seconds after first playback data;
- cancel-after-first-chunk cleanup in `109.7952 ms`;
- preserved provider title/artist through raw decoder `DynamicInfo`;
- eight localized Chinese Sections, 3 Created and 3 Saved playlists, 196 Favorites,
  210 Songs, 6 Genres and 9 Composers;
- created-playlist counts `35/14/359`, cached detail reads
  `0.3074/0.0473/5.3257 ms`, and a cached shelf read of `1.9107 ms`;
- 3/3 playlist covers and 3/3 sampled artist avatars decoded by background
  `album_art_manager_v2`/WIC;
- first public artist navigation with 19 declared albums, 19 on the complete first page
  and 10 hot songs;
- one real library-only artist with one declared and one first-page album; the shared
  fallback covers every unmapped artist, but no all-25 individual coverage is claimed;
- final native 24/192 playback and native library browse on the restarted helper.

The helper source check and latest loopback mutation regression pass 19 operations, exactly
12 expected writes and zero unexpected calls. No real Apple write has occurred.

## Decision

Apple Music is implemented as:

1. a `stream::Provider` adapter owned by `foo_ui_glasswave.dll`;
2. a separately managed Apple Music helper that owns account, token, DRM and transport;
3. the foobar2000 input that receives encoded bytes and hands them to the normal decoder.

### Process boundary

- The helper owns owner authentication, token acquisition, playback-session creation,
  DRM/license processing and encoded transport.
- The helper contains no audio decoder, PCM bridge, resampler, DSP chain or output device.
- The UI component does not load, link or bundle the Apple DRM runtime.
- The API token, native session, cookies and Music User Tokens never enter the repository,
  component binary, settings backup or diagnostics.
- Helper ownership uses a private runtime directory, protected ACLs and process identity
  checks.

### Platform

- Windows 64-bit is the current and final normal runtime.
- Setup and `run-dev` use
  [`scripts/applemusic/windows`](../../scripts/applemusic/windows).
- The managed native endpoint is `http://127.0.0.1:20025`.
- `run-dev.ps1` auto-starts the managed native helper when no bridge exists or the bridge
  already points at `20025`; fixture/alternate bridges stay independent.
- WSL is historical proof only. The former WSL PID `33462` is stopped, and normal startup
  must not require it.
- Native source revisions and toolchain versions are pinned in
  [`scripts/applemusic/windows/source-pins.json`](../../scripts/applemusic/windows/source-pins.json):
  Adam `72d835ebf4c73f3b5f8b4e2b2496a2042a4a214c` and libhybris-win
  `606bae7cdb49eaf8be79828de6cda0ce9d414295`. Reviewed patches, isolated builds, Go tests
  and PE stack-header checks form the build contract. Runtime manifests identify the
  platform and schema; routine patch, artifact, deployment and cache-payload hashing is not
  part of the contract.

### Authentication

- The preserved native session is the primary tested owner-authenticated path.
- Documentation remains clear that Apple's public API requires a developer token and
  personalized requests require a Music User Token.
- Apple appcache JWT discovery may be cached only within safe validity.
- Concurrent discovery uses one in-flight request.
- A `401` invalidates the token once and retries with one refreshed token.
- Music Assistant's bundled application token remains forbidden.

### Stream policy

For each track:

1. choose the highest complete native ALAC variant;
2. use the highest complete AAC variant only when source metadata proves no ALAC exists;
3. otherwise fail as unavailable.

DRM, token, network, truncation and session failures never trigger codec downgrade.

The helper may decrypt encrypted encoded samples and assemble an MP4 container without
changing codec, rate, depth or samples. The foobar2000 input reports technical facts from
its decoder and preserves provider metadata. See [ADR 0012](0012-native-apple-music-streaming.md).

### Library and Favorite semantics

- Display names come from localized `library-playlists.attributes.name`.
- Favorite Songs is identified by account playlist `tags` containing `favorited`.
- The request must explicitly project `tags`; omission is a regression.
- Name, English catalog title, `user-shared`, `canEdit`, `canDelete`, library ID and
  `playParams.globalId` are not standalone Favorite identities.
- Favorite Songs is excluded from Created/Saved shelves and exposed as a system node.
- Favorite state is not a rating.
- Genres and Composers are derived from song metadata and composer relationships.
- Cached sections and details need explicit fast paths; a normal page read must not force a
  full account refresh.

### Provider capabilities

Capabilities reflect accepted behavior:

- read library artists, albums, songs and playlists;
- read Favorite Songs and distinguish it from ratings;
- expose localized categories and background artwork;
- create/append when a truthful tested contract exists;
- keep private rename/delete/remove/save writes behind guarded methods;
- advertise writes only after real owner-account acceptance;
- return explicit unsupported errors rather than no-op success;
- update local snapshots only after remote success.

## Consequences

- foobar2000 receives complete encoded sources and remains the sole audio authority.
- Native Windows now satisfies the platform requirement; WSL no longer blocks release.
- Progressive playback improves first-audio latency without transferring guessed length or
  decoded PCM.
- A known fast path exists for cached shelves, playlist details, artist pages and artwork.
- Private API and Apple runtime compatibility remain maintenance risks.
- Real AAC-only behavior and all real writes remain release gates.
- Runtime licensing and distribution review remain open.

## Open gates

- Prove one real AAC-only full song and no-ALAC selection.
- Execute real Favorite and playlist/library mutations, or mark unsupported.
- Verify expiry, account switch, network loss, crash and cancellation recovery.
- Verify language/account category rebuild.
- Complete runtime licensing and distribution review. The current helper scripts do not
  establish a code-signing contract.

## Rejected alternatives

- browser/external-player capture;
- helper decode plus PCM transfer;
- full-file transfer required before any sound;
- AAC fallback after an ALAC/session failure;
- rating endpoints used as Favorites;
- name or hard-coded account ID used as Favorite identity;
- per-request homepage/script token discovery;
- Music Assistant bundled credential;
- Apple runtime linked into `foo_ui_glasswave.dll`;
- WSL as the shipped helper host.

Research and pinned evidence:
[docs/research/apple-music.md](../research/apple-music.md).
