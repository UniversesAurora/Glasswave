#include "pch.h"
#include "ui/remote_pages_parts.h"

// A source's artists and album browsing: the followed artists, the saved albums and an artist's page.
namespace glasswave::ui::remotepages {

namespace {

// ---- followed artists ---------------------------------------------------------------------------

class ArtistsPage final : public PagedPage {
  public:
    ArtistsPage(PageHost& host, stream::Provider& provider, View view)
        : PagedPage(host, provider, view, Query(stream::ListKind::FollowedArtists)) {}

  protected:
    std::unique_ptr<media::ListSource> BuildSource() override { return Grid(CurrentRows<stream::Artist>()); }

    void ApplyPageState() override {
        if (auto* source = dynamic_cast<ArtistGridSource*>(m_source)) {
            source->ReplaceArtists(CurrentRows<stream::Artist>());
        }
    }

  private:
    std::unique_ptr<ArtistGridSource> Grid(std::vector<stream::Artist> artists) {
        return std::make_unique<ArtistGridSource>(
            m_provider, std::move(artists), L"No artists.",
            [this](const stream::Artist& a) {
                m_host.Open(ArtistView(m_provider, a.id, a.name, m_view.sectionId));
            },
            [this](POINT screen, const stream::Artist& a) {
                RecordMenuContext context;
                context.sectionId = m_view.sectionId;
                context.removed = Guard([this](const stream::Id&) { m_host.Reload(); }); // unfollowed
                context.page.push_back(RefreshAction(m_host));
                ShowRecordMenu(m_host, m_provider, a, screen, context);
            });
    }
};

// ---- album browsing (an artist's albums, the saved albums) ------------------------------------

class AlbumBrowsePage : public PagedPage {
  public:
    bool StyleRebuildsPage() const override { return true; }

    void OnPresentationChanged() override {
        if (!Expanded() || !m_session) {
            PagedPage::OnPresentationChanged();
            return;
        }
        // Expanded, the songs come album by album in the albums' own order: more of them are
        // needed only as the list scrolls on, all of them only to sort or filter (the tile
        // style alone does not need the whole list here, unlike a list of loose songs).
        m_allMode = CompleteNeeded();
        m_session->SetMode(m_allMode || m_drainRequested ? stream::ListLoadMode::All
                                                         : stream::ListLoadMode::OnDemand);
        PresentationChanged();
        UpdateLoadingText();
        FinishDrainIfReady();
        StartNextAlbumTracks();
        m_host.ListChanged();
    }

    void NearEnd() override {
        if (!Expanded()) {
            PagedPage::NearEnd();
            return;
        }
        m_tracksWanted = true;
        StartNextAlbumTracks();
    }

  protected:
    AlbumBrowsePage(PageHost& host, stream::Provider& provider, View view, stream::ListQuery query)
        : PagedPage(host, provider, std::move(view), std::move(query)) {}

    bool Expanded() const { return m_host.SavedStyle(media::ViewStyle::Icons) == media::ViewStyle::Tiles; }

    std::unique_ptr<media::ListSource> BuildSource() override {
        NormalizeAlbums();
        if (Expanded())
            return Songs(m_provider, m_query.kind, m_tracks, m_tracks.empty() ? L"Loading album songs..." : L"No songs.");
        return IconsSource();
    }

    // The icon view: the albums as tiles (an artist's page shows its overview instead).
    virtual std::unique_ptr<media::ListSource> IconsSource() { return Grid(m_albums); }

