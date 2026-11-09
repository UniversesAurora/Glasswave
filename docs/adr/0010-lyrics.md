# ADR 0010: Lyrics pane and lyric sources

Status: accepted (2026-11-09)

## Context

The owner wants a lyrics pane that opens to the right of the window and looks and moves
like Apple Music's lyrics (word-by-word fill, blur, springs, drag to browse, click to seek,
translations), on the window's own Aero glass instead of Apple Music's cover backdrop. Lyrics
should come from the common Chinese services (QQ Music, NetEase, Kugou) and from Spotify and
Apple Music, through a mature, actively maintained foobar2000 plugin rather than sources we
write and keep alive ourselves.

Survey (2026-11):

- **ESLyric** (closed source, 1.0.7.0 of 2026-09) is the lyrics plugin of the Chinese
  foobar2000 builds. Its sources and parsers are JavaScript files with a documented contract
  (`getConfig` / `getLyrics` / `parseLyric`, `request`, `mxml`, `zlib`...); the community
  keeps word-timed QQ / Kugou / NetEase sources current (Robotxm/ESLyric-LyricsSource,
  updated 2026-06). Its control API (`ActiveXObject("ESLyric")`) only styles ESLyric's own
  windows; no plugin call returns lyric content to another component.
- **OpenLyrics** (MIT, 1.13 of 2026-01) has no Kugou, Spotify or Apple Music source and no
  word timing.
- **LDDC / foo_lddc** needs a separate desktop program (GPL-3).

## Decision

- **Lyric data**: `lyrics::Service` (src/foo_ui_glasswave/lyrics) finds lyrics in this order:
  the cache (`<profile>\glasswave\lyrics`), the track's own lyrics (tags, a `.lrc` next to
  the file), the streaming provider's lyrics for its own tracks (`stream::Provider::Lyrics`;
  NetEase returns word-timed YRC converted to enhanced LRC), then every registered
  `lyrics::Searcher` in parallel, the best match winning. Everything is exchanged as LRC
  (enhanced LRC for word timing, ESLyric's same-timestamp reference lines for translations).
- **Searchers run ESLyric's scripts**: Glasswave hosts the ESLyric script contract on an
  embedded QuickJS engine, so the maintained ESLyric sources and parsers run unchanged. Scripts
  are not shipped: Glasswave reads the folder ESLyric itself installs
  (`<profile>\eslyric-data\scripts`) and its own (`<profile>\glasswave\lyrics\scripts`).
  Spotify and Apple Music sources are written by us in the same format. The host is specified
  in [docs/plan/lyrics-script-host.md](../plan/lyrics-script-host.md).
- **The view is native**: Direct2D on a composition surface, drawn through a device context
  of our own (effects and command lists persist), paced by the compositor clock
  (`gfx::FrameClock`, `DCompositionWaitForCompositorClock`) so it runs at the display's refresh
  rate while something moves and stops completely otherwise. A WebView2 page was considered
  and rejected: a browser process tree for one pane, and a composition seam for the glass,
  without any visual capability D2D lacks.
- **Glass**: the pane is part of the window frame. A restored window grows to the right by the
  pane's width; the pane shows the same glass as the frame, with a dark tint for legible white
  text.

## Consequences

- Searchers are additive: without scripts, local and NetEase lyrics still work.
- Behaviour and tuning of the view are written down in
  [docs/design/lyrics-view.md](../design/lyrics-view.md); the numbers were derived from Apple
  Music captures and the published behaviour of the AMLL project (AGPL; studied, not copied).
- The queue and history lists the owner wants at the bottom of the pane are separate work.
