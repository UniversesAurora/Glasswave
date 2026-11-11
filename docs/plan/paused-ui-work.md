# Restored Claude UI work

The owner's Claude interface work was paused while its quota recovers.
On November 12 the owner requested that the current repairs be committed and the
Claude work be restored. It is now restored as uncommitted working-tree changes.
The original backup remains in Git stash `d1f5aeb`, named
`paused-ui-work-before-applemusic-503-fix-20261111`.

The stash contains six tracked files and four new files. Git stores the new
`album_wall.cpp/.h` and `player_window.cpp/.h` files in the stash's third parent.
They are not missing when the ordinary stash diff shows only six paths.

The streaming repairs were committed before applying the stash. Restoration
completed without conflicts, including Claude's pane reparenting and the current
fixed viewport changes. Do not apply the stash again. Retain the backup until the
owner's resumed Claude work has been integrated; do not commit unfinished Claude
changes as part of the streaming-repair checkpoint.

The installed development UI component was separately copied to
`.local/tests/paused-ui-runtime-20261111`. Streaming-helper repairs do not
require replacing that UI component. The restored combined source has not been
rebuilt or deployed. The owner requested no further investigation or tests at
this checkpoint.

The live development profile is authoritative. Capture fresh state before any
probe that changes it; historical backup paths are not permission to overwrite
later owner changes.

## Album wall scale follow-up

The restored `AlbumWall` code has a DPI-scale surface rebinding issue. On a scale
change it nulls the cached slot surfaces and repaints them, but existing tile
visuals can retain brushes for the old surfaces because the rebuild only
reassigns tiles with invalid slot indices. Rebind every tile after a scale
change if the owner resumes that task. This note does not authorize continuing
the investigation now. Do not repair the stash in place.
