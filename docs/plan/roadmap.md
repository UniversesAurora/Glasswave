# Roadmap

## M0 — Walking skeleton (done)

- [x] Repository, bootstrap (pinned SDK + portable latest foobar2000), MSBuild solution
- [x] `user_interface` module, main window, foobar2000 main menu, keyboard shortcuts
- [x] Per-window Aero glass (HostBackdropBrush + Win7 color matrix) with reflection
- [x] Self-drawn Win7 frame, rounded corners, caption buttons (normal/hover/pressed/inactive)
- [x] Win7-measured shadow windows replacing the DWM shadow
- [x] WMP 12 library layout and transport capsule: shuffle, repeat, stop, prev/next, play orb,
      mute, volume slider (all wired to foobar2000)

## M1 — WMP 12 fidelity and core UX

- [ ] Visual review loop against references; capture real Win7 hover/pressed states
- [ ] Seek bar (WMP 12 style, above the transport), time display, now-playing thumbnail
- [ ] Navigation bar: back/forward orbs, breadcrumb, Play/Burn/Sync tabs (Play = playlist)
- [ ] Command bar: Organize / view / search box
- [ ] Playlist view (virtualized list, WMP 12 grouping by album with art)
- [ ] Device-loss recovery, multi-monitor/DPI torture tests, Win32 build check
- [ ] Unit tests for pure logic (colorization, slider mapping, shadow profile)

## M2 — Library and hosting

- [ ] Library navigation tree (Artist / Album / Genre), album grid with art
- [ ] `ui_element` host for third-party panels (lyrics, visualizations)
- [ ] Settings page (colorization presets, reflection, blur tint)

## M3 — Windows Media Center experience

- [ ] WMC-style full-screen mode: menu strip, focus glow with breathing, smooth scrolling
- [ ] Album art transitions, page transitions
- [ ] WMP-style visualizations on `visualisation_manager`

## Later

- [ ] Apple Music research (docs/research/apple-music.md)
