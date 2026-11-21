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

std::wstring Provider::SignInRequiredMessage() const {
    std::wstring text = ui::Tr(L"Please sign in to {0}.");
    const size_t mark = text.find(L"{0}");
    if (mark != std::wstring::npos) text.replace(mark, 3, ui::Tr(Name()));
    return text;
}

bool Provider::ReadAlbumSaved(const Id& album) const {
    const auto albums = SavedAlbums();
    return std::any_of(albums.begin(), albums.end(),
                       [&album](const Album& item) { return item.id == album; });
}

std::optional<bool> Provider::ReadSongsSavedState(const std::vector<Id>&) const {
    return std::nullopt;
}

std::optional<bool> Provider::ReadPlaylistSavedState(const Id&) const {
    return std::nullopt;
}

std::optional<bool> Provider::ReadArtistPreferenceState(const Id&) const {
    return std::nullopt;
}

void Provider::WriteSongsSaved(std::vector<Id>, bool, Done done) {
    if (done) done(L"This source cannot add single songs to its library.");
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

namespace {
// A change the service never answers, or one the source never confirms, stops overriding the
// source's state after this long (as a favorite does).
constexpr auto kEditTimeout = std::chrono::minutes(2);
}  // namespace

std::optional<bool> Provider::PendingEditState(EditKind kind, const Id& id) const {
    std::lock_guard lock(m_editsMutex);
    const auto it = m_pendingEdits.find({kind, id});
    if (it == m_pendingEdits.end()) return std::nullopt;
    if (std::chrono::steady_clock::now() - it->second.since >= kEditTimeout) {
        m_pendingEdits.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

bool Provider::IsAlbumSaved(const Id& album) const {
    if (const auto pending = PendingEditState(EditKind::Album, album)) return *pending;
    return ReadAlbumSaved(album);
}

std::optional<bool> Provider::PlaylistSavedState(const Id& playlist) const {
    if (const auto pending = PendingEditState(EditKind::Playlist, playlist)) return pending;
    return ReadPlaylistSavedState(playlist);
}

std::optional<bool> Provider::ArtistPreferenceState(const Id& artist) const {
    if (const auto pending = PendingEditState(EditKind::Follow, artist)) return pending;
    return ReadArtistPreferenceState(artist);
}

std::optional<bool> Provider::SongsSavedState(const std::vector<Id>& tracks) const {
    // The songs with a pending change answer for themselves; the source answers for the rest.
    std::vector<Id> rest;
    bool removed = false;
    for (const Id& track : tracks) {
        if (const auto pending = PendingEditState(EditKind::Song, track))
            removed |= !*pending;
        else
            rest.push_back(track);
    }
    if (rest.size() == tracks.size()) return ReadSongsSavedState(tracks);
    if (removed) return false;
    if (rest.empty()) return true;
    return ReadSongsSavedState(rest);
}

// The new state shows at once and listeners hear OnLibraryChanged; `write` runs the service's
// part with a completion that may come from any thread. Like a favorite, only the latest click
// on an id decides what it shows: an earlier write's answer neither confirms nor rolls back a
// later one. On failure the change is taken back (and listeners hear it); on success it stays
// until the source's next library read shows it (ConfirmLibraryEdits) or it times out.
void Provider::SetEdit(EditKind kind, std::vector<Id> ids, bool value, Done done,
                       std::function<void(Done)> write) {
    uint64_t serial = 0;
    {
        std::lock_guard lock(m_editsMutex);
        serial = ++m_editSerial;
        const auto now = std::chrono::steady_clock::now();
        for (const Id& id : ids)
            m_pendingEdits[{kind, id}] = {value, serial, 0, now};
    }
    NotifyLibraryChanged();
    write([this, kind, ids = std::move(ids), serial, done = std::move(done)](std::wstring error) mutable {
        if (core_api::is_main_thread()) {
            FinishEdit(kind, ids, serial, std::move(error), std::move(done));
            return;
        }
        fb2k::inMainThread([this, kind, ids = std::move(ids), serial, error = std::move(error),
                            done = std::move(done)]() mutable {
            FinishEdit(kind, ids, serial, std::move(error), std::move(done));
        });
    });
}

void Provider::FinishEdit(EditKind kind, const std::vector<Id>& ids, uint64_t serial, std::wstring error,
                          Done done) {
    bool tookBack = false;
    {
        std::lock_guard lock(m_editsMutex);
        const uint64_t ackSerial = error.empty() ? ++m_editSerial : 0;
        for (const Id& id : ids) {
            const auto it = m_pendingEdits.find({kind, id});
            if (it == m_pendingEdits.end() || it->second.serial != serial) continue;
            if (error.empty()) {
                it->second.ackSerial = ackSerial;
            } else {
                m_pendingEdits.erase(it);
                tookBack = true;
            }
        }
    }
    if (!error.empty())
        console::print("Glasswave: library change failed: ", pfc::stringcvt::string_utf8_from_wide(error.c_str()).get_ptr());
    if (tookBack) NotifyLibraryChanged();
    if (done) done(std::move(error));
}

uint64_t Provider::LibraryEditMark() const {
    std::lock_guard lock(m_editsMutex);
    return m_editSerial;
}

void Provider::ConfirmLibraryEdits(uint64_t mark) {
    std::lock_guard lock(m_editsMutex);
    std::erase_if(m_pendingEdits, [mark](const auto& entry) {
        return entry.second.ackSerial != 0 && entry.second.ackSerial <= mark;
    });
}

void Provider::SetFollowed(const Id& artist, bool follow, Done done) {
    SetEdit(EditKind::Follow, {artist}, follow, std::move(done),
            [this, artist, follow](Done write) { WriteFollowed(artist, follow, std::move(write)); });
}

void Provider::SetAlbumSaved(const Id& album, bool saved, Done done) {
    SetEdit(EditKind::Album, {album}, saved, std::move(done),
            [this, album, saved](Done write) { WriteAlbumSaved(album, saved, std::move(write)); });
}

void Provider::SetPlaylistSaved(const Id& playlist, bool saved, Done done) {
    SetEdit(EditKind::Playlist, {playlist}, saved, std::move(done),
            [this, playlist, saved](Done write) { WritePlaylistSaved(playlist, saved, std::move(write)); });
}

void Provider::SetSongsSaved(std::vector<Id> tracks, bool saved, Done done) {
    if (!Can(Capability::SaveSongs)) {
        if (done) done(L"This source cannot add single songs to its library.");
        return;
    }
    if (tracks.empty()) {
        if (done) done({});
        return;
    }
    SetEdit(EditKind::Song, tracks, saved, std::move(done),
            [this, tracks, saved](Done write) { WriteSongsSaved(tracks, saved, std::move(write)); });
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
    // The changes the previous account's user made are not this account's state: a write still in
    // flight completes as dropped and finds nothing to settle.
    {
        std::lock_guard lock(m_favoritesMutex);
        m_pendingFavorites.clear();
    }
    {
        std::lock_guard lock(m_editsMutex);
        m_pendingEdits.clear();
    }
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
