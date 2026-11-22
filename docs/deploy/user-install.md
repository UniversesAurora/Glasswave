# End-User Installation & Setup SOP

This guide is for users installing a published `foo_ui_glasswave.fb2k-component` release
into their own foobar2000 installation.

## 1. Prerequisites

- **OS**: Windows 11 (build 22000 or later, x64).
- **Host player**: Official **foobar2000 2.x (64-bit / x64)** — standard installation is
  recommended for full Windows shell integration (file associations, Start Menu, Explorer
  context menus, Audio CD AutoPlay). Portable 64-bit installs also work.

## 2. Install or Upgrade Glasswave

1. Download `foo_ui_glasswave.fb2k-component` from the release page.
2. In foobar2000, open **File → Preferences → Components**, click **Install...**, and
   select `foo_ui_glasswave.fb2k-component` (or double-click the `.fb2k-component` file if
   foobar2000 is registered).
3. Click **OK** / **Apply** to restart foobar2000.
4. On first launch, choose **Glasswave** in the User Interface module dialog (or switch
   later under **Preferences → Display → User Interface Module**).

Upgrading repeats the exact same steps: foobar2000 replaces only the component folder
(`<profile>\user-components-x64\foo_ui_glasswave\`) and preserves all user accounts,
playlists, media library entries, settings, and caches.

## 3. What Works Out of the Box

A single `.fb2k-component` package includes:

- `foo_ui_glasswave.dll` (core UI, local library, CD support, embedded QuickJS lyric host)
- `glasswave_cavern.dll` (Dolby Atmos E-AC-3 JOC object renderer)
- `lang\zh-CN.json` (Simplified Chinese UI translation; loaded automatically from the
  component folder, overridable via `<profile>\glasswave\lang\`)
- `lyrics\scripts\searcher\lrclib.js` (built-in MIT LRCLIB lyric searcher)
- `applemusic\runtime\` (open-source Windows Apple Music bridge binaries + licenses)

The following helper tools are **downloaded automatically on first use** into
`<profile>\glasswave\` (no manual setup or Node.js installation required):

- **NetEase Cloud Music backend** (`ncm-api-win-x64.exe`, SHA-256 verified) — downloaded
  when signing into NetEase Cloud Music.
- **aria2** (`aria2c.exe`) — downloaded when queuing the first track download.

## 4. Storage Paths (Cache & Downloads)

By default, Glasswave stores its profile state under `<profile>\glasswave\`, where
`<profile>` is `%APPDATA%\foobar2000-v2` for a standard install (or `<install>\profile`
for a portable install).

To keep large audio/image caches or downloaded music off your `C:` drive:

1. Open **Preferences → Display → Glasswave**.
2. Under **Shared cache**, click **Change cache folder...** and pick your preferred folder
   (for example `D:\Glasswave\cache`), then restart foobar2000.
   - To move an existing cache without re-downloading, close foobar2000 and move the `v1`
     subfolder (`index.sqlite3` and payload folders) into the new cache folder before
     starting foobar2000.
3. Under **Downloads**, click **Change folder...** to choose where downloaded songs are
   saved.

## 5. Optional Non-Redistributable Assets

Because of copyright and licensing restrictions (see
[release-packaging.md](release-packaging.md#2-package-manifest--redistribution-boundaries)),
the public `.fb2k-component` does **not** ship third-party proprietary binaries. All of
these are optional and are picked up automatically when placed in your profile folder:

### 5.1 Original Windows 7 / WMP 12 / WMC Artwork (Optional)

Without these files, Glasswave draws every control procedurally with Direct2D (ADR 0005).
To enable original Windows 7 artwork (ADR 0007), copy the following files from your own
licensed Windows 7 installation into `<profile>\glasswave\`:

```text
<profile>\glasswave\
├─ wmploc.dll
├─ ehres.dll
└─ aero.msstyles
```

Restart foobar2000. The console logs each detected file on startup.

### 5.2 Apple Music Native Playback Libraries (Required for Apple Music)

The open-source Windows bridge (`engine.exe` + `libhybris-win`) is already bundled inside
the component. Apple's proprietary Android shared libraries cannot be redistributed in the
public package.

To activate Apple Music:

1. Extract the `arm64-v8a` / `x86_64` compatible Android libraries (`libandroidappmusic.so`
   and companion `.so` files from your official Apple Music Android APK) together with the
   required companion `.so` runtime files into:

   ```text
   <profile>\glasswave\applemusic\runtime\rootfs\system\lib64\
   ```

2. Restart foobar2000 and sign in under the **Apple Music** section in Glasswave.
   - Glasswave automatically allocates a free local loopback port, generates a per-session
     random token, writes `config.yaml`, and keeps all writable account/DRM state under
     `<profile>\glasswave\applemusic\runtime\`. No manual `bridge.json` is needed.

### 5.3 Additional Lyric Search Scripts (Optional)

Glasswave automatically discovers ESLyric-compatible JavaScript lyric searchers from three
roots (later roots override earlier ones):

1. `<component>\lyrics\scripts\` — built-in MIT scripts shipped with Glasswave (`lrclib.js`)
2. `<profile>\eslyric-data\scripts\` — automatically shared if you have ESLyric installed
3. `<profile>\glasswave\lyrics\scripts\` — place custom or community scripts (such as
   `ESLyric-LyricsSource` `searcher/`, `parser/`, `lib/` folders) here

Reload scripts or restart foobar2000, then adjust source priority under
**Preferences → Display → Glasswave → Lyrics**.

## 6. Clean Uninstall

1. In **File → Preferences → Components**, right-click **Glasswave** and select **Remove**,
   then restart foobar2000.
2. Optionally delete `<profile>\glasswave\` and your custom cache folder if you no longer
   want to keep cached data or settings.
