#pragma once

#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::ui {

namespace detail {
inline void AppendGroupPart(std::wstring& key, std::wstring_view value) {
    key += std::to_wstring(value.size());
    key += L':';
    key.append(value);
}

inline void AppendFoldedGroupPart(std::wstring& key, std::wstring_view value) {
    std::wstring folded(value);
    for (wchar_t& c : folded) c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    AppendGroupPart(key, folded);
}

inline std::wstring AlbumNameGroupKey(std::wstring_view provider, std::wstring_view album,
                                      std::wstring_view artist) {
    std::wstring key;
    AppendGroupPart(key, provider);
    AppendGroupPart(key, L"album-name");
    AppendFoldedGroupPart(key, album);
    AppendFoldedGroupPart(key, artist);
    return key;
}
}  // namespace detail

struct RemoteAlbumGroupRow {
    std::wstring_view provider;
    std::wstring_view albumId;
    std::wstring_view album;
    std::wstring_view artist;
    std::wstring_view trackId;
};

// Stable column identity for grouping remote tracks in expanded tiles. Providers normally
// supply album ids; when one is absent, the album and artist names keep that provider's
// tracks together until an id is available. A completely unidentified track stays separate.
inline std::wstring RemoteAlbumGroupKey(std::wstring_view provider, std::wstring_view albumId,
                                        std::wstring_view album, std::wstring_view artist,
                                        std::wstring_view trackId) {
    if (!albumId.empty()) {
        std::wstring key;
        detail::AppendGroupPart(key, provider);
        detail::AppendGroupPart(key, L"album-id");
        detail::AppendGroupPart(key, albumId);
        return key;
    } else if (!album.empty() && !artist.empty()) {
        return detail::AlbumNameGroupKey(provider, album, artist);
    } else {
        std::wstring key;
        detail::AppendGroupPart(key, provider);
        detail::AppendGroupPart(key, L"track-id");
        detail::AppendGroupPart(key, trackId);
        return key;
    }
}

// Resolves missing ids within the visible provider batch: one unique id observed for the
// same folded name/artist pair is enough to join a missing-id row to it. Ambiguous pairs keep
// the neutral name key, and rows without album identity retain their track id.
inline std::vector<std::wstring> RemoteAlbumGroupKeys(const std::vector<RemoteAlbumGroupRow>& rows) {
    std::map<std::wstring, std::set<std::wstring>> idsByName;
    for (const auto& row : rows) {
        if (row.provider.empty() || row.albumId.empty() || row.album.empty() || row.artist.empty()) continue;
        idsByName[detail::AlbumNameGroupKey(row.provider, row.album, row.artist)].emplace(row.albumId);
    }
    std::vector<std::wstring> keys;
    keys.reserve(rows.size());
    for (const auto& row : rows) {
        if (row.provider.empty()) {
            keys.emplace_back();
            continue;
        }
        if (!row.albumId.empty()) {
            keys.push_back(
                RemoteAlbumGroupKey(row.provider, row.albumId, row.album, row.artist, row.trackId));
            continue;
        }
        if (!row.album.empty() && !row.artist.empty()) {
            const auto found = idsByName.find(detail::AlbumNameGroupKey(row.provider, row.album, row.artist));
            if (found != idsByName.end() && found->second.size() == 1) {
                keys.push_back(RemoteAlbumGroupKey(row.provider, *found->second.begin(), row.album,
                                                   row.artist, row.trackId));
                continue;
            }
        }
        keys.push_back(RemoteAlbumGroupKey(row.provider, {}, row.album, row.artist, row.trackId));
    }
    return keys;
}

}  // namespace glasswave::ui
