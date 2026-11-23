#include "pch.h"
#include "ui/remote_pages_parts.h"

// A source's song lists and list pages: favorites, a playlist, an album, an artist's top songs,
// a home list, the provider's groups and their songs, a playlist shelf.
namespace glasswave::ui::remotepages {

namespace {

// ---- Favorites -------------------------------------------------------------------------------

class FavoritesPage final : public TrackPagedPage {
  public:
    FavoritesPage(PageHost& host, stream::Provider& provider, View view)
        : TrackPagedPage(host, provider, std::move(view), Query(stream::ListKind::Favorites),
                         L"No favorites.") {}

    void OnFavoritesChanged() override {
        // Unliked songs leave the list in place. Only trust the flags once the favorites
        // were read; before that every song would look unliked.
        auto* source = SongSource();
        if (!source || !m_state.pages) return Page::OnFavoritesChanged();
        m_host.BeforeListChange();
        if (source->RemoveUnfavorited()) m_host.ListChanged();
    }
};

// ---- a playlist ------------------------------------------------------------------------------

class PlaylistPage final : public TrackPagedPage {
  public:
    PlaylistPage(PageHost& host, stream::Provider& provider, View view)
        : TrackPagedPage(host, provider, view, PlaylistQuery(provider, view.remoteId), L"No songs.") {}

    void ApplyTrackState() override {
        if (const auto& info = m_state.playlist; info && !info->id.empty()) {
            const std::wstring previousCover = m_info ? m_info->coverUrl : std::wstring();
            if (CoverChanged(m_provider, previousCover, info->coverUrl))
                ArtCache::Get().Invalidate(m_provider.ArtKey(stream::ArtKind::Playlist, info->id));
            const bool saved = m_info && (m_info->saved || m_info->own);
            m_info = *info;
            if (const auto current = m_provider.PlaylistSavedState(m_view.remoteId))
                m_info->saved = *current;
            else if (saved)
                m_info->saved = true;
            if (!m_info->name.empty()) m_host.SetPageName(m_info->name);
        }
    }

    DetailInfo Detail() const override {
        DetailInfo d;
        d.visible = true;
        d.title = m_info && !m_info->name.empty() ? m_info->name : m_view.name;
        if (d.title.empty()) d.title = ProviderPlaylistText(m_provider, L"Playlist");
        if (m_info) {
            d.subtitle = Joined({m_info->creator, Count(m_info->trackCount, L"songs")});
            d.tagline = m_info->tagline;
            d.facts = PlaylistFacts(*m_info);
            d.brief = m_info->description;
        }
        if (!m_info || !m_info->coverUrl.empty()) {
            d.artKey = m_provider.ArtKey(stream::ArtKind::Playlist, m_view.remoteId);
            d.artItem = m_provider.ArtItem(stream::ArtKind::Playlist, m_view.remoteId);
        }
        d.action = DetailInfo::Action::Play;
        d.actionLabel = Tr(L"Play");
        if (!Own() && m_provider.AccountVerified() && m_provider.Can(stream::Capability::SavePlaylists)) {
            d.saveLabel = m_provider.Text(Saved() ? L"Collected" : L"Collect");
            d.saveBusy = m_busy;
        }
        if (m_provider.Can(stream::Capability::Download)) d.downloadLabel = Tr(L"Download all");
        return d;
    }

    // Download all: every song of the page, read to the end first.
    void DownloadAction() override { DownloadSongs(AllSongsFetch(), m_host.Owner()); }

    // The band's Play: every song of the playlist, from the first.
    void DetailAction() override { PlayRemotePlaylist(m_provider, m_view.remoteId); }

    void SaveAction() override {
        if (m_busy || !VerifiedOrPrompt()) return;
        const bool next = !Saved();
        m_busy = true;
        m_host.ChromeChanged();
        m_provider.SetPlaylistSaved(m_view.remoteId, next, Guard([this, next](std::wstring error) {
                                        m_busy = false;
                                        if (!error.empty()) {
                                            // The change shown from the click on was taken back.
                                            if (m_info) m_info->saved = !next;
                                            ShowProviderError(m_provider, error);
                                        } else if (m_info) {
                                            m_info->saved =
                                                m_provider.PlaylistSavedState(m_view.remoteId).value_or(next);
                                        }
                                        m_host.ChromeChanged();
                                    }));
    }