    void ApplyPageState() override {
        if (const auto& header = m_state.artist; header && !header->id.empty()) {
            stream::Artist artist = *header;
            if (m_artist) {
                if (m_artist->profile && !artist.profile) {
                    stream::CopyArtistProfile(*m_artist, artist);
                    if (!m_artist->name.empty()) artist.name = m_artist->name;
                    if (!m_artist->avatarUrl.empty()) artist.avatarUrl = m_artist->avatarUrl;
                }
                if (artist.bio.empty()) artist.bio = m_artist->bio;
                KeepCounts(*m_artist, artist);
                if (!artist.followKnown && m_artist->followKnown) {
                    artist.followKnown = true;
                    artist.followed = m_artist->followed;
                }
            }
            m_artist = std::move(artist);
        }
        NormalizeAlbums();
        AssembleTracks();
        if (auto* grid = dynamic_cast<AlbumGridSource*>(m_source)) {
            grid->ReplaceAlbums(m_albums);
        } else if (auto* songs = dynamic_cast<RemoteSongSource*>(m_source)) {
            songs->ReplaceTracks(m_tracks);
        }
        if (Expanded()) StartNextAlbumTracks();
        SettleWaiters();
    }

    void ConfigureSource() override {
        if (auto* source = dynamic_cast<RemoteSongSource*>(m_source))
            source->SetActivateHandler(
                Guard([this](size_t row, const std::wstring& identity) { ActivateExpanded(row, identity); }));
    }

    stream::ListLoadMode InitialMode() const override { return stream::ListLoadMode::OnDemand; }

    void AddPageSpecificActions(std::vector<MenuAction>& actions) override {
        if (Expanded()) {
            if (auto* source = SongSource(); source && source->Count()) {
                AddBulkActions(actions, ExpandedSongsFetch(), &m_provider);
                actions.push_back({});
            }
        }
        AddAlbumPageActions(actions);
    }

    void OnLibraryChanged() override {
        UpdateLoadingText();
        m_host.ListChanged();
        m_host.ChromeChanged();
    }

    bool HasPagedError() const override { return PagedPage::HasPagedError() || !m_trackError.empty(); }

    std::wstring StatusText() const override {
        if (!m_trackError.empty()) return LoadingStatus(false, m_trackError);
        const bool expanding =
            Expanded() && m_state.error.empty() && ((CompleteNeeded() && !AlbumsAndTracksComplete()) || m_filling);
        const bool albumLoading = std::any_of(m_albumTrackSessions.begin(), m_albumTrackSessions.end(),
                                              [](const auto& entry) {
                                                  return entry.second && VisiblyLoading(entry.second->View());
                                              });
        return expanding || albumLoading ? LoadingStatus(true) : PagedPage::StatusText();
    }

    void RetryPage() override {
        if (!m_state.error.empty() && !m_state.loading && m_session) m_session->Refresh();
        m_trackError.clear();
        for (const auto& [id, session] : m_albumTrackSessions)
            if (session && !session->View().error.empty()) session->Refresh();
        StartNextAlbumTracks();
    }

    virtual std::unique_ptr<AlbumGridSource> Grid(std::vector<stream::Album> albums) = 0;
    virtual void AddAlbumPageActions(std::vector<MenuAction>&) {}

    void ForgetAlbum(const stream::Id& id) {
        std::erase_if(m_albums, [&id](const stream::Album& album) { return album.id == id; });
        const auto session = m_albumTrackSessions.find(id);
        if (session != m_albumTrackSessions.end()) {
            if (session->second) session->second->Cancel();
            m_albumTrackSessions.erase(session);
        }
        m_albumTrackVersions.erase(id);
        AssembleTracks();
    }

    std::vector<stream::Album> m_albums;
    std::vector<stream::Track> m_tracks;
    std::optional<stream::Artist> m_artist;

  private:
    void NormalizeAlbums() {
        m_albums.clear();
        std::unordered_set<stream::Id> ids;
        for (const auto& album : CurrentRows<stream::Album>()) {
            if (!album.id.empty() && !ids.insert(album.id).second) continue;
            m_albums.push_back(album);
        }
    }

    // The album's track list as its shared session holds it (borrowed, never copied), or null
    // before the session is open.
    const stream::ListState* AlbumTrackState(const stream::Id& id) const {
        const auto found = m_albumTrackSessions.find(id);
        return found != m_albumTrackSessions.end() && found->second ? &found->second->View() : nullptr;
    }

