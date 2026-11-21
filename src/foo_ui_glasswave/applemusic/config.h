#pragma once

namespace glasswave::applemusic {

enum class AppleMusicDisplayRegion : int {
    Account = 0,
    Japan = 1,
    China = 2,
};

// Display names may use another catalog localization without changing the signed-in
// account's storefront, library IDs, playback assets or audio selection.
AppleMusicDisplayRegion GetAppleMusicDisplayRegion();
void SetAppleMusicDisplayRegion(AppleMusicDisplayRegion region);

// Apple Music's "Audio Quality" choices: AAC 256 kbps, ALAC up to 24-bit/48 kHz, or ALAC up to
// 24-bit/192 kHz. Sampled when a track starts, like the Atmos settings below.
enum class AudioQuality : int {
    High = 0,
    Lossless = 1,
    HiResLossless = 2,
};
AudioQuality GetAudioQuality();
void SetAudioQuality(AudioQuality quality);

// Render layouts describe decoded output, not HDMI bitstream pass-through modes.
enum class AtmosOutput : int {
    StereoSpeakers = 0,
    Headphones = 1,
    Speakers51 = 2,
    Speakers71 = 3,
};

// Off uses the audio quality above. Consumers sample these settings
// when a track starts; changing them does not reset the current playlist.
bool GetAtmosEnabled();
void SetAtmosEnabled(bool enabled);

AtmosOutput GetAtmosOutput();
void SetAtmosOutput(AtmosOutput output);

// Test-only playback preference override. Values are thread-local, affect only
// the current decoder thread, and never change the persisted global settings.
class ScopedAtmosPlaybackOverride {
  public:
    ScopedAtmosPlaybackOverride(bool enabled, AtmosOutput output) noexcept;
    ~ScopedAtmosPlaybackOverride() noexcept;

    ScopedAtmosPlaybackOverride(const ScopedAtmosPlaybackOverride&) = delete;
    ScopedAtmosPlaybackOverride& operator=(const ScopedAtmosPlaybackOverride&) = delete;

  private:
    bool m_hadEnabled = false;
    bool m_previousEnabled = false;
    bool m_hadOutput = false;
    AtmosOutput m_previousOutput = AtmosOutput::StereoSpeakers;
};

// Runtime readiness reported by the input after loading the native object renderer. It is not
// persisted and does not disable the preference; missing renderers fall back to lossless audio.
bool IsAtmosRendererReady();
void SetAtmosRendererReady(bool ready);

} // namespace glasswave::applemusic