  protected:
    void ConfigureSource() override {
        TrackPagedPage::ConfigureSource();
        if (auto* source = SongSource()) {
            source->SetDeleteHandler([this, alive = LifetimeToken()]() -> bool {
                if (alive.expired() || !Own() || !m_provider.Can(stream::Capability::RemovePlaylistTracks)) return false;
                auto* s = SongSource();
                if (!s) return false;
                auto ids = TrackIds(m_provider, s->SelectedItems());
                if (ids.empty()) return false;
                Remove(ids);
                return true;
            });
        }
    }

    stream::Id ExcludeRemotePlaylist() const override { return m_view.remoteId; }

    void AddTrackPageActions(std::vector<MenuAction>& actions) override {
        const std::wstring name = m_info && !m_info->name.empty() ? m_info->name : m_view.name;
        auto account = PlaylistAccountActions(m_host, m_provider, m_view.remoteId, name, m_view.sectionId, Flags(),
                                              Guard([this] { SaveAction(); }));
        if (account.empty()) return;
        actions.push_back({});
        actions.insert(actions.end(), std::make_move_iterator(account.begin()), std::make_move_iterator(account.end()));
    }

  public:
    void AddSongActions(std::vector<MenuAction>& actions, const metadb_handle_list& items) override {
        if (!Own() || !m_provider.Can(stream::Capability::RemovePlaylistTracks)) return;
        auto ids = TrackIds(m_provider, items);
        if (ids.empty()) return;
        actions.push_back({});
        actions.push_back(
            {ProviderPlaylistText(m_provider, L"Remove from this playlist"), [this, ids] { Remove(ids); }});
    }

    void OnLibraryChanged() override {
        TrackPagedPage::OnLibraryChanged();
        if (auto info = m_provider.FindPlaylist(m_view.remoteId)) {
            const bool saved = m_info && m_info->saved;
            m_info = std::move(info);
            if (const auto current = m_provider.PlaylistSavedState(m_view.remoteId))
                m_info->saved = *current;
            else
                m_info->saved = m_info->saved || saved;
            if (!m_info->name.empty()) m_host.SetPageName(m_info->name);
        }
        m_host.ChromeChanged();
    }

  private:
    PlaylistFlags Flags() const {
        const auto from = m_view.shelfKnown ? std::optional<Shelf>(m_view.shelf) : std::nullopt;
        return ResolvePlaylistFlags(m_provider, m_view.remoteId, from, m_info ? &*m_info : nullptr);
    }
    bool Own() const { return Flags().own; }
    bool Saved() const { return Flags().saved; }

    // The rows go at once; the request confirms in the background and a failure reloads.
    void Remove(const std::vector<stream::Id>& ids) {
        auto* source = SongSource();
        if (!source) return;
        m_host.BeforeListChange();
        const size_t removed = source->RemoveTracks(ids);
        if (m_info) m_info->trackCount = std::max(0, m_info->trackCount - static_cast<int>(removed));
        m_host.ListChanged();
        m_host.ChromeChanged();
        m_provider.RemoveFromPlaylist(m_view.remoteId, ids, Guard([this](std::wstring error) {
                                          if (error.empty()) return;
                                          ShowProviderError(m_provider, error);
                                          m_host.Reload(); // restore the rows
                                      }));
    }

    std::optional<stream::Playlist> m_info;
    bool m_busy = false;
};

// ---- an album --------------------------------------------------------------------------------

class AlbumPage final : public TrackPagedPage {
  public:
    AlbumPage(PageHost& host, stream::Provider& provider, View view)
        : TrackPagedPage(host, provider, view, AlbumQuery(provider, view.remoteId), L"No songs.") {}

    void ApplyTrackState() override {
        if (const auto& info = m_state.album; info && !info->id.empty()) {
            const std::wstring previousCover = m_info ? m_info->coverUrl : std::wstring();
            if (CoverChanged(m_provider, previousCover, info->coverUrl))
                ArtCache::Get().Invalidate(m_provider.ArtKey(stream::ArtKind::Album, info->id));
            m_info = *info;
            if (!m_info->name.empty()) m_host.SetPageName(m_info->name);
        }
    }

