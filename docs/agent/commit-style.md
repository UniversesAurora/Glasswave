# Commit message style

Linux-kernel style, in English:

```
subsystem: imperative summary under 72 characters

Why the change is needed: the problem or the requirement, with enough
context that a reader without the conversation understands it.

How it is done: the approach, the key design points, and alternatives
that were considered and rejected. Mention measurements or references
when visuals are involved.

How it was verified: build, run in the dev instance, screenshots
compared, tests run. Say plainly what was not verified.

Co-Authored-By: ...
```

- Wrap the body at 72 columns. Blank line after the summary.
- Subsystems: `aero`, `gfx`, `ui`, `build`, `scripts`, `docs`, `deps`.
- One logical change per commit; rebase or fast-forward only, no merge commits.
