#pragma once

#include "stream/model.h"

#include <string>
#include <vector>

namespace glasswave::applemusic {

// What Apple keeps of a song beyond its stream::Track and cover: the ids of its library and
// catalog entries, the library's flags and the names the track has no field for.
struct SongExtra {
    std::wstring catalogId;
    std::wstring libraryId;
    std::wstring libraryAlbumId;
    std::wstring reportingId;
    std::wstring isrc;
    std::wstring contentRating;
    bool customTitle = false;
    bool customAlbum = false;
    std::vector<std::wstring> genreNames;
    std::vector<std::wstring> composerNames;
    std::wstring composerName;
};

struct Song : SongExtra {
    stream::Track track;
    std::wstring coverUrl;
};

} // namespace glasswave::applemusic
