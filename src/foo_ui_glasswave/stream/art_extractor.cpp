#include "pch.h"
#include "stream/art_extractor.h"

#include "core/log.h"

#include <map>

namespace glasswave::stream {

const char* ArtKindName(ArtKind kind) {
    switch (kind) {
    case ArtKind::Track:
        return "song";
    case ArtKind::Album:
        return "album";
    case ArtKind::Artist:
        return "artist";
    case ArtKind::Playlist:
        return "playlist";
    case ArtKind::Station:
        return "station";
    }
    return "";
}

bool ParseArtPath(const Catalog& records, const char* path, ArtRef& ref) {
    if (!path) return false;
    if (records.ParseCoverPath(path, ref.kind, ref.id)) return true;
    ref.kind = ArtKind::Track;
    return records.ParseSongPath(path, ref.id);
}

void LogArtFailure(const std::string& artKey, std::wstring_view what) {
    constexpr ULONGLONG kQuietMs = 5 * 60 * 1000;
    constexpr size_t kMaxKeys = 512;
    static std::mutex mutex;
    static std::map<std::string, ULONGLONG> last;
    {
        std::lock_guard lock(mutex);
        const ULONGLONG now = GetTickCount64();
        if (const auto found = last.find(artKey); found != last.end() && now - found->second < kQuietMs) return;
        if (last.size() >= kMaxKeys) {
            const auto oldest = std::min_element(last.begin(), last.end(), [](const auto& a, const auto& b) {
                return a.second < b.second;
            });
            last.erase(oldest);
        }
        last[artKey] = now;
    }
    core::Log(L"art: cover download failed: " + std::wstring(what));
}

} // namespace glasswave::stream
