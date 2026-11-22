# third_party

Upstream dependencies are fetched by `scripts/bootstrap.ps1`. The source-only Glasswave
Cavern adapter described below is tracked separately from fetched upstream code. For the
complete inventory of all external codebases (including runtime helpers, lyric scripts,
patch authoring rules and the upgrade SOP), see [docs/third-party.md](../docs/third-party.md).

| Dependency | Version | Destination | License |
| --- | --- | --- | --- |
| foobar2000 SDK | 2026-10-01 | `third_party/foobar2000-sdk` | BSD-style (pfc/libPPUI zlib-style) |
| foobar2000 (dev instance) | 2.26.3 x64 | `.local/fb2k-dev` | freeware, local testing only |
| quickjs-ng amalgamation | 0.18.0 | `third_party/quickjs-ng` | MIT |
| miniz amalgamation | 3.1.2 | `third_party/miniz` | MIT |

To upgrade: update the URL and version (and Authenticode subject for executables) in
`deps.lock.json`, ensure `.local/fb2k-dev/foobar2000.exe` is closed, run
`pwsh -File scripts/bootstrap.ps1` (do **not** pass `-Force`, which wipes `.local/fb2k-dev/profile/`),
and rebuild.

## Apple Music helper

`scripts/setup-applemusic.ps1` builds the native Windows helper from Adam nightly and
libhybris-win at the revisions recorded in
`scripts/applemusic/windows/source-pins.json`. Upstream code remains in isolated ignored
checkouts/worktrees; only the maintained adapter patches, source pins and build/lifecycle
scripts are tracked. Upstream notices remain in their source trees. See the existing Adam
and ALAC notices under `scripts/applemusic/licenses` and the per-file upstream notices for
libhybris-win/Android compatibility code. The helper performs session, transport and
container operations; audio decoding stays in foobar2000. Runtime files and account state
remain under `.local/applemusic` and are never committed. The earlier Linux/WSL proof is
retained for reference; the default setup and runtime do not depend on WSL.

## Atmos object renderer

`third_party/glasswave-cavern` contains Glasswave's NativeAOT C ABI adapter and source pin,
not a vendored Cavern checkout. `scripts/applemusic/atmos/build-renderer.ps1` fetches the
pinned source into `.local`, applies the tracked buffer-drain adapter patch, builds the
native DLL and verifies real EC-3/JOC fixtures. Its manifest records the pinned source and
independent/interleaved render results. PCM comparisons validate audio rendering; ordinary
copy/build artifacts are not hashed. Binary products stay local.

Cavern uses its own license, including source/creator links and sale/advertising/public-use
conditions. See the adapter's `NOTICE.md` and complete upstream license copied beside the
development renderer. Neither upstream source nor its embedded audio resources are
relicensed as Glasswave code.
