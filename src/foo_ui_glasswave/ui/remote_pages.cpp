#include "pch.h"
#include "ui/downloads_page.h"
#include "ui/remote_pages.h"
#include "media/favorites.h"
#include "media/playback_scheduler.h"
#include "media/recent_lists.h"
#include "media/special_playlists.h"
#include "ui/art_cache.h"
#include "ui/artist_overview.h"
#include "ui/i18n.h"
#include "ui/item_menu.h"
#include "ui/remote_grids.h"
#include "ui/song_actions.h"
#include "ui/song_table.h"
#include "ui/text.h"
#include "ui/text_prompt.h"
#include "stream/list_session.h"

#include <cstdint>
#include <cwctype>
#include <unordered_set>

namespace glasswave::ui {

namespace {

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

// The service gave a page's subject another picture than the one known before. A first
// address is no change: invalidating then threw away the load the page had just started, so
// the cover came only after a retry (owner, 2026-11-14).
bool CoverChanged(const stream::Provider& provider, const std::wstring& before, const std::wstring& now) {
    return !before.empty() && !now.empty() && !provider.SameCover(before, now);
}

// A list of values in one fact: "a, b" or, in Chinese, "a、b".
std::wstring ListText(const std::vector<std::wstring>& values) {
    std::wstring out;
    for (const auto& value : values) {
        if (value.empty()) continue;
        if (!out.empty()) out += UsingChinese() ? L"、" : L", ";
        out += value;
    }
    return out;
}

// A number with the UI language's digit grouping (4,888,171).
std::wstring NumberText(long long n) {
    std::wstring digits = std::to_wstring(n < 0 ? -n : n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += L',';
        out += digits[i];
    }
    return n < 0 ? L"-" + out : out;
}

// A date as a service writes it ("2017", "1979-01-18", "January 18, 1979") in the UI
// language's long form; text it cannot read is shown as it is.
std::wstring DateText(const std::wstring& text) {
    if (text.size() == 4 && std::all_of(text.begin(), text.end(), iswdigit)) return text;
    SYSTEMTIME date{};
    static constexpr const wchar_t* kMonths[] = {L"January", L"February", L"March",     L"April",
                                                 L"May",     L"June",     L"July",      L"August",
                                                 L"September", L"October", L"November", L"December"};
    int year = 0, month = 0, day = 0;
    if (swscanf_s(text.c_str(), L"%d-%d-%d", &year, &month, &day) == 3) {
    } else {
        wchar_t name[16]{};
        if (swscanf_s(text.c_str(), L"%15ls %d, %d", name, static_cast<unsigned>(std::size(name)), &day, &year) != 3)
            return text;
        for (int i = 0; i < 12; ++i)
            if (_wcsicmp(name, kMonths[i]) == 0) month = i + 1;
    }
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31) return text;
    date.wYear = static_cast<WORD>(year);
    date.wMonth = static_cast<WORD>(month);
    date.wDay = static_cast<WORD>(day);
    wchar_t out[64]{};
    if (!GetDateFormatEx(UsingChinese() ? L"zh-CN" : L"en-US", DATE_LONGDATE, &date, nullptr, out,
                         static_cast<int>(std::size(out)), nullptr))
        return text;
    return out;
}

// An artist's profile as details-pane facts, most telling first; what the source does not
// know is left out.
std::vector<DetailInfo::Fact> ArtistFacts(const stream::Artist& artist) {
    std::vector<DetailInfo::Fact> facts;
    const auto add = [&facts](const wchar_t* label, std::wstring value) {
        if (!value.empty()) facts.push_back({Tr(label), std::move(value)});
    };
    using Kind = stream::ArtistKind;
    add(artist.kind == Kind::Group ? L"Formed" : artist.kind == Kind::Unknown ? L"Born or formed" : L"Born",
        artist.born.empty() ? std::wstring() : DateText(artist.born));
    add(L"From", artist.origin);
    add(L"Area", artist.area.empty() ? std::wstring() : Tr(artist.area));
    add(L"Type", artist.kind == Kind::Group    ? Tr(L"Band or group")
                 : artist.kind == Kind::Male   ? Tr(L"Male artist")
                 : artist.kind == Kind::Female ? Tr(L"Female artist")
                 : artist.kind == Kind::Person ? Tr(L"Solo artist")
                                               : std::wstring());
    add(L"Genre", ListText(artist.genres));
    add(L"Also known as", ListText(artist.aliases));
    add(L"Roles", ListText(artist.roles));
    add(L"Music videos", artist.videoCount > 0 ? NumberText(artist.videoCount) : std::wstring());
    return facts;
}

// A playlist's details as details-pane facts.
std::vector<DetailInfo::Fact> PlaylistFacts(const stream::Playlist& playlist) {
    std::vector<DetailInfo::Fact> facts;
    const auto add = [&facts](const wchar_t* label, std::wstring value) {
        if (!value.empty()) facts.push_back({Tr(label), std::move(value)});
    };
    std::vector<std::wstring> traits;
    for (const auto& trait : playlist.traits) traits.push_back(Tr(trait));
    add(L"Type", playlist.type.empty() ? std::wstring() : Tr(playlist.type));
    add(L"Tags", ListText(playlist.tags));
    add(L"Created", playlist.created.empty() ? std::wstring() : DateText(playlist.created));
    add(L"Updated", playlist.updated.empty() ? std::wstring() : DateText(playlist.updated));
    add(L"Updates", playlist.updates);
    add(L"Audio", ListText(traits));
    add(L"Also known as", ListText(playlist.aliases));
    return facts;
}

// An album's details as details-pane facts.
std::vector<DetailInfo::Fact> AlbumFacts(const stream::Album& album) {
    std::vector<DetailInfo::Fact> facts;
    const auto add = [&facts](const wchar_t* label, std::wstring value) {
        if (!value.empty()) facts.push_back({Tr(label), std::move(value)});
    };
    std::vector<std::wstring> traits;
    for (const auto& trait : album.traits) traits.push_back(Tr(trait));
    add(L"Type", album.type.empty() ? std::wstring() : Tr(album.type));
    add(L"Edition", album.edition);
    add(L"Released", album.released.empty() ? std::wstring() : DateText(album.released));
    add(L"Genre", ListText(album.genres));
    add(L"Label", album.label);
    add(L"Audio", ListText(traits));
    add(L"Also known as", ListText(album.aliases));
    add(L"Copyright", album.copyright);
    add(L"UPC", album.upc);
    return facts;
}

bool Contains(const std::vector<stream::Album>& albums, const stream::Id& id) {
    return std::any_of(albums.begin(), albums.end(), [&id](const stream::Album& a) { return a.id == id; });
}

bool Contains(const std::vector<stream::Artist>& artists, const stream::Id& id) {
    return std::any_of(artists.begin(), artists.end(), [&id](const stream::Artist& a) { return a.id == id; });
}

bool Contains(const std::vector<stream::Playlist>& playlists, const stream::Id& id) {
    return std::any_of(playlists.begin(), playlists.end(),
                       [&id](const stream::Playlist& p) { return p.id == id; });
}

bool IsSavedAlbum(stream::Provider& provider, const stream::Id& id) {
    return provider.IsAlbumSaved(id);
}
bool IsFollowed(stream::Provider& provider, const stream::Id& id) {
    return Contains(provider.FollowedArtists(), id);
}
bool SupportsArtistPreference(const stream::Provider& provider) {
    return provider.Can(stream::Capability::FollowArtists) ||
           provider.Can(stream::Capability::FavoriteArtists);
}

std::optional<stream::NavigationSection> FindSection(const stream::Provider& provider, const stream::Id& id) {
    const auto sections = provider.Sections();
    const auto found = std::find_if(sections.begin(), sections.end(),
                                    [&id](const auto& section) { return section.id == id; });
    return found == sections.end() ? std::nullopt : std::optional<stream::NavigationSection>(*found);
}

std::optional<stream::NavigationSection> FindSection(const stream::Provider& provider,
                                                     stream::SectionKind kind,
                                                     std::optional<stream::Shelf> shelf = std::nullopt) {
    const auto sections = provider.Sections();
    const auto found = std::find_if(sections.begin(), sections.end(), [kind, shelf](const auto& section) {
        return section.kind == kind &&
               (!shelf || (section.kind == stream::SectionKind::PlaylistShelf && section.shelf == *shelf));
    });
    return found == sections.end() ? std::nullopt : std::optional<stream::NavigationSection>(*found);
}

stream::NavigationSection CreatedPlaylistSection(const stream::Provider& provider) {
    if (const auto section =
            FindSection(provider, stream::SectionKind::PlaylistShelf, stream::Shelf::Created))
        return *section;
    return {L"created", L"Created playlists", stream::SectionKind::PlaylistShelf, stream::Shelf::Created,
            false};
}

std::wstring ProviderPlaylistText(const stream::Provider& provider, std::wstring_view english) {
    return provider.Text(english);
}

// A list is visibly loading only while it has nothing to show. Re-reading behind rows that
// are already on screen (a cached snapshot, a refresh) stays silent and updates in place.
// Counts one record of an artist has and the next lacks stay (the album list's record has
// the album count, the profile read after it may not: the stats line came and went).
void KeepCounts(const stream::Artist& from, stream::Artist& to) {
    if (to.albumCount < 0) to.albumCount = from.albumCount;
    if (to.trackCount < 0) to.trackCount = from.trackCount;
    if (to.videoCount < 0) to.videoCount = from.videoCount;
}

bool VisiblyLoading(const stream::ListState& state) {
    if (!state.loading && !state.refreshing) return false;
    return std::visit([](const auto& rows) { return rows.empty(); }, state.page.rows);
}

std::wstring LoadingArtistTracksLabel(const stream::Provider& provider, const stream::Id& artist) {
    std::wstring label = provider.ArtistTracksLabel(artist);
    if (label.empty()) return L"Loading songs...";
    label[0] = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(label[0])));
    return L"Loading " + label + L"...";
}

