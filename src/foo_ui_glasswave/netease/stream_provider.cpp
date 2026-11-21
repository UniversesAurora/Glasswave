#include "pch.h"
#include "netease/stream_provider.h"
#include "core/text.h"
#include "cache/provider_snapshot.h"
#include "cache/store.h"
#include "stream/playback_source.h"
#include "netease/catalog.h"
#include "netease/config.h"
#include "netease/session.h"
#include "ui/i18n.h"
#include "ui/netease_login.h"

#include <ctime>
#include <array>
#include <future>
#include <unordered_set>
#include <limits>
#include <list>
#include <unordered_map>

namespace glasswave::netease {

namespace {

using stream::Id;

Session& Ncm() {
    return Session::Get();
}

using core::LowerAscii;
using core::Utf8;
using core::Wide;

std::string SnapshotContext() {
    const std::array<std::string_view, 1> parts{"ncm-bootstrap-v1"};
    return cache::provider_snapshot::Context(parts);
}

std::string SourceIdentity() {
    const std::wstring external = GetServiceUrlOverride();
    return external.empty() ? "managed:ncm" : "external:" + Utf8(external);
}

std::vector<int64_t> Nums(const std::vector<Id>& ids) {
    std::vector<int64_t> out;
    out.reserve(ids.size());
    for (const auto& id : ids)
        if (const int64_t n = Num(id)) out.push_back(n);
    return out;
}

// Records of a client's answer, in the signed-in account's context: whether a playlist is the
// account's own or saved, and which artists it follows.
stream::Playlist AccountPlaylist(const Playlist& playlist) {
    return ToPlaylist(playlist, Ncm().CurrentAccount().userId, Ncm().FindPlaylistScope(playlist.id));
}

std::vector<stream::Playlist> AccountPlaylists(const std::vector<Playlist>& playlists) {
    std::vector<stream::Playlist> out;
    out.reserve(playlists.size());
    for (const auto& playlist : playlists)
        out.push_back(AccountPlaylist(playlist));
    return out;
}

stream::Artist AccountArtist(const Artist& artist, const std::vector<stream::Artist>& followed) {
    stream::Artist out = ToArtist(artist);
    MarkFollowed(out, followed);
    return out;
}

std::vector<stream::Artist> AccountArtists(const std::vector<Artist>& artists) {
    std::vector<stream::Artist> out;
    out.reserve(artists.size());
    const auto followed = Ncm().FollowedArtists(); // one copy, not one per artist
    for (const auto& artist : artists)
        out.push_back(AccountArtist(artist, followed));
    return out;
}

std::wstring ErrorOf(std::wstring error) {
    return error.empty() ? std::wstring(L"The request failed.") : std::move(error);
}

bool ListCancelled(const std::shared_ptr<std::atomic_bool>& cancelled) {
    return cancelled && cancelled->load(std::memory_order_acquire);
}

bool ToInt(size_t value, int& out, std::wstring& error) {
    if (value > static_cast<size_t>(std::numeric_limits<int>::max())) {
        error = L"The requested list page is too large.";
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

// The first row and the row count of an offset-paged request, in the service client's ints.
struct OffsetWindow {
    int offset = 0;
    int count = 0;
};

bool OffsetWindowOf(const stream::ListCursor& cursor, size_t limit, OffsetWindow& window,
                    std::wstring& error) {
    return ToInt(cursor.offset, window.offset, error) && ToInt(limit, window.count, error);
}

std::wstring IdsRevision(uint64_t accountRevision, const std::vector<int64_t>& ids,
                         int64_t sourceVersion = 0) {
    uint64_t hash = 1469598103934665603ull;
    for (const int64_t id : ids) {
        uint64_t value = static_cast<uint64_t>(id);
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= value & 0xff;
            hash *= 1099511628211ull;
            value >>= 8;
        }
    }
    wchar_t digest[17];
    swprintf_s(digest, L"%016llx", static_cast<unsigned long long>(hash));
    return L"ids2:" + std::to_wstring(accountRevision) + L":" + std::to_wstring(sourceVersion) + L":" +
           std::to_wstring(ids.size()) + L":" + digest;
}

std::vector<Song> OrderDetails(const std::vector<int64_t>& ids, const std::vector<Song>& details) {
    std::unordered_map<int64_t, size_t> byId;
    byId.reserve(details.size());
    for (size_t i = 0; i < details.size(); ++i)
        if (details[i].id) byId.emplace(details[i].id, i);
    std::vector<Song> ordered;
    ordered.reserve(ids.size());
    for (const int64_t id : ids) {
        const auto found = byId.find(id);
        if (found != byId.end()) ordered.push_back(details[found->second]);
    }
    return ordered;
}

std::vector<int64_t> Unique(const std::vector<int64_t>& ids) {
    std::vector<int64_t> unique;
    std::unordered_set<int64_t> seen;
    unique.reserve(ids.size());
    seen.reserve(ids.size());
    for (const int64_t id : ids)
        if (id && seen.insert(id).second) unique.push_back(id);
    return unique;
}

constexpr size_t kSnapshotPageLimit = 1000; // rows of a playlist / favorites page at most

// The details of `ids` (unique), asked for in batches of 200 that run side by side: a large
// page of a playlist costs about one request's time instead of one per batch.
bool SongDetailsInBatches(Client& client, const std::vector<int64_t>& ids, abort_callback& abort,
                          std::vector<Song>& out, std::wstring& error) {
    constexpr size_t kBatch = 200;
    out.clear();
    if (ids.size() <= kBatch) return ids.empty() || client.SongDetails(ids, abort, out, error);
    struct Batch {
        std::vector<int64_t> ids;
        std::vector<Song> songs;
        std::wstring error;
        bool ok = false;
    };
    std::vector<Batch> batches;
    for (size_t start = 0; start < ids.size(); start += kBatch)
        batches.push_back({std::vector<int64_t>(ids.begin() + static_cast<std::ptrdiff_t>(start),
                                                ids.begin() + static_cast<std::ptrdiff_t>(
                                                                  std::min(start + kBatch, ids.size())))});
    std::vector<std::future<void>> running;
    running.reserve(batches.size() - 1);
    for (size_t i = 1; i < batches.size(); ++i)
        running.push_back(std::async(std::launch::async, [&client, &abort, &batch = batches[i]] {
            batch.ok = client.SongDetails(batch.ids, abort, batch.songs, batch.error);
        }));
    batches[0].ok = client.SongDetails(batches[0].ids, abort, batches[0].songs, batches[0].error);
    for (auto& task : running)
        task.wait();
    for (auto& batch : batches) {
        if (!batch.ok) {
            error = std::move(batch.error);
            out.clear();
            return false;
        }
        out.insert(out.end(), std::make_move_iterator(batch.songs.begin()),
                   std::make_move_iterator(batch.songs.end()));
    }
    return true;
}

// Registered while the component loads, so the UI finds it however early it is created.
// Started by the UI or at foobar2000 start-up on the main thread, whichever comes first, so
// ncm:// items also play with another UI; stopped before the component is unloaded (no
// joins in static destructors).
const bool g_registered = [] {
    stream::Register(StreamProvider::Get());
    return true;
}();

class StreamProviderInit : public initquit {
  public:
    void on_init() override { StreamProvider::Get().Start(); }
    void on_quit() override { StreamProvider::Get().Shutdown(); }
};
FB2K_SERVICE_FACTORY(StreamProviderInit);

} // namespace

// Full ID lists are immutable generations shared by every page of one cursor. The LRU is
// deliberately process-local: a cold continuation refetches and validates the revision.
struct StreamProvider::IdSnapshotStore {
    enum class Kind {
        Playlist,
        Favorites
    };

    struct Key {
        uint64_t accountRevision = 0;
        Kind kind = Kind::Playlist;
        int64_t listId = 0;
        std::wstring revision;

        bool operator==(const Key&) const = default;
    };

    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            size_t value = std::hash<uint64_t>{}(key.accountRevision);
            value ^= std::hash<int>{}(static_cast<int>(key.kind)) + 0x9e3779b9u + (value << 6) + (value >> 2);
            value ^= std::hash<int64_t>{}(key.listId) + 0x9e3779b9u + (value << 6) + (value >> 2);
            value ^= std::hash<std::wstring>{}(key.revision) + 0x9e3779b9u + (value << 6) + (value >> 2);
            return value;
        }
    };

    struct Snapshot {
        Key key;
        std::vector<int64_t> ids;
        Playlist info;
        size_t estimatedBytes = 0;
    };

