#include "pch.h"
#include "ui/remote_pages_parts.h"

namespace glasswave::ui {

using namespace remotepages;

// ---- Playing a service's list ------------------------------------------------------------------

void PlayRemoteList(stream::Provider& provider, stream::ListQuery query, bool recordRecent) {
    const uint64_t token = media::BeginPendingPlay(); // what plays stops now
    query = PreparedQuery(provider, std::move(query));
    auto request = std::make_shared<AllSongsRequest>();
    request->provider = &provider;
    request->failed = [token] { media::DropPendingPlay(token); };
    request->ready = [token, &provider, weak = std::weak_ptr<AllSongsRequest>(request), id = query.id,
                      recordRecent](metadb_handle_list items) {
        if (!media::TakePendingPlay(token) || items.get_count() == 0) return; // superseded, or empty
        media::PlayItems(items, 0);
        if (auto self = weak.lock(); recordRecent && self && !self->name.empty())
            media::RecordRecentList(std::wstring(provider.Key()), id, self->name);
    };
    StartAllSongs(request, query);
}

// ---- Page ------------------------------------------------------------------------------------

// Drops the UI's own page-level requests for a provider. The provider's notification has
// already treated its list sessions the way the change requires (an account switch purges
// them, a language or display change only retires the in-memory reads); the persisted
// snapshots must survive everything but an account switch.
void InvalidateRemotePageCache(std::wstring_view provider) {
    CancelSongsRequests(provider);
    PagedPlaybackFeed::CancelProvider(provider);
}

std::unique_ptr<Page> MakeRemotePage(PageHost& host, stream::Provider& provider, const View& view) {
    if (auto page = MakeListPage(host, provider, view)) return page;
    if (auto page = MakeArtistPage(host, provider, view)) return page;
    return MakeSongPage(host, provider, view);
}

// ---- views -----------------------------------------------------------------------------------

View RemoteView(const stream::Provider& provider, View::Kind kind) {
    View view;
    view.library = View::Library::Remote;
    view.provider = provider.Key();
    view.kind = kind;
    return view;
}

View SectionView(const stream::Provider& provider, const stream::NavigationSection& section) {
    View::Kind kind = View::Kind::RemoteSectionTracks;
    switch (section.kind) {
    case stream::SectionKind::Favorites:
        kind = View::Kind::RemoteFavorites;
        break;
    case stream::SectionKind::Artists:
        kind = View::Kind::RemoteArtists;
        break;
    case stream::SectionKind::Albums:
        kind = View::Kind::RemoteAlbums;
        break;
    case stream::SectionKind::PlaylistShelf:
        kind = View::Kind::RemoteShelf;
        break;
    case stream::SectionKind::Tracks:
        kind = View::Kind::RemoteSectionTracks;
        break;
    case stream::SectionKind::Groups:
        kind = View::Kind::RemoteGroups;
        break;
    }
    View view = RemoteView(provider, kind);
    view.sectionId = section.id;
    view.name = section.label;
    view.shelf = section.shelf;
    view.shelfKnown = section.kind == stream::SectionKind::PlaylistShelf;
    return view;
}

View GroupTracksView(const stream::Provider& provider, const stream::NavigationSection& section,
                     const stream::BrowseGroup& group) {
    View view = RemoteView(provider, View::Kind::RemoteGroupTracks);
    view.sectionId = section.id;
    view.groupId = group.id;
    view.name = group.label;
    return view;
}

View ShelfView(const stream::Provider& provider, Shelf shelf, stream::Id sectionId,
               std::wstring sectionLabel) {
    View view = RemoteView(provider, View::Kind::RemoteShelf);
    view.shelf = shelf;
    view.shelfKnown = true;
    view.sectionId = std::move(sectionId);
    view.name = sectionLabel.empty() ? std::wstring(ShelfLabel(shelf)) : std::move(sectionLabel);
    return view;
}

View PlaylistView(const stream::Provider& provider, const stream::Id& id, std::wstring name,
                  std::optional<Shelf> shelf, stream::Id sectionId) {
    View view = RemoteView(provider, View::Kind::RemotePlaylist);
    view.remoteId = id;
    view.name = std::move(name);
    view.shelf = shelf.value_or(Shelf::Created);
    view.shelfKnown = shelf.has_value();
    view.sectionId = std::move(sectionId);
    return view;
}

View ArtistView(const stream::Provider& provider, const stream::Id& id, std::wstring name,
                stream::Id sectionId) {
    View view = RemoteView(provider, View::Kind::RemoteArtist);
    view.remoteId = id;
    view.name = std::move(name);
    view.sectionId = std::move(sectionId);
    return view;
}

View AlbumView(const stream::Provider& provider, const stream::Id& id, std::wstring name, stream::Id sectionId) {
    View view = RemoteView(provider, View::Kind::RemoteAlbum);
    view.remoteId = id;
    view.name = std::move(name);
    view.sectionId = std::move(sectionId);
    return view;
}

View HomeListView(const stream::Provider& provider, const stream::Id& list, std::wstring name) {
    View view = RemoteView(provider, View::Kind::RemoteHomeList);
    view.remoteId = list;
    view.name = std::move(name);
    return view;
}

View SongView(const stream::Provider& provider, const stream::Id& track, std::wstring title) {
    View view = RemoteView(provider, View::Kind::RemoteSong);
    view.remoteId = track;
    view.name = std::move(title);
    return view;
}

View TopTracksView(const stream::Provider& provider, const stream::Id& artistId, std::wstring artistName,
                   stream::Id sectionId) {
    View view = RemoteView(provider, View::Kind::RemoteTopTracks);
    view.remoteId = artistId;
    view.name = std::move(artistName);
    view.sectionId = std::move(sectionId);
    return view;
}

const wchar_t* ShelfLabel(Shelf shelf) {
    switch (shelf) {
    case Shelf::Saved:
        return L"Collected playlists";
    case Shelf::Recommended:
        return L"Recommended";
    case Shelf::Created:
        break;
    }
    return L"Created playlists";
}

namespace {

class RemotePagingLifecycle : public initquit {
  public:
    void on_init() override {}
    void on_quit() override {
        CancelAllSongsRequests();
        PagedPlaybackFeed::Shutdown();
    }
};

FB2K_SERVICE_FACTORY(RemotePagingLifecycle);

} // namespace

} // namespace glasswave::ui