    void AssembleTracks() {
        std::vector<stream::Track> tracks;
        for (const auto& album : m_albums) {
            const auto* state = AlbumTrackState(album.id);
            if (!state) continue;
            const auto& rows = Rows<stream::Track>(state->page.rows);
            tracks.insert(tracks.end(), rows.begin(), rows.end());
        }
        m_tracks = std::move(tracks);
    }

    bool AlbumsAndTracksComplete() const {
        if (!m_state.complete) return false;
        for (const auto& album : m_albums) {
            if (album.id.empty()) continue;
            const auto* state = AlbumTrackState(album.id);
            if (!state || !state->complete) return false;
        }
        return true;
    }

    void RecomputeTrackError() {
        m_trackError.clear();
        for (const auto& [id, session] : m_albumTrackSessions) {
            if (!session) continue;
            const auto& state = session->View();
            if (!state.error.empty() && !state.loading) {
                m_trackError = state.error;
                return;
            }
        }
    }

    // Everything is needed: to sort or filter the songs, or for a play / add of all of them.
    bool CompleteNeeded() const {
        return m_drain.Waiting() || (m_source && (m_source->HasFilter() || m_source->SortColumn() != SIZE_MAX));
    }

    // One album's songs at a time, the next only when wanted (the list reached its end, or
    // everything is needed) and never sooner than the source's gap between album reads after the
    // last request (Provider::ListRequestGap; NetEase answers a quick run with "操作频繁").
    void StartNextAlbumTracks() {
        if (!Expanded() || !m_trackError.empty()) return;
        const bool all = CompleteNeeded();
        for (const auto& album : m_albums) {
            if (album.id.empty()) continue;
            if (!m_albumTrackSessions.contains(album.id)) {
                if (!all && !m_tracksWanted) return;
                // The gap counts from the last request that went to the service: albums the
                // disk cache holds follow one another at once.
                const ULONGLONG gap = static_cast<ULONGLONG>(
                    m_provider.ListRequestGap(stream::ListKind::AlbumTracks).count());
                const ULONGLONG now = GetTickCount64(), since = now - m_lastTrackRequest;
                if (m_lastTrackRequest && since < gap) {
                    ScheduleAlbumTracks(gap - since);
                    return;
                }
                m_tracksWanted = false;
                OpenAlbumTracks(album);
                return;
            }
            const auto* state = AlbumTrackState(album.id);
            if (!state || !state->complete) return;
        }
        // Every album listed so far has its songs: the next page of albums, when wanted.
        if (!m_state.complete) {
            if ((all || m_tracksWanted) && m_session && !m_state.loading && m_state.error.empty())
                m_session->RequestNext();
            return;
        }
        m_tracksWanted = false; // nothing left to want
        SettleWaiters();
    }

    bool AnyAlbumLoading() const {
        for (const auto& [id, session] : m_albumTrackSessions)
            if (session && !session->View().complete && session->View().error.empty()) return true;
        return false;
    }

    // The screen is full (nothing more is wanted, nothing is on its way): one band covered the
    // whole fill, instead of coming and going with each album (owner, 2026-11-14).
    void EndFillIfIdle() {
        if (!m_filling || m_tracksWanted || m_trackTimerArmed || AnyAlbumLoading() || m_state.loading) return;
        m_filling = false;
        UpdateLoadingText();
        m_host.ListChanged();
    }

    void ScheduleAlbumTracks(ULONGLONG waitMs) {
        if (m_trackTimerArmed) return;
        m_trackTimerArmed = true;
        fb2k::callLater(static_cast<double>(waitMs) / 1000.0, Guard([this] {
                            m_trackTimerArmed = false;
                            StartNextAlbumTracks();
                            EndFillIfIdle();
                        }));
    }

