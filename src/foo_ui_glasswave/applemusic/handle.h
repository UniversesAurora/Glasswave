#pragma once

#include "stream/model.h"

#include <string>
#include <string_view>

namespace glasswave::applemusic {

// The input claims apple://song/<id>. Ids are percent-encoded UTF-8 path segments so a
// catalog id and a library id cannot be confused by punctuation or a second slash.
std::string SongPath(std::wstring_view id);
bool ParseSongPath(const char* path, stream::Id& id);

enum class CoverKind {
    Track,
    Album,
    Artist,
    Station,
    Playlist
};

// Non-playable paths used by the album-art extractor.
std::string CoverPath(CoverKind kind, std::wstring_view id);
bool ParseCoverPath(const char* path, CoverKind& kind, stream::Id& id);

bool IsLibraryId(std::wstring_view id);
std::wstring TrackResourceType(std::wstring_view id);

} // namespace glasswave::applemusic