template <class T> const std::vector<T>& Rows(const stream::ListRows& rows) {
    static const std::vector<T> empty;
    const auto* value = std::get_if<std::vector<T>>(&rows);
    return value ? *value : empty;
}

stream::ListQuery Query(stream::ListKind kind) {
    stream::ListQuery query;
    query.kind = kind;
    return query;
}

stream::ListQuery PreparedQuery(stream::Provider& provider, stream::ListQuery query,
                                bool catalogSearch = false) {
    query.cache = catalogSearch ? stream::ListCachePolicy::None : provider.ListPolicy(query);
    return query;
}

metadb_handle_list Items(stream::Provider& provider, const std::vector<stream::Track>& tracks,
                         size_t count = SIZE_MAX) {
    metadb_handle_list items;
    count = std::min(count, tracks.size());
    for (size_t i = 0; i < count; ++i)
        items.add_item(provider.TrackItem(tracks[i].id));
    return items;
}

struct AllSongsRequest {
    stream::Provider* provider = nullptr;
    std::function<void(metadb_handle_list)> ready;
    std::function<void()> failed;       // the read ended without the list
    std::wstring name;                  // the list's name, set before `ready` runs
    std::shared_ptr<stream::ListSession> session;
    PageHost* host = nullptr;
    LoadingBadge badge; // "Loading..." on the list showing when the read began
    std::weak_ptr<int> alive;
    bool active = false;
    bool settling = false;              // a look at the settled state is queued
};

std::vector<std::shared_ptr<AllSongsRequest>>& ActiveSongsRequests() {
    static std::vector<std::shared_ptr<AllSongsRequest>> requests;
    return requests;
}

void FinishAllSongsRequest(const std::shared_ptr<AllSongsRequest>& request) {
    if (!request) return;
    // Ended without the list (failed, cancelled with its provider or the window): say so.
    request->ready = {};
    if (auto failed = std::exchange(request->failed, {})) failed();
    request->active = false;
    std::erase_if(ActiveSongsRequests(), [&](const auto& active) { return active.get() == request.get(); });
    auto session = std::move(request->session);
    request->session.reset();
    if (session) session->Cancel();
}

void CancelAllSongsRequests() {
    auto requests = std::move(ActiveSongsRequests());
    ActiveSongsRequests().clear();
    for (auto& request : requests)
        FinishAllSongsRequest(request);
}

void CancelSongsRequests(std::wstring_view provider) {
    std::vector<std::shared_ptr<AllSongsRequest>> requests;
    std::erase_if(ActiveSongsRequests(), [&](const auto& request) {
        if (request->provider->Key() != provider) return false;
        requests.push_back(request);
        return true;
    });
    for (auto& request : requests)
        FinishAllSongsRequest(request);
}

void BeginAllSongs(const std::shared_ptr<AllSongsRequest>& request) {
    if (!request->host || request->alive.expired()) return;
    request->badge.Begin(*request->host, request->host->Source());
}

void EndAllSongs(const std::shared_ptr<AllSongsRequest>& request) {
    request->badge.End();
}

// A whole list is taken once its read has settled. The read is shared with the list's page, so
// what plays is what the page shows; a cache the service is being asked about again is not
// taken, the rows that answer brings are (owner, 2026-11-20: the queue began with a song the page
// did not show yet).
void SettleAllSongs(const std::shared_ptr<AllSongsRequest>& request) {
    if (!request->ready || !request->session) return;
    const stream::ListState& state = request->session->View();
    if (state.loading || state.refreshing) return; // its end is notified
    if (state.complete) {
        metadb_handle_list items = Items(*request->provider, Rows<stream::Track>(state.page.rows));
        if (state.page.playlist) request->name = state.page.playlist->name;
        else if (state.page.album) request->name = state.page.album->name;
        auto ready = std::move(request->ready);
        request->ready = {};
        request->failed = {};
        EndAllSongs(request);
        FinishAllSongsRequest(request);
        ready(std::move(items));
        return;
    }
    if (!state.error.empty()) {
        EndAllSongs(request);
        FinishAllSongsRequest(request); // tells `failed`
        ShowProviderError(*request->provider, state.error);
    }
}

// Notifications are synchronous, and a cache hit is followed at once by the refresh it starts:
// the state is looked at when the message in hand is done, so that refresh is seen.
void CompleteAllSongs(const std::shared_ptr<AllSongsRequest>& request, const stream::ListState& state) {
    if (!request || !request->ready || request->settling) return;
    if (!state.complete && (state.error.empty() || state.loading)) return;
    request->settling = true;
    fb2k::inMainThread([weak = std::weak_ptr<AllSongsRequest>(request)] {
        auto self = weak.lock();
        if (!self) return;
        self->settling = false;
        SettleAllSongs(self);
    });
}

// Joins the shared read of `query` (the one its page holds, if open) and reads it whole.
void StartAllSongs(const std::shared_ptr<AllSongsRequest>& request, const stream::ListQuery& query) {
    ActiveSongsRequests().push_back(request);
    request->active = true;
    BeginAllSongs(request);
    auto session = stream::ListSession::Open(
        *request->provider, query,
        [weak = std::weak_ptr<AllSongsRequest>(request)](const stream::ListState& state) {
            CompleteAllSongs(weak.lock(), state);
        },
        stream::ListLoadMode::All);
    if (request->active)
        request->session = std::move(session);
    else
        session->Cancel();
}

SongsFetch FetchListSongs(stream::Provider& provider, stream::ListQuery query, PageHost* host = nullptr) {
    query = PreparedQuery(provider, std::move(query));
    return [&provider, query = std::move(query), host](std::function<void(metadb_handle_list)> ready) {
        auto request = std::make_shared<AllSongsRequest>();
        request->provider = &provider;
        request->ready = std::move(ready);
        request->host = host;
        if (host) request->alive = host->Lifetime();
        StartAllSongs(request, query);
    };
}

SongsFetch FetchPlaylistSongs(stream::Provider& provider, const stream::Id& id, PageHost* host = nullptr) {
    auto query = Query(stream::ListKind::PlaylistTracks);
    query.id = id;
    return FetchListSongs(provider, std::move(query), host);
}

SongsFetch FetchAlbumSongs(stream::Provider& provider, const stream::Id& id, PageHost* host = nullptr) {
    auto query = Query(stream::ListKind::AlbumTracks);
    query.id = id;
    return FetchListSongs(provider, std::move(query), host);
}

std::vector<std::wstring> FeedTrackIds(const std::vector<stream::Track>& tracks, size_t count = SIZE_MAX) {
    std::vector<std::wstring> ids;
    count = std::min(count, tracks.size());
    ids.reserve(count);
    for (size_t i = 0; i < count; ++i)
        ids.push_back(tracks[i].id);
    return ids;
}

bool FeedIdsMatch(const std::vector<std::wstring>& ids, const std::vector<stream::Track>& tracks) {
    return tracks.size() >= ids.size() &&
           std::equal(ids.begin(), ids.end(), tracks.begin(),
                      [](const std::wstring& id, const stream::Track& track) { return id == track.id; });
}

class PagedPlaybackFeed final {
  public:
    static bool Start(stream::Provider& provider, stream::ListQuery query,
                      const std::vector<stream::Track>& prefix, size_t prefixCount, size_t start,
                      PageHost& host, media::ListSource* statusSource) {
        auto* scheduler = media::PlaybackScheduler::Existing();
        prefixCount = std::min(prefixCount, prefix.size());
        if (!scheduler || prefixCount == 0 || start >= prefixCount) return false;
        metadb_handle_list items = Items(provider, prefix, prefixCount);
        std::vector<std::wstring> fedIds = FeedTrackIds(prefix, prefixCount);
        CancelAll();
        scheduler = media::PlaybackScheduler::Existing();
        if (!scheduler) return false;
        const uint64_t previousGeneration = scheduler->ContentGeneration();
        media::PlayQueue(items, start);
        scheduler = media::PlaybackScheduler::Existing();
        if (!scheduler || scheduler->ContentGeneration() == previousGeneration) return false;
        auto feed = std::shared_ptr<PagedPlaybackFeed>(
            new PagedPlaybackFeed(provider, std::move(query), std::move(fedIds),
                                  scheduler->ContentGeneration(), host, statusSource));
        Active().push_back(feed);
        feed->m_session = stream::ListSession::Open(
            provider, feed->m_query,
            [weak = std::weak_ptr<PagedPlaybackFeed>(feed)](const stream::ListState& state) {
                if (auto current = weak.lock()) current->Changed(state);
            },
            stream::ListLoadMode::All);
        return true;
    }

