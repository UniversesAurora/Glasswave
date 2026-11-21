#include "pch.h"
#include "app/settings.h"

namespace glasswave::settings {

namespace {

using cfg_var_modern::cfg_int;

// {8D3A51C0-6E2F-4B19-A47C-1F5E9B2D7C40} and following
cfg_int cfg_thumbnails({0x8d3a51c0, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                       kThumbnailCache.defaultValue);
cfg_int cfg_coverThreads({0x8d3a51c1, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                         kCoverThreads.defaultValue);
cfg_int cfg_listBitmaps({0x8d3a51c2, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                        kListBitmaps.defaultValue);
cfg_int cfg_playHistory({0x8d3a51c3, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                        kPlayHistory.defaultValue);
cfg_int cfg_playHistoryRows({0x8d3a51c4, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                            kPlayHistoryRows.defaultValue);
cfg_int cfg_volumeTipDelay({0x8d3a51c5, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                           kVolumeTipDelay.defaultValue);
cfg_int cfg_lyricLine({0x8d3a51c6, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}}, 1);
cfg_int cfg_lyricSize({0x8d3a51c7, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                      kLyricSize.defaultValue);
cfg_int cfg_translationSize({0x8d3a51c8, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                            kTranslationSize.defaultValue);
cfg_var_modern::cfg_string cfg_lyricFonts({0x8d3a51c9, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                                          "");
cfg_int cfg_hideTranslation({0x8d3a51ce, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}}, 0);
cfg_int cfg_lyricsCopy({0x8d3a51cd, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}}, 0);
cfg_int cfg_swapTranslation({0x8d3a51cc, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}}, 0);
cfg_int cfg_playerTimeRemaining({0x8d3a51cb, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}}, 0);
cfg_int cfg_tint({0x8d3a51cf, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}}, kTint.defaultValue);
cfg_int cfg_tintIntensity({0x8d3a51d0, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                          kTintIntensity.defaultValue);
cfg_int cfg_autoLoadCds({0x8d3a51d1, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}}, 1);
// New variables take a fresh random GUID: the run above is also continued elsewhere
// (search_scope.cpp, recent_lists.cpp), and a reused GUID shares its stored value.
// {370497FA-4228-4D2D-9704-4B648C510457}
cfg_int cfg_songTags({0x370497fa, 0x4228, 0x4d2d, {0x97, 0x04, 0x4b, 0x64, 0x8c, 0x51, 0x04, 0x57}}, 1);
// "left top right bottom" in screen px; empty until the mini player has been shown.
// {11CD7F56-CA0D-424C-8AC9-AFACEAC2569F}
cfg_var_modern::cfg_string cfg_miniPlayerRect({0x11cd7f56, 0xca0d, 0x424c, {0x8a, 0xc9, 0xaf, 0xac, 0xea, 0xc2, 0x56, 0x9f}},
                                              "");
int g_lyricStyle = 0;

int Get(cfg_int& value, const Range& range) {
    return static_cast<int>(std::clamp<int64_t>(value.get(), range.min, range.max));
}

void Set(cfg_int& value, const Range& range, int v) { value.set(std::clamp(v, range.min, range.max)); }

}  // namespace

int LyricSizePercent() { return Get(cfg_lyricSize, kLyricSize); }
void SetLyricSizePercent(int percent) {
    Set(cfg_lyricSize, kLyricSize, percent);
    ++g_lyricStyle;
}
int TranslationSizePercent() { return Get(cfg_translationSize, kTranslationSize); }
void SetTranslationSizePercent(int percent) {
    Set(cfg_translationSize, kTranslationSize, percent);
    ++g_lyricStyle;
}
std::wstring LyricFonts() {
    return pfc::stringcvt::string_wide_from_utf8(cfg_lyricFonts.get().c_str()).get_ptr();
}
void SetLyricFonts(const std::wstring& fonts) {
    cfg_lyricFonts.set(pfc::stringcvt::string_utf8_from_wide(fonts.c_str()).get_ptr());
    ++g_lyricStyle;
}
std::vector<std::wstring> LyricFontList() {
    std::vector<std::wstring> list;
    const std::wstring all = LyricFonts();
    size_t at = 0;
    while (at <= all.size()) {
        size_t end = all.find_first_of(L",;", at);
        if (end == std::wstring::npos) end = all.size();
        std::wstring name = all.substr(at, end - at);
        const size_t first = name.find_first_not_of(L" \t\"'"), last = name.find_last_not_of(L" \t\"'");
        if (first != std::wstring::npos) list.push_back(name.substr(first, last - first + 1));
        at = end + 1;
    }
    return list;
}
bool HideTranslation() { return cfg_hideTranslation.get() != 0; }
void SetHideTranslation(bool hide) { cfg_hideTranslation.set(hide ? 1 : 0); }
int LyricsCopyOptions() { return static_cast<int>(cfg_lyricsCopy.get()); }
void SetLyricsCopyOptions(int flags) { cfg_lyricsCopy.set(flags); }
bool SwapTranslation() { return cfg_swapTranslation.get() != 0; }
void SetSwapTranslation(bool swap) {
    cfg_swapTranslation.set(swap ? 1 : 0);
    ++g_lyricStyle;
}
int LyricStyleGeneration() { return g_lyricStyle; }

int Tint() { return Get(cfg_tint, kTint); }
void SetTint(int tint) { Set(cfg_tint, kTint, tint); }
int TintIntensity() { return Get(cfg_tintIntensity, kTintIntensity); }
void SetTintIntensity(int slider) { Set(cfg_tintIntensity, kTintIntensity, slider); }

bool AutoLoadCds() { return cfg_autoLoadCds.get() != 0; }
void SetAutoLoadCds(bool load) { cfg_autoLoadCds.set(load ? 1 : 0); }

bool LyricLineShown() { return cfg_lyricLine.get() != 0; }
bool SongTagsShown() { return cfg_songTags.get() != 0; }
void SetSongTagsShown(bool shown) { cfg_songTags.set(shown ? 1 : 0); }
void SetLyricLineShown(bool shown) { cfg_lyricLine.set(shown ? 1 : 0); }
bool PlayerTimeRemaining() { return cfg_playerTimeRemaining.get() != 0; }
void SetPlayerTimeRemaining(bool remaining) { cfg_playerTimeRemaining.set(remaining ? 1 : 0); }

std::optional<RECT> MiniPlayerRect() {
    RECT r{};
    if (sscanf_s(cfg_miniPlayerRect.get().c_str(), "%ld %ld %ld %ld", &r.left, &r.top, &r.right, &r.bottom) != 4 ||
        r.right <= r.left || r.bottom <= r.top)
        return std::nullopt;
    return r;
}
void SetMiniPlayerRect(const RECT& r) {
    char text[64];
    sprintf_s(text, "%ld %ld %ld %ld", r.left, r.top, r.right, r.bottom);
    cfg_miniPlayerRect.set(text);
}
int ThumbnailCacheEntries() { return Get(cfg_thumbnails, kThumbnailCache); }
void SetThumbnailCacheEntries(int entries) { Set(cfg_thumbnails, kThumbnailCache, entries); }
int CoverThreads() { return Get(cfg_coverThreads, kCoverThreads); }
void SetCoverThreads(int threads) { Set(cfg_coverThreads, kCoverThreads, threads); }
int ListBitmapCache() { return Get(cfg_listBitmaps, kListBitmaps); }
void SetListBitmapCache(int bitmaps) { Set(cfg_listBitmaps, kListBitmaps, bitmaps); }
int PlayHistoryLimit() { return Get(cfg_playHistory, kPlayHistory); }
void SetPlayHistoryLimit(int records) { Set(cfg_playHistory, kPlayHistory, records); }
int PlayHistoryRows() { return Get(cfg_playHistoryRows, kPlayHistoryRows); }
void SetPlayHistoryRows(int rows) { Set(cfg_playHistoryRows, kPlayHistoryRows, rows); }
int VolumeTipDelayMs() { return Get(cfg_volumeTipDelay, kVolumeTipDelay); }
void SetVolumeTipDelayMs(int ms) { Set(cfg_volumeTipDelay, kVolumeTipDelay, ms); }

}  // namespace glasswave::settings
