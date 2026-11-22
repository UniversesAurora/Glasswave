# Third-Party Source & Dependency Maintenance Guide

This document is the single source of truth for **every** external codebase, library, runtime
helper, script, and reference repository used by Glasswave — whether fetched at build time,
patched from an isolated checkout, downloaded at runtime, copied/adapted, or kept solely as a
clean-room reference.

Keep this inventory current whenever a dependency is added, upgraded, patched, or removed.

---

## 1. Complete Inventory

### 1.1 Build-Time C/C++ Libraries & Dev Host (`third_party/deps.lock.json`)

Fetched by `pwsh -File scripts/bootstrap.ps1`. Never committed to git (ignored under
`third_party/` and `.local/`).

| Name | Current Pin | Upstream | Local Destination | How Glasswave Uses It | License |
|---|---|---|---|---|---|
| **foobar2000 SDK** | `2026-10-01` | `https://www.foobar2000.org/downloads/SDK-2026-10-01.7z` | `third_party/foobar2000-sdk` | Core plugin API, `pfc`, `libPPUI`, `foobar2000_component_client` linked into `foo_ui_glasswave.dll`. Unmodified. | BSD-style (`sdk-license.txt`); `pfc`/`libPPUI` zlib-style |
| **quickjs-ng** | `0.18.0` | `https://github.com/quickjs-ng/quickjs` (`quickjs-amalgam.zip`) | `third_party/quickjs-ng` | Embedded JS engine powering ESLyric-compatible lyric search scripts (`lyrics/script_host.cpp`). Unmodified amalgamation. | MIT |
| **miniz** | `3.1.2` | `https://github.com/richgel999/miniz` (`miniz-3.1.2.zip`) | `third_party/miniz` | ZIP/deflate decompression for archive/lyric helpers. Unmodified amalgamation. | MIT |
| **foobar2000 (dev host)** | `2.26.3` (x64) | `https://www.foobar2000.org/downloads/foobar2000-x64_v2.26.3.exe` | `.local/fb2k-dev` | Portable dev instance unpacked with 7-Zip (never runs installer). Authenticode verified (`CN=Illustrate Ltd, O=Illustrate Ltd, L=Bury, C=GB`). | Freeware (local dev/testing only) |

### 1.2 Isolated-Checkout + Tracked-Patch Repositories

Upstream clones live strictly in ignored `.local/` directories and are **never** committed.
Glasswave tracks only the pin file, minimal unified `git diff` patch, adapter code, and build
script.

| Name | Current Pin | Upstream & Branch | Pin File | Tracked Patch | Build Script & Output | How Glasswave Uses It | License |
|---|---|---|---|---|---|---|---|
| **apple-music-linux (Adam)** | `960bd8a354e9370ca8ddad69c484d7cdc41125e0` | `https://github.com/silentone12725/apple-music-linux.git` (`nightly`) | `scripts/applemusic/windows/source-pins.json` | `scripts/applemusic/glasswave.patch` | `scripts/setup-applemusic.ps1` → `.local/applemusic/engine.exe` | Native Windows Apple Music session/DRM/API/stream bridge helper. Audio decoding stays in foobar2000. | See `scripts/applemusic/licenses/` |
| **libhybris-win** | `96125ef983874134deaa1cddd7b763f55f70c1b1` | `https://github.com/silentone12725/libhybris-win.git` (`master`) | `scripts/applemusic/windows/source-pins.json` | `scripts/applemusic/windows/libhybris-win.patch` | `scripts/setup-applemusic.ps1` (linked into `engine.exe`) | Windows compatibility shim loading Apple's unmodified Android `.so` libraries for DRM/Widevine. | Per-file upstream notices in source tree |
| **Cavern** | `c54117222a83deabf71c3fd162ed71a4b263a55d` | `https://github.com/VoidXH/Cavern.git` (`master`) | `third_party/glasswave-cavern/source-pin.json` | `scripts/applemusic/atmos/cavern-buffer-drain.patch` | `scripts/applemusic/atmos/build-renderer.ps1` → `.local/applemusic/renderers/build/win-x64/glasswave_cavern.dll` | NativeAOT C ABI Dolby Atmos (E-AC-3 JOC) object renderer via `third_party/glasswave-cavern/NativeExports.cs`. | Custom Cavern license (`LICENSE.md`, see `third_party/glasswave-cavern/NOTICE.md`) |

