#pragma once

// What the streaming pages' source files share (ui/remote_pages*.cpp and ui/record_menu.cpp):
// text and record helpers, the list engines that read a whole list (AllSongsRequest,
// PagedPlaybackFeed), the queries, and the base pages (RemotePage, PagedPage, TrackPagedPage).

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
#include "ui/win32.h"
#include <cstdint>
#include <cwctype>
#include <unordered_set>

namespace glasswave::ui::remotepages {


using stream::Shelf;

inline std::wstring Joined(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (const auto& part : parts) {
        if (part.empty()) continue;
        if (!out.empty()) out += L" · ";
        out += part;
    }
    return out;
}

inline std::wstring Count(int n, const wchar_t* unit) {
    return n > 0 ? std::to_wstring(n) + L" " + Tr(unit) : std::wstring();
}

// The service gave a page's subject another picture than the one known before. A first
// address is no change: invalidating then threw away the load the page had just started, so
// the cover came only after a retry (owner, 2026-11-14).
inline bool CoverChanged(const stream::Provider& provider, const std::wstring& before, const std::wstring& now) {
    return !before.empty() && !now.empty() && !provider.SameCover(before, now);
}

// A list of values in one fact: "a, b" or, in Chinese, "a、b".
inline std::wstring ListText(const std::vector<std::wstring>& values) {
    std::wstring out;
    for (const auto& value : values) {
        if (value.empty()) continue;
        if (!out.empty()) out += UsingChinese() ? L"、" : L", ";
        out += value;
    }
    return out;
}

// A number with the UI language's digit grouping (4,888,171).
inline std::wstring NumberText(long long n) {
    std::wstring digits = std::to_wstring(n < 0 ? -n : n), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += L',';
        out += digits[i];
    }
    return n < 0 ? L"-" + out : out;
}

