#include "pch.h"
#include "ui/remote_pages.h"
#include "media/favorites.h"
#include "ui/art_cache.h"
#include "ui/i18n.h"
#include "ui/remote_grids.h"
#include "ui/song_actions.h"
#include "ui/song_table.h"
#include "ui/text_prompt.h"

#include <list>

namespace glasswave::ui {

namespace {

constexpr size_t kTopTracksPage = 100;  // hot songs per request
constexpr size_t kArtistAlbumsPage = 24;
constexpr size_t kSavedAlbumsPage = 100;
constexpr size_t kAlbumTracksPage = 12;  // albums whose songs load at once in Expanded tile

using stream::Shelf;

std::wstring Joined(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (const auto& part : parts) {
        if (part.empty()) continue;
        if (!out.empty()) out += L" · ";
        out += part;
    }
    return out;
}

std::wstring Count(int n, const wchar_t* unit) {
    return n > 0 ? std::to_wstring(n) + L" " + Tr(unit) : std::wstring();
}

bool Contains(const std::vector<stream::Album>& albums, const stream::Id& id) {
    return std::any_of(albums.begin(), albums.end(), [&id](const stream::Album& a) { return a.id == id; });
}

bool Contains(const std::vector<stream::Artist>& artists, const stream::Id& id) {
    return std::any_of(artists.begin(), artists.end(), [&id](const stream::Artist& a) { return a.id == id; });
}

bool Contains(const std::vector<stream::Playlist>& playlists, const stream::Id& id) {
    return std::any_of(playlists.begin(), playlists.end(), [&id](const stream::Playlist& p) { return p.id == id; });
}

bool IsSavedAlbum(stream::Provider& provider, const stream::Id& id) { return Contains(provider.SavedAlbums(), id); }
bool IsFollowed(stream::Provider& provider, const stream::Id& id) { return Contains(provider.FollowedArtists(), id); }

// Turns a provider's track list into foobar2000 items once it has been fetched.
SongsFetch FetchPlaylistSongs(stream::Provider& provider, stream::Id id) {
    return [&provider, id](std::function<void(metadb_handle_list)> ready) {
        provider.FetchPlaylist(id, [&provider, ready](stream::Playlist, std::vector<stream::Track> tracks,
                                                      std::wstring error) {
            if (!error.empty()) return ShowProviderError(provider, error);
            metadb_handle_list items;
            for (const auto& track : tracks) items.add_item(provider.TrackItem(track.id));
            ready(items);
        });
    };
}

SongsFetch FetchAlbumSongs(stream::Provider& provider, stream::Id id) {
    return [&provider, id](std::function<void(metadb_handle_list)> ready) {
        provider.FetchAlbum(id, [&provider, ready](stream::Album, std::vector<stream::Track> tracks,
                                                   std::wstring error) {
            if (!error.empty()) return ShowProviderError(provider, error);
            metadb_handle_list items;
            for (const auto& track : tracks) items.add_item(provider.TrackItem(track.id));
            ready(items);
        });
    };
}

std::unique_ptr<RemoteSongSource> Songs(stream::Provider& provider, const std::vector<stream::Track>& tracks,
                                        std::wstring empty) {
    return std::make_unique<RemoteSongSource>(provider, tracks, std::move(empty));
}

// Album collect/uncollect, from any menu. `done` runs after a successful write.
MenuAction SaveAlbumAction(stream::Provider& provider, const stream::Id& id, std::function<void(bool saved)> done) {
    const bool saved = IsSavedAlbum(provider, id);
    return {saved ? L"Uncollect album" : L"Collect album", [&provider, id, saved, done] {
                provider.SetAlbumSaved(id, !saved, [&provider, saved, done](std::wstring error) {
                    if (!error.empty()) return ShowProviderError(provider, error);
                    if (done) done(!saved);
                });
            }};
}

MenuAction FollowAction(stream::Provider& provider, const stream::Id& id, std::function<void(bool followed)> done) {
    const bool followed = IsFollowed(provider, id);
    return {followed ? L"Unfollow artist" : L"Follow artist", [&provider, id, followed, done] {
                provider.SetFollowed(id, !followed, [&provider, followed, done](std::wstring error) {
                    if (!error.empty()) return ShowProviderError(provider, error);
                    if (done) done(!followed);
                });
            }};
}

// "Go to artist" for an album's credited artists (a submenu when there are several).
void AddGoToArtist(std::vector<MenuAction>& actions, PageHost& host, stream::Provider& provider,
                   const std::vector<stream::ArtistRef>& artists) {
    std::vector<MenuAction> links;
    for (const auto& artist : artists)
        if (!artist.id.empty())
            links.push_back({artist.name.empty() ? L"Artist" : artist.name, [&host, &provider, artist] {
                                 host.Open(ArtistView(provider, artist.id, artist.name));
                             }});
    if (links.size() == 1) actions.push_back({L"Go to artist", links.front().run});
    else if (!links.empty()) actions.push_back({L"Go to artist", nullptr, std::move(links)});
}

// ---- common base ---------------------------------------------------------------------------

class RemotePage : public Page {
public:
    RemotePage(PageHost& host, stream::Provider& provider, View view) : Page(host, std::move(view)), m_provider(provider) {}
    bool CanRefresh() const override { return true; }

protected:

    // A fetched song list replaces the current one; a failed refresh keeps rows on screen.
    void ShowTracks(const std::vector<stream::Track>& tracks, const std::wstring& error, const wchar_t* empty) {
        if (!error.empty() && m_host.Source() && m_host.Source()->Count()) return;
        m_host.ShowSource(Songs(m_provider, tracks, error.empty() ? std::wstring(empty) : error));
    }

    bool SignedInOrPrompt() {
        if (m_provider.SignedIn()) return true;
        m_provider.SignIn(m_host.Owner());
        return false;
    }

    RemoteSongSource* SongSource() const { return dynamic_cast<RemoteSongSource*>(m_host.Source()); }

    stream::Provider& m_provider;
};

// ---- Favorites -------------------------------------------------------------------------------

class FavoritesPage final : public RemotePage {
public:
    using RemotePage::RemotePage;