    static constexpr size_t kMaxBytes = 16u * 1024u * 1024u;
    static constexpr size_t kMaxSnapshots = 32;

    std::shared_ptr<const Snapshot> Find(const Key& key) {
        std::lock_guard lock(m_mutex);
        const auto found = m_index.find(key);
        if (found == m_index.end()) return {};
        m_order.splice(m_order.begin(), m_order, found->second);
        return *found->second;
    }

    std::shared_ptr<const Snapshot> Put(Key key, std::vector<int64_t> ids, Playlist info = {}) {
        const std::wstring& revision = key.revision;
        size_t estimate = sizeof(Snapshot) + ids.capacity() * sizeof(int64_t) + 512;
        // The revision string is held once by the immutable snapshot and once by the index.
        estimate += 2 * revision.capacity() * sizeof(wchar_t);
        const auto addString = [&estimate](const std::wstring& value) {
            estimate += value.capacity() * sizeof(wchar_t);
        };
        addString(info.name);
        addString(info.coverUrl);
        addString(info.creatorName);
        addString(info.description);
        auto snapshot = std::make_shared<const Snapshot>(
            Snapshot{std::move(key), std::move(ids), std::move(info), estimate});

        std::lock_guard lock(m_mutex);
        EraseResourceLocked(snapshot->key);
        if (snapshot->estimatedBytes > kMaxBytes) return snapshot;

        m_order.push_front(snapshot);
        m_index.emplace(snapshot->key, m_order.begin());
        m_bytes += snapshot->estimatedBytes;
        while (m_order.size() > kMaxSnapshots || m_bytes > kMaxBytes) {
            auto oldest = std::prev(m_order.end());
            if (*oldest == snapshot) {
                // A new, individually valid snapshot must still obey the byte budget. Keeping
                // it here when no older entries remain would exceed the cache bound.
                m_index.erase((*oldest)->key);
                m_bytes -= (*oldest)->estimatedBytes;
                m_order.pop_back();
                break;
            }
            const auto erased = m_index.find((*oldest)->key);
            if (erased != m_index.end()) EraseLocked(erased);
        }
        return snapshot;
    }

    void Clear() {
        std::lock_guard lock(m_mutex);
        m_index.clear();
        m_order.clear();
        m_bytes = 0;
    }

    // What the two id-paged lists say when a continuation does not fit its snapshot.
    struct Wording {
        const wchar_t* noRevision;
        const wchar_t* changed;
        const wchar_t* notRetained;
        const wchar_t* outside;
    };
    static constexpr Wording kPlaylistWording{
        L"The playlist cursor has no snapshot revision.",
        L"The playlist changed while its pages were being loaded. Reload the list.",
        L"The playlist snapshot could not be retained.",
        L"The playlist cursor is outside the current track list."};
    static constexpr Wording kFavoritesWording{
        L"The favorites cursor has no snapshot revision.",
        L"Favorites changed while their pages were being loaded. Reload the list.",
        L"The favorites snapshot could not be retained.",
        L"The favorites cursor is outside the current track list."};

    // One page of a list the service gives as a single id list (a playlist's tracks, the
    // favorites). The first page reads the ids with `fetchIds(ids, info, revision, error)` and
    // keeps them under their revision; the pages after it are cut from the same snapshot, so the
    // order cannot shift between pages, and a revision that no longer matches is an error. The
    // page's rows are the songs of its slice of ids, read in batches. Worker thread; `snapshot`
    // is the list's id list afterwards, for the caller's own page fields.
    template <class FetchIds>
    bool PageFromIdSnapshot(Key key, const Wording& wording, const stream::ListCursor& cursor, size_t limit,
                            Client& client, abort_callback& abort, FetchIds&& fetchIds,
                            stream::ListPage& page, std::vector<SongRecord>& songs,
                            std::shared_ptr<const Snapshot>& snapshot, std::wstring& error) {
        if (cursor.offset != 0 && cursor.value.empty()) {
            error = wording.noRevision;
            return false;
        }
        key.revision = cursor.value;
        if (cursor.offset != 0) snapshot = Find(key);
        if (!snapshot) {
            std::vector<int64_t> ids;
            Playlist info;
            std::wstring revision;
            if (!fetchIds(ids, info, revision, error)) return false;
            key.revision = std::move(revision);
            if (cursor.offset != 0 && cursor.value != key.revision) {
                error = wording.changed;
                return false;
            }
            snapshot = Put(std::move(key), std::move(ids), std::move(info));
        }
        if (!snapshot) {
            error = wording.notRetained;
            return false;
        }
        if (cursor.offset > snapshot->ids.size()) {
            error = wording.outside;
            return false;
        }
        const size_t count = std::min({limit, snapshot->ids.size() - cursor.offset, kSnapshotPageLimit});
        const auto begin = snapshot->ids.begin() + static_cast<std::ptrdiff_t>(cursor.offset);
        std::vector<int64_t> ids(begin, begin + static_cast<std::ptrdiff_t>(count));
        std::vector<Song> details;
        if (!SongDetailsInBatches(client, Unique(ids), abort, details, error)) return false;
        songs = ToSongs(OrderDetails(ids, details));
        page.rows = TracksOf(songs);
        page.total = snapshot->ids.size();
        page.next = {snapshot->key.revision, cursor.offset + count};
        page.more = cursor.offset + count < snapshot->ids.size();
        page.revision = snapshot->key.revision;
        return true;
    }

  private:
    using Iterator = std::list<std::shared_ptr<const Snapshot>>::iterator;

    void EraseResourceLocked(const Key& key) {
        for (auto it = m_order.begin(); it != m_order.end();) {
            const auto& current = (*it)->key;
            if (current.accountRevision != key.accountRevision || current.kind != key.kind ||
                current.listId != key.listId) {
                ++it;
                continue;
            }
            m_bytes -= (*it)->estimatedBytes;
            m_index.erase(current);
            it = m_order.erase(it);
        }
    }

    void EraseLocked(std::unordered_map<Key, Iterator, KeyHash>::iterator found) {
        const auto snapshot = *found->second;
        m_bytes -= snapshot->estimatedBytes;
        m_order.erase(found->second);
        m_index.erase(found);
    }

    std::mutex m_mutex;
    size_t m_bytes = 0;
    std::list<std::shared_ptr<const Snapshot>> m_order;
    std::unordered_map<Key, Iterator, KeyHash> m_index;
};

namespace {

// The executor's only queue: reads, home and station calls, and the snapshot save.
constexpr size_t kReadQueue = 0;

} // namespace

StreamProvider::StreamProvider()
    : m_snapshots("ncm", m_exec, kReadQueue,
                  {[this] { return SnapshotEligible(); },
                   [this](cache::provider_snapshot::Snapshot& payload) { return FillSnapshot(payload); }}) {}

StreamProvider& StreamProvider::Get() {
    static StreamProvider provider;
    return provider;
}

void StreamProvider::Start() {
    if (m_exec.Running()) return;
    Ncm().Initialize();
    m_exec.Start({1});
    m_idSnapshots = std::make_shared<IdSnapshotStore>();
    {
        std::lock_guard lock(m_cacheMutex);
        m_signedIn = Ncm().LoggedIn();
        ResetSessionLocked(m_signedIn);
    }
    // The session reports sign-in changes and account refreshes the same way; only a
    // changed sign-in state is an account change, the rest refreshes library lists.
    Ncm().SetStateCallback([this] { OnSessionChanged(); });
    Ncm().SetLibraryCallback([this] {
        // The session updates its lists before it answers a write, so what it shows is what the
        // user changed: the changes acknowledged so far need not override it any more.
        ConfirmLibraryEdits(LibraryEditMark());
        NotifyLibraryChanged();
        if (AccountVerified()) m_snapshots.Queue();
    });
    Ncm().SetLikesCallback([this] { NotifyFavoritesChanged(); });
    if (m_signedIn) LoadBootstrapSnapshot();
    if (m_signedIn) Ncm().RefreshAccount({});
}

void StreamProvider::Shutdown() {
    const auto finalSnapshot = m_snapshots.Prepare();

    m_exec.Shutdown();
    Catalog::Get().FlushPendingSave();
    ClearIdSnapshots();
    m_idSnapshots.reset();
    Ncm().SetStateCallback({});
    Ncm().SetLibraryCallback({});
    Ncm().SetLikesCallback({});
    Ncm().Shutdown(); // stops the helper and its workers
    {
        std::lock_guard lock(m_cacheMutex);
        ResetSessionLocked(false);
    }
    if (finalSnapshot) m_snapshots.Commit(*finalSnapshot);
}

