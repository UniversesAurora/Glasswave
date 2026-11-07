# Roadmap

## M0 — Walking skeleton (done)

- [x] Repository, bootstrap (pinned SDK + portable latest foobar2000), MSBuild solution
- [x] `user_interface` module, main window, foobar2000 main menu, keyboard shortcuts
- [x] Per-window Aero glass (HostBackdropBrush + Win7 color matrix) with reflection
- [x] Self-drawn Win7 frame, rounded corners, caption buttons (normal/hover/pressed/inactive)
- [x] Win7-measured shadow windows replacing the DWM shadow
- [x] WMP 12 library layout and transport capsule: shuffle, repeat, stop, prev/next, play orb,
      mute, volume slider (all wired to foobar2000)

## M1 — Core functionality and WMP 12 fidelity

- [x] Seek bar (band top rule), time display, now-playing thumbnail
- [x] Navigation bar: back/forward orbs, breadcrumb
- [x] Command bar: Organize menu, Add files/folder, Create playlist, search box
- [x] Virtualized list for playlists and library views; context menus; drag & drop files
- [x] Library navigation (All music / Artist / Album / Genre) with drill-down
- [ ] Visual review loop against the owner's Win7 captures (paused until captures arrive)
- [ ] WMP 12 grouping by album with art in song lists; column sort/resize
- [ ] Play/Burn/Sync tabs (Play = play list pane)
- [ ] Playlist editing: reorder by drag, drag from library to playlists
- [ ] Device-loss recovery, multi-monitor/DPI torture tests
- [ ] Unit tests for pure logic (colorization, slider mapping, shadow profile, library grouping)

## M2 — Library and hosting

- [ ] Album grid with art (WMP 12 album view)
- [ ] `ui_element` host for third-party panels (lyrics, visualizations)
- [ ] Settings page (colorization presets, reflection, blur tint)

## M3 — Windows Media Center experience

- [ ] WMC-style full-screen mode: menu strip, focus glow with breathing, smooth scrolling
- [ ] Album art transitions, page transitions
- [ ] WMP-style visualizations on `visualisation_manager`

## Later

- [ ] Apple Music research (docs/research/apple-music.md)
