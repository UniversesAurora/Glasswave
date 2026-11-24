#pragma once

#include "lyrics/lyrics.h"

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

// The tags a source gives a song (NetEase's 原唱), drawn before its artist in every view.
bool SongTagsShown();
void SetSongTagsShown(bool shown);

// The player's time counter counts down (the time left) instead of up; a click switches it.
bool PlayerTimeRemaining();
void SetPlayerTimeRemaining(bool remaining);

// Where the mini player was left (window rectangle, screen px), kept across restarts (owner,
// 2026-11-20: it came back small in the corner every start). Nothing until it has been shown.
std::optional<RECT> MiniPlayerRect();
void SetMiniPlayerRect(const RECT& rect);

// The glass's colour: one of Windows 7's sixteen (0 Sky ... 15 Frost, aero::kWin7Tints) or
// kTintFromCover, the playing song's cover; and Win7's "Color intensity" slider, 0..100
// (43: the Sky theme's alpha 0x6B).
inline constexpr int kTintFromCover = 16;
inline constexpr Range kTint{0, kTintFromCover, 0};
inline constexpr Range kTintIntensity{0, 100, 43};
int Tint();
void SetTint(int tint);
int TintIntensity();
void SetTintIntensity(int slider);

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

// The lyrics view (library window, mini player, full screen): the main line's size and the
// first / second secondary line sizes, each in percent of the size the view picks for its width;
// and the font families tried in order for every character (then Windows' own fallback).
inline constexpr Range kLyricSize{50, 200, 100};
inline constexpr Range kFirstSecondarySize{25, 150, 55};
inline constexpr Range kSecondSecondarySize{25, 150, 50};
int LyricSizePercent();
void SetLyricSizePercent(int percent);
int FirstSecondarySizePercent();
void SetFirstSecondarySizePercent(int percent);
int SecondSecondarySizePercent();
void SetSecondSecondarySizePercent(int percent);
std::wstring LyricFonts(); // as typed: names separated by commas
void SetLyricFonts(const std::wstring& fonts);
std::vector<std::wstring> LyricFontList();
// The lyrics' Copy... dialog's check boxes, as bits (metadata 1, timestamps 2, merge 4,
// references 8, standard 16).
int LyricsCopyOptions();
void SetLyricsCopyOptions(int flags);
// Which tracks (original, translation, pronunciation) are displayed, and in what order.
// At least one track stays enabled; DisplayOrder selects one of the 6 permutations.
bool ShowOriginalLyric();
void SetShowOriginalLyric(bool show);
bool ShowTranslationLyric();
void SetShowTranslationLyric(bool show);
bool ShowPronunciationLyric();
void SetShowPronunciationLyric(bool show);
inline constexpr Range kLyricDisplayOrder{0, 5, 0};
lyrics::DisplayOrder GetLyricDisplayOrder();
void SetLyricDisplayOrder(lyrics::DisplayOrder order);
lyrics::DisplayOptions GetLyricDisplayOptions();
// Highlighting of the first and second secondary lyric rows independently:
// Off keeps the row dim, Line highlights the whole row while active, WordProgress sweeps
// across the row (using real syllable timing when available, else main line progress).
enum class SecondaryLyricHighlight : int {
    Off = 0,
    Line = 1,
    WordProgress = 2,
};
inline constexpr Range kSecondaryLyricHighlight{0, 2, 0};
SecondaryLyricHighlight GetFirstSecondaryLyricHighlight();
void SetFirstSecondaryLyricHighlight(SecondaryLyricHighlight mode);
SecondaryLyricHighlight GetSecondSecondaryLyricHighlight();
void SetSecondSecondaryLyricHighlight(SecondaryLyricHighlight mode);
// Bumped by every change above, so views can tell their layout is out of date.
int LyricStyleGeneration();

// An audio CD is read as soon as it is inserted (its tracks listed and looked up); otherwise
// its drive is read only when opened.
bool AutoLoadCds();
void SetAutoLoadCds(bool load);

// Hover time before the volume bubble appears (dragging shows it at once).
inline constexpr Range kVolumeTipDelay{0, 3000, 800};
int VolumeTipDelayMs();
void SetVolumeTipDelayMs(int ms);

}  // namespace glasswave::settings
