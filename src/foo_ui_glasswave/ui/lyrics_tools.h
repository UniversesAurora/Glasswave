#pragma once

// The lyrics context menu's tools (ESLyric's): saving the lyrics to a file or the track's tag,
// copying them as text, the lyric's document in a window, the copy options and the timing
// offset dialogs, and the code pages a lyric file can be read in.

#include "lyrics/lyrics.h"

#include <string>
#include <utility>
#include <vector>

namespace glasswave::ui::lyrics_tools {

enum class SaveTarget {
    Default,     // next to a local track, else the profile's lyrics folder
    Tag,         // the track's lyrics tag (local files)
    Profile,     // <profile>\lyrics\Artist - Title.lrc
    TrackFolder, // next to the track (local files)
    Browse       // a file the owner picks
};

// Whether a target can take this track's lyrics (tags and the track's folder need a file).
bool CanSave(const metadb_handle_ptr& track, SaveTarget target);
// Writes `document` (UTF-8 with a byte order mark; the tag as text). Reports failures in a
// message box over `owner`; true when written (or, for the tag, when the update started).
bool Save(HWND owner, const metadb_handle_ptr& track, const std::wstring& document, SaveTarget target);

// Unicode text on the clipboard.
bool CopyText(HWND owner, const std::wstring& text);

// The copy options, remembered for the next time; `karaoke` enables "standard lyrics".
// True when the owner chose Copy.
bool ShowCopyDialog(HWND owner, lyrics::WriteOptions& options, bool karaoke);
// A read-only window with `header` (a few lines) over `text`, with Copy and Close.
void ShowTextWindow(HWND owner, const std::wstring& title, const std::wstring& header, const std::wstring& text);
// A signed number of milliseconds; true with `ms` set when the owner chose OK.
bool AskOffset(HWND owner, int& ms);

// The code pages offered for a lyric file, as ESLyric lists them.
std::vector<std::pair<unsigned, std::wstring>> CodePages();

}  // namespace glasswave::ui::lyrics_tools
