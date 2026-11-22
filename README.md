# Glasswave

A foobar2000 user interface that reproduces the **Windows 7 Aero Glass** desktop,
**Windows Media Player 12** and **Windows Media Center** look and interaction on
Windows 11 — real per-window glass, Win7 frame and caption buttons, WMP 12 transport
controls and WMC-style motion — while keeping foobar2000's audio engine (decoding,
playlists, library, DSP, WASAPI/ASIO output, components) untouched.

Status: **M0 walking skeleton** — glass window, Win7 frame/shadow, WMP 12 library
layout and a working transport bar. See [docs/plan/roadmap.md](docs/plan/roadmap.md).

## How it works (short)

- `foo_ui_glasswave.dll` registers a foobar2000 `user_interface` (a full UI module,
  like Default UI or Columns UI), so it owns the main window.
- Glass is **scoped to our window**: `HostBackdropBrush` (DWM's live blur of what is
  behind the window, documented since Windows 11 22000) fed through a ColorMatrix
  that implements the Win7 DWM colorization formula. No theme patching, no DWM hooks.
- Everything is drawn with Direct2D/DirectWrite into Windows.UI.Composition surfaces;
  hover/press/breathing animations run in the compositor, so an idle window renders
  nothing.
- Visual values come from pixel measurements of native Win7 screenshots, recorded in
  [docs/design/visual-spec.md](docs/design/visual-spec.md). No Microsoft bitmaps are shipped.

Details: [docs/architecture.md](docs/architecture.md) and the ADRs in [docs/adr](docs/adr).

## Build and run

Requirements: Windows 11 (22000+), Visual Studio 2022 with *Desktop development with
C++* (v143, Windows SDK 10.0.22000+), PowerShell 7, 7-Zip.

```bash
pwsh -File scripts/bootstrap.ps1
```

```bash
pwsh -File scripts/build.ps1
```

```bash
pwsh -File scripts/run-dev.ps1
```

`bootstrap` fetches the pinned foobar2000 SDK and unpacks the latest foobar2000 x64 into
`.local/fb2k-dev` as a portable instance (hash- and signature-checked, installer never
executed). `run-dev` deploys the Release build there and starts it. No other foobar2000
installation is touched.

## Packaging, installation and operations

See [docs/deploy/README.md](docs/deploy/README.md) for modular SOPs covering:

- End-user installation (`foo_ui_glasswave.fb2k-component`), custom cache/download folders,
  and optional non-redistributable assets ([docs/deploy/user-install.md](docs/deploy/user-install.md))
- Maintainer release packaging (`scripts/package.ps1`) and redistribution boundaries
  ([docs/deploy/release-packaging.md](docs/deploy/release-packaging.md))
- Development vs production isolation and cleanup
  ([docs/deploy/dev-vs-prod.md](docs/deploy/dev-vs-prod.md))

## Repository layout

| Path | Contents |
| --- | --- |
| `src/foo_ui_glasswave/` | The component (C++20, Win32, Direct2D, Windows.UI.Composition) |
| `src/foo_ui_glasswave/aero/` | Win7 Aero: colorization math, glass material, painters, shadow |
| `src/foo_ui_glasswave/gfx/` | Composition host, D2D/D3D devices, effect interop |
| `src/foo_ui_glasswave/ui/` | Main window, widgets, main menu |
| `lang/` | UI translations bundled into `.fb2k-component` |
| `scripts/` | bootstrap / build / run-dev / package / install-personal |
| `third_party/` | `deps.lock.json` (pinned versions + hashes); fetched code is not committed |
| `docs/` | requirements, architecture, ADRs, visual spec, deploy SOPs, plan, agent guides |
| `dist/` | packaged `.fb2k-component` release archives (ignored) |
| `userdata/` | owner production cache & downloads storage (ignored, never touched by dev scripts) |
| `.local/` | dev instance, upstream build checkouts, references (ignored) |

## License

Not decided yet. The foobar2000 SDK is under its own BSD-style license and is fetched,
not vendored.
