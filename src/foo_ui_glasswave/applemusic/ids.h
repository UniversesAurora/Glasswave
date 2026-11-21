#pragma once

#include <string>
#include <string_view>

namespace glasswave::applemusic {

// Apple's own ids: a library item is "<kind>.<id>" (i. song, l. album, r. artist, p. playlist);
// anything else is a catalog id.
inline bool IsLibraryId(std::wstring_view id) {
    return id.size() > 2 && id[1] == L'.' &&
           (id[0] == L'i' || id[0] == L'l' || id[0] == L'r' || id[0] == L'p');
}

inline std::wstring TrackResourceType(std::wstring_view id) {
    return id.size() > 2 && id[0] == L'i' && id[1] == L'.' ? L"library-songs" : L"songs";
}

} // namespace glasswave::applemusic