### 1.3 Runtime-Fetched External Services & Tools

Downloaded on demand by Glasswave at runtime into user profile storage (not compiled into the DLL).

| Name | Current Pin | Upstream | Pin Location in Code | Integrity Check | How Glasswave Uses It |
|---|---|---|---|---|---|
| **NeteaseCloudMusicApiEnhanced / api-enhanced** | `v4.41.1` | `https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced` | `src/foo_ui_glasswave/netease/service.cpp` (`kServiceVersion`, `kServiceTarballUrl`, `kServiceSha256`) | SHA-256 verified before extracting and executing downloaded Node.js code | Local HTTP API helper for NetEase Cloud Music provider. |
| **aria2** | `1.37.0` | `https://github.com/aria2/aria2` (`release-1.37.0`) | `src/foo_ui_glasswave/download/aria2.cpp` | Official GitHub release archive | Multi-connection resumable track download engine (`download::Service`). |

### 1.4 Lyric Search Scripts

| Name | Current Pin | Upstream | Location | How Glasswave Uses It |
|---|---|---|---|---|
| **ESLyric-LyricsSource** | `ebaf516` | `https://github.com/Robotxm/ESLyric-LyricsSource.git` | `.local/refs/lyrics/ESLyric-LyricsSource` (copied to dev profile by `scripts/run-dev.ps1`) | Community ESLyric JavaScript lyric searchers run inside Glasswave's `quickjs-ng` host. |
| **eslyric-pkg** | static extraction | Owner-supplied extraction from `foo_uie_eslyric.dll` | `.local/refs/lyrics/eslyric-pkg/emb_5923208/scripts` | Built-in ESLyric search scripts copied for dev testing. |
| **lrclib.js** | in-repo | Glasswave-authored | `scripts/lyrics/searcher/lrclib.js` | Native MIT LRCLIB lyric searcher tracked directly in this repo. |

### 1.5 Directly Copied / Ported Third-Party Code Inside `src/`

Currently **none**: Glasswave avoids copy-pasting third-party source directly into `src/foo_ui_glasswave/`.
Whenever an external algorithm or code snippet is ever copied or ported into `src/`, add a row
here recording:

| Local File(s) | Upstream Repo & Commit | Original File(s) | License | Local Deviations & Reason |
|---|---|---|---|---|
| *(none currently)* | — | — | — | — |

### 1.6 Clean-Room Visual / Behavioral Reference Repositories (Not Built or Deployed)

Kept locally under `.local/refs/` for visual measurement or behavior reference only; **never**
compiled, linked, deployed, or committed (see ADR 0005).

| Name | Local Path | Upstream | Purpose |
|---|---|---|---|
| **applemusic-like-lyrics (AMLL)** | `.local/refs/lyrics/amll` | `https://github.com/Steve-xmh/applemusic-like-lyrics.git` | Reference for syllable-lift and line-interval timing heuristics (`ui/lyrics_view.cpp`). Clean-room C++/D2D implementation in Glasswave. |
| **Win7 / WMP 12 / WMC screenshots & probes** | `.local/refs/` | Owner captures | Pixel measurements recorded in `docs/design/visual-spec.md`. |

---

## 2. How to Modify Third-Party Code (Strict Hierarchy)

Always choose the **lowest-coupling** level that cleanly solves the problem:

1. **Level 1 — Adapter / wrapper code in Glasswave (preferred)**
   - Keep Glasswave-specific logic in our own files (`src/foo_ui_glasswave/...`,
     `third_party/glasswave-cavern/NativeExports.cs`) rather than editing upstream code.
   - Example: Cavern's 5.1 → 2.0 `Lo/Ro` downmix, `ListenerAdmission` serialization, and C ABI
     exports live in `third_party/glasswave-cavern/NativeExports.cs`, leaving Cavern's own source
     untouched.
2. **Level 2 — Minimal, isolated `git diff` patch (only when upstream internals lack a hook)**
   - Use this only when upstream has a bug or missing internal hook (e.g., `glasswave.patch`,
     `libhybris-win.patch`, `cavern-buffer-drain.patch`).
   - **Workflow to author or update a patch**:
     1. Check out the exact pinned upstream commit in `.local/`.
     2. Apply the existing patch (`git apply`), make your minimal changes, and verify.
     3. Regenerate the patch using `git diff > <tracked-patch-path>`.
     4. **Never** hand-edit unified diff hunk headers (`@@ -a,b +c,d @@`) or leave unexported
        edits inside `.local/` — `.local/` is disposable and must always be reproducible from
        `pin + tracked patch`.
   - Document *why* each hunk exists in the commit message and keep hunks small and orthogonal so
     individual hunks can be dropped as soon as upstream fixes them.
