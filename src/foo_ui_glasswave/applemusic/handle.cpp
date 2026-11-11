#include "pch.h"
#include "applemusic/client.h"
#include "applemusic/handle.h"

#include <array>

namespace glasswave::applemusic {

namespace {

constexpr size_t kMaxIdBytes = 1024;

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool SafeByte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

bool ParseEncodedId(std::string_view segment, stream::Id& id) {
    if (segment.empty() || segment.size() > kMaxIdBytes * 3) return false;
    std::string bytes;
    bytes.reserve(segment.size());
    for (size_t i = 0; i < segment.size();) {
        const unsigned char c = static_cast<unsigned char>(segment[i]);
        if (c == '%') {
            if (i + 2 >= segment.size()) return false;
            const int hi = HexValue(segment[i + 1]);
            const int lo = HexValue(segment[i + 2]);
            if (hi < 0 || lo < 0) return false;
            bytes.push_back(static_cast<char>((hi << 4) | lo));
            i += 3;
        } else {
            if (!SafeByte(c)) return false;
            bytes.push_back(static_cast<char>(c));
            ++i;
        }
    }
    if (bytes.empty() || bytes.size() > kMaxIdBytes) return false;
    if (std::any_of(bytes.begin(), bytes.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
        return false;
    }

    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                          static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) return false;
    std::wstring wide(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                            wide.data(), count) != count) {
        return false;
    }
    id = std::move(wide);
    return true;
}

std::string PathSuffix(std::wstring_view id) {
    if (id.empty()) return {};
    const std::wstring encoded = Encode(id);
    return pfc::stringcvt::string_utf8_from_wide(encoded.c_str()).get_ptr();
}

} // namespace

std::string SongPath(std::wstring_view id) {
    const std::string suffix = PathSuffix(id);
    return suffix.empty() ? std::string() : "apple://song/" + suffix;
}

bool ParseSongPath(const char* path, stream::Id& id) {
    static constexpr std::string_view prefix = "apple://song/";
    if (!path) return false;
    const std::string_view text(path);
    if (text.size() <= prefix.size() || text.substr(0, prefix.size()) != prefix) return false;
    const std::string_view suffix = text.substr(prefix.size());
    if (suffix.find_first_of("/?#") != std::string_view::npos) return false;
    stream::Id parsed;
    if (!ParseEncodedId(suffix, parsed)) return false;
    id = std::move(parsed);
    return true;
}

std::string CoverPath(CoverKind kind, std::wstring_view id) {
    const char* name = kind == CoverKind::Album      ? "album"
                       : kind == CoverKind::Artist   ? "artist"
                       : kind == CoverKind::Playlist ? "playlist"
                                                     : "song";
    const std::string suffix = PathSuffix(id);
    return suffix.empty() ? std::string() : std::string("apple://cover/") + name + "/" + suffix;
}

bool ParseCoverPath(const char* path, CoverKind& kind, stream::Id& id) {
    struct Prefix {
        const char* text;
        CoverKind kind;
    };
    static constexpr Prefix prefixes[] = {
        {"apple://cover/song/", CoverKind::Track},
        {"apple://cover/album/", CoverKind::Album},
        {"apple://cover/artist/", CoverKind::Artist},
        {"apple://cover/playlist/", CoverKind::Playlist},
    };
    if (!path) return false;
    const std::string_view text(path);
    for (const auto& prefix : prefixes) {
        const std::string_view value(prefix.text);
        if (text.size() <= value.size() || text.substr(0, value.size()) != value) continue;
        const std::string_view suffix = text.substr(value.size());
        if (suffix.find_first_of("/?#") != std::string_view::npos) return false;
        stream::Id parsed;
        if (!ParseEncodedId(suffix, parsed)) return false;
        kind = prefix.kind;
        id = std::move(parsed);
        return true;
    }
    return false;
}

bool IsLibraryId(std::wstring_view id) {
    return id.size() > 2 && id[1] == L'.' &&
           (id[0] == L'i' || id[0] == L'l' || id[0] == L'r' || id[0] == L'p');
}

std::wstring TrackResourceType(std::wstring_view id) {
    return id.size() > 2 && id[0] == L'i' && id[1] == L'.' ? L"library-songs" : L"songs";
}

} // namespace glasswave::applemusic