    std::unique_ptr<media::ListSource> Start(bool) override {
        auto cached = m_provider.CachedFavorites();
        auto source = Songs(m_provider, cached ? *cached : std::vector<stream::Track>{},
                            cached ? L"No favorites." : L"Loading your favorites...");
        m_provider.FetchFavorites(Guard([this](std::vector<stream::Track> tracks, std::wstring error) {
            ShowTracks(tracks, error, L"No favorites.");
        }));
        return source;
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        if (auto* source = SongSource(); source && source->Count()) {
            AddBulkActions(actions, Ready(source->VisibleItems()), &m_provider);
            actions.push_back({});
        }
    }

    void OnFavoritesChanged() override {
        // Unliked songs leave the list in place. Only trust the flags once the favorites
        // were read; before that every song would look unliked.
        auto* source = SongSource();
        if (!source || !m_provider.CachedFavorites()) return Page::OnFavoritesChanged();
        if (source->RemoveUnfavorited()) m_host.ListChanged();
    }
};

// ---- a playlist ------------------------------------------------------------------------------

class PlaylistPage final : public RemotePage {
public:
    using RemotePage::RemotePage;

    std::unique_ptr<media::ListSource> Start(bool) override {
        const stream::Id id = m_view.remoteId;
        m_info = m_provider.FindPlaylist(id);
        auto cached = m_provider.CachedPlaylist(id);
        auto source = Songs(m_provider, cached ? *cached : std::vector<stream::Track>{},
                            cached ? L"No songs." : L"Loading playlist...");
        const std::wstring previousCover = m_info ? m_info->coverUrl : std::wstring();
        m_provider.FetchPlaylist(id, Guard([this, previousCover](stream::Playlist info, std::vector<stream::Track> tracks,
                                                                 std::wstring error) {
            if (!info.id.empty()) {
                if (!m_provider.SameCover(previousCover, info.coverUrl))
                    ArtCache::Get().Invalidate(m_provider.ArtKey(stream::ArtKind::Playlist, info.id));
                m_info = std::move(info);
                if (!m_info->name.empty()) m_host.SetPageName(m_info->name);
            }
            ShowTracks(tracks, error, L"No songs.");
            m_host.ChromeChanged();
        }));
        return source;
    }

    DetailInfo Detail() const override {
        DetailInfo d;
        d.visible = true;
        d.title = m_info && !m_info->name.empty() ? m_info->name : m_view.name;
        if (d.title.empty()) d.title = Tr(L"Playlist");
        if (m_info) {
            d.subtitle = Joined({m_info->creator, Count(m_info->trackCount, L"songs")});
            d.brief = m_info->description;
        }
        d.artKey = m_provider.ArtKey(stream::ArtKind::Playlist, m_view.remoteId);
        d.artItem = m_provider.ArtItem(stream::ArtKind::Playlist, m_view.remoteId);
        if (!Own() && m_provider.SignedIn() && m_provider.Offers().savePlaylists) {
            d.action = DetailInfo::Action::Save;
            d.actionOn = Saved();
            d.actionBusy = m_busy;
            d.actionLabel = Tr(d.actionOn ? L"Collected" : L"Collect");
        }
        return d;
    }

    void DetailAction() override {
        if (m_busy || !SignedInOrPrompt()) return;
        const bool next = !Saved();
        m_busy = true;
        m_host.ChromeChanged();
        m_provider.SetPlaylistSaved(m_view.remoteId, next, Guard([this, next](std::wstring error) {
            m_busy = false;
            if (!error.empty()) ShowProviderError(m_provider, error);
            else if (m_info) m_info->saved = next;
            m_host.ChromeChanged();
        }));
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        AddBulkActions(actions, FetchPlaylistSongs(m_provider, m_view.remoteId), &m_provider, m_view.remoteId);
        if (!m_provider.SignedIn()) return;
        actions.push_back({});
        const std::wstring name = m_info && !m_info->name.empty() ? m_info->name : m_view.name;
        if (Own()) {
            actions.push_back({L"Rename...", [this, name] { RenameRemotePlaylist(m_host, m_provider, m_view.remoteId, name); }});
            actions.push_back({L"Delete playlist", [this, name] {
                                   DeleteRemotePlaylist(m_host, m_provider, m_view.remoteId, name);
                               }});
        } else if (m_provider.Offers().savePlaylists) {
            actions.push_back({Saved() ? L"Uncollect playlist" : L"Collect playlist", [this] { DetailAction(); }});
        }
    }

    void AddSongActions(std::vector<MenuAction>& actions, const metadb_handle_list& items) override {
        if (!Own() || !m_provider.Offers().editPlaylists) return;
        auto ids = TrackIds(m_provider, items);
        if (ids.empty()) return;
        actions.push_back({});
        actions.push_back({L"Remove from this playlist", [this, ids] { Remove(ids); }});
    }

    void OnLibraryChanged() override {
        // A rename or a regenerated cover; the songs stay as they are.
        if (auto info = m_provider.FindPlaylist(m_view.remoteId)) {
            const bool saved = m_info && m_info->saved;
            m_info = std::move(info);
            m_info->saved = m_info->saved || saved;
            if (!m_info->name.empty()) m_host.SetPageName(m_info->name);
        }
        m_host.ChromeChanged();
    }

private:
    bool Own() const {
        return m_provider.ShelfOf(m_view.remoteId) == Shelf::Created || (m_info && m_info->own);
    }
    bool Saved() const {
        return m_provider.ShelfOf(m_view.remoteId) == Shelf::Saved || (m_info && m_info->saved) ||
               Contains(m_provider.Playlists(Shelf::Saved), m_view.remoteId);
    }

