#pragma once

// Glasswave's user-tunable settings (Preferences > Display > Glasswave). Each value is
// stored in foobar2000's configuration and clamped to its range when read, so code can
// use it directly. Provider-specific settings live with their provider (netease/config.h).
namespace glasswave::settings {

struct Range {
    int min, max, defaultValue;
};

// Decoded cover thumbnails kept in memory (all lists, grids and headers).
inline constexpr Range kThumbnailCache{100, 5000, 600};
int ThumbnailCacheEntries();
void SetThumbnailCacheEntries(int entries);

// Threads decoding covers in the background; takes effect after a restart.
inline constexpr Range kCoverThreads{1, 8, 3};
int CoverThreads();
void SetCoverThreads(int threads);

// Covers each list keeps as device bitmaps while scrolling and switching views.
inline constexpr Range kListBitmaps{32, 1024, 128};
int ListBitmapCache();
void SetListBitmapCache(int bitmaps);

// Songs remembered by Recently Played / Most Played; the least recently played go first.
inline constexpr Range kPlayHistory{100, 20000, 2000};
int PlayHistoryLimit();
void SetPlayHistoryLimit(int records);

// Rows shown on the Recently Played and Most Played pages.
inline constexpr Range kPlayHistoryRows{20, 1000, 100};
int PlayHistoryRows();
void SetPlayHistoryRows(int rows);

// Hover time before the volume bubble appears (dragging shows it at once).
inline constexpr Range kVolumeTipDelay{0, 3000, 800};
int VolumeTipDelayMs();
void SetVolumeTipDelayMs(int ms);

}  // namespace glasswave::settings
