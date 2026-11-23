# Personal Daily-Driver Setup, Update & Migration SOP

This SOP documents the owner's production foobar2000 installation, how to update it in one
command after code changes, and how to back up or migrate it to another machine.

For public packaging rules see [release-packaging.md](release-packaging.md); for how this
install stays isolated from `.local/fb2k-dev` see [dev-vs-prod.md](dev-vs-prod.md).

## 1. Production Path Map

| Role | Path | Notes |
| --- | --- | --- |
| **foobar2000 binary** (Standard x64) | `E:\music\foobar2000\foobar2000.exe` | Official installer; provides Windows file associations, Start Menu, and shell integration. |
| **foobar2000 profile** | `%APPDATA%\foobar2000-v2\` | Holds `config.sqlite`, `metadb.sqlite`, `playlists-v2.0\`, `library-v2.0\`, installed components, accounts, and lyrics. |
| **Glasswave component** | `%APPDATA%\foobar2000-v2\user-components-x64\foo_ui_glasswave\` | Extracted from `dist\foo_ui_glasswave.fb2k-component` (DLLs, `lang\`, MIT lyric scripts, open-source Apple Music engine). |
| **Owner's 3rd-party plugins** | `%APPDATA%\foobar2000-v2\user-components-x64\{foo_out_asio, foo_out_asio+dsd, foo_input_sacd, foo_input_dvda, foo_dsd_processor, foo_playcount}` | Managed independently of Glasswave releases. |
| **Shared disk cache** | `E:\music\Glasswave\userdata\cache\v1\` | Configured in **Preferences → Display → Glasswave → Cache folder** (`cfg_var.B83BE425-E106-48B8-A2CD-9A9377FA6211`). Keeps large audio/image/list caches off `C:`. |
| **Downloaded music** | `E:\music\Glasswave\userdata\downloads\` | Configured in **Preferences → Display → Glasswave → Download folder** (`cfg_var.6B1E4A70-2C9D-4F3B-8E51-7A2D0C9F4B11`). |
| **Owner non-redistributable assets** | `%APPDATA%\foobar2000-v2\glasswave\` | Win7 artwork (`wmploc.dll`, `ehres.dll`, `aero.msstyles`, plus optional `hd\{aero,ehres,wmploc}\*@2x.png` from `upscale-*.ps1`), Apple Android `.so` (`applemusic\runtime\rootfs\system\lib64\`), community lyric scripts (`lyrics\scripts\`), ESLyric scripts (`%APPDATA%\foobar2000-v2\eslyric-data\scripts\`). |

Note: Production intentionally has **no** `applemusic\bridge.json`. Glasswave runs in
auto-managed mode (dynamic free loopback port, per-process random token, auto-generated
`config.yaml`), so it has zero hardcoded repo paths and never collides with `.local/fb2k-dev`.

## 2. Day-to-Day Update SOP

When changes have been verified in `.local/fb2k-dev` and you want to update your daily
player:

1. **Close production foobar2000** (`E:\music\foobar2000\foobar2000.exe`).
   `install-personal.ps1` checks this and refuses to run while the target process is open.
2. Run:
   ```powershell
   pwsh -File scripts/install-personal.ps1 -Build
   ```
   (Omit `-Build` if `dist\foo_ui_glasswave.fb2k-component` was already built via
   [release-packaging.md](release-packaging.md#3-build--audit-the-package).)
3. Start `E:\music\foobar2000\foobar2000.exe` normally from the Start Menu or taskbar.

### What `scripts/install-personal.ps1` Does (and Does Not) Touch

- **Updates**:
  - Replaces `%APPDATA%\foobar2000-v2\user-components-x64\foo_ui_glasswave\` cleanly from
    `dist\foo_ui_glasswave.fb2k-component`.
  - Re-syncs owner local Win7 artwork (`.local\refs\` and `.local\fb2k-dev\profile\glasswave\hd\`), Apple Android `.so` libraries
    (`.local\applemusic\windows\runtime\rootfs\system\lib64\`), and local lyric scripts
    (`.local\refs\lyrics\`). Pass `-SkipLocalAssets` to update only the component archive.
- **Never touches**:
  - `config.sqlite`, `metadb.sqlite`, `playlists-v2.0\`, `library-v2.0\`, `dsp-presets\`
  - NetEase login (`netease\session.dat`), Apple Music login (`applemusic\runtime\files\`,
    `widevine\`), manual lyric selections (`*.selected.lrc`), `play-stats.txt`, `downloads.txt`
  - Third-party components (`foo_out_asio`, `foo_input_sacd`, etc.)
  - `E:\music\Glasswave\userdata\cache\` and `E:\music\Glasswave\userdata\downloads\`

## 3. Backup & New-Machine Migration SOP

### Minimal Backup (Settings, Accounts, Playlists, Non-Redistributable Assets)

Back up the following (typically ~150–300 MB total, excluding disposable cache):

- `%APPDATA%\foobar2000-v2\` (entire folder)
- Optionally `E:\music\Glasswave\userdata\downloads\` (your downloaded audio files)

### Full Backup (Also Preserve Cached Songs/Covers/Lists)

Additionally back up:

- `E:\music\Glasswave\userdata\cache\v1\` (preserves `account_scopes` + all cached audio,
  covers, and lists so the new machine starts warm with zero re-downloads)

### Restore on a New Machine

1. Install official **foobar2000 2.x x64** (Standard install) as in
   [user-install.md](user-install.md#1-prerequisites).
2. Close foobar2000 and restore `%APPDATA%\foobar2000-v2\`.
3. If you backed up the cache, restore `v1\` into your chosen cache folder and verify
   **Preferences → Display → Glasswave → Cache folder** points to its parent directory.
4. Launch foobar2000: Glasswave, your third-party plugins, playlists, Apple Music & NetEase
   sessions, and cached media come back intact.