void StreamProvider::LoadBootstrapSnapshot() {
    std::string sourceOrigin;
    try {
        sourceOrigin = SourceIdentity();
    } catch (...) {
        return;
    }
    const auto loaded = m_snapshots.Load(sourceOrigin, SnapshotContext());
    if (!loaded) return;
    const auto& cached = loaded->snapshot;
    std::wstring accountId = Wide(cached.accountId);
    if (accountId.empty()) return;

    {
        std::lock_guard lock(m_cacheMutex);
        if (!m_exec.Running() || !m_signedIn || m_gate.verified) return;
        m_gate.BeginRestore(std::move(accountId));
        m_cachedCreated = cached.created;
        m_cachedSaved = cached.saved;
    }
    for (const auto& playlist : cached.created)
        Catalog::Get().Records().Restore(playlist);
    for (const auto& playlist : cached.saved)
        Catalog::Get().Records().Restore(playlist);
    NotifyLibraryChanged();
}

void StreamProvider::ResetSessionLocked(bool authPending) {
    m_gate.Reset(authPending);
    m_snapshots.Reset();
    m_accountId.clear();
    m_cachedCreated.clear();
    m_cachedSaved.clear();
}

bool StreamProvider::SnapshotEligible() const {
    std::lock_guard lock(m_cacheMutex);
    return SnapshotEligibleLocked();
}

bool StreamProvider::SnapshotEligibleLocked() const {
    return m_exec.Running() && m_gate.verified && !m_accountId.empty() && !m_gate.restoring;
}

bool StreamProvider::FillSnapshot(cache::provider_snapshot::Snapshot& payload) const {
    std::wstring accountId;
    {
        std::lock_guard lock(m_cacheMutex);
        if (!SnapshotEligibleLocked()) return false;
        accountId = m_accountId;
    }
    const Account account = Ncm().CurrentAccount();
    payload.sourceOrigin = SourceIdentity();
    payload.context = SnapshotContext();
    payload.locale = ui::UsingChinese() ? "zh-Hans-CN" : "en-US";
    payload.accountId = Utf8(accountId);
    payload.accountName = Utf8(account.nickname);
    payload.sourceStorefront = "CN";
    payload.created = Ncm().CreatedPlaylists();
    payload.saved = Ncm().CollectedPlaylists();
    return true;
}

stream::Capabilities StreamProvider::Offers() const {
    using enum stream::Capability;
    return {ReadFavorites,    AddFavorite,          RemoveFavorite,       LibraryArtists,
            FollowArtists,    LibraryAlbums,        SaveAlbums,           LibraryPlaylists,
            SavePlaylists,    CreatePlaylists,      AppendPlaylistTracks, RemovePlaylistTracks,
            RenamePlaylists,  DeletePlaylists,      ReorderPlaylists,     RecommendedPlaylists,
            NativeLyrics,     SongDetails,          TrackPopularity,      SimilarArtists,
            CatalogSearch,    SearchSuggestions,    Download,             Home,
            Stations};
}

std::vector<stream::ListKind> StreamProvider::CatalogSearchKinds() const {
    using enum stream::ListKind;
    return {CatalogSearchTopResults, CatalogSearch,       CatalogSearchArtists,
            CatalogSearchAlbums,     CatalogSearchPlaylists, CatalogSearchLyrics};
}

void StreamProvider::FetchCatalogSearchSuggestions(CatalogSearchSuggestionQuery query,
                                                   CatalogSearchSuggestionsDone done) {
    if (ListCancelled(query.cancelled)) return;
    done = stream::GuardDone(std::move(done), query.cancelled);
    if (query.term.empty() || !m_exec.Running()) {
        if (done) done({}, {});
        return;
    }
    m_exec.Post(
        kReadQueue, m_exec.Generation(), [this, query = std::move(query), done = std::move(done)]() mutable {
            if (ListCancelled(query.cancelled) || m_exec.Abort().is_aborting()) return;
            std::wstring error;
            SearchSuggestions found;
            if (Ncm().Service().EnsureStarted(m_exec.Abort(), &error))
                Ncm().Api().SearchSuggest(query.term, m_exec.Abort(), found, error);
            if (m_exec.Abort().is_aborting() || ListCancelled(query.cancelled)) return;
            CatalogSearchSuggestions suggestions;
            for (auto& term : found.terms)
                suggestions.ordered.emplace_back(CatalogSearchSuggestionTerm{term, term});
            // The suggested songs are summaries (no cover, no quality); they are shown, and opened
            // through the song page, but never written over a known song's record. Opening a
            // suggested artist, album or playlist finds it in the catalog.
            const auto followed = Ncm().FollowedArtists();
            for (auto& hit : found.items) {
                std::visit(
                    [&](auto& row) {
                        using Row = std::decay_t<decltype(row)>;
                        if constexpr (std::is_same_v<Row, Song>) {
                            suggestions.ordered.emplace_back(ToTrack(row));
                        } else if constexpr (std::is_same_v<Row, Artist>) {
                            Catalog::Get().Records().Put(ToArtist(row));
                            suggestions.ordered.emplace_back(AccountArtist(row, followed));
                        } else if constexpr (std::is_same_v<Row, Album>) {
                            const stream::Album album = ToAlbum(row);
                            Catalog::Get().Records().Put(album);
                            suggestions.ordered.emplace_back(album);
                        } else {
                            const stream::Playlist playlist = AccountPlaylist(row);
                            Catalog::Get().Records().Put(playlist);
                            suggestions.ordered.emplace_back(playlist);
                        }
                    },
                    hit);
            }
            if (!suggestions.ordered.empty()) error.clear();
            m_exec.PostMain(m_exec.Generation(),
                            [cancelled = query.cancelled, done = std::move(done),
                             suggestions = std::move(suggestions),
                             error = error.empty() ? std::wstring() : ErrorOf(std::move(error))]() mutable {
                                if (ListCancelled(cancelled)) return;
                                if (done) done(std::move(suggestions), std::move(error));
                            });
        });
}

namespace {

// A card of the PC client's home page as the UI shows it, or nothing for kinds it cannot show
// (podcasts). Personal FM, heartbeat mode and the similar-song / similar-artist mixes keep no
// target until radio mode comes; the daily songs play.
// The home cards' song lists (ListKind::HomeList): today's songs, and a style's daily songs
// ("style:<category>:<tag>:<the card's first songs>").
constexpr std::wstring_view kDailyList = L"daily", kStyleList = L"style:";
constexpr std::wstring_view kPersonalFm = L"fm", kHeartbeat = L"heart";
// The personal FM seeded by the card's songs or artists: "similar:<JSON array of ids>".
constexpr std::wstring_view kSimilar = L"similar:";

// A parameter of the client's action address ("...?tab=style&categoryId=1000&..."), as written.
std::wstring QueryValue(std::wstring_view url, std::wstring_view name) {
    const size_t query = url.find(L'?');
    if (query == std::wstring_view::npos) return {};
    for (size_t at = query + 1; at < url.size();) {
        const size_t end = std::min(url.find(L'&', at), url.size());
        const std::wstring_view pair = url.substr(at, end - at);
        if (pair.size() > name.size() && pair.substr(0, name.size()) == name && pair[name.size()] == L'=')
            return std::wstring(pair.substr(name.size() + 1));
        at = end + 1;
    }
    return {};
}

std::optional<stream::HomeCard> ToHomeCard(const HomeCard& card) {
    if (card.resourceType == L"musicPodcast" || card.resourceType == L"podcast" ||
        card.moduleType.find(L"podcast") != std::wstring::npos)
        return std::nullopt;
    stream::HomeCard out;
    out.key = card.moduleType + L":" + card.resourceType + L":" + card.resourceId;
    out.label = card.label;
    out.title = card.title;
    out.subtitle = card.subtitle;
    out.imageUrl = card.coverUrl;
    if (card.resourceType == L"dailySongs" && card.subResourceType == L"dailySong") {
        // "每日推荐 | 从「…」听起" under the date "11-15": the date's day goes on the picture.
        out.label = L"每日推荐";
        if (const size_t bar = out.title.find(L" | "); bar != std::wstring::npos) out.title.erase(0, bar + 3);
        if (const size_t dash = card.label.rfind(L'-'); dash != std::wstring::npos)
            out.badge = card.label.substr(dash + 1);
        out.list = std::wstring(kDailyList);
    } else if (card.resourceType == L"dailySongs" && card.subResourceType == L"style_dailySong") {
        // A style's daily songs: the list its client opens (orpheus://songrcmd?tab=style&...).
        const auto value = [&](std::wstring_view name) { return QueryValue(card.targetUrl, name); };
        const std::wstring category = value(L"categoryId"), tag = value(L"tagId"), songs = value(L"songId");
        if (!category.empty() && !tag.empty())
            out.list = std::wstring(kStyleList) + category + L":" + tag + L":" + songs;
    } else if (card.resourceType == L"fm") {
        out.station = std::wstring(kPersonalFm);
    } else if (card.resourceType == L"star") {
        out.station = std::wstring(kHeartbeat);
    } else if ((card.resourceType == L"similarSong" || card.resourceType == L"similarArtist") &&
               card.resourceId.starts_with(L"[")) {
        out.station = std::wstring(kSimilar) + card.resourceId;
    } else if (card.resourceType == L"playList" && !card.resourceId.empty()) {
        stream::Playlist playlist;
        playlist.id = card.resourceId;
        playlist.name = card.label;
        playlist.coverUrl = card.coverUrl;
        out.target = stream::CatalogSearchItem(std::move(playlist));
    }
    return out;
}

} // namespace