    static void CancelProvider(std::wstring_view provider) {
        std::vector<std::shared_ptr<PagedPlaybackFeed>> cancel;
        std::erase_if(Active(), [&](const auto& feed) {
            if (feed->m_provider.Key() != provider) return false;
            cancel.push_back(feed);
            return true;
        });
        for (auto& feed : cancel)
            feed->Finish();
    }

    static void Shutdown() { CancelAll(); }

  private:
    PagedPlaybackFeed(stream::Provider& provider, stream::ListQuery query, std::vector<std::wstring> fedIds,
                      uint64_t generation, PageHost& host, media::ListSource* statusSource)
        : m_provider(provider), m_query(std::move(query)), m_fedIds(std::move(fedIds)), m_host(&host),
          m_generation(generation) {
        m_badge.Begin(host, statusSource);
    }

    static std::vector<std::shared_ptr<PagedPlaybackFeed>>& Active() {
        static std::vector<std::shared_ptr<PagedPlaybackFeed>> feeds;
        return feeds;
    }

    static void CancelAll() {
        auto feeds = std::move(Active());
        Active().clear();
        for (auto& feed : feeds)
            feed->Finish();
    }

    void Changed(const stream::ListState& state) {
        auto* scheduler = media::PlaybackScheduler::Existing();
        if (!scheduler || scheduler->ContentGeneration() != m_generation || !state.error.empty()) {
            Finish();
            return;
        }
        if (state.dataVersion == m_dataVersion) return;
        const auto& tracks = Rows<stream::Track>(state.page.rows);
        if (state.updatedRows) {
            // Metadata-only publications retain occurrence order and already-fed identities.
            // They must not replace or append the queue, and the read continuation stays live.
            if (!FeedIdsMatch(m_fedIds, tracks)) {
                Finish();
                return;
            }
            m_dataVersion = state.dataVersion;
            m_hasPublication = true;
            if (state.complete) Finish();
            return;
        }
        size_t suffix = 0;
        if (!m_hasPublication || state.appendOffset == 0) {
            if (!FeedIdsMatch(m_fedIds, tracks)) {
                Finish();
                return;
            }
            suffix = m_fedIds.size();
        } else {
            if (state.appendOffset != m_fedIds.size() || state.appendOffset > tracks.size()) {
                Finish();
                return;
            }
            suffix = state.appendOffset;
        }
        if (tracks.size() > suffix) {
            metadb_handle_list items;
            for (size_t i = suffix; i < tracks.size(); ++i)
                items.add_item(m_provider.TrackItem(tracks[i].id));
            if (!scheduler->AppendSourceBatch(items, m_generation)) {
                Finish();
                return;
            }
            m_fedIds.reserve(tracks.size());
            for (size_t i = suffix; i < tracks.size(); ++i)
                m_fedIds.push_back(tracks[i].id);
        }
        m_dataVersion = state.dataVersion;
        m_hasPublication = true;
        if (state.complete) {
            Finish();
            return;
        }
    }

    void Finish() {
        if (m_finished) return;
        m_finished = true;
        if (m_session) m_session->Cancel();
        std::erase_if(Active(), [this](const auto& feed) { return feed.get() == this; });
        m_badge.End();
    }

    stream::Provider& m_provider;
    stream::ListQuery m_query;
    std::shared_ptr<stream::ListSession> m_session;
    std::vector<std::wstring> m_fedIds;
    uint64_t m_dataVersion = 0;
    PageHost* m_host = nullptr;
    LoadingBadge m_badge; // "Loading..." on the list the play began from
    uint64_t m_generation = 0;
    bool m_hasPublication = false;
    bool m_finished = false;
};

stream::ListQuery PlaylistQuery(stream::Provider& provider, const stream::Id& id) {
    (void)provider;
    auto query = Query(stream::ListKind::PlaylistTracks);
    query.id = id;
    return query;
}

stream::ListQuery AlbumQuery(stream::Provider& provider, const stream::Id& id) {
    (void)provider;
    auto query = Query(stream::ListKind::AlbumTracks);
    query.id = id;
    return query;
}

stream::ListQuery IdQuery(stream::ListKind kind, const stream::Id& id) {
    auto query = Query(kind);
    query.id = id;
    return query;
}

stream::ListQuery GroupsQuery(const stream::Id& section) {
    auto query = Query(stream::ListKind::Groups);
    query.section = section;
    return query;
}

stream::ListQuery SectionTracksQuery(const stream::Id& section, const stream::Id& group) {
    auto query = Query(stream::ListKind::SectionTracks);
    query.id = group;
    query.section = section;
    return query;
}

stream::ListQuery PlaylistsQuery(stream::Shelf shelf, const stream::Id& section) {
    auto query = Query(stream::ListKind::Playlists);
    query.shelf = shelf;
    query.section = section;
    return query;
}

stream::ListQuery ArtistAlbumsQuery(const stream::Id& artist) {
    auto query = Query(stream::ListKind::ArtistAlbums);
    query.id = artist;
    return query;
}

stream::ListQuery SavedAlbumsQuery() {
    return Query(stream::ListKind::SavedAlbums);
}

std::unique_ptr<RemoteSongSource> Songs(stream::Provider& provider, stream::ListKind kind,
                                        const std::vector<stream::Track>& tracks, std::wstring empty,
                                        size_t count = SIZE_MAX) {
    return std::make_unique<RemoteSongSource>(provider, kind, tracks, std::move(empty), count);
}

bool MatchesPublishedPrefix(RemoteSongSource& source, stream::Provider& provider,
                            const std::vector<stream::Track>& tracks, size_t count) {
    const metadb_handle_list visible = source.VisibleItems();
    count = std::min(count, tracks.size());
    if (visible.get_count() != count) return false;
    for (size_t i = 0; i < count; ++i) {
        const metadb_handle_ptr expected = provider.TrackItem(tracks[i].id);
        if (!expected.is_valid() || visible.get_item(i) != expected) return false;
    }
    return true;
}

// Album collect/uncollect, from any menu. `done` runs after a successful write.
MenuAction SaveAlbumAction(stream::Provider& provider, const stream::Id& id,
                           std::function<void(bool saved)> done) {
    const bool saved = IsSavedAlbum(provider, id);
    return {provider.Text(saved ? L"Uncollect album" : L"Collect album"),
            [&provider, id, saved, done] {
                provider.SetAlbumSaved(id, !saved, [&provider, saved, done](std::wstring error) {
                    if (!error.empty()) return ShowProviderError(provider, error);
                    if (done) done(!saved);
                });
            }};
}

MenuAction FollowAction(stream::Provider& provider, const stream::Id& id,
                        std::function<void(bool followed)> done) {
    const bool followed = provider.ArtistPreferenceState(id).value_or(IsFollowed(provider, id));
    return {provider.Text(followed ? L"Unfollow artist" : L"Follow artist"),
            [&provider, id, followed, done] {
                provider.SetFollowed(id, !followed, [&provider, followed, done](std::wstring error) {
                    if (!error.empty()) return ShowProviderError(provider, error);
                    if (done) done(!followed);
                });
            }};
}

// "Go to artist" for an album's credited artists (a submenu when there are several).
void AddGoToArtist(std::vector<MenuAction>& actions, PageHost& host, stream::Provider& provider,
                   const std::vector<stream::ArtistRef>& artists, const stream::Id& sectionId) {
    std::vector<MenuAction> links;
    for (const auto& artist : artists)
        if (!artist.id.empty())
            links.push_back(
                {artist.name.empty() ? L"Artist" : artist.name, [&host, &provider, artist, sectionId] {
                     host.Open(ArtistView(provider, artist.id, artist.name, sectionId));
                 }});
    if (links.size() == 1)
        actions.push_back({L"Go to artist", links.front().run});
    else if (!links.empty())
        actions.push_back({L"Go to artist", nullptr, std::move(links)});
}

// ---- common base ---------------------------------------------------------------------------

class RemotePage : public Page {
  public:
    RemotePage(PageHost& host, stream::Provider& provider, View view)
        : Page(host, std::move(view)), m_provider(provider) {}
    bool CanRefresh() const override { return true; }

  protected:
    bool SignedInOrPrompt() {
        if (m_provider.SignedIn() || m_provider.SessionRestoring()) return true;
        m_provider.SignIn(m_host.Owner());
        return false;
    }
    bool VerifiedOrPrompt() {
        if (m_provider.AccountVerified()) return true;
        if (!m_provider.SessionRestoring()) m_provider.SignIn(m_host.Owner());
        return false;
    }

    RemoteSongSource* SongSource() const { return dynamic_cast<RemoteSongSource*>(m_host.Source()); }

    stream::Provider& m_provider;
};

struct PagedListMeta {
    std::optional<stream::Playlist> playlist;
    std::optional<stream::Album> album;
    std::optional<stream::Artist> artist;
    bool loading = false;
    bool complete = false;
    bool refreshing = false;
    bool fromCache = false;
    std::wstring error;
    size_t pages = 0;
    uint64_t version = 0;
    uint64_t dataVersion = 0;
    size_t appendOffset = 0;
};

