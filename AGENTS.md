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
6. [docs/third-party.md](docs/third-party.md) — inventory, patch rules and upgrade SOP for all
   external codebases, runtime helpers and lyric scripts
7. [docs/deploy/README.md](docs/deploy/README.md) — packaging, end-user install, personal daily
   driver update and dev/prod isolation SOPs
8. [docs/agent/handoff.md](docs/agent/handoff.md) — the owner's working rules and the state
   left by the previous agent (read this first when you are new to the project)

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
pwsh -File scripts/build.ps1        # Release|x64 (x64 only); -Configuration Debug
pwsh -File scripts/run-dev.ps1      # deploy to .local/fb2k-dev and start it
```

- Release is the default because foobar2000 blocks start-up with a modal dialog for Debug
  components.
- Builds must stay warning-free at /W4.
- Run only necessary tests directly affected by the current changes. Do not run exhaustive
  test sweeps or repeat accepted tests whose code and assumptions have not changed.
- Prefer asking the owner to check visual changes and actual interaction in the running
  dev instance. Do not proactively automate the UI or take screenshots for visual checks;
  use those only when the owner explicitly requests them. Keep builds and meaningful
  automated functional tests autonomous, and distinguish them from owner visual acceptance.
- Update `docs/plan/status.md` at the end of a work session (what changed, what is next).

## Conventions

- **Codex only — delegation:** This rule applies only when the coordinating agent is Codex;
  it does not constrain OpenCode, Claude Code or other agents. Delegate substantial coding
  and investigation to subagents while the coordinator reads and decomposes the work,
  assigns bounded tasks, then reviews and accepts the results. Use the default subagent
  invocation without specifying a model or reasoning effort; the configured runtime owns
  model routing. Do not substitute a manually selected model, a free/manual OpenCode CLI or
  a separate CLI workflow. If default delegation fails, report the actual error and pause
  only delegation-dependent work.
  Verify the effective model recorded for a new or resumed session against the configured
  default before assigning substantial work. A resumed session may use a different model;
  preserve its handoff and use a fresh default invocation rather than overriding parameters.
  Thread-slot failures and generic provider errors do not establish an owner account quota.
  Delegate only through dedicated subagent tools, never by creating or forking ordinary
  app chats/tasks. Workers that need another split must ask the coordinator instead of
  creating user-visible side conversations.
- This is a development project: do not add or retain backward compatibility for older
  configuration, cache or storage formats. Keep one current format. When it changes,
  explicitly update the existing development data or remove only the affected derived
  storage and fetch it again. Do not hide migrations in runtime compatibility branches.
- Do not hash data unless a feature actually requires it (for example protocol signatures,
  cryptographic authentication, or an indexed lookup whose identity requires hashing).
  Do not verify identical content after ordinary assignment, copying, packaging or deployment.
  Use generation IDs and atomic publication for cache ownership, not redundant payload hashes.
- During memory-fault investigation, keep the repository memory watchdog running. Record
  system commit, available physical memory and kernel pools as well as process private bytes;
  stop only repository audio processes immediately when pressure crosses the safety limit.
- When API metadata, downloaded media and observed playback disagree, distinguish facts
  from hypotheses. Promptly ask the owner to verify the original file, official client or
  listening behavior when firsthand evidence is needed; do not unilaterally declare an
  upstream service wrong. Continue independent implementation and automated checks while
  that verification is pending, and report unresolved user-verification items explicitly.

- Language: code, comments, docs, commit messages and script output in **English**.
- C++20, `/W4`, `.clang-format` (Microsoft base, 110 columns). Namespaces `glasswave::*`.
- Units: layout in DIP; convert with the window scale; hairlines = `floor(scale)` device px.
- Commits: Linux-kernel style, see [docs/agent/commit-style.md](docs/agent/commit-style.md).
- Git: rebase / fast-forward only, no merge commits.