    // The rows go at once; the request confirms in the background and a failure reloads.
    void Remove(const std::vector<stream::Id>& ids) {
        auto* source = SongSource();
        if (!source) return;
        const size_t removed = source->RemoveTracks(ids);
        if (m_info) m_info->trackCount = std::max(0, m_info->trackCount - static_cast<int>(removed));
        m_host.ListChanged();
        m_host.ChromeChanged();
        m_provider.RemoveFromPlaylist(m_view.remoteId, ids, Guard([this](std::wstring error) {
            if (error.empty()) return;
            ShowProviderError(m_provider, error);
            m_host.Reload();  // restore the rows
        }));
    }

    std::optional<stream::Playlist> m_info;
    bool m_busy = false;
};

// ---- an album --------------------------------------------------------------------------------

class AlbumPage final : public RemotePage {
public:
    using RemotePage::RemotePage;

    std::unique_ptr<media::ListSource> Start(bool) override {
        const stream::Id id = m_view.remoteId;
        m_info = m_provider.FindAlbum(id);
        auto cached = m_provider.CachedAlbum(id);
        auto source = Songs(m_provider, cached ? *cached : std::vector<stream::Track>{},
                            cached ? L"No songs." : L"Loading album...");
        const std::wstring previousCover = m_info ? m_info->coverUrl : std::wstring();
        m_provider.FetchAlbum(id, Guard([this, previousCover](stream::Album info, std::vector<stream::Track> tracks,
                                                              std::wstring error) {
            if (!info.id.empty()) {
                // Only a different image needs a reload; the grid's decode stays usable.
                if (!m_provider.SameCover(previousCover, info.coverUrl))
                    ArtCache::Get().Invalidate(m_provider.ArtKey(stream::ArtKind::Album, info.id));
                m_info = std::move(info);
                if (!m_info->name.empty()) m_host.SetPageName(m_info->name);
            }
            ShowTracks(tracks, error, L"No songs.");
            m_host.ChromeChanged();
        }));
        return source;
    }

    DetailInfo Detail() const override {
        DetailInfo d;
        d.visible = true;
        d.title = m_info && !m_info->name.empty() ? m_info->name : m_view.name;
        if (d.title.empty()) d.title = Tr(L"Album");
        if (m_info) {
            d.subtitle = Joined({stream::JoinArtists(m_info->artists), m_info->year ? std::to_wstring(m_info->year) : L"",
                                 Count(m_info->trackCount, L"songs")});
            d.brief = m_info->description;
        }
        d.artKey = m_provider.ArtKey(stream::ArtKind::Album, m_view.remoteId);
        d.artItem = m_provider.ArtItem(stream::ArtKind::Album, m_view.remoteId);
        if (m_provider.SignedIn() && m_provider.Offers().saveAlbums) {
            d.action = DetailInfo::Action::Save;
            d.actionOn = IsSavedAlbum(m_provider, m_view.remoteId);
            d.actionBusy = m_busy;
            d.actionLabel = Tr(d.actionOn ? L"Collected" : L"Collect");
        }
        return d;
    }

    void DetailAction() override {
        if (m_busy || !SignedInOrPrompt()) return;
        m_busy = true;
        m_host.ChromeChanged();
        m_provider.SetAlbumSaved(m_view.remoteId, !IsSavedAlbum(m_provider, m_view.remoteId),
                                 Guard([this](std::wstring error) {
                                     m_busy = false;
                                     if (!error.empty()) ShowProviderError(m_provider, error);
                                     m_host.ChromeChanged();
                                 }));
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        AddBulkActions(actions, FetchAlbumSongs(m_provider, m_view.remoteId), &m_provider);
        if (m_provider.SignedIn() && m_provider.Offers().saveAlbums) {
            actions.push_back({});
            actions.push_back(SaveAlbumAction(m_provider, m_view.remoteId, Guard([this](bool) { m_host.ChromeChanged(); })));
        }
    }

    void AddSongActions(std::vector<MenuAction>& actions, const metadb_handle_list&) override {
        if (!IsSavedAlbum(m_provider, m_view.remoteId)) return;
        actions.push_back({});
        actions.push_back(SaveAlbumAction(m_provider, m_view.remoteId, Guard([this](bool) { m_host.ChromeChanged(); })));
    }

    void OnLibraryChanged() override { m_host.ChromeChanged(); }

private:
    std::optional<stream::Album> m_info;
    bool m_busy = false;
};

// ---- an artist's hot songs ------------------------------------------------------------------

class TopTracksPage final : public RemotePage {
public:
    using RemotePage::RemotePage;

    std::unique_ptr<media::ListSource> Start(bool) override {
        LoadNext();
        return Songs(m_provider, {}, L"Loading hot songs...");
    }

    void NearEnd() override { LoadNext(); }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        if (auto* source = SongSource(); source && source->Count()) {
            AddBulkActions(actions, Ready(source->VisibleItems()), &m_provider);
            actions.push_back({});
        }
        if (m_provider.SignedIn() && m_provider.Offers().followArtists)
            actions.push_back(FollowAction(m_provider, m_view.remoteId, {}));
    }

private:
    void LoadNext() {
        if (m_loading || !m_more) return;
        m_loading = true;
        const size_t offset = m_offset;
        m_provider.FetchArtistTopTracks(
            m_view.remoteId, offset, kTopTracksPage,
            Guard([this, offset](std::vector<stream::Track> tracks, bool more, std::wstring error) {
                m_loading = false;
                m_offset = offset + tracks.size();
                m_more = more && !tracks.empty();  // no progress: stop instead of looping
                if (offset == 0) {
                    ShowTracks(tracks, error, L"No songs.");
                } else if (auto* source = SongSource(); source && !tracks.empty()) {
                    source->AppendTracks(tracks);  // regroups albums by their hottest song
                    m_host.ListChanged();
                }
                m_host.ChromeChanged();
            }));
    }

    size_t m_offset = 0;
    bool m_more = true;
    bool m_loading = false;
};

// ---- a shelf of playlists ----------------------------------------------------------------------

class ShelfPage final : public RemotePage {
public:
    using RemotePage::RemotePage;

