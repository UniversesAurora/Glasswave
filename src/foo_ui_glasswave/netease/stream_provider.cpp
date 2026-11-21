#include "pch.h"
#include "netease/stream_provider.h"
#include "core/text.h"
#include "cache/provider_snapshot.h"
#include "cache/store.h"
#include "stream/playback_source.h"
#include "netease/catalog.h"
#include "netease/config.h"
#include "netease/handle.h"
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

using core::Utf8;
using core::Wide;
using core::LowerAscii;

std::string SnapshotContext() {
    const std::array<std::string_view, 1> parts{"ncm-bootstrap-v1"};
    return cache::provider_snapshot::Context(parts);
}

std::string SourceIdentity() {
    const std::wstring external = GetServiceUrlOverride();
    return external.empty() ? "managed:ncm" : "external:" + Utf8(external);
}

int64_t Num(const Id& id) {
    return id.empty() ? 0 : static_cast<int64_t>(_wcstoi64(id.c_str(), nullptr, 10));
}

Id IdText(int64_t id) {
    return id ? std::to_wstring(id) : Id();
}

std::vector<int64_t> Nums(const std::vector<Id>& ids) {
    std::vector<int64_t> out;
    out.reserve(ids.size());
    for (const auto& id : ids)
        if (const int64_t n = Num(id)) out.push_back(n);
    return out;
}

// A publish time as the calendar day the service means. It stores release days as midnight in
// China (UTC+8), so the day is read there, not in the local time zone.
bool ReleaseDay(int64_t publishTimeMs, std::tm& day) {
    if (publishTimeMs <= 0) return false;
    const time_t seconds = static_cast<time_t>(publishTimeMs / 1000 + 8 * 3600);
    return gmtime_s(&day, &seconds) == 0;
}

int Year(int64_t publishTimeMs) {
    std::tm day{};
    return ReleaseDay(publishTimeMs, day) ? day.tm_year + 1900 : 0;
}

// "YYYY-MM-DD" of a publish time.
std::wstring Date(int64_t publishTimeMs) {
    std::tm day{};
    if (!ReleaseDay(publishTimeMs, day)) return {};
    wchar_t text[16]{};
    swprintf_s(text, L"%04d-%02d-%02d", day.tm_year + 1900, day.tm_mon + 1, day.tm_mday);
    return text;
}

// The service's album type as a UI key ("专辑" -> Album); an unknown one is shown as written.
std::wstring AlbumType(const Album& album) {
    if (album.subType.find(L"现场") != std::wstring::npos) return L"Live album";
    if (album.type == L"专辑") return L"Album";
    if (album.type == L"EP/Single") return L"EP or single";
    if (album.type == L"EP") return L"EP";
    if (album.type == L"Single") return L"Single";
    if (album.type == L"精选集" || album.type == L"合辑") return L"Compilation";
    return album.type;
}

std::vector<stream::ArtistRef> Refs(const std::vector<ArtistRef>& refs) {
    std::vector<stream::ArtistRef> out;
    out.reserve(refs.size());
    for (const auto& ref : refs)
        out.push_back({IdText(ref.id), ref.name});
    return out;
}

stream::Track ToTrack(const Song& song) {
    stream::Track track;
    track.id = IdText(song.id);
    track.title = song.title;
    track.note = song.note;
    track.album = song.album;
    track.albumId = IdText(song.albumId);
    track.artists = Refs(song.artistRefs);
    if (track.artists.empty()) // songs cached before ids were stored
        for (const auto& name : song.artists)
            track.artists.push_back({{}, name});
    track.durationMs = song.durationMs;
    track.popularity = song.popularity;
    if (song.original) track.badges.push_back(L"Original");
    return track;
}

Song FromTrack(const stream::Track& track) {
    Song song;
    song.id = Num(track.id);
    song.title = track.title;
    song.note = track.note;
    song.album = track.album;
    song.albumId = Num(track.albumId);
    song.durationMs = track.durationMs;
    song.popularity = track.popularity;
    song.original = std::find(track.badges.begin(), track.badges.end(), L"Original") != track.badges.end();
    for (const auto& ref : track.artists) {
        if (ref.name.empty()) continue;
        song.artists.push_back(ref.name);
        song.artistRefs.push_back({Num(ref.id), ref.name});
    }
    if (!song.artists.empty()) song.albumArtist = song.artists.front();
    return song;
}

std::vector<stream::Track> ToTracks(const std::vector<Song>& songs) {
    std::vector<stream::Track> out;
    out.reserve(songs.size());
    for (const auto& song : songs)
        out.push_back(ToTrack(song));
    return out;
}

std::optional<std::vector<stream::Track>> ToTracks(const std::optional<std::vector<Song>>& songs) {
    if (!songs) return std::nullopt;
    return ToTracks(*songs);
}

bool IsOwn(const Playlist& playlist) {
    const int64_t user = Ncm().CurrentAccount().userId;
    return playlist.creatorId ? playlist.creatorId == user : !playlist.subscribed;
}

stream::Playlist ToPlaylist(const Playlist& playlist) {
    stream::Playlist out;
    out.id = IdText(playlist.id);
    out.name = playlist.name;
    out.coverUrl = playlist.coverUrl;
    out.creator = playlist.creatorName;
    out.description = playlist.description;
    out.trackCount = playlist.trackCount;
    out.created = Date(playlist.createTime);
    out.updated = Date(playlist.updateTime);
    out.updates = playlist.updateFrequency;
    out.tags = playlist.tags;
    if (!playlist.englishTitle.empty() && playlist.englishTitle != playlist.name)
        out.aliases.push_back(playlist.englishTitle);
    if (playlist.chart) out.type = L"Chart";
    out.own = Ncm().FindPlaylistScope(playlist.id) == Session::PlaylistScope::Created || IsOwn(playlist);
    out.saved = !out.own && (playlist.subscribed ||
                             Ncm().FindPlaylistScope(playlist.id) == Session::PlaylistScope::Collected);
    return out;
}

Playlist FromPlaylist(const stream::Playlist& playlist) {
    Playlist out;
    out.id = Num(playlist.id);
    out.name = playlist.name;
    out.coverUrl = playlist.coverUrl;
    out.creatorName = playlist.creator;
    out.description = playlist.description;
    out.trackCount = playlist.trackCount == stream::kUnknownCount ? 0 : playlist.trackCount;
    out.subscribed = playlist.saved;
    // Catalog has no stream::Playlist boolean fields. A non-own sentinel keeps a cache
    // restoration from turning recommended/other-user lists into owned lists.
    out.creatorId = playlist.own ? Ncm().CurrentAccount().userId : -1;
    return out;
}

