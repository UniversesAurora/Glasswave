#include "pch.h"
#include "stream/provider.h"
#include "stream/playback_source.h"
#include "stream/list_session.h"
#include "ui/i18n.h"

namespace glasswave::stream {

namespace {

std::vector<Provider*>& Registry() {
    static std::vector<Provider*> providers;
    return providers;
}

const wchar_t* KindName(ArtKind kind) {
    switch (kind) {
    case ArtKind::Track:
        return L"song";
    case ArtKind::Album:
        return L"album";
    case ArtKind::Artist:
        return L"artist";
    case ArtKind::Playlist:
        return L"playlist";
    case ArtKind::Station:
        return L"station";
    }
    return L"";
}

} // namespace

std::wstring JoinArtists(const std::vector<ArtistRef>& artists) {
    std::wstring out;
    for (const auto& artist : artists) {
        if (artist.name.empty()) continue;
        if (!out.empty()) out += L"; ";
        out += artist.name;
    }
    return out;
}

std::wstring Provider::ArtKey(ArtKind kind, const Id& id) const {
    return ArtKeyPrefix(kind) + id;
}

std::wstring Provider::ArtKeyOf(const metadb_handle_ptr& item) const {
    const Id id = TrackId(item);
    if (id.empty()) return {};
    const Id album = AlbumOfTrack(id);
    return !album.empty() ? ArtKey(ArtKind::Album, album) : ArtKey(ArtKind::Track, id);
}

Id Provider::AlbumOfTrack(const Id& track) const {
    const auto found = FindTrack(track);
    return found ? found->albumId : Id();
}

std::wstring Provider::ArtKeyPrefix(ArtKind kind) const {
    return std::wstring(Key()) + L':' + KindName(kind) + L':';
}

std::wstring Provider::Text(std::wstring_view english) const {
    const std::wstring term = Wording(english);
    const std::wstring_view context = TextContext();
    return context.empty() ? ui::Tr(term) : ui::Tr(term, context);
}

bool Provider::IsAlbumSaved(const Id& album) const {
    const auto albums = SavedAlbums();
    return std::any_of(albums.begin(), albums.end(),
                       [&album](const Album& item) { return item.id == album; });
}

std::optional<bool> Provider::SongsSavedState(const std::vector<Id>&) const {
    return std::nullopt;
}

void Provider::SetSongsSaved(std::vector<Id>, bool, Done done) {
    if (done) done(L"This source cannot add single songs to its library.");
}

std::optional<bool> Provider::PlaylistSavedState(const Id&) const {
    return std::nullopt;
}

std::optional<bool> Provider::ArtistPreferenceState(const Id&) const {
    return std::nullopt;
}

DownloadSource Provider::ResolveDownload(const Id&, abort_callback&) {
    throw pfc::exception("This source cannot download songs.");
}

std::wstring Provider::ArtistTracksLabel(const Id&) const {
    return L"Hot songs";
}

std::vector<ArtistSection> Provider::ArtistSections(const Id& artist) const {
    // The albums are the artist page's own list (an empty section: the same query, one session).
    std::vector<ArtistSection> sections{{L"top-songs", ArtistTracksLabel(artist), ArtistSectionRowKind::Tracks},
                                        {L"", L"Albums", ArtistSectionRowKind::Albums, true}};
    if (Can(Capability::SimilarArtists))
        sections.push_back({L"similar-artists", L"Similar artists", ArtistSectionRowKind::Artists});
    return sections;
}

std::vector<ListKind> Provider::CatalogSearchKinds() const {
    return Can(Capability::CatalogSearch) ? std::vector<ListKind>{ListKind::CatalogSearch}
                                          : std::vector<ListKind>{};
}

std::vector<NavigationSection> Provider::Sections() const {
    const Capabilities offers = Offers();
    const bool reorder = offers.Has(Capability::ReorderPlaylists);
    std::vector<NavigationSection> sections;
    if (offers.Has(Capability::ReadFavorites))
        sections.push_back({L"favorites", L"Favorites", SectionKind::Favorites, Shelf::Created, false});
    if (offers.Has(Capability::LibraryArtists))
        sections.push_back({L"artists", L"Artists", SectionKind::Artists, Shelf::Created, false});
    if (offers.Has(Capability::LibraryAlbums))
        sections.push_back({L"albums", L"Albums", SectionKind::Albums, Shelf::Created, false});
    sections.push_back(
        {L"created", L"Created playlists", SectionKind::PlaylistShelf, Shelf::Created, reorder});
    if (offers.Has(Capability::LibraryPlaylists))
        sections.push_back(
            {L"saved", L"Collected playlists", SectionKind::PlaylistShelf, Shelf::Saved, reorder});
    if (offers.Has(Capability::RecommendedPlaylists))
        sections.push_back(
            {L"recommended", L"Recommended", SectionKind::PlaylistShelf, Shelf::Recommended, false});
    return sections;
}

std::vector<Playlist> Provider::SectionPlaylists(const Id& section) const {
    const auto sections = Sections();
    const auto found = std::find_if(sections.begin(), sections.end(), [&section](const auto& item) {
        return item.id == section && item.kind == SectionKind::PlaylistShelf;
    });
    return found == sections.end() ? std::vector<Playlist>() : Playlists(found->shelf);
}

void Provider::FetchSectionPlaylists(const Id& section, PlaylistsDone done) {
    const auto sections = Sections();
    const auto found = std::find_if(sections.begin(), sections.end(), [&section](const auto& item) {
        return item.id == section && item.kind == SectionKind::PlaylistShelf;
    });
    if (found == sections.end()) {
        if (done) done({}, L"The provider playlist section is unavailable.");
        return;
    }
    FetchPlaylists(found->shelf, std::move(done));
}

std::optional<NavigationSection> Provider::SectionOfPlaylist(const Id& playlist) const {
    for (const auto& section : Sections()) {
        if (section.kind != SectionKind::PlaylistShelf) continue;
        const auto rows = SectionPlaylists(section.id);
        const auto found = std::find_if(rows.begin(), rows.end(),
                                        [&playlist](const auto& item) { return item.id == playlist; });
        if (found != rows.end()) return section;
    }
    return std::nullopt;
}

void Provider::OnLanguageChanged() {
    RefreshLibrary([this](std::wstring) { fb2k::inMainThread([this] { NotifyLanguageChanged(); }); });
}

namespace {
// A write whose answer never comes (the service stopped, the account changed) stops
// overriding the service's state after this long.
constexpr auto kFavoriteAnswerTimeout = std::chrono::minutes(2);
}  // namespace

bool Provider::IsFavorite(const Id& track) const {
    {
        std::lock_guard lock(m_favoritesMutex);
        if (const auto it = m_pendingFavorites.find(track); it != m_pendingFavorites.end()) {
            if (std::chrono::steady_clock::now() - it->second.since < kFavoriteAnswerTimeout)
                return it->second.favorite;
            m_pendingFavorites.erase(it);
        }
    }
    return ReadFavorite(track);
}

// The new state shows at once; the write runs on the provider's own queues, which pages,
// playback and list changes do not cancel. Only the latest click on a song decides what it
// shows: an earlier write's answer neither confirms nor rolls back a later one.
void Provider::SetFavorite(const Id& track, bool favorite, Done done) {
    uint64_t serial = 0;
    {
        std::lock_guard lock(m_favoritesMutex);
        serial = ++m_favoriteSerial;
        m_pendingFavorites[track] = {favorite, serial, std::chrono::steady_clock::now()};
    }
    NotifyFavoritesChanged();
    WriteFavorite(track, favorite, [this, track, serial, done = std::move(done)](std::wstring error) mutable {
        if (core_api::is_main_thread()) {
            FinishFavorite(track, serial, std::move(error), std::move(done));
            return;
        }
        fb2k::inMainThread([this, track, serial, error = std::move(error), done = std::move(done)]() mutable {
            FinishFavorite(track, serial, std::move(error), std::move(done));
        });
    });
}

void Provider::FinishFavorite(const Id& track, uint64_t serial, std::wstring error, Done done) {
    bool settled = false;
    {
        std::lock_guard lock(m_favoritesMutex);
        if (const auto it = m_pendingFavorites.find(track); it != m_pendingFavorites.end() && it->second.serial == serial) {
            m_pendingFavorites.erase(it);
            settled = true;
        }
    }
    if (!error.empty())
        console::print("Glasswave: favorite change failed: ", pfc::stringcvt::string_utf8_from_wide(error.c_str()).get_ptr());
    // Success: the service's state is the new one now. Failure: it is the old one again.
    if (settled) NotifyFavoritesChanged();
    if (done) done(std::move(error));
}

void Provider::AddListener(Listener* listener) {
    if (std::find(m_listeners.begin(), m_listeners.end(), listener) == m_listeners.end())
        m_listeners.push_back(listener);
}

void Provider::RemoveListener(Listener* listener) {
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener), m_listeners.end());
}

