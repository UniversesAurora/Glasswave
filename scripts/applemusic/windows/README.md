# Native Windows Apple Music helper

The runtime is Adam's pinned Go engine with Glasswave's authenticated encoded-stream
adapter and the upstream Windows DRM loader. Electron, VLC playback and helper decoding
are not built. `source-pins.json` records upstream revisions, reviewed patch paths and tool
versions. Runtime/session files stay under `.local/applemusic/windows`.

```powershell
pwsh -File scripts/setup-applemusic.ps1 -Action Check
pwsh -File scripts/setup-applemusic.ps1 -Action Start
pwsh -File scripts/run-dev.ps1
```

`run-dev.ps1` does not start or stop the helper. When the installed runtime manifest and
engine are present, it prepares `bridge.json` before starting the dev foobar2000 instance
if the production bridge is absent or already points at the default
`http://127.0.0.1:20025`. The file retains `token` and `url` and adds:

```json
{
  "managed": {
    "runtimeDirectory": "E:\\music\\Glasswave\\.local\\applemusic\\windows\\runtime"
  }
}
```

The runtime directory is always absolute. Writing the descriptor does not require a live
engine. If the private local API token is absent, `run-dev.ps1` creates it before writing
the descriptor. A non-default or malformed bridge is left unchanged so mock fixtures and
explicitly selected alternate endpoints keep their own lifecycle.

With `managed` present, the foobar2000 component starts `engine.exe` in the listed runtime
directory, builds the same environment as the manual `start.ps1` diagnostic flow, and owns
that child process through a Windows Job Object. Shutdown terminates only the process the
component created. An engine that was already started externally is not adopted or killed.
Manual `Start`, `Stop`, `Restart` and `Status` remain available for external diagnostics and
manage only the process recorded by the helper scripts.

Run the dev-only lifecycle regression (including external-service preservation) with:

```powershell
pwsh -File scripts/test-applemusic-lifecycle.ps1 -IncludeExternalPreservation
```

It checks authenticated direct launch, retained sign-in, normal/forced-parent cleanup
and restores the dev instance. It performs no Apple library writes.

Normal setup/start/stop/restart/status do not invoke WSL. Authentication is local; never
paste credentials or tokens into scripts or chat.

To rebuild without replacing a running runtime:

```powershell
pwsh -File scripts/setup-applemusic.ps1 -Action Build
```

This creates isolated pinned Git worktrees, applies the adapters, builds native libraries,
runs Go tests, links the engine, applies the PE stack setting and writes the runtime manifest
with schema 2, platform and generation time. The build checks pinned revisions and tool
versions; it does not use patch, artifact, deployment or payload hashes as the current
validation contract. Stop the owned service before promoting a tested build:

```powershell
pwsh -File scripts/setup-applemusic.ps1 -Action Stop
pwsh -File scripts/setup-applemusic.ps1 -Action Build -PromoteRuntime
pwsh -File scripts/setup-applemusic.ps1 -Action Start
```

The final `Start` above is optional during normal app-managed use: the next
`run-dev.ps1` launch prepares the descriptor, and foobar2000 starts the runtime.

Reuse the installed pinned Go/MinGW/LLVM/MSVC tools. `Prepare` provisions the pinned Go
version through mise. Build scripts report missing prerequisites instead of installing
unrelated toolchains. Android runtime files are acquired locally; account
state, binary artifacts and crash dumps are not committed.

The PE stack reserve/commit is 64 MiB/2 MiB. Guest ELF functions lack Windows stack probes:
the original 4 KiB commit caused an initialization access violation at a large stack frame.
The build uses the normal linker and `editbin /STACK`, not patches to Apple libraries.

For upstream updates, change the source pins and rebase the small adapter patches onto the
new revisions. Re-run native account/restart, real ALAC decode/seek, progressive fixture,
cancellation, artwork/library and mock write-contract checks before accepting a new build.
