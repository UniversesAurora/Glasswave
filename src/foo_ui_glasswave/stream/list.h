#pragma once

#include "stream/model.h"

#include <cstddef>
#include <atomic>
#include <memory>
#include <functional>
#include <optional>
#include <variant>

namespace glasswave::stream {

enum class ListKind {
    PlaylistTracks,
    Favorites,
    AlbumTracks,
    SectionTracks,
    ArtistTopTracks,
    ArtistAlbums,
    SavedAlbums,
    FollowedArtists,
    Playlists,
    Groups,
    CatalogSearch,
    // Appended because persisted cursors and diagnostics may use stable enum values.
    ArtistPlaylists,
    CatalogSearchArtists,
    CatalogSearchAlbums,
    CatalogSearchPlaylists,
    CatalogSearchTopResults,
};
enum class ListCachePolicy {
    None,
    Memory,
    Persistent
};

struct ListQuery {
    ListKind kind = ListKind::PlaylistTracks;
    Id id;
    Id section;
    Shelf shelf = Shelf::Created;
    std::wstring search;
    // nullopt lets the provider choose; search defaults to None.
    std::optional<ListCachePolicy> cache;
    // Transport lifetime is excluded from semantic identity and persistence.
    std::shared_ptr<std::atomic_bool> cancelled;
    bool operator==(const ListQuery& other) const {
        return kind == other.kind && id == other.id && section == other.section && shelf == other.shelf &&
               search == other.search && cache == other.cache;
    }
};

// The provider owns the cursor encoding. For offset services, `offset` counts source records
// consumed, not rows emitted after availability filtering. `value` carries any opaque server
// cursor or stable snapshot version needed to make the continuation safe.
struct ListCursor {
    std::wstring value;
    size_t offset = 0;
    bool operator==(const ListCursor&) const = default;
};

using ListRows = std::variant<std::vector<Track>, std::vector<Playlist>, std::vector<Album>,
                              std::vector<Artist>, std::vector<BrowseGroup>, std::vector<CatalogSearchItem>>;
struct ListPage {
    ListRows rows = std::vector<Track>{};
    std::optional<Playlist> playlist;
    std::optional<Album> album;
    std::optional<Artist> artist;
    ListCursor next;
    bool more = false;
    std::optional<size_t> total;
    // Stable identity of the source ordering for this page and its next cursor.
    std::wstring revision;
    // Rows are usable now; optional related records are still being fetched. Persisted
    // snapshots retain this marker so views requiring those relations can wait safely.
    bool enrichmentPending = false;
};
using ListPageDone = std::function<void(ListPage page, std::wstring error)>;

// CatalogSearch is the existing Songs category. The appended kinds are independent requests,
// cursors and optional cache entries; CatalogSearchTopResults preserves the server's mixed
// ranking and must not be synthesized from the other categories.
inline bool IsCatalogSearch(ListKind kind) {
    return kind == ListKind::CatalogSearch || kind == ListKind::CatalogSearchArtists ||
           kind == ListKind::CatalogSearchAlbums || kind == ListKind::CatalogSearchPlaylists ||
           kind == ListKind::CatalogSearchTopResults;
}

inline ListQuery ArtistSectionQuery(const Id& artist, const ArtistSection& section) {
    ListQuery query;
    query.id = artist;
    query.section = section.id;
    switch (section.rowKind) {
    case ArtistSectionRowKind::Tracks:
        query.kind = ListKind::ArtistTopTracks;
        break;
    case ArtistSectionRowKind::Albums:
        query.kind = ListKind::ArtistAlbums;
        break;
    case ArtistSectionRowKind::Playlists:
        query.kind = ListKind::ArtistPlaylists;
        break;
    }
    return query;
}

} // namespace glasswave::stream