    DetailInfo Detail() const override {
        DetailInfo d;
        d.visible = true;
        d.title = m_info && !m_info->name.empty() ? m_info->name : m_view.name;
        if (d.title.empty()) d.title = Tr(L"Album");
        if (m_info) {
            d.subtitle = Joined({stream::JoinArtists(m_info->artists),
                                 m_info->year ? std::to_wstring(m_info->year) : L"",
                                 Count(m_info->trackCount, L"songs")});
            d.facts = AlbumFacts(*m_info);
            // The album's length, as the official clients show it: once every song is read.
            if (m_state.complete) {
                long long ms = 0;
                for (const auto& track : CurrentRows<stream::Track>()) ms += std::max(0, track.durationMs);
                const long long minutes = (ms + 30'000) / 60'000;
                if (minutes > 0) {
                    std::wstring length = minutes >= 60 ? std::to_wstring(minutes / 60) + L" " + Tr(L"hr") + L" " : L"";
                    if (minutes % 60 || minutes < 60) length += std::to_wstring(minutes % 60) + L" " + Tr(L"min");
                    d.facts.insert(d.facts.begin() + std::min<size_t>(2, d.facts.size()), {Tr(L"Length"), length});
                }
            }
            d.brief = m_info->description;
        }
        d.artKey = m_provider.ArtKey(stream::ArtKind::Album, m_view.remoteId);
        d.artItem = m_provider.ArtItem(stream::ArtKind::Album, m_view.remoteId);
        d.action = DetailInfo::Action::Play;
        d.actionLabel = Tr(L"Play");
        if (m_provider.AccountVerified() && m_provider.Can(stream::Capability::SaveAlbums)) {
            d.saveLabel = m_provider.Text(IsSavedAlbum(m_provider, m_view.remoteId) ? L"Collected" : L"Collect");
            d.saveBusy = m_busy;
        }
        if (m_provider.Can(stream::Capability::Download)) d.downloadLabel = Tr(L"Download all");
        return d;
    }

    // Download all: every song of the page, read to the end first.
    void DownloadAction() override { DownloadSongs(AllSongsFetch(), m_host.Owner()); }

    // The band's Play: every song of the album, from the first.
    void DetailAction() override { PlayRemoteAlbum(m_provider, m_view.remoteId); }

    void SaveAction() override {
        if (m_busy || !VerifiedOrPrompt()) return;
        m_busy = true;
        m_host.ChromeChanged();
        m_provider.SetAlbumSaved(m_view.remoteId, !IsSavedAlbum(m_provider, m_view.remoteId),
                                 Guard([this](std::wstring error) {
                                     m_busy = false;
                                     if (!error.empty()) ShowProviderError(m_provider, error);
                                     m_host.ChromeChanged();
                                 }));
    }

  protected:
    void AddTrackPageActions(std::vector<MenuAction>& actions) override { AddCollectAction(actions); }

  public:
    void AddSongActions(std::vector<MenuAction>& actions, const metadb_handle_list&) override {
        AddCollectAction(actions);
    }

    void OnLibraryChanged() override {
        TrackPagedPage::OnLibraryChanged();
        if (auto info = m_provider.FindAlbum(m_view.remoteId)) m_info = std::move(info);
        m_host.ChromeChanged();
    }

  private:
    // Collect or Uncollect, for a signed-in account of a source that can save albums.
    void AddCollectAction(std::vector<MenuAction>& actions) {
        if (!m_provider.AccountVerified() || !m_provider.Can(stream::Capability::SaveAlbums)) return;
        actions.push_back({});
        actions.push_back(
            SaveAlbumAction(m_provider, m_view.remoteId, Guard([this](bool) { m_host.ChromeChanged(); })));
    }

    std::optional<stream::Album> m_info;
    bool m_busy = false;
};

// ---- an artist's provider-labelled track list -----------------------------------------------

class TopTracksPage final : public TrackPagedPage {
  public:
    TopTracksPage(PageHost& host, stream::Provider& provider, View view)
        : TrackPagedPage(host, provider, view, IdQuery(stream::ListKind::ArtistTopTracks, view.remoteId),
                         LoadingArtistTracksLabel(provider, view.remoteId)) {}

  protected:
    void AddTrackPageActions(std::vector<MenuAction>& actions) override {
        if (m_provider.AccountVerified() && SupportsArtistPreference(m_provider))
            actions.push_back(FollowAction(m_provider, m_view.remoteId, {}));
    }
};

// ---- a home card's daily song list -----------------------------------------------------------

// The songs of a home card's list (每日推荐, a style's daily songs): fixed for the day, so a list
// page as a playlist's is (shown from the cache at once, read again behind it), with its name and
// how it is made in the band, and the first song's cover.
class HomeListPage final : public TrackPagedPage {
  public:
    HomeListPage(PageHost& host, stream::Provider& provider, View view)
        : TrackPagedPage(host, provider, view, IdQuery(stream::ListKind::HomeList, view.remoteId), L"No songs.") {}