    void OpenAlbumTracks(const stream::Album& album) {
        if (album.id.empty()) return;
        const uint64_t generation = ++m_albumTrackGenerations[album.id];
        m_albumTrackSessions[album.id] = {};
        auto query = Query(stream::ListKind::AlbumTracks);
        query.id = album.id;
        auto changed = Guard([this, id = album.id, generation](const stream::ListState& state) {
            if (m_albumTrackGenerations[id] != generation) return;
            if (state.loading || state.refreshing) m_lastTrackRequest = GetTickCount64();
            if (VisiblyLoading(state)) m_filling = true; // read from the service: the band shows
            const auto [version, inserted] = m_albumTrackVersions.try_emplace(id, state.dataVersion);
            const bool dataChanged = inserted || version->second != state.dataVersion;
            version->second = state.dataVersion;
            RecomputeTrackError();
            if (dataChanged) {
                m_host.BeforeListChange();
                AssembleTracks();
                if (auto* source = dynamic_cast<RemoteSongSource*>(m_source); source && Expanded())
                    source->ReplaceTracks(m_tracks);
            }
            UpdateLoadingText();
            SettleWaiters();
            m_host.ListChanged(); // may find the screen not yet full (NearEnd: another album)
            if (state.complete || (!state.error.empty() && !state.loading)) StartNextAlbumTracks();
            EndFillIfIdle();
        });
        auto session = stream::ListSession::Open(m_provider, PreparedQuery(m_provider, std::move(query)),
                                                 std::move(changed), stream::ListLoadMode::All);
        m_albumTrackSessions[album.id] = session;
        m_albumTrackVersions[album.id] = session->View().dataVersion;
        // A shared session may already hold the album's tracks (another page read them).
        if (!Rows<stream::Track>(session->View().page.rows).empty()) {
            m_host.BeforeListChange();
            AssembleTracks();
            if (auto* source = dynamic_cast<RemoteSongSource*>(m_source); source && Expanded())
                source->ReplaceTracks(m_tracks);
            m_host.ListChanged();
        }
    }

    void EnsureExpandedSongs(std::function<void(bool)> done) {
        if (!m_session || !m_trackError.empty() || (!m_state.error.empty() && !m_state.loading)) {
            done(false);
            return;
        }
        m_drain.Wait(std::move(done));
        m_session->SetMode(stream::ListLoadMode::All);
        if (!m_state.loading && !m_state.complete) m_session->RequestNext();
        StartNextAlbumTracks();
        SettleWaiters();
    }

    // Settled once every album's songs are read, or one album's read failed.
    void SettleWaiters() {
        if (!m_drain.Waiting() || !m_state.complete) return;
        for (const auto& album : m_albums) {
            if (album.id.empty()) continue;
            const auto* state = AlbumTrackState(album.id);
            if (!state || (!state->complete && state->error.empty())) return;
            if (!state->error.empty() && !state->loading) return m_drain.Settle(false);
        }
        m_drain.Settle(true);
    }

    SongsFetch ExpandedSongsFetch() {
        return AllSongs([this](SongDrain::Waiter done) { EnsureExpandedSongs(std::move(done)); });
    }

    // A song of the expanded view plays its album, at once (owner, 2026-11-14): waiting for
    // every album of the artist first left a double click doing nothing for a long while.
    void ActivateExpanded(size_t row, const std::wstring&) {
        if (auto* source = SongSource()) source->PlayGroupFrom(row);
    }

    std::map<stream::Id, std::shared_ptr<stream::ListSession>> m_albumTrackSessions;
    std::map<stream::Id, uint64_t> m_albumTrackVersions; // data version last assembled
    std::map<stream::Id, uint64_t> m_albumTrackGenerations;
    SongDrain m_drain;
    std::wstring m_trackError;
    bool m_tracksWanted = true; // the first album's songs fill the empty list
    bool m_trackTimerArmed = false;
    bool m_filling = false; // filling the screen from the service: the band stays until it is full
    ULONGLONG m_lastTrackRequest = 0; // the last album read from the service
};

class ArtistPage final : public AlbumBrowsePage {
  public:
    ArtistPage(PageHost& host, stream::Provider& provider, View view)
        : AlbumBrowsePage(host, provider, view, ArtistAlbumsQuery(view.remoteId)) {}