    std::unique_ptr<media::ListSource> Start(bool) override {
        const auto cached = m_provider.Playlists(m_view.shelf);
        auto source = Grid(cached, cached.empty() ? L"Loading playlists..." : L"No playlists.");
        m_provider.FetchPlaylists(m_view.shelf, Guard([this](std::vector<stream::Playlist> playlists, std::wstring error) {
            if (!error.empty() && m_host.Source() && m_host.Source()->Count()) return;
            m_host.ShowSource(Grid(std::move(playlists), error.empty() ? std::wstring(L"No playlists.") : error));
            if (error.empty()) {
                // The section fetch is authoritative: a playlist edited elsewhere may have a
                // new cover. The art cache keeps the old bitmap while the new one loads.
                ArtCache::Get().Invalidate(m_provider.ArtKeyPrefix(stream::ArtKind::Playlist));
            }
            m_host.ChromeChanged();
        }));
        return source;
    }

    bool CanRefresh() const override { return m_view.shelf != Shelf::Recommended; }  // a daily set

    void AddPageActions(std::vector<MenuAction>& actions) override {
        if (m_view.shelf == Shelf::Created && m_provider.Offers().editPlaylists)
            actions.push_back({L"New " + m_provider.Name() + L" playlist",
                               [this] { CreateRemotePlaylist(m_host, m_provider); }});
    }

    void OnLibraryChanged() override {
        if (m_view.shelf == Shelf::Recommended) return;
        m_host.ShowSource(Grid(m_provider.Playlists(m_view.shelf), L"No playlists."));
    }

private:
    std::unique_ptr<PlaylistGridSource> Grid(std::vector<stream::Playlist> playlists, std::wstring empty) {
        const Shelf shelf = m_view.shelf;
        return std::make_unique<PlaylistGridSource>(
            m_provider, std::move(playlists), std::move(empty),
            [this, shelf](const stream::Playlist& p) { m_host.Open(PlaylistView(m_provider, p.id, p.name, shelf)); },
            [this](POINT screen, const stream::Playlist& p) { TileMenu(screen, p); });
    }

    void TileMenu(POINT screen, const stream::Playlist& playlist) {
        ShowRemotePlaylistMenu(m_host, m_provider, playlist.id, playlist.name, m_view.shelf, screen,
                               Guard([this](const stream::Id& id) {
                                   // Uncollected on the collected shelf: the tile goes at once.
                                   auto* grid = dynamic_cast<PlaylistGridSource*>(m_host.Source());
                                   if (m_view.shelf == Shelf::Saved && grid) {
                                       grid->RemovePlaylist(id);
                                       m_host.ListChanged();
                                   }
                               }));
    }
};

// ---- followed artists ---------------------------------------------------------------------------

class ArtistsPage final : public RemotePage {
public:
    using RemotePage::RemotePage;

    std::unique_ptr<media::ListSource> Start(bool) override {
        const auto cached = m_provider.FollowedArtists();
        auto source = Grid(cached, cached.empty() ? L"Loading artists..." : L"No artists.");
        m_provider.FetchFollowedArtists(Guard([this](std::vector<stream::Artist> artists, std::wstring error) {
            if (!error.empty() && m_host.Source() && m_host.Source()->Count()) return;
            m_host.ShowSource(Grid(std::move(artists), error.empty() ? std::wstring(L"No artists.") : error));
            m_host.ChromeChanged();
        }));
        return source;
    }

private:
    std::unique_ptr<ArtistGridSource> Grid(std::vector<stream::Artist> artists, std::wstring empty) {
        return std::make_unique<ArtistGridSource>(
            m_provider, std::move(artists), std::move(empty),
            [this](const stream::Artist& a) { m_host.Open(ArtistView(m_provider, a.id, a.name)); },
            [this](POINT screen, const stream::Artist& a) {
                std::vector<MenuAction> actions{{L"View", [this, a] { m_host.Open(ArtistView(m_provider, a.id, a.name)); }}};
                if (m_provider.Offers().followArtists)
                    actions.push_back(FollowAction(m_provider, a.id, Guard([this](bool) { m_host.Reload(); })));
                ShowActionMenu(m_host.Owner(), screen, actions);
            });
    }
};

// ---- album browsing (an artist's albums, the saved albums) ------------------------------------

// Album metadata pages plus, for Expanded tile, the songs of those albums in pages. The
// state outlives its page (Back/Forward and view style switches show it again), so a
// result is applied to the pager even when its page is gone, and reported to whichever
// page listens now.
class AlbumPager {
public:
    using FetchPage = std::function<void(size_t offset, size_t limit, stream::Provider::AlbumPageDone)>;
    struct Listener {
        std::function<void(const std::vector<stream::Album>& added, std::optional<bool> followed,
                           const std::wstring& error)>
            albums;
        std::function<void(const std::vector<stream::Track>& added, const std::wstring& error)> tracks;
    };

    AlbumPager(stream::Provider& provider, FetchPage fetch, size_t pageSize)
        : m_provider(provider), m_fetch(std::move(fetch)), m_pageSize(pageSize) {}

    std::vector<stream::Album> albums;
    std::vector<stream::Track> tracks;  // songs of albums[0, tracksLoaded)
    size_t tracksLoaded = 0;
    std::optional<stream::Artist> artist;  // the artist page's profile
    Listener listener;

    bool MoreAlbums() const { return m_more; }
    bool TracksPending() const { return tracksLoaded < albums.size(); }

    void LoadAlbums(const std::shared_ptr<AlbumPager>& self) {
        if (m_albumsLoading || !m_more) return;
        m_albumsLoading = true;
        const size_t offset = m_offset;
        m_fetch(offset, m_pageSize, [weak = std::weak_ptr<AlbumPager>(self), offset](
                                        std::vector<stream::Album> added, bool more, std::optional<bool> followed,
                                        std::wstring error) {
            auto pager = weak.lock();
            if (!pager) return;
            pager->m_albumsLoading = false;
            pager->m_offset = offset + added.size();
            pager->m_more = more && !added.empty();  // no progress: stop instead of looping
            pager->albums.insert(pager->albums.end(), added.begin(), added.end());
            if (pager->listener.albums) pager->listener.albums(added, followed, error);
        });
    }

