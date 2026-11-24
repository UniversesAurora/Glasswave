#pragma once

namespace glasswave::applemusic {

enum class AppleMusicDisplayRegion : int {
    Account = 0,
    Japan = 1,
    China = 2,
};

enum class AppleMusicDisplayScope : int {
    None = 0,
    Names = 1 << 0,      // Song titles, artist names, album names (and their artwork)
    Playlists = 1 << 1,  // Editorial/non-owned playlist names, descriptions, covers
    Editorial = 1 << 2,  // Artist bios, album descriptions, genres, composers
    Lyrics = 1 << 3,     // Apple Music lyrics translation & script replacement locale
};

inline constexpr int kAppleMusicDisplayScopeMask = 0x0F;
inline constexpr int kAppleMusicDisplayScopeMetadataMask =
    static_cast<int>(AppleMusicDisplayScope::Names) | static_cast<int>(AppleMusicDisplayScope::Playlists) |
    static_cast<int>(AppleMusicDisplayScope::Editorial);
inline constexpr int kAppleMusicDisplayScopeDefault = static_cast<int>(AppleMusicDisplayScope::Names);

// Display names may use another catalog localization without changing the signed-in
// account's storefront, library IDs, playback assets or audio selection.
AppleMusicDisplayRegion GetAppleMusicDisplayRegion();
void SetAppleMusicDisplayRegion(AppleMusicDisplayRegion region);

int GetAppleMusicDisplayScope();
void SetAppleMusicDisplayScope(int mask);
bool HasAppleMusicDisplayScope(AppleMusicDisplayScope flag);
void SetAppleMusicDisplayScopeFlag(AppleMusicDisplayScope flag, bool enabled);

inline bool RegionalNamesActive() {
    return GetAppleMusicDisplayRegion() != AppleMusicDisplayRegion::Account &&
           HasAppleMusicDisplayScope(AppleMusicDisplayScope::Names);
}
inline bool RegionalPlaylistsActive() {
    return GetAppleMusicDisplayRegion() != AppleMusicDisplayRegion::Account &&
           HasAppleMusicDisplayScope(AppleMusicDisplayScope::Playlists);
}
inline bool RegionalEditorialActive() {
    return GetAppleMusicDisplayRegion() != AppleMusicDisplayRegion::Account &&
           HasAppleMusicDisplayScope(AppleMusicDisplayScope::Editorial);
}
inline bool RegionalLyricsActive() {
    return GetAppleMusicDisplayRegion() != AppleMusicDisplayRegion::Account &&
           HasAppleMusicDisplayScope(AppleMusicDisplayScope::Lyrics);
}
inline bool RegionalSongsActive() { return RegionalNamesActive() || RegionalEditorialActive(); }
inline bool RegionalAlbumsActive() { return RegionalNamesActive() || RegionalEditorialActive(); }
inline bool RegionalArtistsActive() { return RegionalNamesActive() || RegionalEditorialActive(); }
inline bool RegionalAnyMetadataActive() {
    return RegionalNamesActive() || RegionalPlaylistsActive() || RegionalEditorialActive();
}

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