// Listeners may remove themselves while being notified, so iterate over a copy.
void Provider::NotifyAccountObserved() {
    // While the session is still being restored the reads are stopped at the gate (their rows
    // stay); once the account is verified only the reads that were waiting start.
    if (SessionRestoring() && !AccountVerified())
        InvalidateListCache(Key(), true, ListCacheInvalidation::RetireInMemory);
    else
        ResumeLists(Key());
    for (Listener* listener : std::vector<Listener*>(m_listeners))
        listener->OnAccountObserved(*this);
}

void Provider::NotifyAccountChanged() {
    ResetSourceAvailability();
    InvalidateListCache(Key(), false, ListCacheInvalidation::PurgePersistent);
    for (Listener* listener : std::vector<Listener*>(m_listeners))
        listener->OnAccountChanged(*this);
}

void Provider::NotifyLibraryChanged() {
    for (Listener* listener : std::vector<Listener*>(m_listeners))
        listener->OnLibraryChanged(*this);
}

void Provider::NotifyFavoritesChanged() {
    for (Listener* listener : std::vector<Listener*>(m_listeners))
        listener->OnFavoritesChanged(*this);
}

void Provider::NotifyLanguageChanged() {
    InvalidateListCache(Key(), false, ListCacheInvalidation::RetireInMemory);
    for (Listener* listener : std::vector<Listener*>(m_listeners))
        listener->OnLanguageChanged(*this);
}