class PagedPage : public RemotePage {
  public:
    std::unique_ptr<media::ListSource> Start(bool) override {
        OpenSession();
        m_allMode = InitialMode() == stream::ListLoadMode::All;
        auto source = BuildSource();
        m_source = source.get();
        ConfigureSource();
        ApplyPageState();
        UpdateLoadingText();
        m_starting = false;
        return source;
    }

    void NearEnd() override {
        if (m_session && !m_state.complete && m_state.error.empty() && !m_state.loading)
            m_session->RequestNext();
    }

    void OnPresentationChanged() override {
        if (!m_session) return;
        m_allMode = m_host.Source() && m_host.Source()->NeedsCompleteList();
        m_session->SetMode(m_allMode || m_drainRequested ? stream::ListLoadMode::All
                                                         : stream::ListLoadMode::OnDemand);
        PresentationChanged();
        UpdateLoadingText();
        FinishDrainIfReady();
        m_host.ListChanged();
    }

    void OnLibraryChanged() override {
        UpdateLoadingText();
        m_host.ListChanged();
        m_host.ChromeChanged();
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        AddPageSpecificActions(actions);
        if (HasPagedError()) {
            if (!actions.empty() && !actions.back().label.empty()) actions.push_back({});
            actions.push_back({L"Retry", Guard([this] { RetryPage(); })});
        }
    }

  protected:
    PagedPage(PageHost& host, stream::Provider& provider, View view, stream::ListQuery query,
              bool catalogSearch = false)
        : RemotePage(host, provider, std::move(view)),
          m_query(PreparedQuery(provider, std::move(query), catalogSearch)) {}

    virtual std::unique_ptr<media::ListSource> BuildSource() = 0;
    virtual void ApplyPageState() = 0;
    virtual void ConfigureSource() {}
    virtual void PresentationChanged() {}
    virtual void AddPageSpecificActions(std::vector<MenuAction>&) {}
    virtual stream::ListLoadMode InitialMode() const { return stream::ListLoadMode::OnDemand; }
    virtual bool HasPagedError() const { return !m_state.error.empty() && !m_state.loading; }
    virtual bool PlaybackReady() const { return m_state.complete; }
    // Metadata-only completion can advance ListState::version/frontier without changing
    // dataVersion. Track pages use this hook to publish a newly usable gated prefix.
    virtual bool SourceNeedsApply(const stream::ListState&, bool dataChanged) const { return dataChanged; }
    virtual void RetryPage() {
        if (m_session) m_session->Refresh();
    }

    const stream::ListState& CurrentListState() const { return m_session->View(); }

    template <class T> const std::vector<T>& CurrentRows() const {
        return Rows<T>(CurrentListState().page.rows);
    }

    virtual std::wstring StatusText() const {
        if ((m_session && VisiblyLoading(CurrentListState())) ||
            ((m_allMode || m_drainRequested) && !m_state.complete && m_state.error.empty()))
            return Tr(L"Loading...");
        if (!m_state.error.empty()) return Tr(m_state.error);
        return {};
    }

    void UpdateLoadingText() {
        if (m_source) m_source->SetLoadingText(StatusText());
    }

    void EnsureAllSongs(std::function<void(bool)> done) {
        if (!m_session) {
            done(false);
            return;
        }
        if (PlaybackReady()) {
            done(true);
            return;
        }
        if (!m_state.error.empty() && !m_state.loading) {
            done(false);
            return;
        }
        m_songWaiters.push_back(std::move(done));
        m_drainRequested = true;
        UpdateLoadingText();
        m_host.ListChanged();
        m_session->SetMode(stream::ListLoadMode::All);
        if (!m_state.loading) m_session->RequestNext();
    }

    void FinishDrainIfReady() {
        if (!m_drainRequested) return;
        const bool failed = !m_state.error.empty() && !m_state.loading;
        if (!PlaybackReady() && !failed) return;
        auto waiters = std::move(m_songWaiters);
        m_songWaiters.clear();
        m_drainRequested = false;
        m_session->SetMode(m_allMode ? stream::ListLoadMode::All : stream::ListLoadMode::OnDemand);
        const bool complete = PlaybackReady();
        for (auto& waiter : waiters)
            waiter(complete);
    }

    SongsFetch AllSongsFetch() {
        auto guarded = Guard([this](std::function<void(metadb_handle_list)> ready) {
            EnsureAllSongs(Guard([this, ready = std::move(ready)](bool complete) {
                if (!complete) return;
                auto* source = SongSource();
                if (source) ready(source->VisibleItems());
            }));
        });
        return [guarded = std::move(guarded)](std::function<void(metadb_handle_list)> ready) mutable {
            guarded(std::move(ready));
        };
    }

    void ActivateSong(size_t row, const std::wstring& identity) {
        if (!m_session) return;
        auto* source = SongSource();
        if (!source) return;
        if (PlaybackReady()) {
            source->PlayFrom(row);
            return;
        }
        if (!source->NeedsCompleteList() && source->GetViewStyle() == media::ViewStyle::Details &&
            source->SortColumn() == SIZE_MAX && !source->HasFilter()) {
            const auto& tracks = CurrentRows<stream::Track>();
            const size_t published = source->NeedsAlbumIdentity()
                                         ? std::min(CurrentListState().metadataReadyRows, tracks.size())
                                         : tracks.size();
            if (row < published && MatchesPublishedPrefix(*source, m_provider, tracks, published) &&
                PagedPlaybackFeed::Start(m_provider, m_query, tracks, published, row, m_host, source))
                return;
        }
        if (!m_state.error.empty() && !m_state.loading) {
            if (const size_t current = source->FindRowByIdentity(identity); current != SIZE_MAX)
                source->PlayFrom(current);
            return;
        }
        EnsureAllSongs(Guard([this, identity](bool complete) {
            auto* loaded = SongSource();
            if (!complete || !loaded) return;
            const size_t resolved = loaded->FindRowByIdentity(identity);
            if (resolved != SIZE_MAX) loaded->PlayFrom(resolved);
        }));
    }

    stream::ListQuery m_query;
    std::shared_ptr<stream::ListSession> m_session;
    PagedListMeta m_state;
    media::ListSource* m_source = nullptr;
    uint64_t m_generation = 0;
    uint64_t m_lastDataVersion = 0;
    bool m_starting = true;
    bool m_allMode = false;
    bool m_drainRequested = false;
    std::vector<std::function<void(bool)>> m_songWaiters;

  private:
    void UpdateMeta(const stream::ListState& state) {
        m_state.playlist = state.page.playlist;
        m_state.album = state.page.album;
        m_state.artist = state.page.artist;
        m_state.loading = state.loading;
        m_state.complete = state.complete;
        m_state.refreshing = state.refreshing;
        m_state.fromCache = state.fromCache;
        m_state.error = state.error;
        m_state.pages = state.pages;
        m_state.version = state.version;
        m_state.dataVersion = state.dataVersion;
        m_state.appendOffset = state.appendOffset;
    }

    void OpenSession() {
        const uint64_t generation = ++m_generation;
        auto changed = Guard([this, generation](const stream::ListState& state) {
            if (generation != m_generation) return;
            const bool dataChanged = state.dataVersion != m_lastDataVersion;
            const bool sourceChanged = SourceNeedsApply(state, dataChanged);
            UpdateMeta(state);
            if (m_starting || !m_source) {
                if (dataChanged) m_lastDataVersion = state.dataVersion;
                return;
            }
            if (sourceChanged) {
                m_host.BeforeListChange();
                ApplyPageState();
            }
            if (dataChanged) {
                m_lastDataVersion = state.dataVersion;
            }
            UpdateLoadingText();
            FinishDrainIfReady();
            m_host.ListChanged();
            if (dataChanged) m_host.ChromeChanged();
        });
        m_session = stream::ListSession::Open(m_provider, m_query, std::move(changed), InitialMode());
        UpdateMeta(m_session->View());
        m_lastDataVersion = m_state.dataVersion;
    }
};

class TrackPagedPage : public PagedPage {
  public:
    TrackPagedPage(PageHost& host, stream::Provider& provider, View view, stream::ListQuery query,
                   std::wstring empty, bool catalogSearch = false)
        : PagedPage(host, provider, std::move(view), std::move(query), catalogSearch),
          m_empty(std::move(empty)) {}

  protected:
    std::unique_ptr<media::ListSource> BuildSource() override {
        const auto& tracks = CurrentRows<stream::Track>();
        const bool tiles = m_host.SavedStyle(media::ViewStyle::Details) == media::ViewStyle::Tiles;
        m_convertedTracks =
            tiles ? std::min(CurrentListState().metadataReadyRows, tracks.size()) : tracks.size();
        auto source = Songs(m_provider, m_query.kind, tracks, m_empty, m_convertedTracks);
        if (tiles) source->SetViewStyle(media::ViewStyle::Tiles);
        m_sourceDataVersion = CurrentListState().dataVersion;
        return source;
    }

