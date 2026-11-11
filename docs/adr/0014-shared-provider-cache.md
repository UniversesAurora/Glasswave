# ADR 0014: Shared provider cache and bounded next-track preparation

Status: accepted 2026-11-10; functional acceptance is tracked in
`docs/plan/shared-cache.md`.

## Context

The original NetEase cover cache keyed downloads by URL and Apple borrowed it.
Apple's progressive audio spool was temporary; NetEase streamed directly from the CDN.
The owner requested one cache for all providers, reusable original encoded songs,
last-known artwork with background refresh, future list caching, and preparation of the
next actual song before the current song finishes.

## Decision

- Use `cache/` for typed audio/image/data keys and immutable disk payloads. Keys use
  provider, opaque persisted account generation, stable resource ID and actual source
  encoding or artwork rendition; signed URLs and credentials are never persisted as keys.
- Use the Windows-bundled SQLite API for transactional WAL metadata. This avoids a
  separately installed database and a custom crash-recovery journal. Metadata publication
  follows payload flush and same-volume rename. Startup removes only recognized owned
  staging/orphan names; a corrupt index can be quarantined and rebuilt.
- A global sequence assigns write tickets; pending generation and published revision are
  distinct. Late writers, scope revocation and explicit clear cannot resurrect old entries.
  Reader leases pin immutable generations while replacements and eviction proceed.
- Audio stores original encoded bytes only. A bounded shared transfer pool and independent
  file positions support concurrent readers, progressive start and complete random access.
  Cancelling one reader does not cancel another. Inputs retain authoritative provider
  entitlement/format resolution and delegate decoding to foobar2000. Decoder rejection
  invalidates its stored source. Cache disk failure falls back to transient playback.
- Immutable generation IDs, chunk tokens and atomic manifests define cache identity.
  Publication, lookup, copying, packaging and deployment do not calculate payload hashes.
  Account-scope and request epochs reject stale writes and results. Separate LRU budgets
  protect audio from image churn; active readers can temporarily retain bytes above the
  configured eviction target.
- Artwork returns last-known disk bytes first, then revalidates once per session and on
  explicit invalidation, with a short demand-driven freshness/backoff window. Use conditional
  GET, retain old pixels on transient failure, reject invalid decoded images, and honor
  no-store. Only changed valid bytes trigger content replacement. UI account epochs and
  request epochs reject late results. Decoded thumbnails remain in `ui::ArtCache`.
- The next-track host follows playback/playlist/queue events and uses one bounded prime
  operation. A prime opens the provider input through the core without starting output or
  changing core playback state. Formal playback adopts the same encoded transfer or disk
  entry. Source availability logic must not skip the current track from a speculative prime.
- Ordered/repeat candidates and the core queue may be inspected without consuming them.
  Random/shuffle selection belongs to the core; no independent random choice or private
  engine hook is introduced. Native core lookahead starts the exact selected input when
  it becomes known. Public SDK boundaries must be explicit in acceptance claims.

## Consequences

Common preferences expose song/image budgets and clear operations. Account changes revoke
stored access without affecting unrelated providers. Service/transfer workers stop before
component unload, and no health/refresh timer runs while idle. The data category reserves
the storage foundation for future persistent list snapshots; this ADR does not claim all
network lists already have disk caching.

The current shared-store formats are index schema v2 and list schema v3. Each persisted data
kind has one current metadata format; development-format changes update or delete the
affected derived data instead of retaining compatibility readers. Functional identity,
image-change and PCM hashes remain test evidence; they do not enter cache keys or the runtime
validation path.

## References

- [HTTP cache validation and incomplete responses, RFC 9111](https://www.rfc-editor.org/rfc/rfc9111.html)
- [HTTP conditional requests, RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#section-13)
- [Windows MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
- [SQLite write-ahead logging](https://www.sqlite.org/wal.html)
- [SQLite atomic commit](https://www.sqlite.org/atomiccommit.html)