void Provider::NotifyDisplayChanged() {
    // Existing list leases carry the previous display context. Stop their enrichment before
    // notifying the UI so the visible page can reopen against the new projection.
    InvalidateListCache(Key(), false, ListCacheInvalidation::RetireInMemory);
    for (Listener* listener : std::vector<Listener*>(m_listeners))
        listener->OnDisplayChanged(*this);
}

void Provider::NotifyListChanged() {
    RevalidateLists(Key());
}

ListCachePolicy Provider::ListPolicy(const ListQuery& query) const {
    if (query.cache) return *query.cache;
    if (IsCatalogSearch(query.kind)) return ListCachePolicy::None;
    if (query.kind == ListKind::Playlists && query.shelf == Shelf::Recommended)
        return ListCachePolicy::Memory;
    return ListCachePolicy::Persistent;
}

void Provider::FetchListPage(const ListQuery& query, const ListCursor& cursor, size_t limit,
                             ListPageDone done) {
    const auto tracks = [done](std::vector<Track> rows, std::wstring error) {
        ListPage page;
        page.total = rows.size();
        page.rows = std::move(rows);
        done(std::move(page), std::move(error));
    };
    switch (query.kind) {
    case ListKind::ArtistTopTracks:
        FetchArtistTopTracks(
            query.id, cursor.offset, limit,
            [done, offset = cursor.offset](std::vector<Track> rows, bool more, std::wstring error) {
                ListPage page;
                page.next.offset = offset + rows.size();
                page.more = more;
                page.rows = std::move(rows);
                done(std::move(page), std::move(error));
            });
        return;
    case ListKind::ArtistAlbums:
    case ListKind::SavedAlbums: {
        auto albums = [done, offset = cursor.offset](std::vector<Album> rows, bool more, std::optional<bool>,
                                                     std::wstring error) {
            ListPage page;
            page.next.offset = offset + rows.size();
            page.more = more;
            page.rows = std::move(rows);
            done(std::move(page), std::move(error));
        };
        if (query.kind == ListKind::ArtistAlbums)
            FetchArtistAlbums(query.id, cursor.offset, limit, std::move(albums));
        else
            FetchSavedAlbums(cursor.offset, limit, std::move(albums));
        return;
    }
    default:
        break;
    }
    if (cursor.offset || !cursor.value.empty()) {
        done({}, L"This list does not expose another page.");
        return;
    }
    switch (query.kind) {
    case ListKind::PlaylistTracks:
        FetchPlaylist(query.id, [done](Playlist info, std::vector<Track> rows, std::wstring error) {
            ListPage page;
            page.playlist = std::move(info);
            page.total = rows.size();
            page.rows = std::move(rows);
            done(std::move(page), std::move(error));
        });
        break;
    case ListKind::AlbumTracks:
        FetchAlbum(query.id, [done](Album info, std::vector<Track> rows, std::wstring error) {
            ListPage page;
            page.album = std::move(info);
            page.total = rows.size();
            page.rows = std::move(rows);
            done(std::move(page), std::move(error));
        });
        break;
    case ListKind::Favorites:
        FetchFavorites(tracks);
        break;
    case ListKind::SectionTracks:
        FetchSectionTracks(query.section, query.id, tracks);
        break;
    case ListKind::CatalogSearch:
        SearchCatalog(query.search, tracks);
        break;
    case ListKind::Playlists: {
        auto lists = [done](std::vector<Playlist> rows, std::wstring error) {
            ListPage page;
            page.total = rows.size();
            page.rows = std::move(rows);
            done(std::move(page), std::move(error));
        };
        if (!query.section.empty())
            FetchSectionPlaylists(query.section, std::move(lists));
        else
            FetchPlaylists(query.shelf, std::move(lists));
        break;
    }
    case ListKind::FollowedArtists:
        FetchFollowedArtists([done](std::vector<Artist> rows, std::wstring error) {
            ListPage page;
            page.total = rows.size();
            page.rows = std::move(rows);
            done(std::move(page), std::move(error));
        });
        break;
    case ListKind::Groups:
        FetchGroups(query.section, [done](std::vector<BrowseGroup> rows, std::wstring error) {
            ListPage page;
            page.total = rows.size();
            page.rows = std::move(rows);
            done(std::move(page), std::move(error));
        });
        break;
    default:
        done({}, L"This list is unavailable.");
        break;
    }
}

const std::vector<Provider*>& Providers() {
    return Registry();
}

void StartProviders() {
    for (Provider* provider : Registry())
        provider->Start();
}

void Register(Provider& provider) {
    auto& providers = Registry();
    if (std::find(providers.begin(), providers.end(), &provider) == providers.end())
        providers.push_back(&provider);
}

void Unregister(Provider& provider) {
    auto& providers = Registry();
    providers.erase(std::remove(providers.begin(), providers.end(), &provider), providers.end());
}

Provider* FindProvider(std::wstring_view key) {
    for (Provider* provider : Registry())
        if (provider->Key() == key) return provider;
    return nullptr;
}

std::vector<std::wstring> TrackBadges(const metadb_handle_ptr& item) {
    Provider* provider = ProviderOf(item);
    if (!provider) return {};
    const auto track = provider->FindTrack(provider->TrackId(item));
    return track ? track->badges : std::vector<std::wstring>{};
}

Provider* ProviderOf(const metadb_handle_ptr& item) {
    if (!item.is_valid()) return nullptr;
    for (Provider* provider : Registry())
        if (provider->Owns(item)) return provider;
    return nullptr;
}

} // namespace glasswave::stream
