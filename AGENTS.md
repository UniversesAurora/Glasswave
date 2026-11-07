# Agent guide for Glasswave

This file is the single source of truth for coding agents (Claude Code reads it via
`CLAUDE.md`). Keep it short; link to docs instead of copying them.

## What this project is

A foobar2000 `user_interface` component (`foo_ui_glasswave.dll`) that recreates Windows 7
Aero Glass + WMP 12 + Windows Media Center visuals on Windows 11. The audio engine is
foobar2000's; we only build the UI. Read before changing anything substantial:

1. [docs/requirements.md](docs/requirements.md) — the owner's requirements (original text)
2. [docs/architecture.md](docs/architecture.md) — how the pieces fit
3. [docs/adr/](docs/adr) — decisions already made; do not re-litigate without a new ADR
4. [docs/design/visual-spec.md](docs/design/visual-spec.md) — measured Win7 values
5. [docs/plan/status.md](docs/plan/status.md) — where things stand, what is next

## Non-negotiables

- **Fidelity bar: 1:1 Windows 7 Aero / WMP 12 / WMC**, rendered crisper than the 96-DPI
  originals (vector, hairlines snapped to device pixels). "A blue translucent theme" is a
  failure. Every color/size must trace back to a measurement in `visual-spec.md`; add the
  measurement there when you introduce a value.
- Glass and effects stay **scoped to our own window**. Never patch themes, inject into or
  hook DWM, or change system settings.
- **No Microsoft bitmaps** (msstyles, WMP/WMC resources) in the repo (ADR 0005). Reference
  screenshots live in `.local/refs/` and are never committed.
- Reuse foobar2000 core services and mature components instead of re-implementing audio,
  library, DSP or output features.
- Idle means idle: no timers or looping animations unless something is hovered/focused or
  actually changing.
- Use only the repo's dev instance (`.local/fb2k-dev`). Never touch other foobar2000 installs.

## Workflow

```bash
pwsh -File scripts/bootstrap.ps1    # once, or after deps.lock.json changes
pwsh -File scripts/build.ps1        # Release|x64 by default; -Configuration Debug, -Platform Win32
pwsh -File scripts/run-dev.ps1      # deploy to .local/fb2k-dev and start it
```

- Release is the default because foobar2000 blocks start-up with a modal dialog for Debug
  components.
- Builds must stay warning-free at /W4.
- Verify visual changes in the running dev instance (screenshot + zoom) and compare with the
  reference images before claiming a visual task is done.
- Update `docs/plan/status.md` at the end of a work session (what changed, what is next).

## Conventions

- Language: code, comments, docs, commit messages and script output in **English**.
- C++20, `/W4`, `.clang-format` (Microsoft base, 110 columns). Namespaces `glasswave::*`.
- Units: layout in DIP; convert with the window scale; hairlines = `floor(scale)` device px.
- Commits: Linux-kernel style, see [docs/agent/commit-style.md](docs/agent/commit-style.md).
- Git: rebase / fast-forward only, no merge commits.
