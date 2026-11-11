# ADR 0015: Shared paged list sessions and optional persistent snapshots

Status: accepted 2026-11-10; functional acceptance is tracked in
`docs/plan/paged-lists-and-scheduler.md`.

Providers expose one typed page with a provider-owned continuation cursor. A shared session
coalesces reads and aggregates a contiguous prefix. Each consumer independently requests
OnDemand or All loading. Nonempty local search, explicit sorting and expanded album layouts
require All, while default server order remains demand-driven. UI delivery is progressive
in both modes. Transport cancellation and request generations reject late work.

Cache policy is selected per query/provider: None, Memory or Persistent. Semantic keys
include provider, account scope, list kind/identity and effective language, while transport
tokens are excluded. Persistent snapshots use immutable page chunks and an atomically
published manifest in the shared Data store. This avoids quadratic payload rewriting and
mixing pages across refreshes. List-only revocation is independent of song/image caches.

Cached rows appear first. Refresh assembles a new generation behind the published snapshot
until it reaches the old displayed frontier, then replaces it atomically. Partial errors
keep usable rows. Exact duplicate occurrences, selection, focus and scroll anchors remain
part of the presentation contract, independent of provider and layout.

Related metadata is an optional second stage. Providers publish usable page rows first,
then enrich only the same occurrence range without changing the cursor or source order.
Generation-scoped cancellation rejects late replies. Ordinary tables display baseline rows;
album-dependent grouping publishes only the contiguous metadata-ready prefix. An unchanged
or failed metadata attempt advances readiness without rebuilding ordinary rows. Persistent
pages use random immutable chunk identities and one current schema, with no content hashes
or old-format reader branches; the manifest atomically publishes each metadata replacement.