void StreamProvider::FetchHome(HomeDone done) {
    done = stream::GuardDone(std::move(done));
    if (!m_exec.Running()) {
        if (done) done({}, L"NetEase Cloud Music is not running.");
        return;
    }
    m_exec.Post(kReadQueue, m_exec.Generation(), [this, done = std::move(done)]() mutable {
        if (m_exec.Abort().is_aborting()) return;
        std::wstring error;
        std::vector<HomeCard> cards;
        std::vector<HomeBlock> blocks;
        if (Ncm().Service().EnsureStarted(m_exec.Abort(), &error)) {
            std::wstring blockError;
            if (!Ncm().Api().HomeCards(m_exec.Abort(), cards, error)) cards.clear();
            if (!Ncm().Api().HomeBlocks(m_exec.Abort(), blocks, blockError) && error.empty())
                error = blockError;
        }
        if (m_exec.Abort().is_aborting()) return;
        if (!cards.empty() || !blocks.empty()) error.clear();
        // Playlists the cards and rows name are known before anything opens them; a summary never
        // replaces a richer record.
        for (const auto& card : cards)
            if (card.resourceType == L"playList" && !card.resourceId.empty()) {
                stream::Playlist playlist;
                playlist.id = IdText(_wtoi64(card.resourceId.c_str()));
                playlist.name = card.label;
                playlist.coverUrl = card.coverUrl;
                Catalog::Get().Records().Restore(playlist);
            }
        stream::Home home;
        for (const auto& card : cards)
            if (auto mapped = ToHomeCard(card)) home.cards.push_back(std::move(*mapped));
        std::vector<SongRecord> songs;
        for (const auto& block : blocks) {
            stream::HomeShelf shelf;
            shelf.id = block.code;
            shelf.title = block.title;
            for (const auto& playlist : block.playlists) {
                stream::Playlist row = AccountPlaylist(playlist);
                Catalog::Get().Records().Restore(row);
                if (const auto known = Catalog::Get().Records().FindPlaylist(row.id)) row = *known;
                shelf.items.emplace_back(std::move(row));
            }
            for (const auto& song : block.songs) {
                songs.push_back(ToSong(song));
                shelf.items.emplace_back(songs.back().track);
                // The rows show a song by its album's cover: the album must be known with it.
                if (song.albumId && !song.coverUrl.empty()) {
                    stream::Album album;
                    album.id = IdText(song.albumId);
                    album.name = song.album;
                    album.coverUrl = song.coverUrl;
                    Catalog::Get().Records().Restore(album);
                }
            }
            if (!shelf.items.empty()) home.shelves.push_back(std::move(shelf));
        }
        m_exec.PostMain(m_exec.Generation(),
                        [done = std::move(done), home = std::move(home), songs = std::move(songs),
                         error = error.empty() ? std::wstring() : ErrorOf(std::move(error))]() mutable {
                            if (!songs.empty()) Catalog::Get().PutAndHint(songs);
                            if (done) done(std::move(home), std::move(error));
                        });
    });
}

// The stations: the personal FM (私人漫游, three songs a call, 不感兴趣 for its songs) and heartbeat
// mode (心动模式: songs like the last one, from and around the liked songs).
void StreamProvider::FetchStationTracks(const stream::Id& station, TracksDone done) {
    done = stream::GuardDone(std::move(done));
    if (!m_exec.Running() ||
        (station != kPersonalFm && station != kHeartbeat && !station.starts_with(kSimilar))) {
        if (done) done({}, L"This station is unavailable.");
        return;
    }
    m_exec.Post(kReadQueue, m_exec.Generation(), [this, station, done = std::move(done)]() mutable {
        if (m_exec.Abort().is_aborting()) return;
        std::wstring error;
        std::vector<Song> songs;
        Client& client = Ncm().Api();
        if (!Ncm().LoggedIn()) {
            error = L"Please sign in to NetEase Cloud Music.";
        } else if (Ncm().Service().EnsureStarted(m_exec.Abort(), &error)) {
            if (station == kPersonalFm) {
                client.PersonalFm(m_exec.Abort(), songs, error);
            } else if (station.starts_with(kSimilar)) {
                client.FmMode(L"FAMILIAR", std::wstring_view(station).substr(kSimilar.size()), m_exec.Abort(),
                              songs, error);
            } else {
                // The first seed is one of the liked songs; later ones, the last song played.
                int64_t seed = m_heartbeatSeed.load();
                if (!seed) {
                    std::vector<int64_t> liked;
                    if (client.Likelist(Ncm().CurrentAccount().userId, m_exec.Abort(), liked, error) &&
                        !liked.empty())
                        seed = liked[GetTickCount64() % std::min<size_t>(liked.size(), 50)];
                }
                if (seed && client.Heartbeat(seed, Ncm().LikedPlaylistId(), m_exec.Abort(), songs, error) &&
                    !songs.empty())
                    m_heartbeatSeed = songs.back().id;
            }
        }
        if (m_exec.Abort().is_aborting()) return;
        m_exec.PostMain(m_exec.Generation(),
                        [done = std::move(done), songs = ToSongs(songs),
                         error = error.empty() ? std::wstring() : ErrorOf(std::move(error))]() mutable {
                            if (!songs.empty()) Catalog::Get().PutAndHint(songs);
                            if (done)
                                done(TracksOf(songs), songs.empty() ? std::move(error) : std::wstring());
                        });
    });
}

// The FM's songs (its own and the similar-song mixes) take 不感兴趣.
bool StreamProvider::CanDislike(const stream::Id& station) const {
    return station == kPersonalFm || station.starts_with(kSimilar);
}

void StreamProvider::DislikeStationTrack(const stream::Id& station, const stream::Id& track, Done done) {
    done = stream::GuardDone(std::move(done));
    if (!CanDislike(station) || !m_exec.Running()) {
        if (done) done(L"This station does not take feedback.");
        return;
    }
    m_exec.Post(kReadQueue, m_exec.Generation(), [this, id = Num(track), done = std::move(done)]() mutable {
        if (m_exec.Abort().is_aborting()) return;
        std::wstring error;
        if (Ncm().Service().EnsureStarted(m_exec.Abort(), &error))
            Ncm().Api().FmTrash(id, m_exec.Abort(), error);
        fb2k::inMainThread([done = std::move(done), error = error.empty() ? std::wstring() : ErrorOf(std::move(error))] {
            if (done) done(error);
        });
    });
}

std::vector<settings::Field> StreamProvider::SettingFields() {
    using Field = settings::Field;
    std::vector<Field> fields;
    Field account;
    account.kind = Field::Kind::Note;
    account.label = L"Account";
    account.note = [this] { return SignedIn() ? AccountName() : std::wstring(L"Not signed in"); };
    fields.push_back(std::move(account));

    Field signIn;
    signIn.kind = Field::Kind::Button;
    signIn.label = L"Sign in...";
    signIn.press = [this](HWND owner) { SignIn(owner); };
    fields.push_back(std::move(signIn));

    Field signOut;
    signOut.kind = Field::Kind::Button;
    signOut.label = L"Sign out";
    signOut.press = [this](HWND) {
        if (SignedIn()) SignOut();
    };
    fields.push_back(std::move(signOut));

    // The order of Quality's values.
    Field quality;
    quality.kind = Field::Kind::Choice;
    quality.label = L"Audio quality";
    quality.choices = {L"Best available", L"Lossless", L"Hi-Res", L"Very high (320 kbps)", L"Standard"};
    quality.defaultValue = static_cast<int>(Quality::Auto);
    quality.get = [] { return static_cast<int>(GetQuality()); };
    quality.set = [](int value) { SetQuality(static_cast<Quality>(value)); };
    const auto qualityChoices = quality.choices;
    fields.push_back(std::move(quality));

    Field download;
    download.kind = Field::Kind::Choice;
    download.label = L"Download quality";
    download.choices = qualityChoices;
    download.defaultValue = static_cast<int>(Quality::Auto);
    download.get = [] { return static_cast<int>(GetDownloadQuality()); };
    download.set = [](int value) { SetDownloadQuality(static_cast<Quality>(value)); };
    fields.push_back(std::move(download));

    Field service;
    service.kind = Field::Kind::Text;
    service.label = L"Service address (empty: built-in)";
    service.restart = true;
    service.getText = [] { return GetServiceUrlOverride(); };
    service.setText = [](const std::wstring& url) { SetServiceUrlOverride(url); };
    fields.push_back(std::move(service));
    return fields;
}

