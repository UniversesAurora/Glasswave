# third_party

Fetched by `scripts/bootstrap.ps1`; only this README and `deps.lock.json` are committed.

| Dependency | Version | Destination | License |
| --- | --- | --- | --- |
| foobar2000 SDK | 2026-10-01 | `third_party/foobar2000-sdk` | BSD-style (pfc/libPPUI zlib-style) |
| foobar2000 (dev instance) | 2.26.2 x64 | `.local/fb2k-dev` | freeware, local testing only |

To upgrade: download the new file, put its URL and SHA-256 (and Authenticode subject for
executables) into `deps.lock.json`, run `pwsh -File scripts/bootstrap.ps1 -Force`, rebuild.
