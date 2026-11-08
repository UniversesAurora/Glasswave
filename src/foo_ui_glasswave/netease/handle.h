#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::netease {

// Stable metadb path for a NetEase song. The input plugin claims only this prefix.
std::string SongPath(int64_t id);
bool ParseSongPath(const char* path, int64_t& id);

// Non-playable paths used only by the album-art extractor.
std::string CoverPath(bool playlist, int64_t id);
bool ParseCoverPath(const char* path, bool& playlist, int64_t& id);

std::wstring JoinArtists(const std::vector<std::wstring>& artists);

}  // namespace glasswave::netease