    void ConfigureSource() override {
        if (auto* source = dynamic_cast<RemoteSongSource*>(m_source))
            source->SetActivateHandler(
                Guard([this](size_t row, const std::wstring& identity) { ActivateSong(row, identity); }));
    }

    bool SourceNeedsApply(const stream::ListState& state, bool dataChanged) const override {
        if (dataChanged) return true;
        auto* source = SongSource();
        return source && UsableTrackCount(state, source) != m_convertedTracks;
    }

    bool PlaybackReady() const override {
        if (!m_state.complete) return false;
        auto* source = SongSource();
        if (!source || !source->NeedsAlbumIdentity()) return true;
        return CurrentListState().metadataReadyRows >= CurrentRows<stream::Track>().size();
    }

    void PresentationChanged() override {
        auto* source = SongSource();
        if (!source || UsableTrackCount(CurrentListState(), source) == m_convertedTracks) return;
        m_host.BeforeListChange();
        ApplyPageState();
    }

    void ApplyPageState() override {
        auto* source = dynamic_cast<RemoteSongSource*>(m_source);
        if (!source) {
            ApplyTrackState();
            return;
        }
        const stream::ListState& state = CurrentListState();
        const auto& tracks = Rows<stream::Track>(state.page.rows);
        const size_t previousCount = m_convertedTracks;
        const size_t usableCount = UsableTrackCount(state, source);
        const bool dataChanged = state.dataVersion != m_sourceDataVersion;
        bool appended = false;
        bool replaced = false;

        if (usableCount > previousCount) {
            const bool ordinaryAppend = !state.updatedRows && state.appendOffset == previousCount;
            const bool frontierAppend =
                state.updatedRows && state.appendOffset == 0 && state.metadataReadyRows >= usableCount;
            if (!dataChanged || ordinaryAppend || frontierAppend) {
                source->AppendTracks(tracks, previousCount, usableCount);
                appended = true;
            } else {
                source->ReplaceTracks(tracks, usableCount);
                replaced = true;
            }
        } else if (usableCount < previousCount) {
            source->ReplaceTracks(tracks, usableCount);
            replaced = true;
        } else if (dataChanged && !state.updatedRows && state.appendOffset == 0) {
            // A refresh replaced the accumulated prefix at the same size.
            source->ReplaceTracks(tracks, usableCount);
            replaced = true;
        }

        if (dataChanged && state.updatedRows && state.appendOffset == 0 && !replaced) {
            const stream::ListRowRange& range = *state.updatedRows;
            if (range.offset > usableCount || range.count > usableCount - range.offset) {
                source->ReplaceTracks(tracks, usableCount);
            } else {
                const size_t begin = range.offset;
                size_t end = begin + range.count;
                if (appended) end = std::min(end, previousCount);
                if (begin < end && !source->UpdateTrackRange(tracks, begin, end - begin)) {
                    source->ReplaceTracks(tracks, usableCount);
                }
            }
        }
        m_convertedTracks = usableCount;
        m_sourceDataVersion = state.dataVersion;
        ApplyTrackState();
    }

    std::wstring StatusText() const override {
        if (!m_state.error.empty()) return Tr(m_state.error);
        auto* source = SongSource();
        const auto& tracks = CurrentRows<stream::Track>();
        const bool metadataPending = source && source->NeedsAlbumIdentity() &&
                                     UsableTrackCount(CurrentListState(), source) < tracks.size();
        if (metadataPending) return Tr(L"Loading...");
        const bool hasRows = source && source->Count() != 0;
        if (!hasRows && (m_state.loading || m_state.refreshing)) return Tr(L"Loading...");
        if ((m_allMode || m_drainRequested) && !m_state.complete && m_state.error.empty())
            return Tr(L"Loading...");
        return {};
    }

    void AddPageSpecificActions(std::vector<MenuAction>& actions) override {
        if (auto* source = SongSource(); source && source->Count()) {
            AddBulkActions(actions, AllSongsFetch(), &m_provider);
            actions.push_back({});
        }
        AddTrackPageActions(actions);
    }

    virtual void AddTrackPageActions(std::vector<MenuAction>&) {}
    virtual void ApplyTrackState() {}

  private:
    size_t UsableTrackCount(const stream::ListState& state, const RemoteSongSource* source) const {
        const auto& tracks = Rows<stream::Track>(state.page.rows);
        return source && source->NeedsAlbumIdentity() ? std::min(state.metadataReadyRows, tracks.size())
                                                      : tracks.size();
    }

    std::wstring m_empty;
    size_t m_convertedTracks = 0;     // Contiguous tracks currently present in the source.
    uint64_t m_sourceDataVersion = 0; // Content version; the usable frontier is tracked separately.
};

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
        d.artKey = m_provider.ArtKey(stream::ArtKind::Playlist, m_view.remoteId);
        d.artItem = m_provider.ArtItem(stream::ArtKind::Playlist, m_view.remoteId);
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
    void AddTrackPageActions(std::vector<MenuAction>& actions) override {
        if (!m_provider.AccountVerified()) return;
        const std::wstring name = m_info && !m_info->name.empty() ? m_info->name : m_view.name;
        if (Own()) {
            const auto offers = m_provider.Offers();
            if (!offers.Has(stream::Capability::RenamePlaylists) && !offers.Has(stream::Capability::DeletePlaylists)) return;
            actions.push_back({});
            if (offers.Has(stream::Capability::RenamePlaylists))
                actions.push_back({ProviderPlaylistText(m_provider, L"Rename..."), [this, name] {
                                       RenameRemotePlaylist(m_host, m_provider, m_view.remoteId, name);
                                   }});
            if (offers.Has(stream::Capability::DeletePlaylists))
                actions.push_back({ProviderPlaylistText(m_provider, L"Delete playlist"),
                                   [this, name, sectionId = m_view.sectionId] {
                                       DeleteRemotePlaylist(m_host, m_provider, m_view.remoteId, name,
                                                            sectionId);
                                   }});
        } else if (m_provider.Can(stream::Capability::SavePlaylists)) {
            actions.push_back({});
            actions.push_back(
                {ProviderPlaylistText(m_provider, Saved() ? L"Uncollect playlist" : L"Collect playlist"),
                 [this] { SaveAction(); }});
        }
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
    bool Own() const {
        return m_provider.ShelfOf(m_view.remoteId) == Shelf::Created || (m_info && m_info->own);
    }
    bool Saved() const {
        if (const auto current = m_provider.PlaylistSavedState(m_view.remoteId)) return *current;
        return m_provider.ShelfOf(m_view.remoteId) == Shelf::Saved || (m_info && m_info->saved) ||
               Contains(m_provider.Playlists(Shelf::Saved), m_view.remoteId);
    }

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

class ShelfPage final : public PagedPage {
  public:
    ShelfPage(PageHost& host, stream::Provider& provider, View view)
        : PagedPage(host, provider, view, PlaylistsQuery(view.shelf, view.sectionId)) {}

    bool CanRefresh() const override { return m_view.shelf != Shelf::Recommended; } // a daily set

  protected:
    std::unique_ptr<media::ListSource> BuildSource() override {
        return Grid(CurrentRows<stream::Playlist>());
    }

    void ApplyPageState() override {
        if (auto* source = dynamic_cast<PlaylistGridSource*>(m_source)) {
            source->ReplacePlaylists(CurrentRows<stream::Playlist>());
        }
    }

    void AddPageSpecificActions(std::vector<MenuAction>& actions) override {
        if (m_view.shelf == Shelf::Created && m_provider.Can(stream::Capability::CreatePlaylists))
            actions.push_back({m_provider.NamedText(L"New {0} playlist"),
                               [this] { CreateRemotePlaylist(m_host, m_provider); }});
    }

    void OnLibraryChanged() override {
        if (m_view.shelf == Shelf::Recommended) return;
        PagedPage::OnLibraryChanged();
    }

  private:
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
};

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
                context.page.push_back({L"Refresh", [this] { m_host.Refresh(); }});
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
                if (m_artist->profile && !artist.profile) stream::CopyArtistProfile(*m_artist, artist);
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
        if (!m_trackError.empty()) return Tr(m_trackError);
        if (Expanded() && CompleteNeeded() && !AlbumsAndTracksComplete() && m_state.error.empty())
            return Tr(L"Loading...");
        if (Expanded() && m_filling && m_state.error.empty()) return Tr(L"Loading...");
        for (const auto& [id, session] : m_albumTrackSessions)
            if (session && VisiblyLoading(session->View())) return Tr(L"Loading...");
        return PagedPage::StatusText();
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
        return !m_waiters.empty() || (m_source && (m_source->HasFilter() || m_source->SortColumn() != SIZE_MAX));
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
        m_waiters.push_back(std::move(done));
        m_session->SetMode(stream::ListLoadMode::All);
        if (!m_state.loading && !m_state.complete) m_session->RequestNext();
        StartNextAlbumTracks();
        SettleWaiters();
    }

    void SettleWaiters() {
        if (m_waiters.empty() || !m_state.complete) return;
        for (const auto& album : m_albums) {
            if (album.id.empty()) continue;
            const auto* state = AlbumTrackState(album.id);
            if (!state || (!state->complete && state->error.empty())) return;
            if (!state->error.empty() && !state->loading) {
                auto waiters = std::move(m_waiters);
                m_waiters.clear();
                for (auto& waiter : waiters)
                    waiter(false);
                return;
            }
        }
        auto waiters = std::move(m_waiters);
        m_waiters.clear();
        for (auto& waiter : waiters)
            waiter(true);
    }

    SongsFetch ExpandedSongsFetch() {
        auto guarded = Guard([this](std::function<void(metadb_handle_list)> ready) {
            EnsureExpandedSongs(Guard([this, ready = std::move(ready)](bool complete) {
                if (complete)
                    if (auto* source = SongSource()) ready(source->VisibleItems());
            }));
        });
        return [guarded = std::move(guarded)](std::function<void(metadb_handle_list)> ready) mutable {
            guarded(std::move(ready));
        };
    }

    // A song of the expanded view plays its album, at once (owner, 2026-11-14): waiting for
    // every album of the artist first left a double click doing nothing for a long while.
    void ActivateExpanded(size_t row, const std::wstring&) {
        if (auto* source = SongSource()) source->PlayGroupFrom(row);
    }

    std::map<stream::Id, std::shared_ptr<stream::ListSession>> m_albumTrackSessions;
    std::map<stream::Id, uint64_t> m_albumTrackVersions; // data version last assembled
    std::map<stream::Id, uint64_t> m_albumTrackGenerations;
    std::vector<std::function<void(bool)>> m_waiters;
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
            if (VisiblyLoading(state)) return Tr(L"Loading...");
            any |= std::visit([](const auto& rows) { return !rows.empty(); }, state.page.rows);
            if (error.empty() && !state.error.empty() && !state.loading) error = state.error;
        }
        // A section that failed is left out; only an overview with nothing at all says why.
        return any ? std::wstring() : Tr(error);
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
        context.page.push_back({L"Refresh", [this] { m_host.Refresh(); }});
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
        context.page.push_back({L"Refresh", [this] { m_host.Refresh(); }});
        ShowRecordMenu(m_host, m_provider, album, screen, context);
    }
};

