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

// A credit. The id is empty when the service names an artist it has no page for (uploaded
// or matched library songs); such a credit is shown but not navigable.
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
    // Details, where the service has them; an empty field is not shown.
    std::wstring tagline;  // one line about the playlist
    std::wstring type;     // untranslated UI key: Editorial playlist, Chart, Personal mix, ...
    std::wstring created;  // "YYYY-MM-DD"
    std::wstring updated;  // "YYYY-MM-DD"
    std::wstring updates;  // how often it changes, as the service writes it ("每日更新")
    std::vector<std::wstring> tags;    // genre and mood tags
    std::vector<std::wstring> aliases; // other titles
    std::vector<std::wstring> traits;  // untranslated UI keys, as stream::Album::traits
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
    // Details, where the service has them; an empty field is not shown.
    std::wstring released;  // release date, "YYYY-MM-DD" or as the service writes it
    std::wstring label;     // record label or company
    std::wstring copyright;
    std::wstring type;      // untranslated UI key: Album, EP, Single, Compilation, Live album
    std::wstring edition;   // the service's own word for the recording ("录音室版"), shown as written
    std::wstring upc;
    std::vector<std::wstring> genres;
    std::vector<std::wstring> aliases;
    // Untranslated UI keys: Lossless, Hi-Res Lossless, Dolby Atmos, Apple Digital Master, Explicit.
    std::vector<std::wstring> traits;
    bool operator==(const Album&) const = default;
};

// What kind of act an artist is, where the service says.
enum class ArtistKind {
    Unknown,
    Person, // a solo artist (the service does not say more)
    Male,
    Female,
    Group,
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
    // The profile, where the service has it; an empty or unknown field is not shown.
    bool profile = false;      // read from the artist's own record (a list row has a summary)
    std::wstring tagline;      // one line about the artist
    std::wstring born;         // birth or formation date (or year), as the service writes it
    std::wstring origin;       // where the artist comes from: a place
    std::wstring area;         // the service's market or language area: an untranslated UI key
    ArtistKind kind = ArtistKind::Unknown;
    std::vector<std::wstring> genres;
    std::vector<std::wstring> aliases; // other names (translations, romanisations)
    std::vector<std::wstring> roles;   // what the artist does: singer, lyricist, producer...
    int videoCount = kUnknownCount;
    bool operator==(const Artist&) const = default;
};

// Who made a song and what each did, in the service's own groups ("出演艺人", "作曲和作词",
// "制作和工程" on Apple Music); titles and roles come localized from the service.
struct Credit {
    std::wstring name;
    std::vector<std::wstring> roles;
    Id artistId; // the service's artist page, when the person has one
};
struct CreditGroup {
    std::wstring title;
    std::vector<Credit> credits;
};

// Everything a service knows of one song, for the song page; an empty field is not shown.
struct SongDetails {
    Track track;            // title, album, artists, length
    int disc = 0, number = 0;
    std::wstring released;  // "YYYY-MM-DD" or as the service writes it
    std::wstring language;  // as the service writes it ("国语", "zh-Hans-CN" read as a name)
    std::wstring lyrics;    // untranslated UI key: Synced, Unsynced
    std::wstring version;   // untranslated UI key: Original, Cover
    std::wstring isrc;
    std::wstring composer;  // as the service credits the composition, when it has no credits
    int bpm = 0;
    std::vector<std::wstring> genres, tags, aliases;
    std::vector<std::wstring> traits; // untranslated UI keys, as Album::traits
    std::vector<CreditGroup> credits;
    std::vector<Track> similar;
};

// Gives `to` the profile of `from` (bio included), for a list row standing in for a record
// that had been read in full.
inline void CopyArtistProfile(const Artist& from, Artist& to) {
    to.profile = from.profile;
    to.bio = from.bio;
    to.tagline = from.tagline;
    to.born = from.born;
    to.origin = from.origin;
    to.area = from.area;
    to.kind = from.kind;
    to.genres = from.genres;
    to.aliases = from.aliases;
    to.roles = from.roles;
    to.videoCount = from.videoCount;
}

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

// What a Groups section groups by, for its icon (WMP's genre or composer icon).
enum class GroupFacet {
    Genre,
    Composer
};

struct NavigationSection {
    Id id;
    std::wstring label;
    SectionKind kind = SectionKind::Tracks;
    Shelf shelf = Shelf::Created;
    bool reorderChildren = false;
    GroupFacet facet = GroupFacet::Genre;
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
