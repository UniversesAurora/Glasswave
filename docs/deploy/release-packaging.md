# Release & Packaging SOP

This SOP governs building, auditing, and publishing a clean, publicly redistributable
`foo_ui_glasswave.fb2k-component` archive. For upstream dependency upgrades and license
tracking, follow [docs/third-party.md](../third-party.md) first.

## 1. Pre-Release Verification Gates

Before packaging a release:

1. **Clean git tree**: verify `git status` is clean on `main` and all changes are committed.
2. **Up-to-date native helpers**:
   - If Cavern changed, rebuild with `pwsh -File scripts/applemusic/atmos/build-renderer.ps1`
     (all real EC-3 probes must pass).
   - If `apple-music-linux` or `libhybris-win` changed, rebuild with
     `pwsh -File scripts/setup-applemusic.ps1`.
3. **Warning-free Release|x64 build**:
   ```powershell
   pwsh -File scripts/build.ps1
   ```
   Must succeed with **0 warnings / 0 errors** at `/W4`.
4. **Targeted automated tests**: run all unit tests affected by changes since the last
   release (for example `scripts/test-component-load.ps1`, `scripts/test-cache-store.ps1`,
   `scripts/test-list-cache.ps1`, `scripts/test-applemusic-bridge-config.ps1`).
5. **Owner visual & playback verification**: confirm UI, playback, streaming providers, and
   preferences work in the running instance.

## 2. Package Manifest & Redistribution Boundaries

Every artifact in Glasswave falls into one of four strict categories:

| Category | Exact Artifacts | Shipped in `.fb2k-component`? | Rationale |
| --- | --- | --- | --- |
| **1. Core & Open-Source Bundled** | `foo_ui_glasswave.dll`, `glasswave_cavern.dll`, `Cavern-LICENSE.md`, `Cavern-NOTICE.md`, `lang/*.json`, `scripts/lyrics/**`, `applemusic/runtime/{engine.exe, libdrm_client.dll, libhybris-core.dll, libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll, hybris-linker/q.so, licenses/*}` | **YES** | Glasswave code + open-source / permitted runtime helpers with license notices. Zero hardcoded paths. |
| **2. Runtime-Fetched** | `ncm-api-win-x64.exe` (`api-enhanced`), `aria2c.exe` | **NO** | Downloaded automatically on first use by Glasswave (`netease/service.cpp` with SHA-256 verification; `download/aria2.cpp`). |
| **3. Non-Redistributable Third-Party Assets** | Microsoft Win7 files (`wmploc.dll`, `ehres.dll`, `aero.msstyles`), Apple Android `.so` files (`rootfs/system/lib64/*.so`), ESLyric built-in scripts (`eslyric-pkg`), unrelated foobar2000 plugins (`foo_out_asio`, `foo_input_sacd`, `foo_input_dvda`, `foo_dsd_processor`, `foo_playcount`) | **NEVER** | Copyright / license boundaries (ADR 0005/0007) or independent plugins. Documented for end users in [user-install.md](user-install.md#5-optional-non-redistributable-assets). |
| **4. Personal & Runtime State** | `session.dat`, `bridge.json`, `config.yaml`, `widevine/`, `files/mpl_db/`, `config.sqlite`, `metadb.sqlite`, `playlists-v2.0/`, `cache/v1/`, `*.selected.lrc`, `*.log`, `*.pdb` | **NEVER** | Private credentials, machine-local paths, or disposable caches. |

## 3. Build & Audit the Package

Run the packaging script (builds `Release|x64` automatically unless `-SkipBuild` is passed):

```powershell
pwsh -File scripts/package.ps1
```

Output: `dist\foo_ui_glasswave.fb2k-component` (gitignored).

`scripts/package.ps1` enforces an automated safety gate before zipping and aborts if any
forbidden filename or directory (`wmploc.dll`, `ehres.dll`, `aero.msstyles`, `rootfs/`,
`widevine/`, `mpl_db/`, `bridge.json`, `config.yaml`, `session.dat`, `*.sqlite*`, `*.log`,
`*.pdb`) is present.

Inspect the archive entries before publishing:

```powershell
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead((Resolve-Path 'dist\foo_ui_glasswave.fb2k-component').Path)
try { $z.Entries | Select-Object FullName, Length | Format-Table -AutoSize } finally { $z.Dispose() }
```

Verify that:
- `foo_ui_glasswave.dll` sits at the archive root (no extra top-level wrapper folder).
- `Cavern-LICENSE.md`, `Cavern-NOTICE.md`, and `applemusic/runtime/licenses/*` are included.
- Zero proprietary binaries, tokens, or personal files appear in the list.

## 4. Publish Checklist

1. Tag or note the release commit and summarize user-visible changes.
2. Attach `dist\foo_ui_glasswave.fb2k-component` to the release.
3. Link end users directly to [user-install.md](user-install.md) for installation,
   cache-folder configuration, and optional asset setup.
4. For the owner's own production machine, update using [personal-setup.md](personal-setup.md#2-day-to-day-update-sop).
