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

std::string CoverPath(CoverKind kind, int64_t id) {
    const char* name = kind == CoverKind::Playlist ? "playlist"
                       : kind == CoverKind::Artist ? "artist"
                       : kind == CoverKind::Album  ? "album"
                                                   : "song";
    return std::string("ncm://cover/") + name + "/" + std::to_string(id);
}

bool ParseCoverPath(const char* path, CoverKind& kind, int64_t& id) {
    struct Prefix {
        const char* text;
        CoverKind kind;
    };
    static constexpr Prefix kPrefixes[] = {{"ncm://cover/song/", CoverKind::Song},
                                           {"ncm://cover/playlist/", CoverKind::Playlist},
                                           {"ncm://cover/artist/", CoverKind::Artist},
                                           {"ncm://cover/album/", CoverKind::Album}};
    if (!path) return false;
    const std::string_view text(path);
    for (const auto& prefix : kPrefixes) {
        const std::string_view value(prefix.text);
        if (text.size() > value.size() && text.substr(0, value.size()) == value) {
            if (!ParseDigits(text.substr(value.size()), id)) return false;
            kind = prefix.kind;
            return true;
        }
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