    DetailInfo Detail() const override {
        DetailInfo d;
        d.visible = true;
        const auto& info = m_state.playlist;
        d.title = info && !info->name.empty() ? info->name : m_view.name;
        d.tagline = info ? info->tagline : std::wstring();
        const auto& tracks = CurrentRows<stream::Track>();
        if (!tracks.empty()) {
            d.subtitle = std::to_wstring(tracks.size()) + Tr(L" songs");
            d.artItem = m_provider.TrackItem(tracks.front().id);
            d.artKey = ArtCache::SourceKey(d.artItem);
        }
        // These lists have no cover of their own (nor in their clients): one is made from the
        // first song's, with the list's name and the source's mark.
        d.artCaption = d.title;
        d.artBadge = m_provider.HomeListBadge(m_view.remoteId);
        d.action = DetailInfo::Action::Play;
        d.actionLabel = Tr(L"Play");
        if (m_provider.Can(stream::Capability::Download)) d.downloadLabel = Tr(L"Download all");
        return d;
    }

    void DetailAction() override { // every song, from the first
        EnsureAllSongs(Guard([this](bool complete) {
            if (auto* source = SongSource(); complete && source && source->Count()) source->PlayFrom(0);
        }));
    }
    void DownloadAction() override { DownloadSongs(AllSongsFetch(), m_host.Owner()); }

  protected:
    void ApplyTrackState() override {
        if (const auto& info = m_state.playlist; info && !info->name.empty()) m_host.SetPageName(info->name);
    }
};

// ---- provider-defined groups and tracks ------------------------------------------------------

class BrowseGroupSource final : public GridSource {
  public:
    using Open = std::function<void(const stream::BrowseGroup&)>;

    BrowseGroupSource(std::vector<stream::BrowseGroup> groups, std::wstring emptyText, Open open)
        : GridSource({{L"Name", 0, true}, {L"Songs", 100, false, true}}, std::move(emptyText)),
          m_groups(std::move(groups)), m_open(std::move(open)) {
        Refilter();
    }

    std::wstring Cell(size_t row, size_t column) const override {
        return row < m_rows.size() ? ItemCell(m_rows[row], column) : std::wstring();
    }
    bool SupportsViewStyle(media::ViewStyle style) const override {
        return style == media::ViewStyle::Details;
    }
    media::ViewStyle GetViewStyle() const override { return media::ViewStyle::Details; }
    void Activate(size_t row) override {
        if (row < m_rows.size() && m_open) m_open(m_groups[m_rows[row]]);
    }
    void ContextMenu(HWND, POINT screen) override {
        const size_t focus = Focus();
        if (focus >= m_rows.size()) return;
        const stream::BrowseGroup group = m_groups[m_rows[focus]];
        ShowActionMenu(m_owner, screen, {{Tr(L"View"), [open = m_open, group] {
                                              if (open) open(group);
                                          }}});
    }
    void SetOwner(HWND owner) { m_owner = owner; }
    void ReplaceGroups(const std::vector<stream::BrowseGroup>& groups) {
        m_groups = groups;
        Refilter();
    }

  private:
    size_t ItemCount() const override { return m_groups.size(); }
    std::wstring ItemCell(size_t item, size_t column) const override {
        if (item >= m_groups.size()) return {};
        return column == 1 ? std::to_wstring(m_groups[item].trackCount) : m_groups[item].label;
    }
    std::wstring FilterText(size_t item) const override { return m_groups[item].label; }
    std::wstring Identity(size_t item) const override {
        return item < m_groups.size() ? L"id:" + m_groups[item].id : std::wstring();
    }

    std::vector<stream::BrowseGroup> m_groups;
    Open m_open;
    HWND m_owner = nullptr;
};

class GroupsPage final : public PagedPage {
  public:
    GroupsPage(PageHost& host, stream::Provider& provider, View view)
        : PagedPage(host, provider, view, GroupsQuery(view.sectionId)) {}

  protected:
    std::unique_ptr<media::ListSource> BuildSource() override {
        auto source = std::make_unique<BrowseGroupSource>(
            CurrentRows<stream::BrowseGroup>(), L"No groups.", [this](const stream::BrowseGroup& group) {
                if (const auto section = FindSection(m_provider, m_view.sectionId))
                    m_host.Open(GroupTracksView(m_provider, *section, group));
            });
        source->SetOwner(m_host.Owner());
        return source;
    }