// ---- All Music > Favorites -------------------------------------------------------------------

class AllFavoritesPage final : public Page {
  public:
    AllFavoritesPage(PageHost& host, View view) : Page(host, std::move(view)) {}
    bool CanRefresh() const override { return true; }

    std::unique_ptr<media::ListSource> Start(bool) override {
        BuildLocalFavorites();
        auto source = std::make_unique<AllFavoritesSource>(m_local, AllFavoritesSource::RemoteFavorites{});
        m_source = source.get();
        ConfigureSource();
        for (auto* provider : stream::Providers())
            if (HasFavorites(*provider)) OpenFeed(*provider);
        Publish();
        return source;
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        if (auto* source = SongSource(); source && source->Count()) {
            AddBulkActions(actions, AllSongsFetch());
            actions.push_back({});
        }
        if (HasError()) {
            if (!actions.empty() && !actions.back().label.empty()) actions.push_back({});
            actions.push_back({L"Retry", Guard([this] { Retry(); })});
        }
    }

    void NearEnd() override {
        for (auto& feed : m_feeds)
            if (feed.session && !feed.State().complete && feed.State().error.empty() && !feed.State().loading)
                feed.session->RequestNext();
    }

    void OnPresentationChanged() override {
        if (!m_source) return;
        m_allMode = m_source->NeedsCompleteList();
        UpdateSessionMode();
        UpdateStatus();
        m_host.ListChanged();
    }

    void OnLibraryChanged() override {
        // An account that signed in (or out) since the feeds were opened has none (or a stale
        // one): the page is built again with the providers there are now.
        std::vector<const stream::Provider*> now;
        for (auto* provider : stream::Providers())
            if (HasFavorites(*provider)) now.push_back(provider);
        if (!std::equal(now.begin(), now.end(), m_feeds.begin(), m_feeds.end(),
                        [](const stream::Provider* provider, const Feed& feed) {
                            return provider == feed.provider;
                        })) {
            m_host.Reload(); // destroys this page
            return;
        }
        UpdateStatus();
        m_host.ChromeChanged();
    }

    void OnFavoritesChanged() override {
        auto* source = SongSource();
        if (source &&
            std::any_of(m_feeds.begin(), m_feeds.end(), [](const Feed& f) { return f.State().pages != 0; })) {
            m_host.BeforeListChange();
            if (!source->RemoveUnfavorited()) return;
            m_host.ListChanged();
        } else {
            Page::OnFavoritesChanged();
        }
    }

  private:
    struct Feed {
        stream::Provider* provider = nullptr;
        std::shared_ptr<stream::ListSession> session;
        uint64_t dataVersion = 0; // last published
        uint64_t generation = 0;
        // The session's state, borrowed rather than copied on every change.
        const stream::ListState& State() const {
            static const stream::ListState empty;
            return session ? session->View() : empty;
        }
    };

    SongTableSource* SongSource() const { return dynamic_cast<SongTableSource*>(m_host.Source()); }

    // A signed-in (or restoring) account of a source with a Favorites section.
    static bool HasFavorites(stream::Provider& provider) {
        return FindSection(provider, stream::SectionKind::Favorites) &&
               (provider.SignedIn() || provider.SessionRestoring());
    }

    void BuildLocalFavorites() {
        if (const t_size playlist = media::FavoritesPlaylist(false); playlist != SIZE_MAX) {
            metadb_handle_list items;
            playlist_manager::get()->playlist_get_all_items(playlist, items);
            for (size_t i = 0; i < items.get_count(); ++i)
                if (!stream::ProviderOf(items[i])) m_local.push_back(items[i]);
        }
    }

    void ConfigureSource() {
        if (auto* source = dynamic_cast<AllFavoritesSource*>(m_source))
            source->SetActivateHandler(
                Guard([this](size_t row, const std::wstring& identity) { Activate(row, identity); }));
    }

    void OpenFeed(stream::Provider& provider) {
        Feed& feed = m_feeds.emplace_back();
        feed.provider = &provider;
        feed.generation = ++m_generation;
        const uint64_t generation = feed.generation;
        auto changed = Guard([this, providerPtr = &provider, generation](const stream::ListState& state) {
            const auto found = std::find_if(m_feeds.begin(), m_feeds.end(), [providerPtr](const Feed& feed) {
                return feed.provider == providerPtr;
            });
            if (found == m_feeds.end() || found->generation != generation) return;
            const bool dataChanged = found->dataVersion != state.dataVersion;
            found->dataVersion = state.dataVersion;
            if (dataChanged) {
                m_host.BeforeListChange();
                Publish();
            } else {
                UpdateStatus();
            }
            if ((Complete() || (!state.error.empty() && !state.loading)) && m_drainRequested) {
                m_drainRequested = false;
                UpdateSessionMode();
            }
            SettleWaiters();
            m_host.ListChanged();
            if (dataChanged) m_host.ChromeChanged();
        });
        auto query = Query(stream::ListKind::Favorites);
        feed.session = stream::ListSession::Open(provider, PreparedQuery(provider, std::move(query)),
                                                 std::move(changed), stream::ListLoadMode::OnDemand);
        feed.dataVersion = feed.session->View().dataVersion;
    }

    AllFavoritesSource::RemoteFavorites RemoteRows() const {
        AllFavoritesSource::RemoteFavorites rows;
        for (const auto& feed : m_feeds)
            rows.push_back({feed.provider, Rows<stream::Track>(feed.State().page.rows)});
        return rows;
    }

    bool Complete() const {
        return std::all_of(m_feeds.begin(), m_feeds.end(),
                           [](const Feed& feed) { return feed.State().complete; });
    }

    bool HasError() const {
        return std::any_of(m_feeds.begin(), m_feeds.end(),
                           [](const Feed& feed) { return !feed.State().error.empty() && !feed.State().loading; });
    }

    std::wstring LoadingText() const {
        if (std::any_of(m_feeds.begin(), m_feeds.end(),
                        [](const Feed& feed) { return VisiblyLoading(feed.State()); }) ||
            ((m_allMode || m_drainRequested) && !Complete() && !HasError()))
            return Tr(L"Loading...");
        const auto failed = std::find_if(m_feeds.begin(), m_feeds.end(),
                                         [](const Feed& feed) { return !feed.State().error.empty(); });
        return failed == m_feeds.end() ? std::wstring() : Tr(failed->State().error);
    }

    void Publish() {
        if (!m_source) return;
        if (auto* source = dynamic_cast<AllFavoritesSource*>(m_source))
            source->ReplaceFavorites(m_local, RemoteRows());
        UpdateStatus();
    }

    void UpdateStatus() {
        if (m_source) m_source->SetLoadingText(LoadingText());
    }

    void UpdateSessionMode() {
        const auto mode =
            m_allMode || m_drainRequested ? stream::ListLoadMode::All : stream::ListLoadMode::OnDemand;
        for (auto& feed : m_feeds)
            if (feed.session) feed.session->SetMode(mode);
    }

    void Retry() {
        for (auto& feed : m_feeds)
            if (!feed.State().error.empty() && feed.session) feed.session->Refresh();
    }

