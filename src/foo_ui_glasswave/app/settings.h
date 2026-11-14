#pragma once

// Glasswave's user-tunable settings (Preferences > Display > Glasswave). Each value is
// stored in foobar2000's configuration and clamped to its range when read, so code can
// use it directly. Provider-specific settings live with their provider (netease/config.h).
namespace glasswave::settings {

// Posted to the main window after the preferences page applied changes.
inline constexpr UINT kChangedMessage = WM_APP + 0x31;

struct Range {
    int min, max, defaultValue;
};

// The single line of lyrics under the playing song (library window, mini player, full screen),
// switched from its context menu.
bool LyricLineShown();
void SetLyricLineShown(bool shown);

// The player's time counter counts down (the time left) instead of up; a click switches it.
bool PlayerTimeRemaining();
void SetPlayerTimeRemaining(bool remaining);

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

// The lyrics view (library window, mini player, full screen): the lines' size and that of
// their translations, each in percent of the size the view picks for its width; the font
// families, tried in order for every character (then Windows' own fallback; none: the
// interface font); and whether translations stand above their lines instead of below.
inline constexpr Range kLyricSize{50, 200, 100};
inline constexpr Range kTranslationSize{25, 150, 55};
int LyricSizePercent();
void SetLyricSizePercent(int percent);
int TranslationSizePercent();
void SetTranslationSizePercent(int percent);
std::wstring LyricFonts(); // as typed: names separated by commas
void SetLyricFonts(const std::wstring& fonts);
std::vector<std::wstring> LyricFontList();
// The translation shown as the lyric (its size, sung word by word) and the original as the
// smaller line under it; only the texts change places.
// The lyrics' Copy... dialog's check boxes, as bits (metadata 1, timestamps 2, merge 4,
// references 8, standard 16).
int LyricsCopyOptions();
void SetLyricsCopyOptions(int flags);
bool SwapTranslation();
// The small line under each lyric hidden: the translation, or the original when swapped.
bool HideTranslation();
void SetHideTranslation(bool hide);
void SetSwapTranslation(bool swap);
// Bumped by every change above, so views can tell their layout is out of date.
int LyricStyleGeneration();

// Hover time before the volume bubble appears (dragging shows it at once).
inline constexpr Range kVolumeTipDelay{0, 3000, 800};
int VolumeTipDelayMs();
void SetVolumeTipDelayMs(int ms);

}  // namespace glasswave::settings
