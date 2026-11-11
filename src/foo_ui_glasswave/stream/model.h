#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Provider-neutral catalogue records shown by the UI. Each streaming provider maps its
// own API objects to these; the UI never sees a provider's native types. Ids are opaque
// strings scoped to their provider.
namespace glasswave::stream {

using Id = std::wstring;
inline constexpr int kUnknownCount = -1; // callers omit this count instead of showing a false zero

struct ArtistRef {
    Id id;
    std::wstring name;
    bool operator==(const ArtistRef&) const = default;
};

struct Track {
    Id id;
    std::wstring title;
    std::wstring album;
    Id albumId;
    std::vector<ArtistRef> artists; // performers, in credit order
    int durationMs = 0;
    int popularity = 0; // provider's ranking score where it has one (orders hot songs by album)
    bool operator==(const Track&) const = default;
};

struct Playlist {
    Id id;
    std::wstring name;
    std::wstring coverUrl;
    std::wstring creator;
    std::wstring description;
    int trackCount = kUnknownCount;
    bool own = false;   // created by the signed-in user (editable, cannot be saved)
    bool saved = false; // in the user's library ("collected")
    bool operator==(const Playlist&) const = default;
};

struct Album {
    Id id;
    std::wstring name;
    std::wstring coverUrl;
    std::vector<ArtistRef> artists;
    std::wstring description;
    int trackCount = kUnknownCount;
    int year = 0; // 0 = unknown
    bool operator==(const Album&) const = default;
};

struct Artist {
    Id id;
    std::wstring name;
    std::wstring avatarUrl;
    std::wstring bio;
    int albumCount = kUnknownCount;
    int trackCount = kUnknownCount;
    bool followKnown = false; // the provider reported the follow state
    bool followed = false;
    bool operator==(const Artist&) const = default;
};

enum class ArtistSectionRowKind {
    Tracks,
    Albums,
    Playlists,
};

// Provider-defined artist view. Keys are stable semantic identities. `label` is an
// untranslated UI key or provider-owned literal; display consumers pass it through Tr.
// Enumerating these descriptors must not access the network.
struct ArtistSection {
    Id id;
    std::wstring label;
    ArtistSectionRowKind rowKind = ArtistSectionRowKind::Tracks;
    bool operator==(const ArtistSection&) const = default;
};

// The playlist sections of a provider's library.
enum class Shelf {
    Created,
    Saved,
    Recommended
};

// Provider-defined navigation entries. Ids are stable across language changes and are used
// for navigation ordering and saved views; labels are untranslated UI keys or user names.
enum class SectionKind {
    Favorites,
    Artists,
    Albums,
    PlaylistShelf,
    Tracks,
    Groups
};

struct NavigationSection {
    Id id;
    std::wstring label;
    SectionKind kind = SectionKind::Tracks;
    Shelf shelf = Shelf::Created;
    bool reorderChildren = false;
};

// One provider-defined value below a Groups section (genre, composer, collection, ...).
struct BrowseGroup {
    Id id;
    std::wstring label;
    size_t trackCount = 0;
    bool operator==(const BrowseGroup&) const = default;
};

// One rank-ordered catalog search result. Vector position is the server rank; the active
// variant arm is the resource type, so equal IDs from different resource kinds stay distinct.
using CatalogSearchItem = std::variant<Track, Artist, Album, Playlist>;
enum class CatalogSearchItemKind {
    Track,
    Artist,
    Album,
    Playlist,
};
inline CatalogSearchItemKind CatalogSearchItemKindOf(const CatalogSearchItem& item) {
    switch (item.index()) {
    case 0:
        return CatalogSearchItemKind::Track;
    case 1:
        return CatalogSearchItemKind::Artist;
    case 2:
        return CatalogSearchItemKind::Album;
    case 3:
        return CatalogSearchItemKind::Playlist;
    default:
        return CatalogSearchItemKind::Track;
    }
}
inline Id CatalogSearchItemId(const CatalogSearchItem& item) {
    return std::visit([](const auto& row) { return row.id; }, item);
}
inline Id CatalogSearchItemKey(const CatalogSearchItem& item) {
    static constexpr std::wstring_view prefixes[] = {L"track:", L"artist:", L"album:", L"playlist:"};
    const auto index = item.index();
    return index < 4 ? std::wstring(prefixes[index]) + CatalogSearchItemId(item) : std::wstring{};
}

// What a cover belongs to; covers are shared by every place that shows the same object.
enum class ArtKind {
    Track,
    Album,
    Artist,
    Playlist
};

std::wstring JoinArtists(const std::vector<ArtistRef>& artists);

} // namespace glasswave::stream