bool StreamProvider::SignedIn() const {
    std::lock_guard lock(m_cacheMutex);
    return m_gate.verified;
}

bool StreamProvider::SessionRestoring() const {
    std::lock_guard lock(m_cacheMutex);
    return m_gate.Waiting();
}

bool StreamProvider::AccountVerified() const {
    std::lock_guard lock(m_cacheMutex);
    return m_gate.verified;
}

std::wstring StreamProvider::AccountName() const {
    return Ncm().CurrentAccount().nickname;
}

void StreamProvider::SignIn(HWND owner) {
    ui::ShowNeteaseLogin(owner, [this] {
        ClearIdSnapshots();
        cache::RotateAccountScope("ncm");
        {
            std::lock_guard lock(m_cacheMutex);
            m_gate.authPending = true;
        }
        OnSessionChanged();
    });
}

void StreamProvider::OnSessionChanged() {
    const bool signedIn = Ncm().LoggedIn();
    const Account account = Ncm().CurrentAccount();
    const std::wstring accountId = IdText(account.userId);
    bool accountChanged = false;
    bool accountObserved = false;
    {
        std::lock_guard lock(m_cacheMutex);
        const bool restoring = m_gate.restoring;
        const bool verified = m_gate.verified;
        if (!signedIn || accountId.empty()) {
            accountChanged = restoring || verified || m_signedIn != signedIn;
        } else if (restoring && accountId != m_gate.cachedId) {
            accountChanged = true;
        } else if (verified && accountId != m_accountId) {
            accountChanged = true;
        }
        if (accountChanged) {
            ResetSessionLocked(signedIn && accountId.empty());
        } else if (signedIn) {
            accountObserved = !verified || restoring;
            m_gate.Verify();
            m_accountId = accountId;
            m_cachedCreated.clear();
            m_cachedSaved.clear();
        }
        m_signedIn = signedIn;
    }
    if (accountChanged) {
        ClearIdSnapshots();
        cache::RotateAccountScope("ncm");
        NotifyAccountChanged();
        return;
    }
    if (accountObserved) NotifyAccountObserved();
    NotifyLibraryChanged();
    if (signedIn) m_snapshots.Queue();
}

void StreamProvider::SignOut() {
    ClearIdSnapshots();
    cache::RotateAccountScope("ncm");
    Ncm().Logout();
}

// Every Session call below drops its task unrun at shutdown; GuardDone completes the `done`
// it carried with stream::kRequestDropped then, as provider.h promises.
void StreamProvider::RefreshLibrary(Done done) {
    done = stream::GuardDone(std::move(done));
    Ncm().RefreshAccount([this, done = std::move(done)](bool ok, std::wstring error) mutable {
        FinishListWrite(ok, std::move(error), std::move(done));
    });
}

stream::Id StreamProvider::TrackId(const metadb_handle_ptr& item) const {
    Id id;
    return item.is_valid() && Catalog::Get().Records().ParseSongPath(item->get_path(), id) ? id : Id();
}

std::wstring StreamProvider::Lyrics(const Id& track, abort_callback& abort) {
    std::wstring lrc, error;
    if (const int64_t id = Num(track)) Session::Get().Api().GetLyrics(id, abort, lrc, error);
    return lrc;
}

