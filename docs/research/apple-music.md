# Research: Apple Music integration (deferred)

Status: not started; preliminary notes only.

- Apple Music content is DRM-protected (FairPlay). Decrypted audio cannot be routed through
  foobar2000's pipeline, so "lossless + exclusive output through foobar2000" is not
  achievable legitimately.
- Feasible directions:
  - Control-only: observe and control the Apple Music app through Windows'
    GlobalSystemMediaTransportControlsSessionManager (now playing, transport, artwork) and
    show it in Glasswave's transport.
  - Catalog/library browsing via MusicKit web APIs (needs an Apple developer token and the
    user's music token); playback would still happen in Apple's player.
- Exclusive-mode output would then be the Apple Music app's own capability, not ours.
- Revisit after M3; record findings here.
