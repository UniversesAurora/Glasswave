#include "pch.h"
#include "netease/config.h"
#include "cache/prefetch_host.h"
#include "stream/playback_source.h"

namespace glasswave::netease {

namespace {

// {3B8E6C21-9F4A-4D7C-A251-6E8B3D1F7C90}
cfg_var_modern::cfg_int cfg_quality({0x3b8e6c21, 0x9f4a, 0x4d7c, {0xa2, 0x51, 0x6e, 0x8b, 0x3d, 0x1f, 0x7c, 0x90}}, 0);
// {2F8C4A91-6D3E-4B7A-8E15-3C7F9A2D6B40}
cfg_var_modern::cfg_int cfg_coverCacheMb({0x2f8c4a91, 0x6d3e, 0x4b7a, {0x8e, 0x15, 0x3c, 0x7f, 0x9a, 0x2d, 0x6b, 0x40}},
                                         kCoverCacheDefaultMb);
// {7C4D2E91-3A6B-4F18-9D72-5E1C8A4B6F23}
cfg_var_modern::cfg_string cfg_service_url({0x7c4d2e91, 0x3a6b, 0x4f18, {0x9d, 0x72, 0x5e, 0x1c, 0x8a, 0x4b, 0x6f, 0x23}}, "");
// {1F9A7C53-6E2D-4B80-853F-2A7D9C510E64}
cfg_var_modern::cfg_int cfg_account_id({0x1f9a7c53, 0x6e2d, 0x4b80, {0x85, 0x3f, 0x2a, 0x7d, 0x9c, 0x51, 0x0e, 0x64}}, 0);
// {5A2C8F17-7D3E-4A92-B61C-4F8E279A3D50}
cfg_var_modern::cfg_string cfg_account_name({0x5a2c8f17, 0x7d3e, 0x4a92, {0xb6, 0x1c, 0x4f, 0x8e, 0x27, 0x9a, 0x3d, 0x50}}, "");

std::wstring Wide(const pfc::string8& s) {
    return pfc::stringcvt::string_wide_from_utf8(s).get_ptr();
}

std::string Utf8(std::wstring_view s) {
    return pfc::stringcvt::string_utf8_from_wide(s.data(), s.size()).get_ptr();
}

}  // namespace

Quality GetQuality() {
    const int64_t v = cfg_quality.get();
    if (v < static_cast<int64_t>(Quality::Auto) || v > static_cast<int64_t>(Quality::Standard))
        return Quality::Auto;
    return static_cast<Quality>(v);
}

void SetQuality(Quality quality) {
    const bool changed = GetQuality() != quality;
    cfg_quality.set(static_cast<int64_t>(quality));
    stream::ResetSourceAvailability();
    if (changed) fb2k::inMainThread([] {
        if (auto* host = cache::PrefetchHost::Existing()) host->NotifyContextChanged();
    });
}

int CoverCacheLimitMb() {
    return static_cast<int>(std::clamp<int64_t>(cfg_coverCacheMb.get(), kCoverCacheMinMb, kCoverCacheMaxMb));
}

void SetCoverCacheLimitMb(int mb) { cfg_coverCacheMb.set(std::clamp(mb, kCoverCacheMinMb, kCoverCacheMaxMb)); }

std::wstring GetServiceUrlOverride() { return Wide(cfg_service_url.get()); }

void SetServiceUrlOverride(std::wstring_view url) { cfg_service_url.set(Utf8(url).c_str()); }

int64_t GetAccountId() { return cfg_account_id.get(); }

std::wstring GetAccountName() { return Wide(cfg_account_name.get()); }

void SetAccount(int64_t userId, std::wstring_view nickname) {
    cfg_account_id.set(userId);
    cfg_account_name.set(Utf8(nickname).c_str());
}

void ClearAccount() {
    cfg_account_id.set(0);
    cfg_account_name.set("");
}

}  // namespace glasswave::netease