namespace {

// A song link with the headers the CDN expects (as the input opens it).
stream::DownloadLink SongLink(const std::wstring& url) {
    return {url,
            {{L"User-Agent",
              L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Glasswave/0.1"},
             {L"Referer", L"https://music.163.com/"}}};
}

std::wstring LevelName(const std::wstring& level) {
    if (level == L"hires") return L"Hi-Res";
    if (level == L"lossless") return L"Lossless";
    return {};
}

} // namespace

// The song's link at the download quality, the first level the service has; a preview clip
// is refused (it is not the song). Tags from the song's detail.
stream::DownloadSource StreamProvider::ResolveDownload(const Id& track, abort_callback& abort) {
    const int64_t id = Num(track);
    if (!id) throw pfc::exception("Not a NetEase song.");
    auto& session = Session::Get();
    if (!session.LoggedIn()) throw pfc::exception("Sign in to NetEase Cloud Music to download songs.");
    std::wstring error;
    if (!session.Service().EnsureStarted(abort, &error))
        throw pfc::exception(Utf8(error.empty() ? L"NetEase service is unavailable." : error).c_str());

    SongUrl url;
    auto status = stream::SourceStatus::Unavailable;
    std::wstring failure;
    for (const auto& level : QualityLevels(GetDownloadQuality())) {
        const auto result = session.Api().GetSongUrl(id, level, abort, url, error);
        status = stream::CombineSourceStatus(status, result);
        if (result == stream::SourceStatus::Playable) break;
        if (failure.empty()) failure = error;
    }
    if (status != stream::SourceStatus::Playable)
        throw pfc::exception(Utf8(failure.empty() ? L"This song cannot be downloaded." : failure).c_str());
    if (url.trial) throw pfc::exception("Only a preview of this song is available.");

    stream::DownloadSource source;
    const std::wstring type = LowerAscii(url.type);
    source.extension = type == L"flac" ? "flac" : type == L"m4a" || type == L"mp4" ? "m4a" : "mp3";
    std::wstring format = source.extension == "flac" ? L"FLAC" : source.extension == "m4a" ? L"AAC" : L"MP3";
    if (source.extension != "flac" && url.bitrate > 0) format += L" " + std::to_wstring((url.bitrate + 500) / 1000) + L" kbps";
    if (const std::wstring level = LevelName(LowerAscii(url.level)); !level.empty()) format += L" (" + level + L")";
    source.format = std::move(format);

    // Tags: the song's detail (disc, number, date, style); the catalog's record if that fails.
    SongInfo info;
    std::wstring ignored;
    SongRecord song;
    if (session.Api().GetSongInfo(id, abort, info, ignored)) {
        song = ToSong(info.song);
        source.tags.disc = info.disc;
        source.tags.number = info.number;
        source.tags.date = Date(info.publishTime); // the same day the pages show
        source.tags.genres = info.styles;
    } else if (auto known = Catalog::Get().FindSong(track)) {
        song = std::move(*known);
    }
    source.tags.title = song.track.title;
    source.tags.album = song.track.album;
    if (!song.track.artists.empty()) source.tags.albumArtist = song.track.artists.front().name;
    for (const auto& artist : song.track.artists)
        if (!artist.name.empty()) source.tags.artists.push_back(artist.name);
    source.tags.coverUrl = song.cover.substr(0, song.cover.find(L'?')); // the full-size picture

    // Links expire (about 20 minutes): a renewal asks for the same level again, and refuses a
    // different codec or bitrate, which would not continue the same file.
    struct Link {
        std::mutex mutex;
        SongUrl url;
    };
    auto link = std::make_shared<Link>();
    link->url = url;
    source.link = [link, id](bool renew, abort_callback& abort) {
        std::wstring address;
        {
            std::lock_guard lock(link->mutex);
            if (renew) {
                SongUrl renewed;
                std::wstring error;
                if (Session::Get().Api().GetSongUrl(id, link->url.level, abort, renewed, error) !=
                    stream::SourceStatus::Playable)
                    throw pfc::exception(Utf8(error.empty() ? L"Cannot renew the download link." : error).c_str());
                if (LowerAscii(renewed.type) != LowerAscii(link->url.type) || renewed.bitrate != link->url.bitrate ||
                    renewed.trial)
                    throw pfc::exception("NetEase renewed the link with a different file.");
                link->url = renewed;
            }
            address = link->url.url;
        }
        return SongLink(address);
    };
    return source;
}

stream::Catalog* StreamProvider::Records() const {
    return &Catalog::Get().Records();
}

metadb_handle_ptr StreamProvider::ArtItem(stream::ArtKind kind, const Id& id) const {
    if (kind == stream::ArtKind::Station)
        return {}; // its stations are cards (personal FM, heartbeat), never tiles
    if (kind == stream::ArtKind::Track) return Catalog::Get().Records().Handle(id); // the song itself
    return Catalog::Get().Records().CoverHandle(kind, id);
}

bool StreamProvider::SameCover(std::wstring_view a, std::wstring_view b) const {
    // NetEase serves one image under many "?param=WxH" size suffixes.
    const auto base = [](std::wstring_view url) { return url.substr(0, url.find(L'?')); };
    return base(a) == base(b);
}

bool StreamProvider::ReadFavorite(const Id& track) const {
    return Ncm().IsLiked(Num(track));
}

std::vector<stream::Playlist> StreamProvider::Playlists(stream::Shelf shelf) const {
    switch (shelf) {
    case stream::Shelf::Created: {
        auto rows = Ncm().CreatedPlaylists();
        if (rows.empty()) {
            std::lock_guard lock(m_cacheMutex);
            if (m_gate.restoring) return m_cachedCreated;
        }
        return rows;
    }
    case stream::Shelf::Saved: {
        auto rows = Ncm().CollectedPlaylists();
        if (rows.empty()) {
            std::lock_guard lock(m_cacheMutex);
            if (m_gate.restoring) return m_cachedSaved;
        }
        return rows;
    }
    case stream::Shelf::Recommended:
        return Ncm().RecommendedPlaylists();
    }
    return {};
}

std::optional<stream::Shelf> StreamProvider::ShelfOf(const Id& playlist) const {
    switch (Ncm().FindPlaylistScope(Num(playlist))) {
    case Session::PlaylistScope::Created:
        return stream::Shelf::Created;
    case Session::PlaylistScope::Collected:
        return stream::Shelf::Saved;
    case Session::PlaylistScope::Recommended:
        return stream::Shelf::Recommended;
    case Session::PlaylistScope::Unknown:
        break;
    }
    return std::nullopt;
}

std::vector<stream::Artist> StreamProvider::FollowedArtists() const {
    return Ncm().FollowedArtists();
}

std::vector<stream::Album> StreamProvider::SavedAlbums() const {
    return Ncm().CollectedAlbums();
}

bool StreamProvider::ReadAlbumSaved(const Id& album) const {
    return Ncm().IsAlbumCollected(Num(album));
}

void StreamProvider::FetchArtist(const Id& artist, ArtistDone done) {
    done = stream::GuardDone(std::move(done));
    Ncm().FetchArtistDetail(Num(artist), [done = std::move(done)](stream::Artist info, std::wstring error) {
        done(std::move(info), std::move(error));
    });
}

void StreamProvider::FetchArtistBio(const Id& artist, TextDone done) {
    done = stream::GuardDone(std::move(done));
    // The profile's brief is a summary; /artist/desc has the full article when it exists.
    Ncm().FetchArtistDescription(
        Num(artist), [done = std::move(done)](std::wstring brief, std::wstring full, std::wstring error) {
            done(full.empty() ? std::move(brief) : std::move(full), std::move(error));
        });
}

// The song's detail and wiki, and the wiki's similar songs in full (so they play like any row).
void StreamProvider::FetchSongDetails(const Id& track, SongDetailsDone done) {
    done = stream::GuardDone(std::move(done));
    if (!m_exec.Running()) {
        if (done) done({}, L"The NetEase provider has not started.");
        return;
    }
    m_exec.Post(kReadQueue, m_exec.Generation(), [this, track, done = std::move(done)]() mutable {
        stream::SongDetails details;
        std::vector<Song> songs;
        std::wstring error;
        if (Ncm().Service().EnsureStarted(m_exec.Abort(), &error)) {
            Client& client = Ncm().Api();
            SongInfo info;
            if (client.GetSongInfo(Num(track), m_exec.Abort(), info, error)) {
                details.track = ToTrack(info.song);
                details.disc = info.disc;
                details.number = info.number;
                details.released = Date(info.publishTime);
                details.aliases = info.aliases;
                details.version = info.originCoverType == 1   ? L"Original"
                                  : info.originCoverType == 2 ? L"Cover"
                                                              : L"";
                if (info.hiRes)
                    details.traits.push_back(L"Hi-Res Lossless");
                else if (info.lossless)
                    details.traits.push_back(L"Lossless");
                details.genres = info.styles;
                details.tags = info.tags;
                details.language = info.language;
                details.bpm = info.bpm;
                songs.push_back(info.song);
                std::vector<Song> similar;
                std::wstring ignored;
                if (!info.similar.empty() &&
                    SongDetailsInBatches(client, Unique(info.similar), m_exec.Abort(), similar, ignored)) {
                    details.similar = ToTracks(OrderDetails(info.similar, similar));
                    songs.insert(songs.end(), similar.begin(), similar.end());
                }
            }
        }
        if (m_exec.Abort().is_aborting()) return;
        m_exec.PostMain(m_exec.Generation(),
                        [done = std::move(done), details = std::move(details), songs = ToSongs(songs),
                         error = ErrorOf(std::move(error))]() mutable {
                            if (!songs.empty()) Catalog::Get().PutAndHint(songs);
                            if (!details.track.id.empty()) error.clear();
                            if (done) done(std::move(details), std::move(error));
                        });
    });
}

void StreamProvider::RestoreListRecords(const stream::ListPage& page) {
    stream::Catalog& records = Catalog::Get().Records();
    if (const auto trackRows = std::get_if<std::vector<stream::Track>>(&page.rows)) {
        Catalog::Get().RestoreAndHint(*trackRows);
    } else if (const auto playlistRows = std::get_if<std::vector<stream::Playlist>>(&page.rows)) {
        for (const auto& playlist : *playlistRows)
            records.Restore(playlist);
    } else if (const auto albumRows = std::get_if<std::vector<stream::Album>>(&page.rows)) {
        for (const auto& album : *albumRows)
            records.Restore(album);
    } else if (const auto artistRows = std::get_if<std::vector<stream::Artist>>(&page.rows)) {
        for (const auto& artist : *artistRows)
            records.Restore(artist);
    }
    if (page.playlist) records.Restore(*page.playlist);
    if (page.album) records.Restore(*page.album);
    if (page.artist) records.Restore(*page.artist);
}

void StreamProvider::PostListPage(stream::ListPageDone done, stream::ListPage page,
                                  std::vector<SongRecord> songs, std::wstring error,
                                  std::shared_ptr<std::atomic_bool> cancelled) {
    if (ListCancelled(cancelled)) return;
    m_exec.PostMain(m_exec.Generation(),
                    [this, done = std::move(done), page = std::move(page), songs = std::move(songs),
                     error = std::move(error), cancelled = std::move(cancelled)]() mutable {
                        if (ListCancelled(cancelled)) return;
                        if (error.empty() && !songs.empty()) Catalog::Get().PutAndHint(songs, false);
                        if (ListCancelled(cancelled)) return;
                        if (done) done(std::move(page), std::move(error));
                    });
}

void StreamProvider::ClearIdSnapshots() {
    const auto snapshots = m_idSnapshots;
    if (snapshots) snapshots->Clear();
}

void StreamProvider::FinishListWrite(bool ok, std::wstring error, Done done) {
    if (ok) {
        ClearIdSnapshots();
        NotifyListChanged();
    }
    if (done) done(ok ? std::wstring() : ErrorOf(std::move(error)));
}

bool StreamProvider::NeedsAccount(const stream::ListQuery& query) const {
    switch (query.kind) {
    case stream::ListKind::PlaylistTracks:
    case stream::ListKind::Favorites:
    case stream::ListKind::SavedAlbums:
    case stream::ListKind::FollowedArtists:
    case stream::ListKind::Playlists:
    case stream::ListKind::ArtistSimilarArtists:
    case stream::ListKind::HomeList:
        return true;
    default:
        return false;
    }
}

void StreamProvider::FetchListPage(const stream::ListQuery& query, const stream::ListCursor& cursor,
                                   size_t requested, stream::ListPageDone done) {
    if (!done || ListCancelled(query.cancelled)) return;
    done = stream::GuardDone(std::move(done), query.cancelled);
    // Playlist and favorites pages come from a known id list and may be large (their details
    // are fetched in parallel batches). Offset-paged endpoints keep the size the service
    // honors, since a short answer is how they report the end.
    const bool snapshotKind =
        query.kind == stream::ListKind::PlaylistTracks || query.kind == stream::ListKind::Favorites;
    const size_t limit = snapshotKind ? requested : std::min<size_t>(requested, 100);
    if (query.kind == stream::ListKind::Playlists) {
        stream::ListPage page;
        const auto playlists = Playlists(query.shelf);
        page.rows = playlists;
        page.total = playlists.size();
        PostListPage(std::move(done), std::move(page), {}, {}, query.cancelled);
        return;
    }
    if (query.kind == stream::ListKind::SectionTracks || query.kind == stream::ListKind::Groups) {
        PostListPage(std::move(done), {}, {}, L"This NetEase list is unavailable.", query.cancelled);
        return;
    }
    if (!m_exec.Running()) {
        PostListPage(std::move(done), {}, {}, L"The NetEase provider has not started.", query.cancelled);
        return;
    }
    m_exec.Post(
        kReadQueue, m_exec.Generation(), [this, query, cursor, limit, done = std::move(done)]() mutable {
            if (ListCancelled(query.cancelled) || m_exec.Abort().is_aborting()) return;
            std::wstring error;
            if (!Ncm().Service().EnsureStarted(m_exec.Abort(), &error)) {
                PostListPage(std::move(done), {}, {}, ErrorOf(std::move(error)), query.cancelled);
                return;
            }
            if (ListCancelled(query.cancelled)) return;

            Client& client = Ncm().Api();
            const auto snapshots = m_idSnapshots;
            if (!snapshots) {
                PostListPage(std::move(done), {}, {}, L"The NetEase list cache is unavailable.",
                             query.cancelled);
                return;
            }
            const uint64_t accountRevision = cache::AccountRevision("ncm");
            stream::ListPage page;
            std::vector<SongRecord> hintSongs; // the page's songs, for the catalog
            std::vector<Song> found;           // what the client read, before it is converted

            switch (query.kind) {
            case stream::ListKind::PlaylistTracks: {
                const int64_t playlistId = Num(query.id);
                if (!playlistId) {
                    error = L"The playlist id is invalid.";
                    break;
                }
                if (cursor.offset != 0 && cursor.value.empty()) {
                    error = L"The playlist cursor has no snapshot revision.";
                    break;
                }
                std::shared_ptr<const IdSnapshotStore::Snapshot> snapshot;
                if (!snapshots->PageFromIdSnapshot(
                        {accountRevision, IdSnapshotStore::Kind::Playlist, playlistId},
                        IdSnapshotStore::kPlaylistWording, cursor, limit, client, m_exec.Abort(),
                        [&](std::vector<int64_t>& ids, Playlist& info, std::wstring& revision,
                            std::wstring& fetchError) {
                            PlaylistSnapshot fresh;
                            if (!client.GetPlaylistSnapshot(playlistId, m_exec.Abort(), fresh, fetchError))
                                return false;
                            revision = IdsRevision(accountRevision, fresh.trackIds, fresh.trackUpdateTime);
                            ids = std::move(fresh.trackIds);
                            info = std::move(fresh.info);
                            return true;
                        },
                        page, hintSongs, snapshot, error))
                    break;
                page.playlist = AccountPlaylist(snapshot->info);
                Catalog::Get().Records().Put(*page.playlist);
                break;
            }
            case stream::ListKind::Favorites: {
                const int64_t userId = Ncm().CurrentAccount().userId;
                if (!userId) {
                    error = L"The NetEase account is not ready. Refresh the library and try again.";
                    break;
                }
                if (cursor.offset != 0 && cursor.value.empty()) {
                    error = L"The favorites cursor has no snapshot revision.";
                    break;
                }
                std::shared_ptr<const IdSnapshotStore::Snapshot> snapshot;
                snapshots->PageFromIdSnapshot(
                    {accountRevision, IdSnapshotStore::Kind::Favorites, 0},
                    IdSnapshotStore::kFavoritesWording, cursor, limit, client, m_exec.Abort(),
                    [&](std::vector<int64_t>& ids, Playlist&, std::wstring& revision,
                        std::wstring& fetchError) {
                        if (!client.Likelist(userId, m_exec.Abort(), ids, fetchError)) return false;
                        revision = IdsRevision(accountRevision, ids);
                        return true;
                    },
                    page, hintSongs, snapshot, error);
                break;
            }
            case stream::ListKind::HomeList: {
                stream::Playlist info;
                info.id = query.id;
                if (query.id == kDailyList) {
                    info.name = L"每日推荐";
                    info.tagline = L"根据你的音乐口味生成，每天6:00更新";
                    client.DailySongs(m_exec.Abort(), found, error);
                } else if (query.id.starts_with(kStyleList)) {
                    std::vector<std::wstring> parts;
                    std::wstring_view rest = std::wstring_view(query.id).substr(kStyleList.size());
                    for (size_t i = 0; i < 2; ++i) {
                        const size_t colon = rest.find(L':');
                        parts.emplace_back(rest.substr(0, colon));
                        rest =
                            colon == std::wstring_view::npos ? std::wstring_view() : rest.substr(colon + 1);
                    }
                    info.tagline = L"根据你的音乐口味生成，每天更新";
                    client.StyleDailySongs(parts[0], parts[1], rest, m_exec.Abort(), found, error);
                } else {
                    error = L"This list is unavailable.";
                    break;
                }
                hintSongs = ToSongs(found);
                page.rows = TracksOf(hintSongs);
                page.playlist = std::move(info);
                page.total = hintSongs.size();
                break;
            }
            case stream::ListKind::AlbumTracks: {
                const int64_t albumId = Num(query.id);
                if (!albumId) {
                    error = L"The album id is invalid.";
                    break;
                }
                Album info;
                if (!client.GetAlbum(albumId, m_exec.Abort(), info, found, error)) break;
                hintSongs = ToSongs(found);
                page.rows = TracksOf(hintSongs);
                page.album = ToAlbum(info);
                page.total = hintSongs.size();
                Catalog::Get().Records().Put(*page.album);
                break;
            }
            case stream::ListKind::ArtistTopTracks: {
                const int64_t artistId = Num(query.id);
                if (!artistId) {
                    error = L"The artist id is invalid.";
                    break;
                }
                OffsetWindow window;
                if (!OffsetWindowOf(cursor, limit, window, error)) break;
                bool more = false;
                size_t received = 0;
                if (!client.GetArtistSongsPage(artistId, window.offset, window.count, L"hot", m_exec.Abort(),
                                               found, &more, error, &received))
                    break;
                hintSongs = ToSongs(found);
                page.rows = TracksOf(hintSongs);
                stream::SetOffsetNext(page, cursor, received, more);
                break;
            }
            case stream::ListKind::ArtistAlbums: {
                const int64_t artistId = Num(query.id);
                if (!artistId) {
                    error = L"The artist id is invalid.";
                    break;
                }
                OffsetWindow window;
                if (!OffsetWindowOf(cursor, limit, window, error)) break;
                std::vector<Album> albums;
                bool more = false;
                size_t received = 0;
                if (!client.GetArtistAlbumsPage(artistId, window.offset, window.count, m_exec.Abort(), albums,
                                                nullptr, &more, error, &received))
                    break;
                auto rows = ToAlbums(albums);
                Catalog::Get().Records().Put(rows);
                page.rows = std::move(rows);
                stream::SetOffsetNext(page, cursor, received, more);
                break;
            }
            case stream::ListKind::SavedAlbums: {
                OffsetWindow window;
                if (!OffsetWindowOf(cursor, limit, window, error)) break;
                std::vector<Album> albums;
                bool more = false;
                size_t received = 0;
                if (!client.GetCollectedAlbumsPage(window.offset, window.count, m_exec.Abort(), albums, more,
                                                   error, &received))
                    break;
                auto rows = ToAlbums(albums);
                Catalog::Get().Records().Put(rows);
                page.rows = std::move(rows);
                stream::SetOffsetNext(page, cursor, received, more);
                break;
            }
            case stream::ListKind::ArtistSimilarArtists: {
                const int64_t artistId = Num(query.id);
                if (!artistId) {
                    error = L"The artist id is invalid.";
                    break;
                }
                std::vector<Artist> artists;
                if (!client.GetSimilarArtists(artistId, m_exec.Abort(), artists, error)) break;
                auto rows = AccountArtists(artists);
                Catalog::Get().Records().Put(rows);
                page.total = rows.size();
                page.rows = std::move(rows);
                break;
            }
            case stream::ListKind::FollowedArtists: {
                OffsetWindow window;
                if (!OffsetWindowOf(cursor, limit, window, error)) break;
                std::vector<Artist> artists;
                bool more = false;
                size_t received = 0;
                if (!client.GetFollowedArtistsPage(window.offset, window.count, m_exec.Abort(), artists, more,
                                                   received, error))
                    break;
                auto rows = AccountArtists(artists);
                Catalog::Get().Records().Put(rows);
                page.rows = std::move(rows);
                stream::SetOffsetNext(page, cursor, received, more);
                break;
            }
            case stream::ListKind::CatalogSearch:
            case stream::ListKind::CatalogSearchAlbums:
            case stream::ListKind::CatalogSearchArtists:
            case stream::ListKind::CatalogSearchPlaylists:
            case stream::ListKind::CatalogSearchLyrics: {
                OffsetWindow window;
                if (!OffsetWindowOf(cursor, limit, window, error)) break;
                SearchPage searched;
                bool ok = false;
                if (query.kind == stream::ListKind::CatalogSearch) {
                    ok = client.SearchSongsPage(query.search, window.count, window.offset, m_exec.Abort(),
                                                found, searched, error);
                    if (ok) {
                        hintSongs = ToSongs(found);
                        page.rows = TracksOf(hintSongs);
                    }
                } else if (query.kind == stream::ListKind::CatalogSearchAlbums) {
                    std::vector<Album> albums;
                    ok = client.SearchAlbumsPage(query.search, window.count, window.offset, m_exec.Abort(),
                                                 albums, searched, error);
                    if (ok) {
                        auto rows = ToAlbums(albums);
                        Catalog::Get().Records().Put(rows);
                        page.rows = std::move(rows);
                    }
                } else if (query.kind == stream::ListKind::CatalogSearchArtists) {
                    std::vector<Artist> artists;
                    ok = client.SearchArtistsPage(query.search, window.count, window.offset, m_exec.Abort(),
                                                  artists, searched, error);
                    if (ok) {
                        auto rows = AccountArtists(artists);
                        Catalog::Get().Records().Put(rows);
                        page.rows = std::move(rows);
                    }
                } else if (query.kind == stream::ListKind::CatalogSearchPlaylists) {
                    std::vector<Playlist> playlists;
                    ok = client.SearchPlaylistsPage(query.search, window.count, window.offset, m_exec.Abort(),
                                                    playlists, searched, error);
                    if (ok) {
                        auto rows = AccountPlaylists(playlists);
                        Catalog::Get().Records().Put(rows);
                        page.rows = std::move(rows);
                    }
                } else {
                    std::vector<LyricHit> hits;
                    ok = client.SearchLyricsPage(query.search, window.count, window.offset, m_exec.Abort(),
                                                 hits, searched, error);
                    if (ok) {
                        std::vector<stream::Track> tracks;
                        for (auto& hit : hits) {
                            hintSongs.push_back(ToSong(hit.song));
                            tracks.push_back(hintSongs.back().track);
                            tracks.back().lyricMatch = std::move(hit.excerpt);
                            tracks.back().lyricText = std::move(hit.text);
                        }
                        page.rows = std::move(tracks);
                    }
                }
                if (!ok) break;
                page.total = searched.total;
                stream::SetOffsetNext(page, cursor, searched.received, searched.more);
                break;
            }
            case stream::ListKind::CatalogSearchTopResults: {
                // NetEase's best matches are one short list (an artist, an album) without pages.
                std::vector<SearchHit> hits;
                if (!client.SearchBestMatches(query.search, m_exec.Abort(), hits, error)) break;
                std::vector<stream::CatalogSearchItem> items;
                for (auto& hit : hits) {
                    std::visit(
                        [&](auto& row) {
                            using Row = std::decay_t<decltype(row)>;
                            if constexpr (std::is_same_v<Row, Song>) {
                                hintSongs.push_back(ToSong(row));
                                items.emplace_back(hintSongs.back().track);
                            } else if constexpr (std::is_same_v<Row, Artist>) {
                                stream::Artist artist = AccountArtist(row, Ncm().FollowedArtists());
                                Catalog::Get().Records().Put(artist);
                                items.emplace_back(std::move(artist));
                            } else if constexpr (std::is_same_v<Row, Album>) {
                                stream::Album album = ToAlbum(row);
                                Catalog::Get().Records().Put(album);
                                items.emplace_back(std::move(album));
                            } else {
                                stream::Playlist playlist = AccountPlaylist(row);
                                Catalog::Get().Records().Put(playlist);
                                items.emplace_back(std::move(playlist));
                            }
                        },
                        hit);
                }
                page.total = items.size();
                page.rows = std::move(items);
                break;
            }
            case stream::ListKind::SectionTracks:
            case stream::ListKind::Groups:
            case stream::ListKind::Playlists:
                error = L"This NetEase list is unavailable.";
                break;
            }

            if (m_exec.Abort().is_aborting()) return;
            if (cache::AccountRevision("ncm") != accountRevision) {
                snapshots->Clear();
                error = L"The NetEase account changed while the list was loading.";
                page = {};
                hintSongs.clear();
            }
            std::wstring resultError = error.empty() ? std::wstring() : ErrorOf(std::move(error));
            PostListPage(std::move(done), std::move(page), std::move(hintSongs), std::move(resultError),
                         query.cancelled);
        });
}

void StreamProvider::WriteFavorite(const Id& track, bool favorite, Done done) {
    done = stream::GuardDone(std::move(done));
    Ncm().ToggleLike(Num(track), favorite,
                     [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                         FinishListWrite(ok, std::move(error), std::move(done));
                     });
}

void StreamProvider::WriteFollowed(const Id& artist, bool follow, Done done) {
    done = stream::GuardDone(std::move(done));
    Ncm().SetArtistFollow(Num(artist), follow,
                          [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                              FinishListWrite(ok, std::move(error), std::move(done));
                          });
}

void StreamProvider::WriteAlbumSaved(const Id& album, bool saved, Done done) {
    done = stream::GuardDone(std::move(done));
    Ncm().SetAlbumCollected(Num(album), saved,
                            [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                                FinishListWrite(ok, std::move(error), std::move(done));
                            });
}

void StreamProvider::WritePlaylistSaved(const Id& playlist, bool saved, Done done) {
    done = stream::GuardDone(std::move(done));
    Ncm().SetPlaylistSubscribed(Num(playlist), saved,
                                [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                                    FinishListWrite(ok, std::move(error), std::move(done));
                                });
}

void StreamProvider::AddToPlaylist(const Id& playlist, std::vector<Id> tracks, Done done) {
    done = stream::GuardDone(std::move(done));
    Ncm().AddToPlaylist(Num(playlist), Nums(tracks),
                        [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                            FinishListWrite(ok, std::move(error), std::move(done));
                        });
}

void StreamProvider::RemoveFromPlaylist(const Id& playlist, std::vector<Id> tracks, Done done) {
    done = stream::GuardDone(std::move(done));
    Ncm().RemoveFromPlaylist(Num(playlist), Nums(tracks),
                             [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                                 FinishListWrite(ok, std::move(error), std::move(done));
                             });
}

void StreamProvider::CreatePlaylist(std::wstring name, CreatedDone done) {
    done = stream::GuardDone(std::move(done));
    Ncm().CreatePlaylist(
        std::move(name), [this, done = std::move(done)](bool ok, int64_t id, std::wstring error) mutable {
            if (ok) {
                ClearIdSnapshots();
                NotifyListChanged();
            }
            if (!done) return;
            if (ok && id)
                done(IdText(id), {});
            else
                done({}, error.empty() ? std::wstring(L"The request failed.") : std::move(error));
        });
}

void StreamProvider::RenamePlaylist(const Id& playlist, std::wstring name, Done done) {
    done = stream::GuardDone(std::move(done));
    Ncm().RenamePlaylist(Num(playlist), std::move(name),
                         [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                             FinishListWrite(ok, std::move(error), std::move(done));
                         });
}

void StreamProvider::DeletePlaylist(const Id& playlist, Done done) {
    done = stream::GuardDone(std::move(done));
    Ncm().DeletePlaylist(Num(playlist), [this, done = std::move(done)](bool ok, std::wstring error) mutable {
        FinishListWrite(ok, std::move(error), std::move(done));
    });
}

void StreamProvider::ReorderPlaylists(std::vector<Id> playlists, Done done) {
    done = stream::GuardDone(std::move(done));
    Ncm().ReorderPlaylists(Nums(playlists),
                           [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                               FinishListWrite(ok, std::move(error), std::move(done));
                           });
}

} // namespace glasswave::netease
