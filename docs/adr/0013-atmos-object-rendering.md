# ADR 0013: Atmos object rendering inside the foobar2000 input

Status: accepted 2026-11-10. Real encoded delivery and full-song stereo rendering
are verified; the acceptance matrix is maintained in `docs/plan/apple-music.md`.

## Context

The owner requested actual Apple Music Dolby Atmos playback, an enable switch and
common listening layouts. The current device is a USB DAC feeding left/right speakers.
Ordinary E-AC-3 decoding discards JOC object metadata and is insufficient for this request.
The encoded-stream helper must continue to contain no audio decoder or PCM output.

## Decision

- Keep highest-available ALAC as the default. An explicit Atmos preference selects an
  available Atmos variant; confirmed absence uses the existing ALAC/AAC policy. Transport,
  authorization and renderer failures do not silently change the selected mix.
- Let the Go helper remux decrypted fragmented MP4 into elementary EC-3 access units.
  Sample bytes, including JOC/OAMD, remain unchanged. The ordinary MP4 route remains
  available; the elementary route is restricted to headless Atmos sessions.
- Decode JOC and render its objects through Cavern in the foobar2000 input component.
  A NativeAOT DLL provides a small versioned C ABI; no external audio process, browser
  player, system spatial effect or PCM-producing helper is introduced.
- Feed rendered float PCM into normal foobar2000 audio chunks, DSP and output. Report
  the source as E-AC-3/JOC and the actual rendered layout, rather than calling it ALAC or
  interpreting rendered float precision as lossless source bit depth.
- Support separate stereo-speaker, headphone, 5.1 and 7.1 targets. Stereo-speaker output
  does not apply headphone HRTF. Two physical speakers cannot reproduce a physical rear
  or overhead speaker layout. Headphones reserve 6 dB through Cavern's listener gain;
  speaker layouts retain the existing Cavern calibration. HDMI bitstream is separate.
- Pull elementary bytes progressively through a non-throwing callback. Cancellation
  aborts that callback and joins its renderer context before releasing the encoded cache.
  A seek can reopen the completed encoded cache and decode/discard to the exact sample.
- Keep per-decoder contexts independent, including read-ahead/gapless contexts. Cavern's
  global listener configuration is serialized only during configuration/render calls.
  The NativeAOT module stays loaded for the process lifetime; contexts are individually
  destroyed. Microsoft does not support unloading initialized NativeAOT libraries.
- Use the existing provider settings framework. Atmos defaults off, and the default
  layout is stereo speakers. Changes apply to subsequent tracks. Diagnostics are bounded,
  private and sanitized; there are no idle polling timers.

## Provenance

The Cavern pin, adapter, build entry point and attribution are in
`third_party/glasswave-cavern` and `scripts/applemusic/atmos/build-renderer.ps1`.
Upstream source and native build products remain in ignored local directories.
Cavern permits free distribution under its own terms, including source/creator links,
sale/advertising restrictions and permission for public use such as screenings. This
development build retains its notice and full license beside the renderer; the upstream
code is not relicensed as the adapter's own code.

## Evidence

- A real owner-authorized source has E-AC-3/Dolby Atmos profile at 48 kHz.
- The helper's EC-3 transport matches an independent copy-only remux byte for byte:
  17925120 bytes, SHA-256
  `d7df58dbdf5016564e36867cfe9d3e07f9e08b0093e3a5aa81665aec3b314b2f`.
- The source contains 5835 EC-3 frames of 1536 samples, totaling 8962560 PCM frames
  (186.720 seconds). An EOF adapter drains the final decoded buffer and returns only its
  valid samples; it neither drops the tail nor pads a partial render block with silence.
  The catalog duration is 186.251 seconds. Native rendering detects 15 dynamic objects;
  complete EOF, short-stream EOF and independent/interleaved context checks are required.
- The full, current matrix and remaining boundaries live in the implementation plan,
  avoiding a second independently maintained list of test statuses here.

## References

- [Dolby Digital Plus JOC](https://professionalsupport.dolby.com/s/article/What-is-Dolby-Digital-Plus-JOC-Joint-Object-Coding)
- [NativeAOT libraries and lifetime](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/libraries)
- [Cavern source](https://github.com/VoidXH/Cavern)