    void LoadTracks(const std::shared_ptr<AlbumPager>& self) {
        if (m_tracksLoading || !TracksPending()) return;
        const size_t first = tracksLoaded, last = std::min(first + kAlbumTracksPage, albums.size());
        std::vector<stream::Id> ids;
        for (size_t i = first; i < last; ++i) ids.push_back(albums[i].id);
        m_tracksLoading = true;
        m_provider.FetchAlbumsTracks(std::move(ids), [weak = std::weak_ptr<AlbumPager>(self), last](
                                                         std::vector<stream::Track> added, std::wstring error) {
            auto pager = weak.lock();
            if (!pager) return;
            pager->m_tracksLoading = false;
            pager->tracksLoaded = std::min(last, pager->albums.size());
            pager->tracks.insert(pager->tracks.end(), added.begin(), added.end());
            if (pager->listener.tracks) pager->listener.tracks(added, error);
        });
    }

    // An album left the saved list: the next metadata page starts one earlier.
    void Remove(const stream::Id& album) {
        const auto it = std::find_if(albums.begin(), albums.end(), [&album](const stream::Album& a) { return a.id == album; });
        if (it == albums.end()) return;
        const size_t index = static_cast<size_t>(it - albums.begin());
        albums.erase(it);
        if (m_offset) --m_offset;
        if (index < tracksLoaded) --tracksLoaded;
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&album](const stream::Track& t) { return t.albumId == album; }),
                     tracks.end());
    }

private:
    stream::Provider& m_provider;
    FetchPage m_fetch;
    size_t m_pageSize;
    size_t m_offset = 0;
    bool m_more = true;
    bool m_albumsLoading = false, m_tracksLoading = false;
};

// The last few album browsing states, by page: Back/Forward finds them again. Small,
// because one state can hold thousands of songs.
class PagerCache {
public:
    static std::shared_ptr<AlbumPager> Find(const std::wstring& key) {
        for (auto it = Entries().begin(); it != Entries().end(); ++it)
            if (it->first == key) {
                Entries().splice(Entries().begin(), Entries(), it);  // most recent first
                return it->second;
            }
        return {};
    }
    static void Put(const std::wstring& key, std::shared_ptr<AlbumPager> pager) {
        Entries().remove_if([&key](const auto& entry) { return entry.first == key; });
        Entries().emplace_front(key, std::move(pager));
        while (Entries().size() > 4) Entries().pop_back();
    }

private:
    static std::list<std::pair<std::wstring, std::shared_ptr<AlbumPager>>>& Entries() {
        static std::list<std::pair<std::wstring, std::shared_ptr<AlbumPager>>> entries;
        return entries;
    }
};

// Shared by the artist page and the saved-albums page: cover tiles (Icon) or the albums'
// songs grouped by album (Expanded tile), both filled page by page.
class AlbumBrowsePage : public RemotePage {
public:
    bool StyleRebuildsPage() const override { return true; }

    void NearEnd() override {
        if (Expanded() && m_pager->TracksPending()) m_pager->LoadTracks(m_pager);
        else m_pager->LoadAlbums(m_pager);
    }

protected:
    AlbumBrowsePage(PageHost& host, stream::Provider& provider, View view)
        : RemotePage(host, provider, std::move(view)) {}

    bool Expanded() const { return m_host.SavedStyle(media::ViewStyle::Icons) == media::ViewStyle::Tiles; }

    // Uses the remembered state on reuse, a fresh one otherwise, and listens to it.
    void Attach(bool reuse, AlbumPager::FetchPage fetch, size_t pageSize) {
        const std::wstring key = ViewKey(m_view);
        if (reuse) m_pager = PagerCache::Find(key);
        if (!m_pager) {
            m_pager = std::make_shared<AlbumPager>(m_provider, std::move(fetch), pageSize);
            PagerCache::Put(key, m_pager);
        }
        m_pager->listener.albums = Guard([this](const std::vector<stream::Album>& added, std::optional<bool> followed,
                                                const std::wstring& error) { OnAlbums(added, followed, error); });
        m_pager->listener.tracks = Guard(
            [this](const std::vector<stream::Track>& added, const std::wstring& error) { OnTracks(added, error); });
    }

    // What the list shows now: the loaded songs or albums, or a placeholder.
    std::unique_ptr<media::ListSource> Current(const wchar_t* loading) {
        if (Expanded())
            return Songs(m_provider, m_pager->tracks, m_pager->tracks.empty() ? L"Loading album songs..." : L"No songs.");
        return Grid(m_pager->albums, m_pager->albums.empty() ? loading : L"No albums.");
    }

    // Keeps loading what the current style shows.
    void Continue() {
        if (!Expanded()) {
            if (m_pager->albums.empty()) m_pager->LoadAlbums(m_pager);
        } else if (m_pager->TracksPending()) {
            m_pager->LoadTracks(m_pager);
        } else {
            m_pager->LoadAlbums(m_pager);
        }
    }

    virtual std::unique_ptr<AlbumGridSource> Grid(std::vector<stream::Album> albums, std::wstring empty) = 0;
    virtual void OnFollowed(std::optional<bool>) {}

    std::shared_ptr<AlbumPager> m_pager;

private:
    void OnAlbums(const std::vector<stream::Album>& added, std::optional<bool> followed, const std::wstring& error) {
        OnFollowed(followed);
        if (added.empty()) {
            if (m_pager->albums.empty() && !Expanded())
                m_host.ShowSource(Grid({}, error.empty() ? std::wstring(L"No albums.") : error));
            else if (m_pager->albums.empty())
                m_host.ShowSource(Songs(m_provider, {}, error.empty() ? std::wstring(L"No albums.") : error));
            return;
        }
        if (Expanded()) {
            m_pager->LoadTracks(m_pager);
        } else if (auto* grid = dynamic_cast<AlbumGridSource*>(m_host.Source())) {
            grid->AppendAlbums(added);
            m_host.ListChanged();
        } else {
            m_host.ShowSource(Grid(m_pager->albums, L"No albums."));
        }
        m_host.ChromeChanged();
    }

