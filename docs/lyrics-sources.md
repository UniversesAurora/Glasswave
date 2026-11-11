# Unified lyrics sources

The lyrics pane has one priority registry for every automatic source. The order string accepts
stable internal tokens:

- `source`: current-track origin. For a local file it reads embedded/tag lyrics; for a provider
  track it calls that provider's exact native lyrics lookup.
- `sidecar`: adjacent same-basename `.lrc` for a real local file. It is inapplicable to streams.
- `applemusic`: universal Apple Music catalog metadata search.
- Plugin searcher keys such as `netease_ex.js`, `qqmusic.js` and `lrclib.js`.

The sidecar is a file-level convention. It is read only for a local track with a native path and
`subsong == 0`; it is not applied to CUE or other nonzero subsongs, and no lyric offset is
guessed. Embedded lyrics and explicit selections retain their existing per-subsong behavior.

Provider-native implementations retain distinct internal keys such as
`native:provider:ncm` and `native:provider:apple`, but all use the public `source` order token.
The matching provider determines which internal implementation is instantiated. Local embedded
lyrics use `native:source:file`; the adjacent file uses `native:sidecar`.

The default current order is:

```text
source,sidecar,applemusic,netease_ex.js,netease.js,qqmusic_ex.js,kugou.js,qqmusic.js,musixmatch.js,lrclib.js
```

The text is a priority list, not an allow-list. Enabled applicable sources omitted from the
text run afterward in stable registry order, so newly installed plugins stay discoverable.
Unknown and duplicate tokens are harmless. Only the per-source enable checkbox disables a
source.

Display names for native origins use the translation keys `File embedded (Source)`,
`Same-name lyric file (Source)`, `NetEase Cloud Music (Source)` and
`Apple Music (Source)`. The universal Apple metadata source keeps the plain `Apple Music`
name. Chinese translations use full-width `（来源）`.

An explicit `.selected.lrc` owner choice remains the first lookup and is independent of source
order, enabled state, account changes and automatic-cache policy. Its filename is still derived
only from the track path and subsong; its stored `glasswave-source` text and lyric body remain
in the current manual-selection format. “Search again” discards only automatic results. Only
the explicit `Use automatic lyrics` action or choosing another candidate changes the owner
selection.

Automatic cache entries store the stable source key, display caption, lyric body and, when the
source is account-scoped, the durable `cache::AccountScope` UUID. The process-local
`AccountRevision` plus durable scope are rechecked at final main-thread delivery, so an account
switch between worker completion and UI publication cannot display stale native lyrics.
Manual selections intentionally bypass this automatic-source account gate. A cached candidate
is used only if its source still appears in the current applicable/enabled order. Live results
replace their same-source cached fallback. Embedded and sidecar sources are never cached, so
changed or removed local files are always re-read.

## Apple bridge

The native Apple Music source uses the existing token-protected `applemusic::Request` helper.
No Apple token enters JavaScript or JSON.

```http
POST /api/v1/glasswave/lyrics
Content-Type: application/json
```

Direct lookup:

```json
{
  "mode": "direct",
  "trackId": "1440833098",
  "trackType": "songs",
  "title": "Song",
  "artist": "Artist",
  "album": "Album",
  "locale": "zh-Hans-CN",
  "durationMs": 245000
}
```

Metadata search:

```json
{
  "mode": "search",
  "trackId": "",
  "trackType": "songs",
  "title": "Song",
  "artist": "Artist",
  "album": "Album",
  "locale": "zh-Hans-CN",
  "durationMs": 245000
}
```

The response preserves source order:

```json
{
  "results": [
    {
      "title": "Song",
      "artist": "Artist",
      "album": "Album",
      "lrc": "[00:01.00]<00:01.00>Hello <00:01.40>world\n[00:01.00]Translation",
      "confidence": 100
    }
  ]
}
```

`lrc` is UTF-8 enhanced LRC produced from Apple's TTML. Word timing and same-timestamp
translations remain intact. No lyrics is HTTP 200 with an empty `results` array. The bridge
does not emit a second `enhancedLRC` synonym.