3. **Level 3 — Ported/copied code inside `src/` (last resort)**
   - Only when a tiny self-contained algorithm cannot reasonably be linked or patched.
   - Must satisfy license compatibility, include a short provenance header comment (upstream repo,
     commit SHA, license), and be listed in Section 1.5 above.

### Non-Negotiables
- **Never commit** upstream source checkouts, downloaded binaries, `.local/` artifacts, or
  Microsoft bitmaps/msstyles/WMP/WMC assets (ADR 0005).
- **Never modify** Apple's Android `.so` binaries.
- **Never add redundant payload/copy SHA-256 checks** after local builds, copies, or deployments
  (per `AGENTS.md`). Cryptographic hashes are reserved strictly for security/integrity checks on
  untrusted remote downloads (e.g., `netease/service.cpp`), protocol requirements (e.g.,
  MusicBrainz SHA-1 DiscID), cache key indexing (`cache/store.cpp`), or offline audio regression
  probes (`third_party/glasswave-cavern/test/Program.cs`).

---

## 3. Upstream Upgrade & Patch-Pruning SOP

Whenever auditing or upgrading dependencies:

1. **Check upstream status**
   - For git-pinned repos (`apple-music-linux`, `libhybris-win`, `Cavern`, `ESLyric-LyricsSource`):
     inspect `git ls-remote <repo> <branch>` or `git log <old>..<new>` in the `.local/` clone.
   - For release-pinned dependencies (`foobar2000-sdk`, `foobar2000-dev`, `quickjs-ng`, `miniz`,
     `api-enhanced`, `aria2`): check official release pages/tags.
2. **Audit every local patch hunk & workaround against the upstream diff**
   - Review every commit between the old pin and new pin touching files we patch or work around.
   - If upstream fixed a bug or adopted a hook, **delete** our obsolete patch hunk or adapter
     workaround rather than carrying dead overrides forward.
   - Verify remaining hunks apply cleanly (`git apply --check`). If context shifted, rebase in
     the `.local/` checkout and re-export via `git diff`.
3. **Update pins & integrity metadata**
   - Update `third_party/deps.lock.json`, `scripts/applemusic/windows/source-pins.json`,
     `third_party/glasswave-cavern/source-pin.json`, or C++ constants (`netease/service.cpp`,
     `download/aria2.cpp`).
   - For `foobar2000-dev`, verify the downloaded installer's Authenticode signature matches
     `CN=Illustrate Ltd, O=Illustrate Ltd, L=Bury, C=GB`.
   - For `api-enhanced`, update `kServiceSha256` from the verified release tarball.
   - Update Section 1 of this file (`docs/third-party.md`) and `third_party/README.md`.
4. **Rebuild & verify affected components**
   - Cavern changed → run `pwsh -File scripts/applemusic/atmos/build-renderer.ps1` (all real EC-3
     probes must pass).
   - Adam / `libhybris-win` changed → run `pwsh -File scripts/setup-applemusic.ps1`.
   - Any C++ / SDK / library change → run `pwsh -File scripts/build.ps1` (Release|x64, 0 warnings
     at `/W4`).
5. **Deploy safely to `.local/fb2k-dev`**
   - Confirm `.local/fb2k-dev/foobar2000.exe` is **not running** (if running, ask the owner to
     close it first; never kill their active player session).
   - **Never pass `-Force` to `scripts/bootstrap.ps1`** when updating `foobar2000-dev` — `-Force`
     deletes `.local/fb2k-dev` including `profile/`. Run plain `pwsh -File scripts/bootstrap.ps1`
     to unpack cleanly over the binaries while keeping `profile/` intact.
   - Deploy updated DLLs via `pwsh -File scripts/run-dev.ps1 -NoLaunch`.
6. **Commit & record**
   - Keep dependency upgrades in their own clean commit (Linux-kernel style, `deps: ...`) and
     summarize what changed and which patches were kept or dropped in `docs/plan/status.md`.
