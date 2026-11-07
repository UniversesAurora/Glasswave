# Status

Updated: 2026-11-07

## Where things stand

M0 is complete and runs in the dev instance (foobar2000 2.26.2 x64, SDK 2026-10-01):
Aero glass, Win7 frame/shadow/caption buttons, WMP 12 library layout, working transport.

The first visual pass was rejected by the owner (controls jagged, caption buttons too harsh,
wrong layout). The second pass re-measured everything from native Win7 screenshots
(docs/design/visual-spec.md) and rebuilt the painters and layout accordingly. Awaiting the
owner's review of this pass.

## Next

1. Owner review of the current visuals; iterate on concrete differences.
2. Seek bar + time + now-playing thumbnail in the transport band.
3. Playlist view in the content area.

## Known issues

- DWM host backdrop blur is stronger than Win7's blur (no API to tune it).
- Hover/pressed colors are extrapolated, not measured.
- Navigation and command bars are empty placeholders.
- No device-lost handling yet.