    void ApplyPageState() override {
        if (auto* source = dynamic_cast<BrowseGroupSource*>(m_source)) {
            source->ReplaceGroups(CurrentRows<stream::BrowseGroup>());
        }
    }
};

class SectionTracksPage final : public TrackPagedPage {
  public:
    SectionTracksPage(PageHost& host, stream::Provider& provider, View view)
        : TrackPagedPage(host, provider, view, SectionTracksQuery(view.sectionId, {}), L"No songs found.") {}
};

class GroupTracksPage final : public TrackPagedPage {
  public:
    GroupTracksPage(PageHost& host, stream::Provider& provider, View view)
        : TrackPagedPage(host, provider, view, SectionTracksQuery(view.sectionId, view.groupId),
                         L"No songs found.") {}
};

// Shares the provider's in-memory section playlist snapshot with NavPane so sidebar and
// playlist tiles update in the same frame without duplicate /playlists fetches.
class ShelfPage final : public RemotePage {
  public:
    ShelfPage(PageHost& host, stream::Provider& provider, View view)
        : RemotePage(host, provider, std::move(view)) {}

    bool CanRefresh() const override { return m_view.shelf != Shelf::Recommended; } // a daily set

    std::unique_ptr<media::ListSource> Start(bool) override {
        auto source = Grid(CurrentPlaylists());
        m_source = source.get();
        UpdateLoadingText();
        return source;
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        if (m_view.shelf == Shelf::Created && m_provider.Can(stream::Capability::CreatePlaylists))
            actions.push_back(NewPlaylistAction(m_host, m_provider));
    }

    void OnLibraryChanged() override {
        if (!m_source) return;
        m_host.BeforeListChange();
        m_source->ReplacePlaylists(CurrentPlaylists());
        UpdateLoadingText();
        m_host.ListChanged();
        m_host.ChromeChanged();
    }

  private:
    std::vector<stream::Playlist> CurrentPlaylists() const {
        if (!m_view.sectionId.empty()) return m_provider.SectionPlaylists(m_view.sectionId);
        return m_provider.Playlists(m_view.shelf);
    }

    void UpdateLoadingText() {
        if (!m_source) return;
        const bool loading =
            m_source->Count() == 0 && (m_provider.SessionRestoring() || !m_provider.AccountVerified());
        m_source->SetLoadingText(LoadingStatus(loading, {}));
    }

    std::unique_ptr<PlaylistGridSource> Grid(std::vector<stream::Playlist> playlists) {
        const Shelf shelf = m_view.shelf;
        return std::make_unique<PlaylistGridSource>(
            m_provider, std::move(playlists), ProviderPlaylistText(m_provider, L"No playlists."),
            [this, shelf](const stream::Playlist& p) {
                m_host.Open(PlaylistView(m_provider, p.id, p.name, shelf, m_view.sectionId));
            },
            [this](POINT screen, const stream::Playlist& p) { TileMenu(screen, p); });
    }

    void TileMenu(POINT screen, const stream::Playlist& playlist) {
        ShowRemotePlaylistMenu(m_host, m_provider, playlist.id, playlist.name, m_view.shelf, m_view.sectionId,
                               screen, Guard([this](const stream::Id& id) {
                                   // Uncollected on the collected shelf: the tile goes at once.
                                   auto* grid = dynamic_cast<PlaylistGridSource*>(m_host.Source());
                                   if (m_view.shelf == Shelf::Saved && grid) {
                                       m_host.BeforeListChange();
                                       grid->RemovePlaylist(id);
                                       m_host.ListChanged();
                                   }
                               }));
    }

    PlaylistGridSource* m_source = nullptr;
};

} // namespace

std::unique_ptr<Page> MakeListPage(PageHost& host, stream::Provider& provider, const View& view) {
    switch (view.kind) {
    case View::Kind::RemoteFavorites:
        return std::make_unique<FavoritesPage>(host, provider, view);
    case View::Kind::RemoteShelf:
        return std::make_unique<ShelfPage>(host, provider, view);
    case View::Kind::RemotePlaylist:
        return std::make_unique<PlaylistPage>(host, provider, view);
    case View::Kind::RemoteAlbum:
        return std::make_unique<AlbumPage>(host, provider, view);
    case View::Kind::RemoteTopTracks:
        return std::make_unique<TopTracksPage>(host, provider, view);
    case View::Kind::RemoteSectionTracks:
        return std::make_unique<SectionTracksPage>(host, provider, view);
    case View::Kind::RemoteGroups:
        return std::make_unique<GroupsPage>(host, provider, view);
    case View::Kind::RemoteGroupTracks:
        return std::make_unique<GroupTracksPage>(host, provider, view);
    case View::Kind::RemoteHomeList:
        return std::make_unique<HomeListPage>(host, provider, view);
    default:
        return nullptr;
    }
}

} // namespace glasswave::ui::remotepages