    void OnTracks(const std::vector<stream::Track>& added, const std::wstring& error) {
        if (!Expanded()) return;
        if (auto* source = SongSource(); source && source->Count()) {
            source->AppendTracks(added);  // keeps the rows on screen and the selection
            m_host.ListChanged();
        } else {
            m_host.ShowSource(Songs(m_provider, m_pager->tracks,
                                    error.empty() || !m_pager->tracks.empty() ? std::wstring(L"No songs.") : error));
        }
        m_host.ChromeChanged();
    }
};

class ArtistPage final : public AlbumBrowsePage {
public:
    ArtistPage(PageHost& host, stream::Provider& provider, View view) : AlbumBrowsePage(host, provider, std::move(view)) {}

    std::unique_ptr<media::ListSource> Start(bool reuse) override {
        const stream::Id id = m_view.remoteId;
        Attach(reuse,
               [&provider = m_provider, id](size_t offset, size_t limit, stream::Provider::AlbumPageDone done) {
                   provider.FetchArtistAlbums(id, offset, limit, std::move(done));
               },
               kArtistAlbumsPage);
        if (m_pager->artist) {
            Continue();  // catch up on pages not loaded yet
            return Current(L"Loading artist...");
        }
        m_provider.FetchArtist(id, Guard([this](stream::Artist artist, std::wstring error) { OnProfile(std::move(artist), error); }));
        return Songs(m_provider, {}, L"Loading artist...");
    }

    DetailInfo Detail() const override {
        DetailInfo d;
        d.visible = true;
        d.artIsArtist = true;
        const auto& artist = m_pager ? m_pager->artist : std::nullopt;
        d.title = artist && !artist->name.empty() ? artist->name : m_view.name;
        if (d.title.empty()) d.title = Tr(L"Artist");
        if (!artist) return d;
        d.subtitle = Joined({Count(artist->albumCount, L"albums"), Count(artist->trackCount, L"songs")});
        d.brief = artist->bio;
        // Ask for the avatar only once the profile is known; asking earlier would cache a
        // missing thumbnail.
        d.artKey = m_provider.ArtKey(stream::ArtKind::Artist, artist->id);
        d.artItem = m_provider.ArtItem(stream::ArtKind::Artist, artist->id);
        if (artist->followKnown && m_provider.Offers().followArtists) {
            d.action = DetailInfo::Action::Follow;
            d.actionOn = artist->followed;
            d.actionBusy = m_busy;
            d.actionLabel = Tr(artist->followed ? L"Following" : L"Follow");
        }
        return d;
    }

    void DetailAction() override {
        auto& artist = m_pager->artist;
        if (!artist || !artist->followKnown || m_busy || !SignedInOrPrompt()) return;
        const bool next = !artist->followed;
        artist->followed = next;  // optimistic; reverted on failure
        m_busy = true;
        m_host.ChromeChanged();
        m_provider.SetFollowed(artist->id, next, Guard([this, next](std::wstring error) {
            m_busy = false;
            if (!error.empty()) {
                if (m_pager->artist) m_pager->artist->followed = !next;
                ShowProviderError(m_provider, error);
            }
            m_host.ChromeChanged();
        }));
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        if (m_pager->artist && m_pager->artist->followKnown && m_provider.Offers().followArtists)
            actions.push_back({m_pager->artist->followed ? L"Unfollow artist" : L"Follow artist", [this] { DetailAction(); }});
    }

private:
    void OnProfile(stream::Artist artist, const std::wstring& error) {
        if (artist.id.empty()) {  // the profile request failed: say so in the list
            m_host.ShowSource(Songs(m_provider, {}, error.empty() ? std::wstring(L"Cannot load this artist.") : error));
            m_host.ChromeChanged();
            return;
        }
        if (artist.name.empty()) artist.name = m_view.name;
        // The avatar URL arrived with the profile; drop a thumbnail cached before it.
        ArtCache::Get().Invalidate(m_provider.ArtKey(stream::ArtKind::Artist, artist.id));
        m_pager->artist = std::move(artist);
        if (!m_pager->artist->name.empty()) m_host.SetPageName(m_pager->artist->name);
        m_provider.FetchArtistBio(m_view.remoteId, Guard([this](std::wstring text, std::wstring bioError) {
            if (!bioError.empty() || text.empty() || !m_pager->artist) return;
            m_pager->artist->bio = std::move(text);
            m_host.ChromeChanged();
        }));
        m_host.ChromeChanged();
        Continue();
    }

    void OnFollowed(std::optional<bool> followed) override {
        if (!followed || !m_pager->artist) return;
        m_pager->artist->followKnown = true;
        m_pager->artist->followed = *followed;
        m_host.ChromeChanged();
    }

    std::unique_ptr<AlbumGridSource> Grid(std::vector<stream::Album> albums, std::wstring empty) override {
        const std::wstring artistName = m_pager->artist ? m_pager->artist->name : m_view.name;
        return std::make_unique<AlbumGridSource>(
            m_provider, std::move(albums), artistName, std::move(empty),
            [this](const stream::Album* album) { OpenTile(album); },
            [this](POINT screen, const stream::Album* album) { TileMenu(screen, album); });
    }

    void OpenTile(const stream::Album* album) {
        const std::wstring artistName = m_pager->artist ? m_pager->artist->name : m_view.name;
        if (!album) m_host.Open(TopTracksView(m_provider, m_view.remoteId, artistName));
        else m_host.Open(AlbumView(m_provider, album->id, album->name, m_view.remoteId, artistName));
    }

    void TileMenu(POINT screen, const stream::Album* album) {
        std::vector<MenuAction> actions;
        if (album) {
            const stream::Album copy = *album;
            actions.push_back({L"View", [this, copy] { OpenTile(&copy); }});
            actions.push_back({});
            AddBulkActions(actions, FetchAlbumSongs(m_provider, copy.id), &m_provider);
            if (m_provider.SignedIn() && m_provider.Offers().saveAlbums) {
                actions.push_back({});
                actions.push_back(SaveAlbumAction(m_provider, copy.id, {}));
            }
        } else {
            actions.push_back({L"View", [this] { OpenTile(nullptr); }});
        }
        if (!actions.empty()) actions.push_back({});
        AddPageActions(actions);
        actions.insert(actions.end(), {{}, {L"Refresh", [this] { m_host.Refresh(); }}});
        ShowActionMenu(m_host.Owner(), screen, actions);
    }