    // The overview's groups open by their own links; the albums' pages are not paged in here.
    void NearEnd() override {
        if (Expanded()) AlbumBrowsePage::NearEnd();
    }

    std::wstring StatusText() const override {
        if (Expanded()) return AlbumBrowsePage::StatusText();
        bool any = false;
        std::wstring error;
        for (const auto& session : m_sectionSessions) {
            if (!session) continue;
            const auto& state = session->View();
            if (VisiblyLoading(state)) return LoadingStatus(true);
            any |= std::visit([](const auto& rows) { return !rows.empty(); }, state.page.rows);
            if (error.empty() && !state.error.empty() && !state.loading) error = state.error;
        }
        // A section that failed is left out; only an overview with nothing at all says why.
        return LoadingStatus(false, any ? std::wstring() : error);
    }

    std::unique_ptr<media::ListSource> Start(bool reuse) override {
        auto source = PagedPage::Start(reuse);
        AfterStart([this] {
            if (!m_artist || !m_artist->profile || m_artist->bio.empty()) LoadProfile();
        });
        return source;
    }

    DetailInfo Detail() const override {
        DetailInfo d;
        d.visible = true;
        d.artIsArtist = true;
        d.title = m_artist && !m_artist->name.empty() ? m_artist->name : m_view.name;
        if (d.title.empty()) d.title = Tr(L"Artist");
        if (!m_artist) return d;
        d.subtitle = Joined({Count(m_artist->albumCount, L"albums"), Count(m_artist->trackCount, L"songs")});
        d.tagline = m_artist->tagline;
        d.facts = ArtistFacts(*m_artist);
        d.brief = m_artist->bio;
        d.artKey = m_provider.ArtKey(stream::ArtKind::Artist, m_artist->id);
        d.artItem = m_provider.ArtItem(stream::ArtKind::Artist, m_artist->id);
        if (m_artist->followKnown && SupportsArtistPreference(m_provider)) {
            const bool followed = Followed();
            d.action = DetailInfo::Action::Follow;
            d.actionOn = followed;
            d.actionBusy = m_busy;
            d.actionLabel = m_provider.Text(followed ? L"Unfollow artist" : L"Follow artist");
        }
        return d;
    }

    void DetailAction() override {
        if (!m_artist || !m_artist->followKnown || m_busy || !VerifiedOrPrompt()) return;
        const bool next = !Followed();
        m_artist->followed = next;
        m_busy = true;
        m_host.ChromeChanged();
        m_provider.SetFollowed(m_artist->id, next, Guard([this, next](std::wstring error) {
                                   m_busy = false;
                                   if (!error.empty()) {
                                       if (m_artist) m_artist->followed = !next;
                                       ShowProviderError(m_provider, error);
                                   }
                                   m_host.ChromeChanged();
                               }));
    }

  protected:
    // The provider's view of the follow state (a write in flight, a change made on another
    // page) over the copy the header was read with, as the follow menu items use it.
    bool Followed() const {
        return m_provider.ArtistPreferenceState(m_artist->id).value_or(m_artist->followed);
    }

    void OnLibraryChanged() override {
        if (m_artist && m_artist->followKnown) m_artist->followed = Followed(); // followed elsewhere
        AlbumBrowsePage::OnLibraryChanged();
    }

    void ApplyPageState() override {
        AlbumBrowsePage::ApplyPageState();
        if (m_artist && !m_artist->name.empty()) m_host.SetPageName(m_artist->name);
    }

    std::unique_ptr<AlbumGridSource> Grid(std::vector<stream::Album> albums) override {
        const std::wstring artistName = m_artist ? m_artist->name : m_view.name;
        return std::make_unique<AlbumGridSource>(
            m_provider, std::move(albums), artistName, m_provider.ArtistTracksLabel(m_view.remoteId),
            L"No albums.", [this](const stream::Album* album) { OpenTile(album); },
            [this](POINT screen, const stream::Album* album) { TileMenu(screen, album); });
    }

