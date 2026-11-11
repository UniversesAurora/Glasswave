#pragma once

#include "media/library_model.h"
#include "stream/model.h"

#include <string>

namespace glasswave::ui {

// One page of the window: what Back/Forward, the breadcrumb, the saved last page and the
// per-page scroll positions refer to.
struct View {
    // All: pages across libraries; Local: foobar2000's Media Library; Remote: a streaming
    // provider's library (`provider` holds its key).
    enum class Library { All, Local, Remote };
    enum class Kind {
        // foobar2000 and cross-library pages
        Playlist, AllMusic, Category, Group, Playlists, SmartPlaylists, AllFavorites, AllRecentPlayed, AllMostPlayed,
        // a provider's pages
        RemoteFavorites, RemoteShelf, RemotePlaylist, RemoteArtists, RemoteAlbums, RemoteArtist, RemoteAlbum,
        RemoteTopTracks, RemoteSearch, RemoteSectionTracks, RemoteGroups, RemoteGroupTracks
    };
    Library library = Library::Local;
    Kind kind = Kind::AllMusic;
    size_t playlist = 0;                               // Playlist: foobar2000 playlist index
    media::GroupKind group = media::GroupKind::Artist;  // Category / Group
    std::wstring key;                                  // Group: the group's key
    std::wstring name;                                 // display name of the page's subject
    std::wstring provider;                             // Remote: provider key
    stream::Shelf shelf = stream::Shelf::Created;      // RemoteShelf, RemotePlaylist (when shelfKnown)
    bool shelfKnown = false;
    // Playlist/artist/album/query id; for RemoteTopTracks this is the artist id.
    stream::Id remoteId;
    stream::Id sectionId;   // provider-defined section id
    stream::Id groupId;     // RemoteGroupTracks: BrowseGroup id
    stream::Id parentId;    // RemoteAlbum opened from an artist page: that artist
    std::wstring parentName;

    bool IsRemote() const { return library == Library::Remote; }
};

// Identity of a page, for history and remembered scroll positions.
std::wstring ViewKey(const View& view);
inline bool SameView(const View& a, const View& b) { return ViewKey(a) == ViewKey(b); }

}  // namespace glasswave::ui