    bool m_busy = false;
};

class SavedAlbumsPage final : public AlbumBrowsePage {
public:
    SavedAlbumsPage(PageHost& host, stream::Provider& provider, View view)
        : AlbumBrowsePage(host, provider, std::move(view)) {}

    std::unique_ptr<media::ListSource> Start(bool reuse) override {
        // Entering the section re-reads the first page, so albums saved elsewhere appear;
        // Back and a view style switch keep the pages already loaded.
        Attach(reuse,
               [&provider = m_provider](size_t offset, size_t limit, stream::Provider::AlbumPageDone done) {
                   provider.FetchSavedAlbums(offset, limit, std::move(done));
               },
               kSavedAlbumsPage);
        auto source = Current(L"Loading albums...");
        Continue();
        return source;
    }

private:
    std::unique_ptr<AlbumGridSource> Grid(std::vector<stream::Album> albums, std::wstring empty) override {
        return std::make_unique<AlbumGridSource>(
            m_provider, std::move(albums), std::wstring(), std::move(empty),
            [this](const stream::Album* album) {
                if (album) m_host.Open(AlbumView(m_provider, album->id, album->name));
            },
            [this](POINT screen, const stream::Album* album) {
                if (album) TileMenu(screen, *album);
            });
    }

    void TileMenu(POINT screen, const stream::Album& album) {
        std::vector<MenuAction> actions{{L"View", [this, album] { m_host.Open(AlbumView(m_provider, album.id, album.name)); }}};
        AddGoToArtist(actions, m_host, m_provider, album.artists);
        actions.push_back({});
        AddBulkActions(actions, FetchAlbumSongs(m_provider, album.id), &m_provider);
        if (m_provider.SignedIn() && m_provider.Offers().saveAlbums) {
            actions.push_back({});
            actions.push_back(SaveAlbumAction(m_provider, album.id, Guard([this, id = album.id](bool saved) {
                if (saved) return;
                m_pager->Remove(id);  // later pages and Back must not bring it back
                if (auto* grid = dynamic_cast<AlbumGridSource*>(m_host.Source())) {
                    grid->RemoveAlbum(id);
                    m_host.ListChanged();
                }
            })));
        }
        ShowActionMenu(m_host.Owner(), screen, actions);
    }
};

// ---- All Music > Favorites -------------------------------------------------------------------

class AllFavoritesPage final : public Page {
public:
    AllFavoritesPage(PageHost& host, View view) : Page(host, std::move(view)) {}

    std::unique_ptr<media::ListSource> Start(bool) override {
        for (auto* provider : stream::Providers()) {
            if (!provider->Offers().favorites || !provider->SignedIn()) continue;
            auto cached = provider->CachedFavorites();
            m_remote.push_back({provider, cached ? std::move(*cached) : std::vector<stream::Track>{}});
            provider->FetchFavorites(Guard([this, provider](std::vector<stream::Track> tracks, std::wstring error) {
                if (!error.empty()) return;  // keep what is shown
                for (auto& [owner, list] : m_remote)
                    if (owner == provider) list = std::move(tracks);
                m_host.ShowSource(Build());
            }));
        }
        return Build();
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        if (auto* source = dynamic_cast<SongTableSource*>(m_host.Source()); source && source->Count()) {
            AddBulkActions(actions, Ready(source->VisibleItems()));
            actions.push_back({});
        }
    }

    void OnFavoritesChanged() override {
        auto* source = dynamic_cast<SongTableSource*>(m_host.Source());
        if (source && source->RemoveUnfavorited()) m_host.ListChanged();
        else Page::OnFavoritesChanged();
    }

private:
    std::unique_ptr<media::ListSource> Build() const {
        // Local favorites are the Favorites playlist without provider tracks (those are
        // favorites in their own service).
        std::vector<metadb_handle_ptr> local;
        if (const t_size playlist = media::FavoritesPlaylist(false); playlist != SIZE_MAX) {
            metadb_handle_list items;
            playlist_manager::get()->playlist_get_all_items(playlist, items);
            for (size_t i = 0; i < items.get_count(); ++i)
                if (!stream::ProviderOf(items[i])) local.push_back(items[i]);
        }
        return std::make_unique<AllFavoritesSource>(local, m_remote);
    }

    AllFavoritesSource::RemoteFavorites m_remote;
};

}  // namespace

// ---- Page ------------------------------------------------------------------------------------

void Page::OnFavoritesChanged() {
    if (auto* source = dynamic_cast<SongTableSource*>(m_host.Source())) {
        source->RefreshFavorites();
        m_host.ListChanged();
    }
}

std::unique_ptr<Page> MakeRemotePage(PageHost& host, stream::Provider& provider, const View& view) {
    switch (view.kind) {
    case View::Kind::RemoteFavorites: return std::make_unique<FavoritesPage>(host, provider, view);
    case View::Kind::RemoteShelf: return std::make_unique<ShelfPage>(host, provider, view);
    case View::Kind::RemotePlaylist: return std::make_unique<PlaylistPage>(host, provider, view);
    case View::Kind::RemoteArtists: return std::make_unique<ArtistsPage>(host, provider, view);
    case View::Kind::RemoteAlbums: return std::make_unique<SavedAlbumsPage>(host, provider, view);
    case View::Kind::RemoteArtist: return std::make_unique<ArtistPage>(host, provider, view);
    case View::Kind::RemoteAlbum: return std::make_unique<AlbumPage>(host, provider, view);
    case View::Kind::RemoteTopTracks: return std::make_unique<TopTracksPage>(host, provider, view);
    default: return nullptr;
    }
}

std::unique_ptr<Page> MakeAllFavoritesPage(PageHost& host, const View& view) {
    return std::make_unique<AllFavoritesPage>(host, view);
}

