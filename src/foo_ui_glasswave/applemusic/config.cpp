#include "pch.h"
#include "applemusic/config.h"
#include "cache/prefetch_host.h"

#include <atomic>
#include <cstdint>
#include <optional>

namespace glasswave::applemusic {

namespace {

// {A2C7E6D3-3B4F-49A0-8E2D-6C5B7A1F9043}
cfg_var_modern::cfg_int cfg_displayRegion(
    {0xa2c7e6d3, 0x3b4f, 0x49a0, {0x8e, 0x2d, 0x6c, 0x5b, 0x7a, 0x1f, 0x90, 0x43}}, 0);
// {A2C7E6D1-3B4F-49A0-8E2D-6C5B7A1F9043}
cfg_var_modern::cfg_int cfg_atmosEnabled(
    {0xa2c7e6d1, 0x3b4f, 0x49a0, {0x8e, 0x2d, 0x6c, 0x5b, 0x7a, 0x1f, 0x90, 0x43}}, 0);
// {A2C7E6D2-3B4F-49A0-8E2D-6C5B7A1F9043}
cfg_var_modern::cfg_int cfg_atmosOutput(
    {0xa2c7e6d2, 0x3b4f, 0x49a0, {0x8e, 0x2d, 0x6c, 0x5b, 0x7a, 0x1f, 0x90, 0x43}},
    static_cast<int64_t>(AtmosOutput::StereoSpeakers));
std::atomic_bool atmosRendererReady = false;
thread_local std::optional<bool> playbackAtmosEnabledOverride;
thread_local std::optional<AtmosOutput> playbackAtmosOutputOverride;

} // namespace

AppleMusicDisplayRegion GetAppleMusicDisplayRegion() {
    switch (static_cast<AppleMusicDisplayRegion>(cfg_displayRegion.get())) {
    case AppleMusicDisplayRegion::Japan:
    case AppleMusicDisplayRegion::China:
        return static_cast<AppleMusicDisplayRegion>(cfg_displayRegion.get());
    case AppleMusicDisplayRegion::Account:
    default:
        return AppleMusicDisplayRegion::Account;
    }
}

void SetAppleMusicDisplayRegion(AppleMusicDisplayRegion region) {
    switch (region) {
    case AppleMusicDisplayRegion::Account:
    case AppleMusicDisplayRegion::Japan:
    case AppleMusicDisplayRegion::China:
        break;
    default:
        region = AppleMusicDisplayRegion::Account;
        break;
    }
    cfg_displayRegion.set(static_cast<int64_t>(region));
}

bool GetAtmosEnabled() {
    if (playbackAtmosEnabledOverride) return *playbackAtmosEnabledOverride;
    return cfg_atmosEnabled.get() != 0;
}

void SetAtmosEnabled(bool enabled) {
    const bool changed = (cfg_atmosEnabled.get() != 0) != enabled;
    cfg_atmosEnabled.set(enabled ? 1 : 0);
    if (changed) fb2k::inMainThread([] {
        if (auto* host = cache::PrefetchHost::Existing()) host->NotifyContextChanged();
    });
}

AtmosOutput GetAtmosOutput() {
    if (playbackAtmosOutputOverride) return *playbackAtmosOutputOverride;
    const auto value = static_cast<AtmosOutput>(cfg_atmosOutput.get());
    switch (value) {
    case AtmosOutput::StereoSpeakers:
    case AtmosOutput::Headphones:
    case AtmosOutput::Speakers51:
    case AtmosOutput::Speakers71:
        return value;
    }
    return AtmosOutput::StereoSpeakers;
}

ScopedAtmosPlaybackOverride::ScopedAtmosPlaybackOverride(bool enabled, AtmosOutput output) noexcept
    : m_hadEnabled(playbackAtmosEnabledOverride.has_value()),
      m_previousEnabled(playbackAtmosEnabledOverride.value_or(false)),
      m_hadOutput(playbackAtmosOutputOverride.has_value()),
      m_previousOutput(playbackAtmosOutputOverride.value_or(AtmosOutput::StereoSpeakers)) {
    playbackAtmosEnabledOverride = enabled;
    playbackAtmosOutputOverride = output;
}

ScopedAtmosPlaybackOverride::~ScopedAtmosPlaybackOverride() noexcept {
    if (m_hadEnabled) {
        playbackAtmosEnabledOverride = m_previousEnabled;
    } else {
        playbackAtmosEnabledOverride.reset();
    }
    if (m_hadOutput) {
        playbackAtmosOutputOverride = m_previousOutput;
    } else {
        playbackAtmosOutputOverride.reset();
    }
}

void SetAtmosOutput(AtmosOutput output) {
    switch (output) {
    case AtmosOutput::StereoSpeakers:
    case AtmosOutput::Headphones:
    case AtmosOutput::Speakers51:
    case AtmosOutput::Speakers71:
        break;
    default:
        output = AtmosOutput::StereoSpeakers;
        break;
    }
    const bool changed = cfg_atmosOutput.get() != static_cast<int64_t>(output);
    cfg_atmosOutput.set(static_cast<int64_t>(output));
    if (changed) fb2k::inMainThread([] {
        if (auto* host = cache::PrefetchHost::Existing()) host->NotifyContextChanged();
    });
}

bool IsAtmosRendererReady() {
    return atmosRendererReady.load(std::memory_order_relaxed);
}

void SetAtmosRendererReady(bool ready) {
    atmosRendererReady.store(ready, std::memory_order_relaxed);
}

} // namespace glasswave::applemusic
