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

int Get(cfg_int& value, const Range& range) {
    return static_cast<int>(std::clamp<int64_t>(value.get(), range.min, range.max));
}

void Set(cfg_int& value, const Range& range, int v) { value.set(std::clamp(v, range.min, range.max)); }

}  // namespace

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
