# ADR 0008: NetEase Cloud Music through a local NeteaseCloudMusicApi Enhanced service

Status: accepted (2026-11-08)

## Context

Glasswave should browse and play NetEase Cloud Music (NCM) alongside the local
foobar2000 library, with more providers (Apple Music) expected later. The NCM web API is
private and changes often; its requests need weapi / eapi / xeapi encryption, device
headers and risk-control handling. Three ways to get it were considered:

1. Implement the protocol in the component (as Ncrust's Windows C++ client does with
   eapi). Single process, no extra runtime, but we own every protocol change and the
   newer xeapi/neapi paths need X25519/AES-GCM work.
2. Use `ncm-api-rs`, a Rust port with an HTTP server mode. Small footprint, but it lags
   the reference implementation (no xeapi at the time of writing).
3. Use NeteaseCloudMusicApi Enhanced, the maintained Node.js reference implementation.
   It tracks protocol changes first and publishes a precompiled Windows x64 server
   (`ncm-api-win-x64.exe`, MIT), so users do not need Node installed.

The owner prioritised reusing existing work and stability over avoiding a second process.

## Decision

Use NeteaseCloudMusicApi Enhanced v4.41.1 as a locally managed helper:

- The component downloads the official precompiled Windows x64 server on first use,
  verifies its pinned SHA-256, and stores it under `<profile>\glasswave\netease\service\`.
  The binary is never committed to this repository.
- The helper is started with `HOST=127.0.0.1` and a chosen free port, inside a job object
  with kill-on-close, and is stopped with Glasswave.
- All API calls go through the helper's local HTTP API; Glasswave itself only talks to
  `127.0.0.1` for metadata and to the NCM CDN for audio and covers.
- An external `api_base` setting can point at a manually run Enhanced instance, which
  also serves as an escape hatch if the bundled version is blocked.
- The helper is a protocol adapter only. Playback stays in foobar2000: `ncm://song/<id>`
  is an `input_entry` that resolves a fresh URL and hands the bytes to the core FLAC/MP3
  decoder.

## Consequences

- NCM protocol maintenance moves upstream; Glasswave only maps JSON to its UI and can
  update the helper independently of the component.
- A second process (~60 MB on disk, tens of MB of memory) runs while NCM is in use. It is
  loopback-only and dies with foobar2000.
- The first use needs a ~65 MB download from GitHub. The download is explicit, hash
  checked and can be skipped by placing the executable manually or using an external
  service.
- x64 only, like the component.
- The local service is trusted code running as the user; its API is unauthenticated, so
  it must never bind a non-loopback interface. This is verified after start.

## Alternatives rejected

- Native C++ weapi/eapi: fewer moving parts, but duplicates a fast-moving protocol and
  would not support xeapi/neapi without significant additional crypto work.
- `ncm-api-rs`: lighter, but its release cadence and protocol coverage were behind
  Enhanced at decision time.
- Embedding a WebView2 login page only: solves login, not catalog, playlists or playback.

## Future providers

Navigation gains a `Library` enum (All / Local / Netease) and provider-specific source
factories. A full provider plugin interface is deliberately deferred until a second
remote provider (Apple Music) exists; the current seam keeps local behaviour unchanged
without speculative abstraction.
