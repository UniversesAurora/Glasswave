# Development vs Production Isolation & Cleanup SOP

This SOP defines the strict filesystem and runtime boundaries between the Glasswave git
repository / dev sandbox and any production foobar2000 installation.

## 1. Repository & Local Directory Layout

```text
E:\music\Glasswave\
├─ src\  docs\  lang\  scripts\  tests\  third_party\   # Tracked in git (source only)
├─ build\  x64\                                         # Gitignored MSBuild outputs
├─ dist\                                                # Gitignored release packages (.fb2k-component)
├─ userdata\                                            # Gitignored owner production storage (NEVER touched by dev scripts)
│   ├─ cache\v1\                                        # Production shared cache (audio, covers, lists)
│   └─ downloads\                                       # Production downloaded music
└─ .local\                                              # Gitignored dev/build workspace
    ├─ fb2k-dev\                                        # Portable dev foobar2000 instance (dev/testing ONLY)
    ├─ applemusic\                                      # Upstream clones (adam-nightly, libhybris-win, Cavern) & build outputs
    ├─ refs\                                            # Owner Win7 assets, reference screenshots, local lyric script trees
    ├─ cache\                                           # Bootstrap download cache (SDK, quickjs, miniz, fb2k installer)
    └─ tools\                                           # Local build helpers
```

## 2. Hard Isolation Rules

1. **Dev scripts touch ONLY `.local/fb2k-dev`**:
   - `scripts/bootstrap.ps1`, `scripts/build.ps1`, `scripts/run-dev.ps1`, and all
     `scripts/test-*.ps1` scripts operate strictly inside the repo and `.local/fb2k-dev`.
   - They must **never** touch `E:\music\foobar2000`, `%APPDATA%\foobar2000-v2`, or
     `userdata\`.
2. **Production updates are strictly explicit**:
   - Only `scripts/install-personal.ps1` (see [personal-setup.md](personal-setup.md)) writes
     to the production profile, only when explicitly invoked by the owner, and never while
     production `foobar2000.exe` is running.
3. **Zero runtime port or database conflict**:
   - Production uses auto-managed ephemeral loopback ports for both Apple Music and NetEase,
     with its own `applemusic\runtime\files\mpl_db`, `netease\session.dat`, and
     `userdata\cache\v1\index.sqlite3`.
   - `.local/fb2k-dev` uses its own `profile\glasswave\cache\v1\` and dev `bridge.json`
     (`127.0.0.1:20025`).
   - Neither instance locks the other's SQLite stores or loopback ports.
4. **Never pass `-Force` to `scripts/bootstrap.ps1`**:
   - `-Force` deletes `.local/fb2k-dev` including its `profile\`. Plain
     `pwsh -File scripts/bootstrap.ps1` updates foobar2000 binaries in place.

## 3. Safe `.local/` Housekeeping Checklist

When reclaiming disk space inside `.local/`:

- **Safe to delete** (temporary or reproducible):
  - `.local\tests\`, `.local\diagnostics\`, `.local\shots\`, `.local\compare\`,
    `.local\assets-extract\`, `.local\atmos-shim-check\`, `.local\session-notes\`,
    `.local\catalog\`
  - Loose `.log`, `.patch`, `.dmp` files directly under `.local\` or
    `.local\applemusic\windows\runtime\dumps\`
  - `build\`, `x64\`, `dist\` (all reproducible via `build.ps1` / `package.ps1`)
- **Do NOT delete** (owner-supplied or expensive to re-acquire):
  - `userdata\` (production 9+ GB cache & downloaded tracks)
  - `.local\refs\` (Win7 `wmploc.dll`, `ehres.dll`, `aero.msstyles`, reference captures,
    local lyric script trees)
  - `.local\applemusic\windows\runtime\rootfs\` (owner-extracted Apple Android `.so` files)
  - `.local\applemusic\renderers\build\win-x64\glasswave_cavern.dll` and
    `.local\applemusic\windows\runtime\engine.exe` (unless planning to rebuild via
    [docs/third-party.md](../third-party.md))