// A date as a service writes it ("2017", "1979-01-18", "January 18, 1979") in the UI
// language's long form; text it cannot read is shown as it is.
inline std::wstring DateText(const std::wstring& text) {
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

// A subject's details as details-pane facts, in the order added (most telling first). Labels
// are UI keys; a fact whose value the source does not know is left out.
class FactList {
  public:
    // The value as it is.
    FactList& Add(const wchar_t* label, std::wstring value) {
        if (!value.empty()) m_facts.push_back({Tr(label), std::move(value)});
        return *this;
    }
    // A UI key (a type, an area the service names in English), translated.
    FactList& Key(const wchar_t* label, const std::wstring& key) {
        return Add(label, key.empty() ? std::wstring() : Tr(key));
    }
    // A date as the service writes it (DateText).
    FactList& Date(const wchar_t* label, const std::wstring& text) {
        return Add(label, text.empty() ? std::wstring() : DateText(text));
    }
    // Several values in one fact (ListText); `keys`: each value is a UI key, translated.
    FactList& List(const wchar_t* label, const std::vector<std::wstring>& values, bool keys = false) {
        if (!keys) return Add(label, ListText(values));
        std::vector<std::wstring> translated;
        for (const auto& value : values) translated.push_back(Tr(value));
        return Add(label, ListText(translated));
    }
    std::vector<DetailInfo::Fact> Take() { return std::move(m_facts); }

  private:
    std::vector<DetailInfo::Fact> m_facts;
};

// An artist's profile as details-pane facts.
inline std::vector<DetailInfo::Fact> ArtistFacts(const stream::Artist& artist) {
    using Kind = stream::ArtistKind;
    const wchar_t* const type = artist.kind == Kind::Group    ? L"Band or group"
                                : artist.kind == Kind::Male   ? L"Male artist"
                                : artist.kind == Kind::Female ? L"Female artist"
                                : artist.kind == Kind::Person ? L"Solo artist"
                                                              : L"";
    return FactList()
        .Date(artist.kind == Kind::Group ? L"Formed" : artist.kind == Kind::Unknown ? L"Born or formed" : L"Born",
              artist.born)
        .Add(L"From", artist.origin)
        .Key(L"Area", artist.area)
        .Key(L"Type", type)
        .List(L"Genre", artist.genres)
        .List(L"Also known as", artist.aliases)
        .List(L"Roles", artist.roles)
        .Add(L"Music videos", artist.videoCount > 0 ? NumberText(artist.videoCount) : std::wstring())
        .Take();
}

// A playlist's details as details-pane facts.
inline std::vector<DetailInfo::Fact> PlaylistFacts(const stream::Playlist& playlist) {
    return FactList()
        .Key(L"Type", playlist.type)
        .List(L"Tags", playlist.tags)
        .Date(L"Created", playlist.created)
        .Date(L"Updated", playlist.updated)
        .Add(L"Updates", playlist.updates)
        .List(L"Audio", playlist.traits, true)
        .List(L"Also known as", playlist.aliases)
        .Take();
}

// An album's details as details-pane facts.
inline std::vector<DetailInfo::Fact> AlbumFacts(const stream::Album& album) {
    return FactList()
        .Key(L"Type", album.type)
        .Add(L"Edition", album.edition)
        .Date(L"Released", album.released)
        .List(L"Genre", album.genres)
        .Add(L"Label", album.label)
        .List(L"Audio", album.traits, true)
        .List(L"Also known as", album.aliases)
        .Add(L"Copyright", album.copyright)
        .Add(L"UPC", album.upc)
        .Take();
}

inline bool Contains(const std::vector<stream::Album>& albums, const stream::Id& id) {
    return std::any_of(albums.begin(), albums.end(), [&id](const stream::Album& a) { return a.id == id; });
}

inline bool Contains(const std::vector<stream::Artist>& artists, const stream::Id& id) {
    return std::any_of(artists.begin(), artists.end(), [&id](const stream::Artist& a) { return a.id == id; });
}

inline bool Contains(const std::vector<stream::Playlist>& playlists, const stream::Id& id) {
    return std::any_of(playlists.begin(), playlists.end(),
                       [&id](const stream::Playlist& p) { return p.id == id; });
}

inline bool IsSavedAlbum(stream::Provider& provider, const stream::Id& id) {
    return provider.IsAlbumSaved(id);
}
inline bool IsFollowed(stream::Provider& provider, const stream::Id& id) {
    return Contains(provider.FollowedArtists(), id);
}
inline bool SupportsArtistPreference(const stream::Provider& provider) {
    return provider.Can(stream::Capability::FollowArtists) ||
           provider.Can(stream::Capability::FavoriteArtists);
}

inline std::optional<stream::NavigationSection> FindSection(const stream::Provider& provider, const stream::Id& id) {
    const auto sections = provider.Sections();
    const auto found = std::find_if(sections.begin(), sections.end(),
                                    [&id](const auto& section) { return section.id == id; });
    return found == sections.end() ? std::nullopt : std::optional<stream::NavigationSection>(*found);
}

inline std::optional<stream::NavigationSection> FindSection(const stream::Provider& provider,
                                                     stream::SectionKind kind,
                                                     std::optional<stream::Shelf> shelf = std::nullopt) {
    const auto sections = provider.Sections();
    const auto found = std::find_if(sections.begin(), sections.end(), [kind, shelf](const auto& section) {
        return section.kind == kind &&
               (!shelf || (section.kind == stream::SectionKind::PlaylistShelf && section.shelf == *shelf));
    });
    return found == sections.end() ? std::nullopt : std::optional<stream::NavigationSection>(*found);
}

inline stream::NavigationSection CreatedPlaylistSection(const stream::Provider& provider) {
    if (const auto section =
            FindSection(provider, stream::SectionKind::PlaylistShelf, stream::Shelf::Created))
        return *section;
    return {L"created", L"Created playlists", stream::SectionKind::PlaylistShelf, stream::Shelf::Created,
            false};
}

inline std::wstring ProviderPlaylistText(const stream::Provider& provider, std::wstring_view english) {
    return provider.Text(english);
}

// A list is visibly loading only while it has nothing to show. Re-reading behind rows that
// are already on screen (a cached snapshot, a refresh) stays silent and updates in place.
// Counts one record of an artist has and the next lacks stay (the album list's record has
// the album count, the profile read after it may not: the stats line came and went).
inline void KeepCounts(const stream::Artist& from, stream::Artist& to) {
    if (to.albumCount < 0) to.albumCount = from.albumCount;
    if (to.trackCount < 0) to.trackCount = from.trackCount;
    if (to.videoCount < 0) to.videoCount = from.videoCount;
}

inline bool VisiblyLoading(const stream::ListState& state) {
    if (!state.loading && !state.refreshing) return false;
    return std::visit([](const auto& rows) { return rows.empty(); }, state.page.rows);
}

// The band over a page's list (ListSource::SetLoadingText): "Loading..." while the page reads
// what it must show, else the error that stopped a read, else nothing.
inline std::wstring LoadingStatus(bool loading, const std::wstring& error = {}) {
    if (loading) return Tr(L"Loading...");
    return error.empty() ? std::wstring() : Tr(error);
}

inline std::wstring LoadingArtistTracksLabel(const stream::Provider& provider, const stream::Id& artist) {
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

inline stream::ListQuery Query(stream::ListKind kind) {
    stream::ListQuery query;
    query.kind = kind;
    return query;
}

inline stream::ListQuery PreparedQuery(stream::Provider& provider, stream::ListQuery query,
                                bool catalogSearch = false) {
    query.cache = catalogSearch ? stream::ListCachePolicy::None : provider.ListPolicy(query);
    return query;
}

inline metadb_handle_list Items(stream::Provider& provider, const std::vector<stream::Track>& tracks,
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

inline std::vector<std::shared_ptr<AllSongsRequest>>& ActiveSongsRequests() {
    static std::vector<std::shared_ptr<AllSongsRequest>> requests;
    return requests;
}

inline void FinishAllSongsRequest(const std::shared_ptr<AllSongsRequest>& request) {
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

inline void CancelAllSongsRequests() {
    auto requests = std::move(ActiveSongsRequests());
    ActiveSongsRequests().clear();
    for (auto& request : requests)
        FinishAllSongsRequest(request);
}

inline void CancelSongsRequests(std::wstring_view provider) {
    std::vector<std::shared_ptr<AllSongsRequest>> requests;
    std::erase_if(ActiveSongsRequests(), [&](const auto& request) {
        if (request->provider->Key() != provider) return false;
        requests.push_back(request);
        return true;
    });
    for (auto& request : requests)
        FinishAllSongsRequest(request);
}

inline void BeginAllSongs(const std::shared_ptr<AllSongsRequest>& request) {
    if (!request->host || request->alive.expired()) return;
    request->badge.Begin(*request->host, request->host->Source());
}

inline void EndAllSongs(const std::shared_ptr<AllSongsRequest>& request) {
    request->badge.End();
}

// A whole list is taken once its read has settled. The read is shared with the list's page, so
// what plays is what the page shows; a cache the service is being asked about again is not
// taken, the rows that answer brings are (owner, 2026-11-20: the queue began with a song the page
// did not show yet).
inline void SettleAllSongs(const std::shared_ptr<AllSongsRequest>& request) {
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
inline void CompleteAllSongs(const std::shared_ptr<AllSongsRequest>& request, const stream::ListState& state) {
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
inline void StartAllSongs(const std::shared_ptr<AllSongsRequest>& request, const stream::ListQuery& query) {
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

inline SongsFetch FetchListSongs(stream::Provider& provider, stream::ListQuery query, PageHost* host = nullptr) {
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

inline SongsFetch FetchPlaylistSongs(stream::Provider& provider, const stream::Id& id, PageHost* host = nullptr) {
    auto query = Query(stream::ListKind::PlaylistTracks);
    query.id = id;
    return FetchListSongs(provider, std::move(query), host);
}

inline SongsFetch FetchAlbumSongs(stream::Provider& provider, const stream::Id& id, PageHost* host = nullptr) {
    auto query = Query(stream::ListKind::AlbumTracks);
    query.id = id;
    return FetchListSongs(provider, std::move(query), host);
}

inline std::vector<std::wstring> FeedTrackIds(const std::vector<stream::Track>& tracks, size_t count = SIZE_MAX) {
    std::vector<std::wstring> ids;
    count = std::min(count, tracks.size());
    ids.reserve(count);
    for (size_t i = 0; i < count; ++i)
        ids.push_back(tracks[i].id);
    return ids;
}

inline bool FeedIdsMatch(const std::vector<std::wstring>& ids, const std::vector<stream::Track>& tracks) {
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

inline stream::ListQuery PlaylistQuery(stream::Provider& provider, const stream::Id& id) {
    (void)provider;
    auto query = Query(stream::ListKind::PlaylistTracks);
    query.id = id;
    return query;
}

inline stream::ListQuery AlbumQuery(stream::Provider& provider, const stream::Id& id) {
    (void)provider;
    auto query = Query(stream::ListKind::AlbumTracks);
    query.id = id;
    return query;
}

inline stream::ListQuery IdQuery(stream::ListKind kind, const stream::Id& id) {
    auto query = Query(kind);
    query.id = id;
    return query;
}

inline stream::ListQuery GroupsQuery(const stream::Id& section) {
    auto query = Query(stream::ListKind::Groups);
    query.section = section;
    return query;
}

inline stream::ListQuery SectionTracksQuery(const stream::Id& section, const stream::Id& group) {
    auto query = Query(stream::ListKind::SectionTracks);
    query.id = group;
    query.section = section;
    return query;
}

inline stream::ListQuery PlaylistsQuery(stream::Shelf shelf, const stream::Id& section) {
    auto query = Query(stream::ListKind::Playlists);
    query.shelf = shelf;
    query.section = section;
    return query;
}

inline stream::ListQuery ArtistAlbumsQuery(const stream::Id& artist) {
    auto query = Query(stream::ListKind::ArtistAlbums);
    query.id = artist;
    return query;
}

inline stream::ListQuery SavedAlbumsQuery() {
    return Query(stream::ListKind::SavedAlbums);
}

inline std::unique_ptr<RemoteSongSource> Songs(stream::Provider& provider, stream::ListKind kind,
                                        const std::vector<stream::Track>& tracks, std::wstring empty,
                                        size_t count = SIZE_MAX) {
    return std::make_unique<RemoteSongSource>(provider, kind, tracks, std::move(empty), count);
}

inline bool MatchesPublishedPrefix(RemoteSongSource& source, stream::Provider& provider,
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
inline MenuAction SaveAlbumAction(stream::Provider& provider, const stream::Id& id,
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

inline MenuAction FollowAction(stream::Provider& provider, const stream::Id& id,
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

// Whether the account made a playlist (it may rename and delete it) or collected it, from
// every place that knows: the provider's pending edit, the shelf the playlist is on or was
// opened from, and its record.
struct PlaylistFlags {
    bool own = false, saved = false;
};
inline PlaylistFlags ResolvePlaylistFlags(stream::Provider& provider, const stream::Id& id, std::optional<Shelf> from,
                                   const stream::Playlist* record) {
    const auto shelf = provider.ShelfOf(id);
    PlaylistFlags flags;
    flags.own = from == Shelf::Created || shelf == Shelf::Created || (record && record->own);
    flags.saved = provider.PlaylistSavedState(id).value_or(from == Shelf::Saved || shelf == Shelf::Saved ||
                                                           (record && record->saved) ||
                                                           Contains(provider.Playlists(Shelf::Saved), id));
    return flags;
}

// The account's commands for a playlist: Rename and Delete for its own, Collect or Uncollect
// for another's (`toggleSaved` makes that change). None without a verified account.
inline std::vector<MenuAction> PlaylistAccountActions(PageHost& host, stream::Provider& provider, const stream::Id& id,
                                               const std::wstring& name, const stream::Id& sectionId,
                                               PlaylistFlags flags, std::function<void()> toggleSaved) {
    std::vector<MenuAction> actions;
    if (!provider.AccountVerified()) return actions;
    if (flags.own) {
        const auto offers = provider.Offers();
        if (offers.Has(stream::Capability::RenamePlaylists))
            actions.push_back({ProviderPlaylistText(provider, L"Rename..."),
                               [&host, &provider, id, name] { RenameRemotePlaylist(host, provider, id, name); }});
        if (offers.Has(stream::Capability::DeletePlaylists))
            actions.push_back({ProviderPlaylistText(provider, L"Delete playlist"),
                               [&host, &provider, id, name, sectionId] {
                                   DeleteRemotePlaylist(host, provider, id, name, sectionId);
                               }});
    } else if (provider.Can(stream::Capability::SavePlaylists)) {
        actions.push_back({ProviderPlaylistText(provider, flags.saved ? L"Uncollect playlist" : L"Collect playlist"),
                           std::move(toggleSaved)});
    }
    return actions;
}

// "Go to artist" for an album's credited artists (a submenu when there are several).
inline void AddGoToArtist(std::vector<MenuAction>& actions, PageHost& host, stream::Provider& provider,
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
            AppendSeparator(actions);
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

    // The whole list is wanted (a presentation that needs it, or a drain) and still being read.
    bool Draining() const { return (m_allMode || m_drainRequested) && !m_state.complete && m_state.error.empty(); }

    virtual std::wstring StatusText() const {
        return LoadingStatus((m_session && VisiblyLoading(CurrentListState())) || Draining(), m_state.error);
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
        m_drain.Wait(std::move(done));
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
        m_drainRequested = false;
        m_session->SetMode(m_allMode ? stream::ListLoadMode::All : stream::ListLoadMode::OnDemand);
        m_drain.Settle(PlaybackReady());
    }

    ReadAllSongs ReadAll() {
        return [this](SongDrain::Waiter done) { EnsureAllSongs(std::move(done)); };
    }
    SongsFetch AllSongsFetch() { return AllSongs(ReadAll()); }

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
        PlayWhenRead(ReadAll(), identity); // a failed read plays what there is
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
    SongDrain m_drain;

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
        if (!m_state.error.empty()) return LoadingStatus(false, m_state.error); // before any loading
        auto* source = SongSource();
        const auto& tracks = CurrentRows<stream::Track>();
        const bool metadataPending = source && source->NeedsAlbumIdentity() &&
                                     UsableTrackCount(CurrentListState(), source) < tracks.size();
        const bool hasRows = source && source->Count() != 0;
        return LoadingStatus(metadataPending || (!hasRows && (m_state.loading || m_state.refreshing)) || Draining());
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

// The pages of each source file, for MakeRemotePage; nullptr for a kind it does not make.
std::unique_ptr<Page> MakeListPage(PageHost& host, stream::Provider& provider, const View& view);
std::unique_ptr<Page> MakeArtistPage(PageHost& host, stream::Provider& provider, const View& view);
std::unique_ptr<Page> MakeSongPage(PageHost& host, stream::Provider& provider, const View& view);

} // namespace glasswave::ui::remotepages