std::vector<stream::Playlist> ToPlaylists(const std::vector<Playlist>& playlists) {
    std::vector<stream::Playlist> out;
    out.reserve(playlists.size());
    for (const auto& playlist : playlists)
        out.push_back(ToPlaylist(playlist));
    return out;
}

stream::Album ToAlbum(const Album& album) {
    stream::Album out;
    out.id = IdText(album.id);
    out.name = album.name;
    out.coverUrl = album.coverUrl;
    out.artists = Refs(album.artistRefs);
    out.description = album.description;
    out.trackCount = album.trackCount;
    out.year = Year(album.publishTime);
    out.released = Date(album.publishTime);
    out.label = album.company;
    out.type = AlbumType(album);
    out.edition = album.subType;
    out.aliases = album.aliases;
    // Tags are one string of genre words ("华语 流行"), separated by spaces or commas.
    std::wstring tag;
    for (const wchar_t c : album.tags + L" ") {
        if (c == L' ' || c == L',' || c == L'，' || c == L'、' || c == L'/') {
            if (!tag.empty()) out.genres.push_back(std::move(tag));
            tag.clear();
        } else {
            tag += c;
        }
    }
    return out;
}

int64_t YearTimestamp(int year) {
    if (year <= 0) return 0;
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = 6;
    value.tm_mday = 1;
    value.tm_hour = 12;
    return static_cast<int64_t>(mktime(&value)) * 1000;
}

Album FromAlbum(const stream::Album& album) {
    Album out;
    out.id = Num(album.id);
    out.name = album.name;
    out.coverUrl = album.coverUrl;
    out.trackCount = album.trackCount == stream::kUnknownCount ? 0 : album.trackCount;
    out.publishTime = YearTimestamp(album.year);
    out.description = album.description;
    for (const auto& ref : album.artists) {
        if (!ref.id.empty() || !ref.name.empty()) out.artistRefs.push_back({Num(ref.id), ref.name});
    }
    return out;
}

std::vector<stream::Album> ToAlbums(const std::vector<Album>& albums) {
    std::vector<stream::Album> out;
    out.reserve(albums.size());
    for (const auto& album : albums)
        out.push_back(ToAlbum(album));
    return out;
}

stream::Artist ToArtist(const Artist& artist, const std::vector<Artist>& followed) {
    stream::Artist out;
    out.id = IdText(artist.id);
    out.name = artist.name;
    out.avatarUrl = artist.avatarUrl;
    out.bio = artist.brief;
    out.albumCount = artist.albumCount;
    out.trackCount = artist.songCount;
    out.followKnown = artist.followKnown;
    out.followed = artist.followed;
    out.profile = artist.profile;
    out.aliases = artist.aliases;
    out.roles = artist.roles;
    out.videoCount = artist.videoCount < 0 ? stream::kUnknownCount : artist.videoCount;
    // The wiki's codes, as the service's artist lists name them. Untranslated UI keys.
    switch (artist.area) {
    case 7: out.area = L"Chinese"; break;
    case 96: out.area = L"Western"; break;
    case 8: out.area = L"Japanese"; break;
    case 16: out.area = L"Korean"; break;
    default: break;
    }
    out.kind = artist.type == 1   ? stream::ArtistKind::Male
               : artist.type == 2 ? stream::ArtistKind::Female
               : artist.type == 3 ? stream::ArtistKind::Group
                                  : stream::ArtistKind::Unknown;
    // The followed-artists list is authoritative even when a profile lags behind a follow.
    for (const auto& saved : followed)
        if (saved.id == artist.id) {
            out.followKnown = out.followed = true;
            break;
        }
    return out;
}

stream::Artist ToArtist(const Artist& artist) {
    return ToArtist(artist, Ncm().FollowedArtists());
}

Artist FromArtist(const stream::Artist& artist) {
    Artist out;
    out.id = Num(artist.id);
    out.name = artist.name;
    out.avatarUrl = artist.avatarUrl;
    out.brief = artist.bio;
    out.albumCount = artist.albumCount == stream::kUnknownCount ? 0 : artist.albumCount;
    out.songCount = artist.trackCount == stream::kUnknownCount ? 0 : artist.trackCount;
    out.followKnown = artist.followKnown;
    out.followed = artist.followed;
    return out;
}

