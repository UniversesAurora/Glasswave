# Glasswave Cavern adapter

This adapter links against Cavern and Cavern.Format from:

- Repository: https://github.com/VoidXH/Cavern
- Commit: `bbb76e9f85a2b53041297c52257a2f486afd8f26`
- Creator attribution: https://cavern.sbence.hu/

The Cavern source is not vendored in this repository. The build script fetches the
pinned revision below `.local/applemusic/renderers/cavern` and includes the complete
upstream licence beside the produced renderer.

Cavern's licence permits modification and free redistribution under its terms, but
prohibits selling the original or modified software and advertising in modified software.
Modified releases must link the upstream repository. Public or commercial use requires
creator attribution with a link; public use such as screenings additionally requires the
creator's permission. See the complete upstream licence accompanying the renderer for
the controlling terms. This adapter is currently built for local personal development.

The adapter uses Cavern's E-AC-3 JOC decoder and object renderer. It does not copy or
embed any Cavern demonstration assets or bitmaps. The linked Cavern assemblies carry
their own embedded resources required by the official library.