    void EnsureAllSongs(std::function<void(bool)> done) {
        if (Complete()) {
            done(true);
            return;
        }
        if (HasError()) {
            done(false);
            return;
        }
        m_waiters.push_back(std::move(done));
        m_drainRequested = true;
        UpdateStatus();
        m_host.ListChanged();
        UpdateSessionMode();
        for (auto& feed : m_feeds) {
            if (!feed.State().loading) feed.session->RequestNext();
        }
    }

    void SettleWaiters() {
        if (m_waiters.empty() || (!Complete() && !HasError())) return;
        const bool complete = Complete() && !HasError();
        auto waiters = std::move(m_waiters);
        m_waiters.clear();
        for (auto& waiter : waiters)
            waiter(complete);
    }

    SongsFetch AllSongsFetch() {
        auto guarded = Guard([this](std::function<void(metadb_handle_list)> ready) {
            EnsureAllSongs(Guard([this, ready = std::move(ready)](bool complete) {
                if (!complete) return;
                if (auto* source = SongSource()) ready(source->VisibleItems());
            }));
        });
        return [guarded = std::move(guarded)](std::function<void(metadb_handle_list)> ready) mutable {
            guarded(std::move(ready));
        };
    }

    void Activate(size_t row, const std::wstring& identity) {
        auto* source = SongSource();
        if (!source) return;
        if (Complete()) {
            source->PlayFrom(row);
            return;
        }
        if (HasError()) {
            const size_t current = source->FindRowByIdentity(identity);
            if (current != SIZE_MAX) source->PlayFrom(current);
            return;
        }
        EnsureAllSongs(Guard([this, identity](bool complete) {
            auto* loaded = SongSource();
            if (!loaded) return;
            if (!complete) {
                const size_t current = loaded->FindRowByIdentity(identity);
                if (current != SIZE_MAX) loaded->PlayFrom(current);
                return;
            }
            const size_t resolved = loaded->FindRowByIdentity(identity);
            if (resolved != SIZE_MAX) loaded->PlayFrom(resolved);
        }));
    }

    std::vector<metadb_handle_ptr> m_local;
    std::vector<Feed> m_feeds;
    media::ListSource* m_source = nullptr;
    uint64_t m_generation = 0;
    bool m_allMode = false;
    bool m_drainRequested = false;
    std::vector<std::function<void(bool)>> m_waiters;
};

} // namespace

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

// ---- A record's menu (ui/item_menu.h) ----------------------------------------------------------

namespace {
MenuAction SearchFor(PageHost& host, const wchar_t* what, const std::wstring& name) {
    return {Tr(what) + L" \u201C" + name + L"\u201D", [&host, name] { host.Search(name); }};
}
} // namespace

std::vector<MenuAction> RecordActions(PageHost& host, stream::Provider& provider, const MenuRecord& record,
                                      const RecordMenuContext& context) {
    std::vector<MenuAction> actions;
    const auto group = [&actions] {
        if (!actions.empty() && !actions.back().label.empty()) actions.push_back({});
    };
    const bool account = provider.AccountVerified();
    if (const auto* album = std::get_if<stream::Album>(&record)) {
        const stream::Album a = *album;
        if (!context.onItsPage)
            actions.push_back({L"View", [&host, &provider, a, section = context.sectionId] {
                                   host.Open(AlbumView(provider, a.id, a.name, section));
                               }});
        group();
        AddBulkActions(actions, FetchAlbumSongs(provider, a.id, &host), &provider, {}, SIZE_MAX,
                       [&provider, id = a.id] { PlayRemoteAlbum(provider, id); });
        if (account && provider.Can(stream::Capability::SaveAlbums)) {
            group();
            actions.push_back(SaveAlbumAction(provider, a.id, [removed = context.removed, id = a.id](bool saved) {
                if (!saved && removed) removed(id);
            }));
        }
        group();
        AddGoToArtist(actions, host, provider, a.artists, context.sectionId);
        group();
        actions.push_back(SearchFor(host, L"Search for album", a.name));
    } else if (const auto* artist = std::get_if<stream::Artist>(&record)) {
        const stream::Artist a = *artist;
        if (!context.onItsPage)
            actions.push_back({L"View", [&host, &provider, a, section = context.sectionId] {
                                   host.Open(ArtistView(provider, a.id, a.name, section));
                               }});
        group();
        // The artist's songs: what its page lists first (top songs, or an uploaded artist's).
        const stream::ListQuery songs = IdQuery(stream::ListKind::ArtistTopTracks, a.id);
        AddBulkActions(actions, FetchListSongs(provider, songs, &host), &provider, {}, SIZE_MAX,
                       [&provider, songs] { PlayRemoteList(provider, songs, false); });
        if (account && SupportsArtistPreference(provider)) {
            group();
            actions.push_back(FollowAction(provider, a.id, [removed = context.removed, id = a.id](bool followed) {
                if (!followed && removed) removed(id);
            }));
        }
        group();
        actions.push_back(SearchFor(host, L"Search for artist", a.name));
    } else if (const auto* playlist = std::get_if<stream::Playlist>(&record)) {
        const stream::Id id = playlist->id;
        const std::wstring name = playlist->name;
        const auto shelf = context.shelf;
        if (!context.onItsPage)
            actions.push_back({L"View", [&host, &provider, id, name, shelf, section = context.sectionId] {
                                   host.Open(PlaylistView(provider, id, name, shelf, section));
                               }});
        group();
        AddBulkActions(actions, FetchPlaylistSongs(provider, id, &host), &provider, id, SIZE_MAX,
                       [&provider, id] { PlayRemotePlaylist(provider, id); });
        const auto info = provider.FindPlaylist(id);
        const bool own = shelf == Shelf::Created || (info && info->own);
        const bool saved = provider.PlaylistSavedState(id).value_or(
            shelf == Shelf::Saved || (info && info->saved) || Contains(provider.Playlists(Shelf::Saved), id));
        if (account && own) {
            const auto offers = provider.Offers();
            group();
            if (offers.Has(stream::Capability::RenamePlaylists))
                actions.push_back({ProviderPlaylistText(provider, L"Rename..."),
                                   [&host, &provider, id, name] { RenameRemotePlaylist(host, provider, id, name); }});
            if (offers.Has(stream::Capability::DeletePlaylists))
                actions.push_back({ProviderPlaylistText(provider, L"Delete playlist"),
                                   [&host, &provider, id, name, section = context.sectionId] {
                                       DeleteRemotePlaylist(host, provider, id, name, section);
                                   }});
        } else if (account && provider.Can(stream::Capability::SavePlaylists)) {
            group();
            actions.push_back(
                {ProviderPlaylistText(provider, saved ? L"Uncollect playlist" : L"Collect playlist"),
                 [&provider, id, saved, removed = context.removed] {
                     provider.SetPlaylistSaved(id, !saved, [&provider, id, saved, removed](std::wstring error) {
                         if (!error.empty()) return ShowProviderError(provider, error);
                         if (saved && removed) removed(id);
                     });
                 }});
        }
        group();
        actions.push_back(SearchFor(host, L"Search for playlist", name));
    }
    if (!context.page.empty()) {
        group();
        for (const auto& action : context.page) actions.push_back(action);
    }
    while (!actions.empty() && actions.back().label.empty() && !actions.back().run) actions.pop_back();
    return actions;
}

void ShowRecordMenu(PageHost& host, stream::Provider& provider, const MenuRecord& record, POINT screen,
                    const RecordMenuContext& context) {
    ShowActionMenu(host.Owner(), screen, RecordActions(host, provider, record, context));
}

void PlayRemotePlaylist(stream::Provider& provider, const stream::Id& playlist) {
    PlayRemoteList(provider, IdQuery(stream::ListKind::PlaylistTracks, playlist), true);
}

