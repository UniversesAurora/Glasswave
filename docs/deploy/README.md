# Deployment, Packaging & Operations SOPs

This directory contains Glasswave's modular Standard Operating Procedures (SOPs) for
end-user installation, release packaging, personal daily-driver maintenance, and
development/production isolation. Documents cross-reference one another rather than
duplicating steps.

| Audience / Scenario | SOP Document | When to Use |
| --- | --- | --- |
| **End users** (public release) | [user-install.md](user-install.md) | Installing or updating `foo_ui_glasswave.fb2k-component` in foobar2000 x64, configuring cache/download folders, and supplying optional non-redistributable assets. |
| **Maintainers** (publishing) | [release-packaging.md](release-packaging.md) | Building, auditing, and publishing a clean `foo_ui_glasswave.fb2k-component` package with zero copyright or personal-data leakage. |
| **Owner** (personal daily driver) | [personal-setup.md](personal-setup.md) | Updating the owner's production install (`E:\music\foobar2000`), syncing local non-redistributable assets, and backing up or migrating to a new machine. |
| **Developers & agents** | [dev-vs-prod.md](dev-vs-prod.md) | Repository directory boundaries (`src/`, `dist/`, `userdata/`, `.local/`), dev vs prod isolation, port safety, and safe `.local/` cleanup. |

## Related References

- [Third-Party Source & Dependency Maintenance Guide](../third-party.md) — upstream pins,
  patch rules, and dependency upgrade SOP.
- [ADR 0005](../adr/0005-no-microsoft-assets.md) &
  [ADR 0007](../adr/0007-original-win7-assets-at-runtime.md) — Win7 artwork policy.
- [ADR 0011](../adr/0011-apple-music.md) &
  [ADR 0013](../adr/0013-atmos-object-rendering.md) — Apple Music & Dolby Atmos architecture.
