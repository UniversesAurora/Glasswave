# ADR 0017: One priority pipeline for automatic lyric sources

Status: accepted and implemented (2026-11-11); owner interaction checks remain manual.

## Context

ADR 0010 gave embedded/sidecar lyrics and native streaming-provider lyrics fixed priority
ahead of plugin searchers. The owner now wants every automatic source to participate in the
existing editable priority list. Native provider entries must be distinguishable from general
plugins with the same service name. The same order should work across local and streamed music.

## Decision

- Keep the existing Searcher registry, source enablement, per-context order and lyric renderer.
  Remove fixed automatic file/provider stages before that ordered pipeline.
- `source` is the portable order slot for the current track's own origin: embedded/tag lyrics
  for a local file, or exact-ID native lyrics for its streaming provider. Unrelated providers
  do not run in this slot.
- `sidecar` is an independent slot for a same-directory, same-name lyric file when a real local
  backing file exists. Do not infer a different album/track or incorrectly apply cue offsets.
- General metadata-search sources keep independent identities and can serve any applicable
  track. Apple Music supplies a native exact-ID entry and a separate general search entry.
- Native provider and file display names have a localized `(Source)` suffix. Registry/cache
  identity is stable and independent of localized names; ordering uses `source` rather than
  requiring users to know provider-specific native keys.
- Cached automatic candidates participate at their source's current rank. They cannot bypass
  a preferred or newly enabled source. Cheap embedded/sidecar checks rerun so edits/removal are
  visible. Account-bound network results use durable account scope and reject stale publication.
- Explicit per-track manual selections remain the owner's override. Preserve their existing
  lookup identity, associations, stored text and source caption. Do not migrate, delete,
  rewrite or account-scope them as part of automatic-cache or priority changes.
- Update affected development order configuration explicitly if needed. Keep one current
  automatic format; do not add runtime readers/aliases for retired stored formats.

## Relationship to earlier decisions

This replaces only ADR 0010's fixed automatic origin/file precedence. It retains the script
host, enhanced-LRC interchange, cancellation and rendering decisions. Artist and search UI
redesign remain separate work.