void PlayRemoteAlbum(stream::Provider& provider, const stream::Id& album) {
    PlayRemoteList(provider, IdQuery(stream::ListKind::AlbumTracks, album), false);
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

void Page::OnFavoritesChanged() {
    if (auto* source = dynamic_cast<SongTableSource*>(m_host.Source())) {
        source->RefreshFavorites();
        m_host.ListChanged();
    }
}

// ---- one song --------------------------------------------------------------------------------

// A song's page: the details band holds the song (cover, title, artist and album, every detail
// the service has, Play); the list under it the credits in the service's groups and the
// similar songs. A source shows only what it has: Apple Music credits, NetEase similar songs.
class SongPage final : public Page {
  public:
    SongPage(PageHost& host, stream::Provider& provider, View view)
        : Page(host, std::move(view)), m_provider(provider) {}

    std::unique_ptr<media::ListSource> Start(bool) override {
        AfterStart([this] {
            m_provider.FetchSongDetails(m_view.remoteId,
                                        Guard([this](stream::SongDetails details, std::wstring error) {
                                            m_error = std::move(error);
                                            if (m_error.empty()) {
                                                m_details = std::move(details);
                                                if (!m_details->track.title.empty())
                                                    m_host.SetPageName(m_details->track.title);
                                            }
                                            m_host.ShowSource(Source());
                                            m_host.ChromeChanged();
                                        }));
        });
        return Source();
    }

    DetailInfo Detail() const override {
        DetailInfo d;
        d.visible = true;
        d.title = m_details && !m_details->track.title.empty() ? m_details->track.title : m_view.name;
        if (d.title.empty()) d.title = Tr(L"Song");
        const metadb_handle_ptr item = m_provider.TrackItem(m_view.remoteId);
        if (item.is_valid()) {
            d.artItem = item;
            d.artKey = ArtCache::SourceKey(item);
        }
        d.action = DetailInfo::Action::Play;
        d.actionLabel = Tr(L"Play");
        if (!m_details) return d;
        const stream::Track& track = m_details->track;
        const std::wstring released = m_details->released.size() >= 4 ? m_details->released.substr(0, 4) : L"";
        d.subtitle = Joined({stream::JoinArtists(track.artists), track.album, released});
        d.facts = Facts(*m_details);
        return d;
    }

    void DetailAction() override {
        const metadb_handle_ptr item = m_provider.TrackItem(m_view.remoteId);
        if (!item.is_valid()) return;
        metadb_handle_list items;
        items.add_item(item);
        media::PlayItems(items, 0);
    }

    void AddPageActions(std::vector<MenuAction>& actions) override {
        if (!m_details) return;
        const stream::Track& track = m_details->track;
        AddGoToArtist(actions, m_host, m_provider, track.artists, {});
        if (!track.albumId.empty())
            actions.push_back({L"Go to album", [this, track] {
                                   View v = AlbumView(m_provider, track.albumId, track.album);
                                   v.select = m_provider.TrackItem(track.id); // shown selected there
                                   m_host.Open(v);
                               }});
    }

  private:
    std::unique_ptr<media::ListSource> Source() {
        const std::wstring empty = !m_error.empty() ? m_error
                                   : !m_details     ? Tr(L"Loading...")
                                                    : Tr(L"The service lists no credits for this song.");
        return std::make_unique<SongInfoSource>(
            m_provider, m_details ? *m_details : stream::SongDetails{}, empty,
            [this](const stream::Id& artist, const std::wstring& name) {
                m_host.Open(ArtistView(m_provider, artist, name));
            });
    }

    static std::vector<DetailInfo::Fact> Facts(const stream::SongDetails& song) {
        std::vector<DetailInfo::Fact> facts;
        const auto add = [&facts](const wchar_t* label, std::wstring value) {
            if (!value.empty()) facts.push_back({Tr(label), std::move(value)});
        };
        std::vector<std::wstring> traits;
        for (const auto& trait : song.traits) traits.push_back(Tr(trait));
        add(L"Length", song.track.durationMs > 0 ? FormatTime(song.track.durationMs / 1000.0) : L"");
        std::wstring position;
        if (song.number > 0) {
            position = Tr(L"Track {0}");
            position.replace(position.find(L"{0}"), 3, std::to_wstring(song.number));
            if (song.disc > 0) {
                std::wstring disc = Tr(L"Disc {0}");
                disc.replace(disc.find(L"{0}"), 3, std::to_wstring(song.disc));
                position = disc + L" · " + position;
            }
        }
        add(L"Position", position);
        add(L"Released", song.released.empty() ? std::wstring() : DateText(song.released));
        add(L"Genre", ListText(song.genres));
        add(L"Tags", ListText(song.tags));
        add(L"Language", LanguageText(song.language));
        add(L"BPM", song.bpm > 0 ? std::to_wstring(song.bpm) : L"");
        add(L"Version", song.version.empty() ? std::wstring() : Tr(song.version));
        add(L"Audio", ListText(traits));
        add(L"Lyrics", song.lyrics.empty() ? std::wstring() : Tr(song.lyrics));
        if (song.credits.empty()) add(L"Composer", song.composer);
        add(L"Also known as", ListText(song.aliases));
        add(L"ISRC", song.isrc);
        return facts;
    }

    // A language tag ("zh-Hans-CN") as the system names the language; other text as it is.
    static std::wstring LanguageText(const std::wstring& language) {
        if (language.find(L'-') == std::wstring::npos) return language;
        wchar_t name[LOCALE_NAME_MAX_LENGTH * 2]{};
        if (GetLocaleInfoEx(language.c_str(), LOCALE_SLOCALIZEDLANGUAGENAME, name, static_cast<int>(std::size(name))))
            return name;
        return language;
    }

    stream::Provider& m_provider;
    std::optional<stream::SongDetails> m_details;
    std::wstring m_error;
};

std::unique_ptr<Page> MakeRemotePage(PageHost& host, stream::Provider& provider, const View& view) {
    switch (view.kind) {
    case View::Kind::RemoteFavorites:
        return std::make_unique<FavoritesPage>(host, provider, view);
    case View::Kind::RemoteShelf:
        return std::make_unique<ShelfPage>(host, provider, view);
    case View::Kind::RemotePlaylist:
        return std::make_unique<PlaylistPage>(host, provider, view);
    case View::Kind::RemoteArtists:
        return std::make_unique<ArtistsPage>(host, provider, view);
    case View::Kind::RemoteAlbums:
        return std::make_unique<SavedAlbumsPage>(host, provider, view);
    case View::Kind::RemoteArtist:
        return std::make_unique<ArtistPage>(host, provider, view);
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
    case View::Kind::RemoteSong:
        return std::make_unique<SongPage>(host, provider, view);
    case View::Kind::RemoteHomeList:
        return std::make_unique<HomeListPage>(host, provider, view);
    default:
        return nullptr;
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

// ---- playlist commands --------------------------------------------------------------------------

void ShowRemotePlaylistMenu(PageHost& host, stream::Provider& provider, const stream::Id& id,
                            const std::wstring& name, std::optional<Shelf> shelf, const stream::Id& sectionId,
                            POINT screen, std::function<void(const stream::Id&)> onUnsaved) {
    stream::Playlist playlist;
    playlist.id = id;
    playlist.name = name;
    RecordMenuContext context;
    context.sectionId = sectionId;
    context.shelf = shelf;
    context.removed = std::move(onUnsaved);
    if (shelf != Shelf::Recommended) context.page.push_back({L"Refresh", [&host] { host.Refresh(); }});
    ShowRecordMenu(host, provider, playlist, screen, context);
}

void CreateRemotePlaylist(PageHost& host, stream::Provider& provider) {
    if (!provider.Can(stream::Capability::CreatePlaylists)) return;
    if (!provider.AccountVerified()) {
        if (!provider.SessionRestoring()) provider.SignIn(host.Owner());
        return;
    }
    const std::wstring title = provider.NamedText(L"New {0} playlist");
    const stream::NavigationSection section = CreatedPlaylistSection(provider);
    PromptText(host.Owner(), title, ProviderPlaylistText(provider, L"Playlist name"), title,
               [&host, &provider, section](std::wstring name) {
                   if (name.empty()) return;
                   provider.CreatePlaylist(name, [&host, &provider, name, section, alive = host.Lifetime()](
                                                     stream::Id id, std::wstring error) {
                       if (!error.empty()) return ShowProviderError(provider, error);
                       if (!alive.expired())
                           host.Open(PlaylistView(provider, id, name, Shelf::Created, section.id));
                   });
               });
}

void RenameRemotePlaylist(PageHost& host, stream::Provider& provider, const stream::Id& id,
                          std::wstring name) {
    if (!provider.Can(stream::Capability::RenamePlaylists)) return;
    if (!provider.AccountVerified()) return;
    // The provider reports the new name as a library change; pages update from there.
    PromptText(host.Owner(), ProviderPlaylistText(provider, L"Rename playlist"),
               ProviderPlaylistText(provider, L"Playlist name"), name,
               [&provider, id, name](std::wstring newName) {
                   if (newName.empty() || newName == name) return;
                   provider.RenamePlaylist(id, newName, [&provider](std::wstring error) {
                       if (!error.empty()) ShowProviderError(provider, error);
                   });
               });
}

void DeleteRemotePlaylist(PageHost& host, stream::Provider& provider, const stream::Id& id, std::wstring name,
                          const stream::Id& sectionId) {
    if (!provider.Can(stream::Capability::DeletePlaylists)) return;
    if (!provider.AccountVerified()) return;
    const std::wstring deleteText = ProviderPlaylistText(provider, L"Delete playlist");
    const std::wstring message = deleteText + L" \"" + name + L"\"?\n" + Tr(L"This cannot be undone.");
    if (MessageBoxW(host.Owner(), message.c_str(), deleteText.c_str(),
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    stream::NavigationSection section = CreatedPlaylistSection(provider);
    if (const auto current = FindSection(provider, sectionId);
        current && current->kind == stream::SectionKind::PlaylistShelf)
        section = *current;
    provider.DeletePlaylist(id, [&host, &provider, id, section, alive = host.Lifetime()](std::wstring error) {
        if (!error.empty()) return ShowProviderError(provider, error);
        if (alive.expired()) return;
        const View& current = host.CurrentView();
        if (current.kind == View::Kind::RemotePlaylist && current.provider == provider.Key() &&
            current.remoteId == id)
            host.Replace(ShelfView(provider, section.shelf, section.id, section.label));
    });
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