    void AddAlbumPageActions(std::vector<MenuAction>& actions) override {
        if (m_artist && m_artist->followKnown && SupportsArtistPreference(m_provider)) {
            actions.push_back({m_provider.Text(Followed() ? L"Unfollow artist" : L"Follow artist"),
                               [this] { DetailAction(); }});
        }
    }

    std::unique_ptr<media::ListSource> IconsSource() override {
        const auto sections = m_provider.ArtistSections(m_view.remoteId);
        ArtistOverviewSource::Actions actions;
        actions.openAlbum = Guard([this](const stream::Album& album) { OpenTile(&album); });
        actions.openPlaylist = Guard([this](const stream::Playlist& playlist) {
            m_host.Open(PlaylistView(m_provider, playlist.id, playlist.name, std::nullopt, m_view.sectionId));
        });
        actions.openArtist = Guard([this](const stream::Artist& artist) {
            m_host.Open(ArtistView(m_provider, artist.id, artist.name, m_view.sectionId));
        });
        actions.menu = Guard([this](POINT screen, const ArtistOverviewSource::Value& value) { OverviewMenu(screen, value); });
        actions.showAll = Guard([this](size_t section, bool open) {
            // Opened, a section reads the rest of its pages; either way the lines change.
            if (open && section < m_sectionSessions.size() && m_sectionSessions[section]) {
                auto& session = m_sectionSessions[section];
                session->SetMode(stream::ListLoadMode::All);
                if (!session->View().loading && !session->View().complete) session->RequestNext();
            }
            UpdateLoadingText();
            m_host.ListChanged();
        });
        auto source = std::make_unique<ArtistOverviewSource>(m_provider, sections, std::move(actions));
        m_overview = source.get();
        // One session per section, its first page only; a section the page's own album list
        // also is (the same query) shares that list's session.
        m_sectionSessions.assign(sections.size(), nullptr);
        for (size_t i = 0; i < sections.size(); ++i) {
            auto changed = Guard([this, i](const stream::ListState& state) {
                if (!m_overview) return;
                const bool shown = m_source == m_overview;
                if (shown) m_host.BeforeListChange();
                m_overview->SetSection(i, state);
                if (!shown) return;
                UpdateLoadingText();
                m_host.ListChanged();
            });
            m_sectionSessions[i] = stream::ListSession::Open(
                m_provider, PreparedQuery(m_provider, stream::ArtistSectionQuery(m_view.remoteId, sections[i])),
                std::move(changed));
            m_overview->SetSection(i, m_sectionSessions[i]->View());
        }
        return source;
    }

  private:
    void OverviewMenu(POINT screen, const ArtistOverviewSource::Value& value) {
        if (const auto* album = std::get_if<stream::Album>(&value)) {
            TileMenu(screen, album);
        } else if (const auto* playlist = std::get_if<stream::Playlist>(&value)) {
            ShowRemotePlaylistMenu(m_host, m_provider, playlist->id, playlist->name, std::nullopt, m_view.sectionId,
                                   screen);
        } else if (const auto* artist = std::get_if<stream::Artist>(&value)) {
            const stream::Artist a = *artist;
            std::vector<MenuAction> actions{
                {L"View", [this, a] { m_host.Open(ArtistView(m_provider, a.id, a.name, m_view.sectionId)); }}};
            ShowActionMenu(m_host.Owner(), screen, actions);
        }
    }

