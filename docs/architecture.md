# Architecture

```
foobar2000.exe (core: decoding, playlists, library, DSP, output, components)
 └─ foo_ui_glasswave.dll  ── user_interface service (owns the main window)
     ├─ ui::MainWindow        Win32 window, WS_EX_NOREDIRECTIONBITMAP, custom non-client area
     │   ├─ layout (DIP → px), hit testing (resize edges, caption, Snap Layouts via HTMAXBUTTON)
     │   ├─ widgets           pre-painted layers; state = compositor opacity animations
     │   └─ core callbacks    play_callback, playlist_callback → repaint only what changed
     ├─ aero::GlassMaterial   HostBackdropBrush → ColorMatrix (Win7 recipe) + reflection
     ├─ aero::ShadowFrame     4 layered click-through windows with the measured Win7 shadow
     ├─ aero::paint           Direct2D painters for Win7 frame / caption / transport elements
     ├─ wmp::paint            Direct2D painters for WMP 12 client chrome (lists, nav, bars)
     ├─ ui::ListView          virtualized list over media::ListSource (header, rows, scrollbar)
     ├─ ui::NavPane           Now Playing, All Music, Local Music, one section per provider
     ├─ ui::Page              the asynchronous page on screen (provider pages, All Favorites)
     ├─ ui settings page      Preferences > Display > Glasswave (app/settings, settings::Field)
     ├─ ui::SearchBox         self-drawn search field (WM_CHAR + IME)
     ├─ media::LibraryModel   library snapshot: dedup, sort, artist/album/genre groups
     ├─ media::*Source        PlaylistSource / TrackSource / GroupSource behind the list
     ├─ ui::SongTableSource   song tables over local and provider tracks; tile grids for providers
     ├─ stream::Provider      a streaming provider (ADR 0009); netease::StreamProvider is the first
     └─ gfx::CompositionHost  DispatcherQueue, Compositor, D3D11/D2D devices, surfaces, animations
```

## Content model

- The list shows one `media::ListSource` at a time. Playlist sources keep selection and focus
  in the foobar2000 playlist itself (so foobar2000 shortcuts and context menus agree); library
  sources keep their own selection.
- Library views follow WMP: activating a track rebuilds the "Now Playing" playlist from the
  view and plays from that track; groups (artist/album/genre) drill down.
- `MainWindow` owns navigation history (`View` records) and rebuilds the source on
  navigation, library changes (debounced 400 ms) and playlist callbacks.

## Streaming providers (ADR 0009)

- `stream/` is the provider-neutral model and interface; nothing in `ui/` or `media/`
  includes a provider's own headers (only its sign-in dialog lives under `ui/`).
- A provider registers itself while the component loads and is started by the window (or
  by its `initquit` service under another UI). Its tracks are metadb items handled by its
  own input, so playlists, the queue and play history mix them with local files.
- Provider pages are `ui::Page` objects created by `MakeRemotePage`. The window keeps the
  one on screen; a page's callbacks are wrapped with `Page::Guard`, so results for a page
  that was left are dropped. Album browsing state (`AlbumPager`) is cached for Back/Forward.
- Provider notifications: account changes re-open the provider's page; library changes
  update the navigation lists and the page in place; favorite changes refresh the stars.

## Lyrics (ADR 0010)

- `lyrics/` is the model (`Lyrics`, `ParseLrc`) and `lyrics::Service`: a manual per-track
  selection, the track's own lyrics, the provider's lyrics (`stream::Provider::Lyrics`),
  then cached / searched fallback sources in provider-specific order. Searchers run
  concurrently, and results reach the window on the main thread. `ScriptHost` implements
  ESLyric's JavaScript contract with QuickJS, while `scripts` discovers and registers
  sources from the profile's ESLyric and Glasswave folders. Worker threads are cancelled
  and joined on shutdown. `ui::ShowLyricsSearch` provides manual search and preview.
- `ui::LyricsView` draws the pane with Direct2D through its own device context (effects
  and command lists persist across frames); `gfx::FrameClock` paces frames to the
  compositor clock while something moves. Behaviour: docs/design/lyrics-view.md.
- The pane (`ui::NowPlayingPane`) sits on the glass right of the client area, joined to it;
  a restored window grows by its width. It shows the header and either `ui::LyricsView` or
  `ui::QueueView` (history, the song card, Up Next), switched from its bottom bar.
- The queue stays foobar2000's active playlist, so files opened from outside land in it.

## Window

- Registered through `user_interface_factory` (`user_interface_v2`). The user selects
  Glasswave in foobar2000's UI module prompt; Default UI stays installed as a fallback.
- `WM_NCCALCSIZE` returns 0 so the whole window is client area; the Win7 frame, caption,
  caption buttons and resize borders are drawn and hit-tested by us. Caption buttons
  report `HTMINBUTTON/HTMAXBUTTON/HTCLOSE` so Windows 11 Snap Layouts still work.
- DWM attributes: `DWMWA_USE_HOSTBACKDROPBRUSH`, no corner rounding, no accent border,
  no system backdrop, NC rendering disabled (removes the Win11 shadow, see ShadowFrame).

## Glass (ADR 0002)

`Compositor.CreateHostBackdropBrush()` gives the blurred content behind *this* window.
`aero_colorization.h` folds the Win7 DWM formula
(`base*colorBalance + luma*afterglow*afterglowBalance + backdrop*blurBalance`) into one
5x4 matrix, applied by a D2D ColorMatrix effect inside a `CompositionEffectBrush`. The
effect description is implemented without Win2D (`gfx/color_matrix_effect.cpp`).
Inactive windows use a second brush with weaker tint. When "Transparency effects" is off
the material falls back to an opaque color computed from the same recipe.

Reflection streaks are painted once per monitor at quarter resolution and anchored to
screen coordinates; moving the window only changes an offset.

## Rendering and motion (ADR 0003)

- All painting is Direct2D/DirectWrite into `CompositionDrawingSurface`s, in DIP with
  hairlines snapped to whole device pixels (`floor(scale)`), so 1-DIP lines stay crisp.
- A widget owns normal / inactive / hover / pressed layers (+ optional halo). Interaction
  animates layer opacity in the compositor; nothing is repainted on hover.
- Resizing never repaints large surfaces: frame, client edge, bars and the transport
  capsule are nine-grid or horizontally stretched strips.
- Looping animations (WMC breathing halo) run only while hovered.

## Layout (WMP 12 library mode)

Glass caption (30 DIP) and 8-DIP frame; client area: navigation bar (29), command bar (28),
content, transport band (45) with the capsule control group centered on the play orb.
All numbers in `aero/aero_metrics.h`, sources in `docs/design/visual-spec.md`.

## foobar2000 integration

- Playback: `playback_control` (play/pause/stop/prev/next, volume in dB, mute).
- Shuffle/repeat map onto playback orders (`playlist_manager::playback_order_*`).
- Menus: `mainmenu_manager` builds the full File/Edit/View/Playback/Library/Help menu
  (right-click content, Alt/F10), so preferences, DSP and output stay reachable.
- Keyboard: `keyboard_shortcut_manager::on_keydown_auto`.
- Planned: hosting `ui_element` panels from other components (lyrics, visualizations) as
  child windows below/above the composition tree (ADR 0006).

## Threading

Everything UI runs on foobar2000's main thread. The DispatcherQueue is created (or reused)
on that thread; composition animations run on the compositor thread.
