# ADR 0009: Streaming providers behind one interface

Status: accepted (2026-11-08). Supersedes the "Future providers" section of ADR 0008.

## Context

ADR 0008 deferred a provider interface until a second remote provider existed. The NetEase
work that followed wired NetEase straight into the window: `MainWindow` kept every NetEase
page's loading state, `Navigate` grew a 400-line switch, and NetEase types, `ncm://` path
parsing and `"ncm:"` key strings spread into the navigation pane, list sources, favorites
and play history. The owner plans more providers (Apple Music first) and asked for an
architecture that can take them without copying all of that.

## Decision

- `stream/` holds the provider-neutral model (`Track`, `Album`, `Artist`, `Playlist`, string
  ids opaque to the UI) and the `stream::Provider` interface: optional features, account,
  a snapshot of the library, asynchronous reads and writes (callbacks on the main thread),
  a listener for account / library / favorites changes, track identity for metadb items,
  cover keys and the provider's own preferences rows. Providers register themselves while
  the component loads; `stream::ProviderOf(item)` maps a foobar2000 item to its provider.
- Each provider lives in its own folder and implements the interface on top of its own
  session (`netease/stream_provider` over `netease::Session`). Playback stays in foobar2000
  through the provider's input, as in ADR 0008.
- The UI works only with `stream::`. Provider pages are `ui::Page` objects
  (`ui/remote_pages`): the window owns only the page on screen and destroys it on
  navigation, which drops every result still in flight. Song tables and tile grids
  (`ui/song_table`, `ui/remote_grids`) take stream records; the navigation pane builds one
  section per registered provider; links in the album column are typed (`media::Link`).
- Preferences rows are data (`settings::Field`), so a provider adds its options without
  changes to the preferences page.

## Consequences

- A new provider is a folder with a `stream::Provider` implementation, its input and its
  sign-in UI. Navigation, breadcrumb, pages, menus, favorites, the Source column and play
  history follow without changes, and features it lacks are hidden.
- Provider records are copied when adapted (lists of a few hundred items per page; not a
  hot path).
- The provider-neutral model only carries what the UI shows. A provider-specific field
  needs a deliberate addition to `stream/model.h`.
- Saved data keyed by provider (view settings, the last page, play history, the navigation
  order) uses the provider key (`ncm`); the earlier NetEase-specific formats were replaced
  without migration (nothing has been released).