    void LoadProfile() {
        m_provider.FetchArtist(
            m_view.remoteId, Guard([this](stream::Artist artist, std::wstring) {
                if (artist.id.empty()) return;
                if (artist.name.empty()) artist.name = m_view.name;
                if (m_artist && artist.bio.empty()) artist.bio = m_artist->bio;
                if (m_artist) KeepCounts(*m_artist, artist);
                if (m_artist && CoverChanged(m_provider, m_artist->avatarUrl, artist.avatarUrl))
                    ArtCache::Get().Invalidate(m_provider.ArtKey(stream::ArtKind::Artist, artist.id));
                m_artist = std::move(artist);
                if (!m_artist->name.empty()) m_host.SetPageName(m_artist->name);
                m_provider.FetchArtistBio(m_view.remoteId,
                                          Guard([this](std::wstring text, std::wstring error) {
                                              if (error.empty() && !text.empty() && m_artist) {
                                                  m_artist->bio = std::move(text);
                                                  m_host.ChromeChanged();
                                              }
                                          }));
                m_host.ChromeChanged();
            }));
    }

    void OpenTile(const stream::Album* album) {
        const std::wstring artistName = m_artist ? m_artist->name : m_view.name;
        if (!album)
            m_host.Open(TopTracksView(m_provider, m_view.remoteId, artistName, m_view.sectionId));
        else
            m_host.Open(AlbumView(m_provider, album->id, album->name, m_view.sectionId)); // under the artist
    }

    void TileMenu(POINT screen, const stream::Album* album) {
        RecordMenuContext context;
        context.sectionId = m_view.sectionId;
        AddPageActions(context.page);
        context.page.push_back({});
        context.page.push_back(RefreshAction(m_host));
        if (album) {
            ShowRecordMenu(m_host, m_provider, *album, screen, context);
            return;
        }
        // The artist's songs tile: the artist's own menu, View opening its songs.
        stream::Artist artist;
        artist.id = m_view.remoteId;
        artist.name = m_artist ? m_artist->name : m_view.name;
        auto actions = RecordActions(m_host, m_provider, artist, context);
        if (!actions.empty() && actions.front().label == L"View")
            actions.front() = {L"View", [this] { OpenTile(nullptr); }};
        ShowActionMenu(m_host.Owner(), screen, actions);
    }

    bool m_busy = false;
    ArtistOverviewSource* m_overview = nullptr; // the icon view's source (owned by the host)
    std::vector<std::shared_ptr<stream::ListSession>> m_sectionSessions;
};

class SavedAlbumsPage final : public AlbumBrowsePage {
  public:
    SavedAlbumsPage(PageHost& host, stream::Provider& provider, View view)
        : AlbumBrowsePage(host, provider, view, SavedAlbumsQuery()) {}

  protected:
    std::unique_ptr<AlbumGridSource> Grid(std::vector<stream::Album> albums) override {
        return std::make_unique<AlbumGridSource>(
            m_provider, std::move(albums), std::wstring(), std::wstring(), L"No albums.",
            [this](const stream::Album* album) {
                if (album)
                    m_host.Open(AlbumView(m_provider, album->id, album->name, m_view.sectionId));
            },
            [this](POINT screen, const stream::Album* album) {
                if (album) TileMenu(screen, *album);
            });
    }

  private:
    void TileMenu(POINT screen, const stream::Album& album) {
        RecordMenuContext context;
        context.sectionId = m_view.sectionId;
        context.removed = Guard([this](const stream::Id& id) { // uncollected: the tile goes at once
            ForgetAlbum(id);
            if (auto* grid = dynamic_cast<AlbumGridSource*>(m_source)) {
                m_host.BeforeListChange();
                grid->RemoveAlbum(id);
                m_host.ListChanged();
            }
        });
        context.page.push_back(RefreshAction(m_host));
        ShowRecordMenu(m_host, m_provider, album, screen, context);
    }
};

} // namespace

std::unique_ptr<Page> MakeArtistPage(PageHost& host, stream::Provider& provider, const View& view) {
    switch (view.kind) {
    case View::Kind::RemoteArtists:
        return std::make_unique<ArtistsPage>(host, provider, view);
    case View::Kind::RemoteAlbums:
        return std::make_unique<SavedAlbumsPage>(host, provider, view);
    case View::Kind::RemoteArtist:
        return std::make_unique<ArtistPage>(host, provider, view);
    default:
        return nullptr;
    }
}

} // namespace glasswave::ui::remotepages
