#include "pch.h"
#include "netease/handle.h"

#include <cstdlib>

namespace glasswave::netease {

namespace {

bool ParseDigits(std::string_view text, int64_t& value) {
    if (text.empty()) return false;
    const std::string copy(text);
    char* end = nullptr;
    const long long parsed = std::strtoll(copy.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed <= 0) return false;
    value = static_cast<int64_t>(parsed);
    return true;
}

}  // namespace

std::string SongPath(int64_t id) { return "ncm://song/" + std::to_string(id); }

bool ParseSongPath(const char* path, int64_t& id) {
    static constexpr std::string_view prefix = "ncm://song/";
    if (!path) return false;
    const std::string_view text(path);
    if (text.size() <= prefix.size() || text.substr(0, prefix.size()) != prefix) return false;
    return ParseDigits(text.substr(prefix.size()), id);
}

std::string CoverPath(bool playlist, int64_t id) {
    return std::string("ncm://cover/") + (playlist ? "playlist/" : "song/") + std::to_string(id);
}

bool ParseCoverPath(const char* path, bool& playlist, int64_t& id) {
    static constexpr std::string_view songPrefix = "ncm://cover/song/";
    static constexpr std::string_view playlistPrefix = "ncm://cover/playlist/";
    if (!path) return false;
    const std::string_view text(path);
    if (text.size() > songPrefix.size() && text.substr(0, songPrefix.size()) == songPrefix) {
        playlist = false;
        return ParseDigits(text.substr(songPrefix.size()), id);
    }
    if (text.size() > playlistPrefix.size() && text.substr(0, playlistPrefix.size()) == playlistPrefix) {
        playlist = true;
        return ParseDigits(text.substr(playlistPrefix.size()), id);
    }
    return false;
}

std::wstring JoinArtists(const std::vector<std::wstring>& artists) {
    std::wstring out;
    for (size_t i = 0; i < artists.size(); ++i) {
        if (i) out += L"; ";
        out += artists[i];
    }
    return out;
}

}  // namespace glasswave::netease