std::vector<stream::Artist> ToArtists(const std::vector<Artist>& artists) {
    std::vector<stream::Artist> out;
    out.reserve(artists.size());
    const std::vector<Artist> followed = Ncm().FollowedArtists(); // one copy, not one per artist
    for (const auto& artist : artists)
        out.push_back(ToArtist(artist, followed));
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

StreamProvider& StreamProvider::Get() {
    static StreamProvider provider;
    return provider;
}

void StreamProvider::Start() {
    if (m_started) return;
    Ncm().Initialize();
    m_abort = std::make_unique<abort_callback_impl>();
    m_listTasks = std::make_unique<core::TaskQueue>(2);
    m_idSnapshots = std::make_shared<IdSnapshotStore>();
    m_started = true;
    ++m_generation;
    m_accountVerified = false;
    m_sessionRestoring = false;
    m_snapshotSavePosted = false;
    m_snapshotSaveDirty = false;
    m_accountId.clear();
    m_cachedAccountId.clear();
    m_cachedCreated.clear();
    m_cachedSaved.clear();
    m_signedIn = Ncm().LoggedIn();
    m_authPending = m_signedIn;
    // The session reports sign-in changes and account refreshes the same way; only a
    // changed sign-in state is an account change, the rest refreshes library lists.
    Ncm().SetStateCallback([this] { OnSessionChanged(); });
    Ncm().SetLibraryCallback([this] {
        NotifyLibraryChanged();
        if (AccountVerified()) QueueSnapshotSave();
    });
    Ncm().SetLikesCallback([this] { NotifyFavoritesChanged(); });
    if (m_signedIn) LoadBootstrapSnapshot();
    if (m_signedIn) Ncm().RefreshAccount({});
}

void StreamProvider::Shutdown() {
    std::optional<cache::provider_snapshot::Snapshot> finalSnapshot;
    std::string finalAccountScope;
    uint64_t finalAccountRevision = 0;
    {
        cache::provider_snapshot::Snapshot payload;
        bool save = false;
        std::wstring accountId;
        {
            std::lock_guard lock(m_cacheMutex);
            save = m_started && m_accountVerified && !m_accountId.empty();
            if (save) accountId = m_accountId;
        }
        const Account account = Ncm().CurrentAccount();
        if (save) {
            try {
                payload.provider = "ncm";
                payload.sourceOrigin = SourceIdentity();
                payload.context = SnapshotContext();
                payload.locale = ui::UsingChinese() ? "zh-Hans-CN" : "en-US";
                payload.accountId = Utf8(accountId);
                payload.accountName = Utf8(account.nickname);
                payload.sourceStorefront = "CN";
                payload.created = ToPlaylists(Ncm().CreatedPlaylists());
                payload.saved = ToPlaylists(Ncm().CollectedPlaylists());
                finalAccountScope = cache::AccountScope("ncm");
                finalAccountRevision = cache::AccountRevision("ncm");
                finalSnapshot = std::move(payload);
            } catch (...) {
            }
        }
    }

    m_started = false;
    Catalog::Get().FlushPendingSave();
    ClearIdSnapshots();
    if (m_abort) m_abort->abort();
    if (m_listTasks) {
        m_listTasks->Stop();
        m_listTasks.reset();
    }
    m_abort.reset();
    m_idSnapshots.reset();
    Ncm().SetStateCallback({});
    Ncm().SetLibraryCallback({});
    Ncm().SetLikesCallback({});
    Ncm().Shutdown(); // stops the helper and its workers
    {
        std::lock_guard lock(m_cacheMutex);
        ++m_generation;
        m_accountVerified = false;
        m_authPending = false;
        m_sessionRestoring = false;
        m_snapshotSavePosted = false;
        m_snapshotSaveDirty = false;
        m_accountId.clear();
        m_cachedAccountId.clear();
        m_cachedCreated.clear();
        m_cachedSaved.clear();
    }
    if (finalSnapshot) {
        try {
            if (cache::AccountRevision("ncm") == finalAccountRevision &&
                cache::AccountScope("ncm") == finalAccountScope) {
                cache::provider_snapshot::Save(cache::GetStore(), finalAccountScope, *finalSnapshot);
            }
        } catch (...) {
        }
    }
}

void StreamProvider::LoadBootstrapSnapshot() {
    std::string accountScope;
    std::string sourceOrigin;
    try {
        accountScope = cache::AccountScope("ncm");
        sourceOrigin = SourceIdentity();
    } catch (...) {
        return;
    }
    const std::string context = SnapshotContext();
    const auto cached =
        cache::provider_snapshot::Load(cache::GetStore(), accountScope, "ncm", sourceOrigin, context);
    if (!cached || cached->accountId.empty()) return;
    std::wstring accountId = Wide(cached->accountId);
    if (accountId.empty()) return;

    {
        std::lock_guard lock(m_cacheMutex);
        if (!m_started || !m_signedIn || m_accountVerified) return;
        m_cachedAccountId = std::move(accountId);
        m_cachedCreated = cached->created;
        m_cachedSaved = cached->saved;
        m_sessionRestoring = true;
    }
    for (const auto& playlist : cached->created)
        Catalog::Get().Restore(FromPlaylist(playlist));
    for (const auto& playlist : cached->saved)
        Catalog::Get().Restore(FromPlaylist(playlist));
    NotifyLibraryChanged();
}

void StreamProvider::QueueSnapshotSave() {
    cache::provider_snapshot::Snapshot payload;
    uint64_t generation = 0;
    uint64_t accountRevision = 0;
    std::string accountScope;
    std::wstring accountId;
    {
        std::lock_guard lock(m_cacheMutex);
        if (!m_started || !m_accountVerified || m_accountId.empty() || m_sessionRestoring) return;
        m_snapshotSaveDirty = true;
        if (m_snapshotSavePosted) return;
        m_snapshotSavePosted = true;
        m_snapshotSaveDirty = false;
        generation = m_generation;
        accountId = m_accountId;
    }
    const Account account = Ncm().CurrentAccount();
    try {
        accountScope = cache::AccountScope("ncm");
        accountRevision = cache::AccountRevision("ncm");
        payload.provider = "ncm";
        payload.sourceOrigin = SourceIdentity();
        payload.context = SnapshotContext();
        payload.locale = ui::UsingChinese() ? "zh-Hans-CN" : "en-US";
        payload.accountId = Utf8(accountId);
        payload.accountName = Utf8(account.nickname);
        payload.sourceStorefront = "CN";
        payload.created = ToPlaylists(Ncm().CreatedPlaylists());
        payload.saved = ToPlaylists(Ncm().CollectedPlaylists());
    } catch (...) {
        std::lock_guard lock(m_cacheMutex);
        if (generation == m_generation) m_snapshotSavePosted = false;
        return;
    }
    if (m_listTasks) {
        m_listTasks->Post([this, generation, accountRevision, accountScope = std::move(accountScope),
                           payload = std::move(payload)]() mutable {
            bool allowed = false;
            {
                std::lock_guard lock(m_cacheMutex);
                allowed = m_started && generation == m_generation && m_accountVerified &&
                          cache::AccountRevision("ncm") == accountRevision;
            }
            if (allowed) {
                try {
                    if (cache::AccountScope("ncm") == accountScope)
                        cache::provider_snapshot::Save(cache::GetStore(), accountScope, payload);
                } catch (...) {
                }
            }
            fb2k::inMainThread([this, generation] { CompleteSnapshotSave(generation); });
        });
    } else {
        std::lock_guard lock(m_cacheMutex);
        m_snapshotSavePosted = false;
    }
}

void StreamProvider::CompleteSnapshotSave(uint64_t generation) {
    bool rerun = false;
    {
        std::lock_guard lock(m_cacheMutex);
        if (generation != m_generation) return;
        m_snapshotSavePosted = false;
        rerun = m_snapshotSaveDirty && m_started && m_accountVerified && !m_sessionRestoring;
        if (!rerun) m_snapshotSaveDirty = false;
    }
    if (rerun) QueueSnapshotSave();
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

namespace {

// What a completion hears when its request is dropped (shutdown, sign-out).
constexpr wchar_t kRequestDropped[] = L"NetEase Cloud Music stopped before the request finished.";

} // namespace

void StreamProvider::FetchCatalogSearchSuggestions(CatalogSearchSuggestionQuery query,
                                                   CatalogSearchSuggestionsDone done) {
    if (ListCancelled(query.cancelled)) return;
    done = stream::GuardDone(std::move(done), kRequestDropped, query.cancelled);
    if (query.term.empty() || !m_started || !m_listTasks || !m_abort) {
        if (done) done({}, {});
        return;
    }
    m_listTasks->Post([this, query = std::move(query), done = std::move(done)]() mutable {
        if (ListCancelled(query.cancelled) || m_abort->is_aborting()) return;
        std::wstring error;
        SearchSuggestions found;
        if (Ncm().Service().EnsureStarted(*m_abort, &error))
            Ncm().Api().SearchSuggest(query.term, *m_abort, found, error);
        if (m_abort->is_aborting() || ListCancelled(query.cancelled)) return;
        CatalogSearchSuggestions suggestions;
        for (auto& term : found.terms)
            suggestions.ordered.emplace_back(CatalogSearchSuggestionTerm{term, term});
        // The suggested songs are summaries (no cover, no quality); they are shown, and opened
        // through the song page, but never written over a known song's record.
        for (auto& hit : found.items) {
            std::visit(
                [&](auto& row) {
                    using Row = std::decay_t<decltype(row)>;
                    if constexpr (std::is_same_v<Row, Song>)
                        suggestions.ordered.emplace_back(ToTrack(row));
                    else if constexpr (std::is_same_v<Row, Artist>)
                        suggestions.ordered.emplace_back(ToArtist(row));
                    else if constexpr (std::is_same_v<Row, Album>)
                        suggestions.ordered.emplace_back(ToAlbum(row));
                    else
                        suggestions.ordered.emplace_back(ToPlaylist(row));
                },
                hit);
        }
        if (!suggestions.ordered.empty()) error.clear();
        fb2k::inMainThread([this, cancelled = query.cancelled, done = std::move(done), found = std::move(found),
                            suggestions = std::move(suggestions),
                            error = error.empty() ? std::wstring() : ErrorOf(std::move(error))]() mutable {
            if (!m_started || ListCancelled(cancelled)) return;
            // Opening a suggested artist, album or playlist finds it in the catalog.
            for (const auto& hit : found.items)
                std::visit(
                    [](const auto& row) {
                        if constexpr (!std::is_same_v<std::decay_t<decltype(row)>, Song>) Catalog::Get().Put(row);
                    },
                    hit);
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
    done = stream::GuardDone(std::move(done), kRequestDropped);
    if (!m_started || !m_listTasks || !m_abort) {
        if (done) done({}, L"NetEase Cloud Music is not running.");
        return;
    }
    m_listTasks->Post([this, done = std::move(done)]() mutable {
        if (m_abort->is_aborting()) return;
        std::wstring error;
        std::vector<HomeCard> cards;
        std::vector<HomeBlock> blocks;
        if (Ncm().Service().EnsureStarted(*m_abort, &error)) {
            std::wstring blockError;
            if (!Ncm().Api().HomeCards(*m_abort, cards, error)) cards.clear();
            if (!Ncm().Api().HomeBlocks(*m_abort, blocks, blockError) && error.empty()) error = blockError;
        }
        if (m_abort->is_aborting()) return;
        if (!cards.empty() || !blocks.empty()) error.clear();
        // Playlists the cards and rows name are known before anything opens them; a summary never
        // replaces a richer record.
        for (const auto& card : cards)
            if (card.resourceType == L"playList" && !card.resourceId.empty()) {
                Playlist playlist;
                playlist.id = _wtoi64(card.resourceId.c_str());
                playlist.name = card.label;
                playlist.coverUrl = card.coverUrl;
                playlist.creatorId = -1;
                if (playlist.id) Catalog::Get().Restore(playlist);
            }
        for (auto& block : blocks)
            for (auto& playlist : block.playlists) {
                playlist.creatorId = -1;
                Catalog::Get().Restore(playlist);
            }
        stream::Home home;
        for (const auto& card : cards)
            if (auto mapped = ToHomeCard(card)) home.cards.push_back(std::move(*mapped));
        std::vector<Song> songs;
        for (const auto& block : blocks) {
            stream::HomeShelf shelf;
            shelf.id = block.code;
            shelf.title = block.title;
            for (const auto& playlist : block.playlists) {
                const auto known = Catalog::Get().FindPlaylist(playlist.id);
                shelf.items.emplace_back(ToPlaylist(known ? *known : playlist));
            }
            for (const auto& song : block.songs) {
                shelf.items.emplace_back(ToTrack(song));
                songs.push_back(song);
                // The rows show a song by its album's cover: the album must be known with it.
                if (song.albumId && !song.coverUrl.empty()) {
                    Album album;
                    album.id = song.albumId;
                    album.name = song.album;
                    album.coverUrl = song.coverUrl;
                    Catalog::Get().Restore(album);
                }
            }
            if (!shelf.items.empty()) home.shelves.push_back(std::move(shelf));
        }
        fb2k::inMainThread([this, done = std::move(done), home = std::move(home), songs = std::move(songs),
                            error = error.empty() ? std::wstring() : ErrorOf(std::move(error))]() mutable {
            if (!m_started) return;
            if (!songs.empty()) Catalog::Get().PutAndHint(songs);
            if (done) done(std::move(home), std::move(error));
        });
    });
}

// The stations: the personal FM (私人漫游, three songs a call, 不感兴趣 for its songs) and heartbeat
// mode (心动模式: songs like the last one, from and around the liked songs).
void StreamProvider::FetchStationTracks(const stream::Id& station, TracksDone done) {
    done = stream::GuardDone(std::move(done), kRequestDropped);
    if (!m_started || !m_listTasks || !m_abort ||
        (station != kPersonalFm && station != kHeartbeat && !station.starts_with(kSimilar))) {
        if (done) done({}, L"This station is unavailable.");
        return;
    }
    m_listTasks->Post([this, station, done = std::move(done)]() mutable {
        if (m_abort->is_aborting()) return;
        std::wstring error;
        std::vector<Song> songs;
        Client& client = Ncm().Api();
        if (!Ncm().LoggedIn()) {
            error = L"Please sign in to NetEase Cloud Music.";
        } else if (Ncm().Service().EnsureStarted(*m_abort, &error)) {
            if (station == kPersonalFm) {
                client.PersonalFm(*m_abort, songs, error);
            } else if (station.starts_with(kSimilar)) {
                client.FmMode(L"FAMILIAR", std::wstring_view(station).substr(kSimilar.size()), *m_abort, songs, error);
            } else {
                // The first seed is one of the liked songs; later ones, the last song played.
                int64_t seed = m_heartbeatSeed.load();
                if (!seed) {
                    std::vector<int64_t> liked;
                    if (client.Likelist(Ncm().CurrentAccount().userId, *m_abort, liked, error) && !liked.empty())
                        seed = liked[GetTickCount64() % std::min<size_t>(liked.size(), 50)];
                }
                if (seed && client.Heartbeat(seed, Ncm().LikedPlaylistId(), *m_abort, songs, error) && !songs.empty())
                    m_heartbeatSeed = songs.back().id;
            }
        }
        if (m_abort->is_aborting()) return;
        fb2k::inMainThread([this, done = std::move(done), songs = std::move(songs),
                            error = error.empty() ? std::wstring() : ErrorOf(std::move(error))]() mutable {
            if (!m_started) return;
            if (!songs.empty()) Catalog::Get().PutAndHint(songs);
            if (done) done(ToTracks(songs), songs.empty() ? std::move(error) : std::wstring());
        });
    });
}

// The FM's songs (its own and the similar-song mixes) take 不感兴趣.
bool StreamProvider::CanDislike(const stream::Id& station) const {
    return station == kPersonalFm || station.starts_with(kSimilar);
}

void StreamProvider::DislikeStationTrack(const stream::Id& station, const stream::Id& track, Done done) {
    done = stream::GuardDone(std::move(done), kRequestDropped);
    if (!CanDislike(station) || !m_started || !m_listTasks || !m_abort) {
        if (done) done(L"This station does not take feedback.");
        return;
    }
    m_listTasks->Post([this, id = Num(track), done = std::move(done)]() mutable {
        if (m_abort->is_aborting()) return;
        std::wstring error;
        if (Ncm().Service().EnsureStarted(*m_abort, &error)) Ncm().Api().FmTrash(id, *m_abort, error);
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
    return m_accountVerified;
}

bool StreamProvider::SessionRestoring() const {
    std::lock_guard lock(m_cacheMutex);
    return m_sessionRestoring || m_authPending;
}

bool StreamProvider::AccountVerified() const {
    std::lock_guard lock(m_cacheMutex);
    return m_accountVerified;
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
            m_authPending = true;
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
        const bool restoring = m_sessionRestoring;
        const bool verified = m_accountVerified;
        if (!signedIn || accountId.empty()) {
            accountChanged = restoring || verified || m_signedIn != signedIn;
        } else if (restoring && accountId != m_cachedAccountId) {
            accountChanged = true;
        } else if (verified && accountId != m_accountId) {
            accountChanged = true;
        }
        if (accountChanged) {
            ++m_generation;
            m_accountVerified = false;
            m_authPending = signedIn && accountId.empty();
            m_sessionRestoring = false;
            m_snapshotSavePosted = false;
            m_snapshotSaveDirty = false;
            m_accountId.clear();
            m_cachedAccountId.clear();
            m_cachedCreated.clear();
            m_cachedSaved.clear();
        } else if (signedIn) {
            accountObserved = !verified || restoring;
            m_accountVerified = true;
            m_authPending = false;
            m_sessionRestoring = false;
            m_accountId = accountId;
            m_cachedAccountId.clear();
            m_cachedCreated.clear();
            m_cachedSaved.clear();
        }
        m_signedIn = signedIn;
    }
    if (accountChanged) {
        ClearIdSnapshots();
        cache::RotateAccountScope("ncm");
        stream::ResetSourceAvailability();
        NotifyAccountChanged();
        return;
    }
    if (accountObserved) NotifyAccountObserved();
    NotifyLibraryChanged();
    if (signedIn) QueueSnapshotSave();
}

void StreamProvider::SignOut() {
    ClearIdSnapshots();
    cache::RotateAccountScope("ncm");
    Ncm().Logout();
}

void StreamProvider::RefreshLibrary(Done done) {
    Ncm().RefreshAccount([this, done = std::move(done)](bool ok, std::wstring error) mutable {
        FinishListWrite(ok, std::move(error), std::move(done));
    });
}

stream::Id StreamProvider::TrackId(const metadb_handle_ptr& item) const {
    int64_t id = 0;
    return item.is_valid() && ParseSongPath(item->get_path(), id) ? IdText(id) : Id();
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
    Song song;
    if (session.Api().GetSongInfo(id, abort, info, ignored)) {
        song = info.song;
        source.tags.disc = info.disc;
        source.tags.number = info.number;
        source.tags.date = Date(info.publishTime); // the same day the pages show
        source.tags.genres = info.styles;
    } else if (auto known = Catalog::Get().FindSong(id)) {
        song = *known;
    }
    source.tags.title = song.title;
    source.tags.album = song.album;
    source.tags.albumArtist = song.albumArtist;
    source.tags.artists = song.artists;
    source.tags.coverUrl = song.coverUrl.substr(0, song.coverUrl.find(L'?')); // the full-size picture

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

metadb_handle_ptr StreamProvider::TrackItem(const Id& track) const {
    const int64_t id = Num(track);
    return id ? Catalog::Get().Handle(id) : metadb_handle_ptr();
}

std::optional<stream::Track> StreamProvider::FindTrack(const Id& track) const {
    if (auto song = Catalog::Get().FindSong(Num(track))) return ToTrack(*song);
    return std::nullopt;
}

metadb_handle_ptr StreamProvider::ArtItem(stream::ArtKind kind, const Id& id) const {
    const int64_t n = Num(id);
    if (!n) return {};
    switch (kind) {
    case stream::ArtKind::Track:
        return Catalog::Get().Handle(n);
    case stream::ArtKind::Album:
        return Catalog::Get().AlbumHandle(n);
    case stream::ArtKind::Artist:
        return Catalog::Get().ArtistHandle(n);
    case stream::ArtKind::Playlist:
        return Catalog::Get().PlaylistHandle(n);
    case stream::ArtKind::Station: // its stations are cards (personal FM, heartbeat), never tiles
        return {};
    }
    return {};
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
        auto rows = ToPlaylists(Ncm().CreatedPlaylists());
        if (rows.empty()) {
            std::lock_guard lock(m_cacheMutex);
            if (m_sessionRestoring) return m_cachedCreated;
        }
        return rows;
    }
    case stream::Shelf::Saved: {
        auto rows = ToPlaylists(Ncm().CollectedPlaylists());
        if (rows.empty()) {
            std::lock_guard lock(m_cacheMutex);
            if (m_sessionRestoring) return m_cachedSaved;
        }
        return rows;
    }
    case stream::Shelf::Recommended:
        return ToPlaylists(Ncm().RecommendedPlaylists());
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
    return ToArtists(Ncm().FollowedArtists());
}

std::vector<stream::Album> StreamProvider::SavedAlbums() const {
    return ToAlbums(Ncm().CollectedAlbums());
}

std::optional<stream::Playlist> StreamProvider::FindPlaylist(const Id& playlist) const {
    if (auto found = Catalog::Get().FindPlaylist(Num(playlist))) return ToPlaylist(*found);
    return std::nullopt;
}

std::optional<stream::Album> StreamProvider::FindAlbum(const Id& album) const {
    if (auto found = Catalog::Get().FindAlbum(Num(album))) return ToAlbum(*found);
    return std::nullopt;
}

void StreamProvider::FetchArtist(const Id& artist, ArtistDone done) {
    Ncm().FetchArtistDetail(Num(artist), [done = std::move(done)](Artist info, std::wstring error) {
        done(info.id ? ToArtist(info) : stream::Artist{}, std::move(error));
    });
}

void StreamProvider::FetchArtistBio(const Id& artist, TextDone done) {
    // The profile's brief is a summary; /artist/desc has the full article when it exists.
    Ncm().FetchArtistDescription(
        Num(artist), [done = std::move(done)](std::wstring brief, std::wstring full, std::wstring error) {
            done(full.empty() ? std::move(brief) : std::move(full), std::move(error));
        });
}

// The song's detail and wiki, and the wiki's similar songs in full (so they play like any row).
void StreamProvider::FetchSongDetails(const Id& track, SongDetailsDone done) {
    done = stream::GuardDone(std::move(done), kRequestDropped);
    if (!m_started || !m_listTasks || !m_abort) {
        if (done) done({}, L"The NetEase provider has not started.");
        return;
    }
    m_listTasks->Post([this, track, done = std::move(done)]() mutable {
        stream::SongDetails details;
        std::vector<Song> songs;
        std::wstring error;
        if (Ncm().Service().EnsureStarted(*m_abort, &error)) {
            Client& client = Ncm().Api();
            SongInfo info;
            if (client.GetSongInfo(Num(track), *m_abort, info, error)) {
                details.track = ToTrack(info.song);
                details.disc = info.disc;
                details.number = info.number;
                details.released = Date(info.publishTime);
                details.aliases = info.aliases;
                details.version = info.originCoverType == 1   ? L"Original"
                                  : info.originCoverType == 2 ? L"Cover"
                                                              : L"";
                if (info.hiRes) details.traits.push_back(L"Hi-Res Lossless");
                else if (info.lossless) details.traits.push_back(L"Lossless");
                details.genres = info.styles;
                details.tags = info.tags;
                details.language = info.language;
                details.bpm = info.bpm;
                songs.push_back(info.song);
                std::vector<Song> similar;
                std::wstring ignored;
                if (!info.similar.empty() && SongDetailsInBatches(client, Unique(info.similar), *m_abort, similar, ignored)) {
                    details.similar = ToTracks(OrderDetails(info.similar, similar));
                    songs.insert(songs.end(), similar.begin(), similar.end());
                }
            }
        }
        if (m_abort->is_aborting()) return;
        fb2k::inMainThread([this, done = std::move(done), details = std::move(details), songs = std::move(songs),
                            error = ErrorOf(std::move(error))]() mutable {
            if (!m_started) return;
            if (!songs.empty()) Catalog::Get().PutAndHint(songs);
            if (!details.track.id.empty()) error.clear();
            if (done) done(std::move(details), std::move(error));
        });
    });
}

void StreamProvider::RestoreListRecords(const stream::ListPage& page) {
    if (const auto trackRows = std::get_if<std::vector<stream::Track>>(&page.rows)) {
        std::vector<Song> songs;
        songs.reserve(trackRows->size());
        for (const auto& track : *trackRows) {
            Song song = FromTrack(track);
            if (song.id) songs.push_back(std::move(song));
        }
        Catalog::Get().RestoreAndHint(songs);
    } else if (const auto playlistRows = std::get_if<std::vector<stream::Playlist>>(&page.rows)) {
        for (const auto& playlist : *playlistRows)
            Catalog::Get().Restore(FromPlaylist(playlist));
    } else if (const auto albumRows = std::get_if<std::vector<stream::Album>>(&page.rows)) {
        for (const auto& album : *albumRows)
            Catalog::Get().Restore(FromAlbum(album));
    } else if (const auto artistRows = std::get_if<std::vector<stream::Artist>>(&page.rows)) {
        for (const auto& artist : *artistRows)
            Catalog::Get().Restore(FromArtist(artist));
    }
    if (page.playlist) Catalog::Get().Restore(FromPlaylist(*page.playlist));
    if (page.album) Catalog::Get().Restore(FromAlbum(*page.album));
    if (page.artist) Catalog::Get().Restore(FromArtist(*page.artist));
}

void StreamProvider::PostListPage(stream::ListPageDone done, stream::ListPage page, std::vector<Song> songs,
                                  std::wstring error, std::shared_ptr<std::atomic_bool> cancelled) {
    if (ListCancelled(cancelled)) return;
    fb2k::inMainThread([this, done = std::move(done), page = std::move(page), songs = std::move(songs),
                        error = std::move(error), cancelled = std::move(cancelled)]() mutable {
        if (!m_started || ListCancelled(cancelled)) return;
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
    done = stream::GuardDone(std::move(done), kRequestDropped, query.cancelled);
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
    if (!m_started || !m_listTasks || !m_abort) {
        PostListPage(std::move(done), {}, {}, L"The NetEase provider has not started.", query.cancelled);
        return;
    }
    m_listTasks->Post([this, query, cursor, limit, done = std::move(done)]() mutable {
        if (ListCancelled(query.cancelled) || m_abort->is_aborting()) return;
        std::wstring error;
        if (!Ncm().Service().EnsureStarted(*m_abort, &error)) {
            PostListPage(std::move(done), {}, {}, ErrorOf(std::move(error)), query.cancelled);
            return;
        }
        if (ListCancelled(query.cancelled)) return;

        Client& client = Ncm().Api();
        const auto snapshots = m_idSnapshots;
        if (!snapshots) {
            PostListPage(std::move(done), {}, {}, L"The NetEase list cache is unavailable.", query.cancelled);
            return;
        }
        const uint64_t accountRevision = cache::AccountRevision("ncm");
        stream::ListPage page;
        std::vector<Song> hintSongs;

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
            IdSnapshotStore::Key key{accountRevision, IdSnapshotStore::Kind::Playlist, playlistId,
                                     cursor.value};
            std::shared_ptr<const IdSnapshotStore::Snapshot> snapshot;
            if (cursor.offset != 0) snapshot = snapshots->Find(key);
            if (!snapshot) {
                PlaylistSnapshot fresh;
                if (!client.GetPlaylistSnapshot(playlistId, *m_abort, fresh, error)) break;
                key.revision = IdsRevision(accountRevision, fresh.trackIds, fresh.trackUpdateTime);
                if (cursor.offset != 0 && cursor.value != key.revision) {
                    error = L"The playlist changed while its pages were being loaded. Reload the list.";
                    break;
                }
                snapshot = snapshots->Put(std::move(key), std::move(fresh.trackIds), std::move(fresh.info));
            }
            if (!snapshot) {
                error = L"The playlist snapshot could not be retained.";
                break;
            }
            if (cursor.offset > snapshot->ids.size()) {
                error = L"The playlist cursor is outside the current track list.";
                break;
            }
            const size_t count = std::min({limit, snapshot->ids.size() - cursor.offset, kSnapshotPageLimit});
            const auto begin = snapshot->ids.begin() + static_cast<std::ptrdiff_t>(cursor.offset);
            std::vector<int64_t> ids(begin, begin + static_cast<std::ptrdiff_t>(count));
            std::vector<Song> details;
            if (!SongDetailsInBatches(client, Unique(ids), *m_abort, details, error)) break;
            hintSongs = OrderDetails(ids, details);
            page.rows = ToTracks(hintSongs);
            page.playlist = ToPlaylist(snapshot->info);
            page.total = snapshot->ids.size();
            page.next = {snapshot->key.revision, cursor.offset + count};
            page.more = cursor.offset + count < snapshot->ids.size();
            page.revision = snapshot->key.revision;
            Catalog::Get().Put(snapshot->info);
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
            IdSnapshotStore::Key key{accountRevision, IdSnapshotStore::Kind::Favorites, 0, cursor.value};
            std::shared_ptr<const IdSnapshotStore::Snapshot> snapshot;
            if (cursor.offset != 0) snapshot = snapshots->Find(key);
            if (!snapshot) {
                std::vector<int64_t> ids;
                if (!client.Likelist(userId, *m_abort, ids, error)) break;
                key.revision = IdsRevision(accountRevision, ids);
                if (cursor.offset != 0 && cursor.value != key.revision) {
                    error = L"Favorites changed while their pages were being loaded. Reload the list.";
                    break;
                }
                snapshot = snapshots->Put(std::move(key), std::move(ids));
            }
            if (!snapshot) {
                error = L"The favorites snapshot could not be retained.";
                break;
            }
            if (cursor.offset > snapshot->ids.size()) {
                error = L"The favorites cursor is outside the current track list.";
                break;
            }
            const size_t count = std::min({limit, snapshot->ids.size() - cursor.offset, kSnapshotPageLimit});
            const auto begin = snapshot->ids.begin() + static_cast<std::ptrdiff_t>(cursor.offset);
            std::vector<int64_t> selected(begin, begin + static_cast<std::ptrdiff_t>(count));
            std::vector<Song> details;
            if (!SongDetailsInBatches(client, Unique(selected), *m_abort, details, error)) break;
            hintSongs = OrderDetails(selected, details);
            page.rows = ToTracks(hintSongs);
            page.total = snapshot->ids.size();
            page.next = {snapshot->key.revision, cursor.offset + count};
            page.more = cursor.offset + count < snapshot->ids.size();
            page.revision = snapshot->key.revision;
            break;
        }
        case stream::ListKind::HomeList: {
            stream::Playlist info;
            info.id = query.id;
            if (query.id == kDailyList) {
                info.name = L"每日推荐";
                info.tagline = L"根据你的音乐口味生成，每天6:00更新";
                client.DailySongs(*m_abort, hintSongs, error);
            } else if (query.id.starts_with(kStyleList)) {
                std::vector<std::wstring> parts;
                std::wstring_view rest = std::wstring_view(query.id).substr(kStyleList.size());
                for (size_t i = 0; i < 2; ++i) {
                    const size_t colon = rest.find(L':');
                    parts.emplace_back(rest.substr(0, colon));
                    rest = colon == std::wstring_view::npos ? std::wstring_view() : rest.substr(colon + 1);
                }
                info.tagline = L"根据你的音乐口味生成，每天更新";
                client.StyleDailySongs(parts[0], parts[1], rest, *m_abort, hintSongs, error);
            } else {
                error = L"This list is unavailable.";
                break;
            }
            page.rows = ToTracks(hintSongs);
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
            if (!client.GetAlbum(albumId, *m_abort, info, hintSongs, error)) break;
            page.rows = ToTracks(hintSongs);
            page.album = ToAlbum(info);
            page.total = hintSongs.size();
            Catalog::Get().Put(info);
            break;
        }
        case stream::ListKind::ArtistTopTracks: {
            const int64_t artistId = Num(query.id);
            int offset = 0, count = 0;
            if (!artistId) {
                error = L"The artist id is invalid.";
                break;
            }
            if (!ToInt(cursor.offset, offset, error) || !ToInt(limit, count, error)) break;
            bool more = false;
            size_t received = 0;
            if (!client.GetArtistSongsPage(artistId, offset, count, L"hot", *m_abort, hintSongs, &more, error,
                                           &received))
                break;
            page.rows = ToTracks(hintSongs);
            page.next = {{}, cursor.offset + received};
            page.more = more && received != 0;
            break;
        }
        case stream::ListKind::ArtistAlbums: {
            const int64_t artistId = Num(query.id);
            int offset = 0, count = 0;
            if (!artistId) {
                error = L"The artist id is invalid.";
                break;
            }
            if (!ToInt(cursor.offset, offset, error) || !ToInt(limit, count, error)) break;
            std::vector<Album> albums;
            bool more = false;
            size_t received = 0;
            if (!client.GetArtistAlbumsPage(artistId, offset, count, *m_abort, albums, nullptr, &more, error,
                                            &received))
                break;
            Catalog::Get().Put(albums);
            page.rows = ToAlbums(albums);
            page.next = {{}, cursor.offset + received};
            page.more = more && received != 0;
            break;
        }
        case stream::ListKind::SavedAlbums: {
            int offset = 0, count = 0;
            if (!ToInt(cursor.offset, offset, error) || !ToInt(limit, count, error)) break;
            std::vector<Album> albums;
            bool more = false;
            size_t received = 0;
            if (!client.GetCollectedAlbumsPage(offset, count, *m_abort, albums, more, error, &received))
                break;
            Catalog::Get().Put(albums);
            page.rows = ToAlbums(albums);
            page.next = {{}, cursor.offset + received};
            page.more = more && received != 0;
            break;
        }
        case stream::ListKind::ArtistSimilarArtists: {
            const int64_t artistId = Num(query.id);
            if (!artistId) {
                error = L"The artist id is invalid.";
                break;
            }
            std::vector<Artist> artists;
            if (!client.GetSimilarArtists(artistId, *m_abort, artists, error)) break;
            for (const auto& artist : artists)
                Catalog::Get().Put(artist);
            page.rows = ToArtists(artists);
            page.total = artists.size();
            break;
        }
        case stream::ListKind::FollowedArtists: {
            int offset = 0, count = 0;
            if (!ToInt(cursor.offset, offset, error) || !ToInt(limit, count, error)) break;
            std::vector<Artist> artists;
            bool more = false;
            size_t received = 0;
            if (!client.GetFollowedArtistsPage(offset, count, *m_abort, artists, more, received, error))
                break;
            for (const auto& artist : artists)
                Catalog::Get().Put(artist);
            page.rows = ToArtists(artists);
            page.next = {{}, cursor.offset + received};
            page.more = more && received != 0;
            break;
        }
        case stream::ListKind::CatalogSearch:
        case stream::ListKind::CatalogSearchAlbums:
        case stream::ListKind::CatalogSearchArtists:
        case stream::ListKind::CatalogSearchPlaylists:
        case stream::ListKind::CatalogSearchLyrics: {
            int offset = 0, count = 0;
            if (!ToInt(cursor.offset, offset, error) || !ToInt(limit, count, error)) break;
            SearchPage found;
            bool ok = false;
            if (query.kind == stream::ListKind::CatalogSearch) {
                ok = client.SearchSongsPage(query.search, count, offset, *m_abort, hintSongs, found, error);
                if (ok) page.rows = ToTracks(hintSongs);
            } else if (query.kind == stream::ListKind::CatalogSearchAlbums) {
                std::vector<Album> albums;
                ok = client.SearchAlbumsPage(query.search, count, offset, *m_abort, albums, found, error);
                if (ok) {
                    Catalog::Get().Put(albums);
                    page.rows = ToAlbums(albums);
                }
            } else if (query.kind == stream::ListKind::CatalogSearchArtists) {
                std::vector<Artist> artists;
                ok = client.SearchArtistsPage(query.search, count, offset, *m_abort, artists, found, error);
                if (ok) {
                    for (const auto& artist : artists)
                        Catalog::Get().Put(artist);
                    page.rows = ToArtists(artists);
                }
            } else if (query.kind == stream::ListKind::CatalogSearchPlaylists) {
                std::vector<Playlist> playlists;
                ok = client.SearchPlaylistsPage(query.search, count, offset, *m_abort, playlists, found, error);
                if (ok) {
                    for (const auto& playlist : playlists)
                        Catalog::Get().Put(playlist);
                    page.rows = ToPlaylists(playlists);
                }
            } else {
                std::vector<LyricHit> hits;
                ok = client.SearchLyricsPage(query.search, count, offset, *m_abort, hits, found, error);
                if (ok) {
                    std::vector<stream::Track> tracks;
                    for (auto& hit : hits) {
                        tracks.push_back(ToTrack(hit.song));
                        tracks.back().lyricMatch = std::move(hit.excerpt);
                        tracks.back().lyricText = std::move(hit.text);
                        hintSongs.push_back(std::move(hit.song));
                    }
                    page.rows = std::move(tracks);
                }
            }
            if (!ok) break;
            page.total = found.total;
            page.next = {{}, cursor.offset + found.received};
            page.more = found.more && found.received != 0;
            break;
        }
        case stream::ListKind::CatalogSearchTopResults: {
            // NetEase's best matches are one short list (an artist, an album) without pages.
            std::vector<SearchHit> hits;
            if (!client.SearchBestMatches(query.search, *m_abort, hits, error)) break;
            std::vector<stream::CatalogSearchItem> items;
            for (auto& hit : hits) {
                std::visit(
                    [&](auto& row) {
                        using Row = std::decay_t<decltype(row)>;
                        if constexpr (std::is_same_v<Row, Song>) {
                            items.emplace_back(ToTrack(row));
                            hintSongs.push_back(std::move(row));
                        } else if constexpr (std::is_same_v<Row, Artist>) {
                            Catalog::Get().Put(row);
                            items.emplace_back(ToArtist(row));
                        } else if constexpr (std::is_same_v<Row, Album>) {
                            Catalog::Get().Put(row);
                            items.emplace_back(ToAlbum(row));
                        } else {
                            Catalog::Get().Put(row);
                            items.emplace_back(ToPlaylist(row));
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

        if (m_abort->is_aborting()) return;
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
    Ncm().ToggleLike(Num(track), favorite,
                     [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                         FinishListWrite(ok, std::move(error), std::move(done));
                     });
}

void StreamProvider::SetFollowed(const Id& artist, bool follow, Done done) {
    Ncm().SetArtistFollow(Num(artist), follow,
                          [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                              FinishListWrite(ok, std::move(error), std::move(done));
                          });
}

void StreamProvider::SetAlbumSaved(const Id& album, bool saved, Done done) {
    Ncm().SetAlbumCollected(Num(album), saved,
                            [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                                FinishListWrite(ok, std::move(error), std::move(done));
                            });
}

void StreamProvider::SetPlaylistSaved(const Id& playlist, bool saved, Done done) {
    Ncm().SetPlaylistSubscribed(Num(playlist), saved,
                                [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                                    FinishListWrite(ok, std::move(error), std::move(done));
                                });
}

void StreamProvider::AddToPlaylist(const Id& playlist, std::vector<Id> tracks, Done done) {
    Ncm().AddToPlaylist(Num(playlist), Nums(tracks),
                        [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                            FinishListWrite(ok, std::move(error), std::move(done));
                        });
}

void StreamProvider::RemoveFromPlaylist(const Id& playlist, std::vector<Id> tracks, Done done) {
    Ncm().RemoveFromPlaylist(Num(playlist), Nums(tracks),
                             [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                                 FinishListWrite(ok, std::move(error), std::move(done));
                             });
}

void StreamProvider::CreatePlaylist(std::wstring name, CreatedDone done) {
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
    Ncm().RenamePlaylist(Num(playlist), std::move(name),
                         [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                             FinishListWrite(ok, std::move(error), std::move(done));
                         });
}

void StreamProvider::DeletePlaylist(const Id& playlist, Done done) {
    Ncm().DeletePlaylist(Num(playlist), [this, done = std::move(done)](bool ok, std::wstring error) mutable {
        FinishListWrite(ok, std::move(error), std::move(done));
    });
}

void StreamProvider::ReorderPlaylists(std::vector<Id> playlists, Done done) {
    Ncm().ReorderPlaylists(Nums(playlists),
                           [this, done = std::move(done)](bool ok, std::wstring error) mutable {
                               FinishListWrite(ok, std::move(error), std::move(done));
                           });
}

} // namespace glasswave::netease