// ---- views -----------------------------------------------------------------------------------

View RemoteView(const stream::Provider& provider, View::Kind kind) {
    View view;
    view.library = View::Library::Remote;
    view.provider = provider.Key();
    view.kind = kind;
    return view;
}

View ShelfView(const stream::Provider& provider, Shelf shelf) {
    View view = RemoteView(provider, View::Kind::RemoteShelf);
    view.shelf = shelf;
    view.shelfKnown = true;
    return view;
}

View PlaylistView(const stream::Provider& provider, const stream::Id& id, std::wstring name, std::optional<Shelf> shelf) {
    View view = RemoteView(provider, View::Kind::RemotePlaylist);
    view.remoteId = id;
    view.name = std::move(name);
    view.shelf = shelf.value_or(Shelf::Created);
    view.shelfKnown = shelf.has_value();
    return view;
}

View ArtistView(const stream::Provider& provider, const stream::Id& id, std::wstring name) {
    View view = RemoteView(provider, View::Kind::RemoteArtist);
    view.remoteId = id;
    view.name = std::move(name);
    return view;
}

View AlbumView(const stream::Provider& provider, const stream::Id& id, std::wstring name, const stream::Id& artistId,
               std::wstring artistName) {
    View view = RemoteView(provider, View::Kind::RemoteAlbum);
    view.remoteId = id;
    view.name = std::move(name);
    view.parentId = artistId;
    view.parentName = std::move(artistName);
    return view;
}

View TopTracksView(const stream::Provider& provider, const stream::Id& artistId, std::wstring artistName) {
    View view = RemoteView(provider, View::Kind::RemoteTopTracks);
    view.remoteId = artistId;
    view.name = std::move(artistName);
    return view;
}

const wchar_t* ShelfLabel(Shelf shelf) {
    switch (shelf) {
    case Shelf::Saved: return L"Collected playlists";
    case Shelf::Recommended: return L"Recommended";
    case Shelf::Created: break;
    }
    return L"Created playlists";
}

// ---- playlist commands --------------------------------------------------------------------------

void ShowRemotePlaylistMenu(PageHost& host, stream::Provider& provider, const stream::Id& id, const std::wstring& name,
                            std::optional<Shelf> shelf, POINT screen, std::function<void(const stream::Id&)> onUnsaved) {
    std::vector<MenuAction> actions{{L"View", [&host, &provider, id, name, shelf] {
                                         host.Open(PlaylistView(provider, id, name, shelf));
                                     }},
                                    {}};
    AddBulkActions(actions, FetchPlaylistSongs(provider, id), &provider, id);
    const bool own = shelf == Shelf::Created;
    if (provider.SignedIn() && own && provider.Offers().editPlaylists) {
        actions.push_back({});
        actions.push_back({L"Rename...", [&host, &provider, id, name] { RenameRemotePlaylist(host, provider, id, name); }});
        actions.push_back({L"Delete playlist", [&host, &provider, id, name] { DeleteRemotePlaylist(host, provider, id, name); }});
    } else if (provider.SignedIn() && !own && provider.Offers().savePlaylists) {
        const bool saved = shelf == Shelf::Saved || Contains(provider.Playlists(Shelf::Saved), id);
        actions.push_back({});
        actions.push_back({saved ? L"Uncollect playlist" : L"Collect playlist", [&provider, id, saved, onUnsaved] {
                               provider.SetPlaylistSaved(id, !saved, [&provider, id, saved, onUnsaved](std::wstring error) {
                                   if (!error.empty()) return ShowProviderError(provider, error);
                                   if (saved && onUnsaved) onUnsaved(id);
                               });
                           }});
    }
    if (shelf != Shelf::Recommended) actions.insert(actions.end(), {{}, {L"Refresh", [&host] { host.Refresh(); }}});
    ShowActionMenu(host.Owner(), screen, actions);
}


void CreateRemotePlaylist(PageHost& host, stream::Provider& provider) {
    if (!provider.SignedIn()) return provider.SignIn(host.Owner());
    const std::wstring title = Tr(L"New " + provider.Name() + L" playlist");
    PromptText(host.Owner(), title, Tr(L"Playlist name"), title, [&host, &provider](std::wstring name) {
        if (name.empty()) return;
        provider.CreatePlaylist(name, [&host, &provider, name, alive = host.Lifetime()](stream::Id id, std::wstring error) {
            if (!error.empty()) return ShowProviderError(provider, error);
            if (!alive.expired()) host.Open(PlaylistView(provider, id, name, Shelf::Created));
        });
    });
}

void RenameRemotePlaylist(PageHost& host, stream::Provider& provider, const stream::Id& id, std::wstring name) {
    if (!provider.SignedIn()) return;
    // The provider reports the new name as a library change; pages update from there.
    PromptText(host.Owner(), Tr(L"Rename playlist"), Tr(L"Playlist name"), name, [&provider, id, name](std::wstring newName) {
        if (newName.empty() || newName == name) return;
        provider.RenamePlaylist(id, newName, [&provider](std::wstring error) {
            if (!error.empty()) ShowProviderError(provider, error);
        });
    });
}

void DeleteRemotePlaylist(PageHost& host, stream::Provider& provider, const stream::Id& id, std::wstring name) {
    if (!provider.SignedIn()) return;
    const std::wstring message = Tr(L"Delete playlist") + L" \"" + name + L"\"?\n" + Tr(L"This cannot be undone.");
    if (MessageBoxW(host.Owner(), message.c_str(), Tr(L"Delete playlist").c_str(),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    provider.DeletePlaylist(id, [&host, &provider, id, alive = host.Lifetime()](std::wstring error) {
        if (!error.empty()) return ShowProviderError(provider, error);
        if (alive.expired()) return;
        const View& current = host.CurrentView();
        if (current.kind == View::Kind::RemotePlaylist && current.provider == provider.Key() && current.remoteId == id)
            host.Replace(ShelfView(provider, Shelf::Created));  // the deleted page leaves history
    });
}

}  // namespace glasswave::ui
