#include "pch.h"
#include "netease/config.h"
#include "netease/cookie.h"
#include "netease/handle.h"
#include "netease/log.h"
#include "netease/session.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace glasswave::netease {

namespace {

std::wstring ErrorOf(const std::wstring& error) {
    return error.empty() ? L"The NetEase service did not return a result." : error;
}

// 私人雷达 (Personal Radar) is one global playlist whose Recommended image differs from
// the one /user/playlist and /playlist/detail return. Pin the Recommended cover so the
// navigation and lists do not flip between the two.
constexpr int64_t kPersonalRadarId = 3136952023;

void PutRecommended(const std::vector<Playlist>& playlists) {
    for (const auto& playlist : playlists) {
        Catalog::Get().Put(playlist);
        if (playlist.id == kPersonalRadarId) Catalog::Get().PreferCover(playlist.id, playlist.coverUrl);
    }
}


}  // namespace

Session& Session::Get() {
    static Session provider;
    return provider;
}

Session::Session() : m_service(ServiceManager::Get()), m_client(m_service), m_tasks(2) {}

Session::~Session() { Shutdown(); }

void Session::Initialize() {
    if (m_initialized.exchange(true)) return;
    m_stopping = false;
    m_abort.reset();
    LoadSessionCookie();
    if (!CookieHasMusicU(SessionCookie())) ClearSessionCookie();
    m_service.Initialize();
    Catalog::Get().Load();
    Log(L"art: extractor supports ncm://song/ = " +
        std::wstring(album_art_extractor::g_is_supported_path("ncm://song/1") ? L"yes" : L"no"));
    std::lock_guard lock(m_mutex);
    m_account = {};
    m_account.nickname = GetAccountName();
    m_account.userId = GetAccountId();
    m_account.loggedIn = CookieHasMusicU(SessionCookie());
    if (!m_account.loggedIn) {
        m_account = {};
        ClearAccount();
    }
}

void Session::Shutdown() {
    if (!m_initialized.exchange(false)) return;
    m_stopping = true;
    // Unblock the workers first: requests in flight and a helper start-up would otherwise
    // keep foobar2000 from closing for up to minutes.
    m_abort.abort();
    m_service.Cancel();
    m_tasks.Stop();
    m_service.Stop();
    std::lock_guard lock(m_mutex);
    m_account = {};
    m_createdPlaylists.clear();
    m_collectedPlaylists.clear();
    m_recommendedPlaylists.clear();
    m_followedArtists.clear();
    m_collectedAlbums.clear();
    m_likedPlaylist = 0;
    std::lock_guard liked(m_likedMutex);
    m_liked.clear();
}

bool Session::LoggedIn() const { return CookieHasMusicU(SessionCookie()); }

Account Session::CurrentAccount() const {
    std::lock_guard lock(m_mutex);
    return m_account;
}

int64_t Session::LikedPlaylistId() const {
    std::lock_guard lock(m_mutex);
    return m_likedPlaylist;
}

bool Session::IsLiked(int64_t songId) const {
    std::lock_guard lock(m_likedMutex);
    return m_liked.count(songId) != 0;
}

std::vector<Playlist> Session::CreatedPlaylists() const {
    std::lock_guard lock(m_mutex);
    return m_createdPlaylists;
}

std::vector<Playlist> Session::CollectedPlaylists() const {
    std::lock_guard lock(m_mutex);
    return m_collectedPlaylists;
}

std::vector<Playlist> Session::RecommendedPlaylists() const {
    std::lock_guard lock(m_mutex);
    return m_recommendedPlaylists;
}

std::vector<Artist> Session::FollowedArtists() const {
    std::lock_guard lock(m_mutex);
    return m_followedArtists;
}

std::vector<Album> Session::CollectedAlbums() const {
    std::lock_guard lock(m_mutex);
    return m_collectedAlbums;
}

std::optional<std::vector<Song>> Session::CachedLikedSongs() const {
    std::lock_guard lock(m_mutex);
    return m_likedSongs;
}

std::optional<std::vector<Song>> Session::CachedPlaylistSongs(int64_t playlistId) const {
    std::lock_guard lock(m_mutex);
    auto it = m_playlistSongs.find(playlistId);
    return it == m_playlistSongs.end() ? std::nullopt : std::optional<std::vector<Song>>(it->second);
}

std::optional<std::vector<Song>> Session::CachedAlbumSongs(int64_t albumId) const {
    std::lock_guard lock(m_mutex);
    auto it = m_albumSongs.find(albumId);
    return it == m_albumSongs.end() ? std::nullopt : std::optional<std::vector<Song>>(it->second);
}

Session::PlaylistScope Session::FindPlaylistScope(int64_t playlistId) const {
    std::lock_guard lock(m_mutex);
    for (const auto& playlist : m_createdPlaylists)
        if (playlist.id == playlistId) return PlaylistScope::Created;
    for (const auto& playlist : m_collectedPlaylists)
        if (playlist.id == playlistId) return PlaylistScope::Collected;
    for (const auto& playlist : m_recommendedPlaylists)
        if (playlist.id == playlistId) return PlaylistScope::Recommended;
    return PlaylistScope::Unknown;
}

void Session::SetAccountLocked(const Account& account) {
    m_account = account;
    SetAccount(account.userId, account.nickname);
}

void Session::PostMain(std::function<void()> fn) {
    if (m_stopping || !fn) return;
    fb2k::inMainThread([this, fn = std::move(fn)] {
        if (!m_stopping) fn();
    });
}

void Session::SetStateCallback(std::function<void()> callback) {
    std::lock_guard lock(m_mutex);
    m_stateCallback = std::move(callback);
}

void Session::SetLibraryCallback(std::function<void()> callback) {
    std::lock_guard lock(m_mutex);
    m_libraryCallback = std::move(callback);
}

void Session::SetLikesCallback(std::function<void()> callback) {
    std::lock_guard lock(m_mutex);
    m_likesCallback = std::move(callback);
}

void Session::NotifyChanged() {
    std::function<void()> callback;
    {
        std::lock_guard lock(m_mutex);
        callback = m_stateCallback;
    }
    if (callback) PostMain([callback = std::move(callback)] { callback(); });
}

void Session::NotifyLibraryChanged() {
    std::function<void()> callback;
    {
        std::lock_guard lock(m_mutex);
        callback = m_libraryCallback;
    }
    if (callback) PostMain([callback = std::move(callback)] { callback(); });
}

void Session::NotifyLikesChanged() {
    std::function<void()> callback;
    {
        std::lock_guard lock(m_mutex);
        callback = m_likesCallback;
    }
    if (callback) PostMain([callback = std::move(callback)] { callback(); });
}

bool Session::RequireLogin(std::wstring& error) const {
    if (LoggedIn()) return true;
    error = L"Please sign in to NetEase Cloud Music.";
    return false;
}

bool Session::EnsureAccount(abort_callback& abort, int64_t& userId, std::wstring& error) {
    {
        std::lock_guard lock(m_mutex);
        if (m_account.loggedIn && m_account.userId) {
            userId = m_account.userId;
            return true;
        }
    }
    Account account;
    if (!m_client.GetAccount(abort, account, error)) return false;
    if (!account.loggedIn) {
        const bool hadSession = !SessionCookie().empty();
        ClearSessionCookie();
        ClearAccount();
        {
            std::lock_guard lock(m_mutex);
            m_account = {};
        }
        if (hadSession) NotifyChanged();
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        SetAccountLocked(account);
    }
    userId = account.userId;
    return true;
}

void Session::StartService(DoneCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        const bool ok = m_service.EnsureStarted(abort, &error);
        PostMain([done = std::move(done), ok, error = std::move(error)] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
    });
}

bool Session::ReloadUserPlaylists(abort_callback& abort, int64_t userId, std::wstring& error) {
    std::vector<Playlist> playlists;
    int64_t likedPlaylist = 0;
    if (!m_client.UserPlaylists(userId, abort, playlists, likedPlaylist, error)) return false;
    for (const auto& playlist : playlists) Catalog::Get().Put(playlist);
    std::vector<Playlist> created, collected;
    for (auto& playlist : playlists)
        (playlist.creatorId == userId ? created : collected).push_back(std::move(playlist));
    std::lock_guard lock(m_mutex);
    m_createdPlaylists = std::move(created);
    m_collectedPlaylists = std::move(collected);
    m_likedPlaylist = likedPlaylist;
    return true;
}

void Session::ApplyPlaylistInfo(const Playlist& info) {
    if (!info.id) return;
    Catalog::Get().Put(info);
    std::lock_guard lock(m_mutex);
    auto update = [&info](std::vector<Playlist>& list) {
        for (auto& playlist : list) {
            if (playlist.id != info.id) continue;
            playlist.name = info.name;
            playlist.coverUrl = info.coverUrl;
            playlist.trackCount = info.trackCount;
            playlist.creatorId = info.creatorId;
            playlist.creatorName = info.creatorName;
            playlist.description = info.description;
            playlist.subscribed = info.subscribed;
            return;
        }
    };
    update(m_createdPlaylists);
    update(m_collectedPlaylists);
    update(m_recommendedPlaylists);
}

void Session::ApplyPlaylistOrder(const std::vector<int64_t>& ids) {
    std::lock_guard lock(m_mutex);
    auto apply = [&ids](std::vector<Playlist>& list) {
        std::map<int64_t, Playlist> byId;
        for (auto& playlist : list) byId.emplace(playlist.id, std::move(playlist));
        std::vector<Playlist> ordered;
        ordered.reserve(list.size());
        for (int64_t id : ids) {
            auto it = byId.find(id);
            if (it == byId.end()) continue;
            ordered.push_back(std::move(it->second));
            byId.erase(it);
        }
        for (auto& [id, playlist] : byId) ordered.push_back(std::move(playlist));
        list = std::move(ordered);
    };
    apply(m_createdPlaylists);
    apply(m_collectedPlaylists);
}

void Session::SchedulePlaylistSummaryRefresh(int64_t playlistId) {
    m_tasks.Post([this, playlistId] {
        if (m_stopping) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        Playlist info;
        std::wstring error;
        if (!m_client.GetPlaylistInfo(playlistId, abort, info, error)) return;
        ApplyPlaylistInfo(info);
        NotifyLibraryChanged();
    });
}

void Session::RefreshAccount(DoneCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!RequireLogin(error)) {
            PostMain([done = std::move(done), error] {
                if (done) done(false, ErrorOf(error));
            });
            return;
        }
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done(false, ErrorOf(error));
            });
            return;
        }
        Account account;
        if (!m_client.GetAccount(abort, account, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done(false, ErrorOf(error));
            });
            return;
        }
        if (!account.loggedIn) {
            ClearSessionCookie();
            ClearAccount();
            {
                std::lock_guard lock(m_mutex);
                m_account = {};
                m_likedPlaylist = 0;
            }
            {
                // Released before NotifyChanged, which takes m_mutex: Logout and Shutdown take
                // m_mutex first, so holding both here in the other order could deadlock.
                std::lock_guard liked(m_likedMutex);
                m_liked.clear();
            }
            PostMain([done = std::move(done)] {
                if (done) done(false, L"Please sign in to NetEase Cloud Music.");
            });
            NotifyChanged();
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            SetAccountLocked(account);
        }
        std::vector<int64_t> ids;
        std::wstring likeError;
        if (m_client.Likelist(account.userId, abort, ids, likeError)) {
            std::lock_guard liked(m_likedMutex);
            m_liked.clear();
            for (int64_t id : ids) m_liked.insert(id);
        }
        std::vector<Playlist> playlists;
        int64_t likedPlaylist = 0;
        std::wstring playlistError;
        if (m_client.UserPlaylists(account.userId, abort, playlists, likedPlaylist, playlistError)) {
            for (const auto& playlist : playlists) Catalog::Get().Put(playlist);
            std::vector<Playlist> created, collected;
            for (auto& playlist : playlists)
                (playlist.creatorId == account.userId ? created : collected).push_back(std::move(playlist));
            std::lock_guard lock(m_mutex);
            m_createdPlaylists = std::move(created);
            m_collectedPlaylists = std::move(collected);
            m_likedPlaylist = likedPlaylist;
        }
        // Prefetch the account's saved artists/albums and the recommended list once per
        // refresh, so the navigation pane's breadcrumb menus can list them immediately.
        std::wstring listError;
        size_t followedCount = 0, albumCount = 0, recommendedCount = 0;
        std::vector<Artist> artists;
        if (m_client.GetFollowedArtists(abort, artists, listError)) {
            followedCount = artists.size();
            for (const auto& artist : artists) Catalog::Get().Put(artist);
            std::lock_guard lock(m_mutex);
            m_followedArtists = std::move(artists);
        }
        std::vector<Album> albums;
        if (m_client.GetCollectedAlbums(abort, albums, listError)) {
            albumCount = albums.size();
            Catalog::Get().Put(albums);
            std::lock_guard lock(m_mutex);
            m_collectedAlbums = std::move(albums);
        }
        std::vector<Playlist> recommended;
        if (m_client.RecommendPlaylists(abort, recommended, listError)) {
            recommendedCount = recommended.size();
            PutRecommended(recommended);
            std::lock_guard lock(m_mutex);
            m_recommendedPlaylists = std::move(recommended);
        }
        Log(L"account: saved lists: " + std::to_wstring(followedCount) + L" artists, " +
            std::to_wstring(albumCount) + L" albums, " + std::to_wstring(recommendedCount) + L" recommended");
        PostMain([done = std::move(done)] {
            if (done) done(true, {});
        });
        NotifyChanged();
    });
}

void Session::RefreshLikes(DoneCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!RequireLogin(error)) {
            PostMain([done = std::move(done), error] {
                if (done) done(false, ErrorOf(error));
            });
            return;
        }
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done(false, ErrorOf(error));
            });
            return;
        }
        int64_t userId = 0;
        if (!EnsureAccount(abort, userId, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done(false, ErrorOf(error));
            });
            return;
        }
        std::vector<int64_t> ids;
        if (!m_client.Likelist(userId, abort, ids, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done(false, ErrorOf(error));
            });
            return;
        }
        {
            std::lock_guard liked(m_likedMutex);
            m_liked.clear();
            for (int64_t id : ids) m_liked.insert(id);
        }
        PostMain([done = std::move(done)] {
            if (done) done(true, {});
        });
    });
}

void Session::FetchMyPlaylists(PlaylistsCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!RequireLogin(error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        int64_t userId = 0;
        if (!EnsureAccount(abort, userId, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        std::vector<Playlist> playlists;
        int64_t liked = 0;
        if (!m_client.UserPlaylists(userId, abort, playlists, liked, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            m_createdPlaylists.clear();
            m_collectedPlaylists.clear();
            for (auto& playlist : playlists)
                (playlist.creatorId == userId ? m_createdPlaylists : m_collectedPlaylists).push_back(playlist);
            m_likedPlaylist = liked;
        }
        for (const auto& playlist : playlists) Catalog::Get().Put(playlist);
        PostMain([done = std::move(done), playlists = std::move(playlists)] {
            if (done) done(std::move(playlists), {});
        });
    });
}

void Session::FetchRecommended(PlaylistsCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!RequireLogin(error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        std::vector<Playlist> playlists;
        if (!m_client.RecommendPlaylists(abort, playlists, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        PutRecommended(playlists);
        {
            std::lock_guard lock(m_mutex);
            m_recommendedPlaylists = playlists;
        }
        PostMain([done = std::move(done), playlists = std::move(playlists)] {
            if (done) done(std::move(playlists), {});
        });
    });
}

void Session::FetchFollowedArtists(ArtistsCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!RequireLogin(error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        std::vector<Artist> artists;
        if (!m_client.GetFollowedArtists(abort, artists, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        for (const auto& artist : artists) Catalog::Get().Put(artist);
        {
            std::lock_guard lock(m_mutex);
            m_followedArtists = artists;
        }
        PostMain([done = std::move(done), artists = std::move(artists)]() mutable {
            if (done) done(std::move(artists), {});
        });
    });
}

void Session::FetchCollectedAlbums(AlbumsCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!RequireLogin(error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        std::vector<Album> albums;
        if (!m_client.GetCollectedAlbums(abort, albums, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        Catalog::Get().Put(albums);
        {
            std::lock_guard lock(m_mutex);
            m_collectedAlbums = albums;
        }
        PostMain([done = std::move(done), albums = std::move(albums)]() mutable {
            if (done) done(std::move(albums), {});
        });
    });
}

void Session::FetchCollectedAlbumsPage(int offset, int limit, AlbumsPageCallback done) {
    m_tasks.Post([this, offset, limit, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!RequireLogin(error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, false, ErrorOf(error));
            });
            return;
        }
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, false, ErrorOf(error));
            });
            return;
        }
        std::vector<Album> albums;
        bool more = false;
        if (!m_client.GetCollectedAlbumsPage(offset, limit, abort, albums, more, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, false, ErrorOf(error));
            });
            return;
        }
        Catalog::Get().Put(albums);
        {
            // Merge the page into the account cache so the breadcrumb menus and the album
            // page's collect state see everything the user has browsed, including albums
            // collected in the official client after the last full refresh.
            std::lock_guard lock(m_mutex);
            for (const auto& album : albums) {
                if (std::none_of(m_collectedAlbums.begin(), m_collectedAlbums.end(),
                                 [&album](const Album& saved) { return saved.id == album.id; }))
                    m_collectedAlbums.push_back(album);
            }
        }
        PostMain([done = std::move(done), albums = std::move(albums), more]() mutable {
            if (done) done(std::move(albums), more, {});
        });
    });
}

void Session::FetchAlbumTracks(std::vector<Album> albums, SongsCallback done) {
    m_tasks.Post([this, albums = std::move(albums), done = std::move(done)]() mutable {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        std::vector<std::vector<Song>> tracks(albums.size());
        std::atomic<size_t> next{0};
        auto fetch = [this, &albums, &tracks, &next] {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= albums.size() || m_stopping) return;
                abort_callback& albumAbort = m_abort;
                Album info;
                std::vector<Song> songs;
                std::wstring ignored;
                if (m_client.GetAlbum(albums[i].id, albumAbort, info, songs, ignored))
                    tracks[i] = std::move(songs);
            }
        };
        std::vector<std::thread> threads;
        const size_t count = std::min<size_t>(6, albums.size());
        for (size_t t = 0; t < count; ++t) threads.emplace_back(fetch);
        for (auto& thread : threads) thread.join();
        if (m_stopping) return;
        std::vector<Song> all;
        for (auto& list : tracks) all.insert(all.end(), list.begin(), list.end());
        if (all.empty()) {
            PostMain([done = std::move(done)] {
                if (done) done({}, L"No songs in the selected albums.");
            });
            return;
        }
        PostMain([done = std::move(done), all = std::move(all)]() mutable {
            Catalog::Get().PutAndHint(all);
            if (done) done(std::move(all), {});
        });
    });
}

void Session::FetchLiked(SongsCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!RequireLogin(error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        int64_t userId = 0;
        if (!EnsureAccount(abort, userId, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }

        int64_t likedPlaylist = 0;
        {
            std::lock_guard lock(m_mutex);
            likedPlaylist = m_likedPlaylist;
        }
        if (!likedPlaylist) {
            std::vector<Playlist> playlists;
            if (m_client.UserPlaylists(userId, abort, playlists, likedPlaylist, error)) {
                std::lock_guard lock(m_mutex);
                m_createdPlaylists.clear();
                m_collectedPlaylists.clear();
                for (auto& playlist : playlists)
                    (playlist.creatorId == userId ? m_createdPlaylists : m_collectedPlaylists).push_back(playlist);
                m_likedPlaylist = likedPlaylist;
            }
        }

        std::vector<int64_t> ids;
        if (!m_client.Likelist(userId, abort, ids, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        // /likelist is authoritative for the like state; the liked playlist detail can lag
        // behind changes made in the official client. Filter the detail by the likelist and
        // append likes the detail has not picked up yet, keeping the playlist's order.
        std::unordered_set<int64_t> liked(ids.begin(), ids.end());
        std::vector<Song> songs;
        std::unordered_set<int64_t> have;
        if (likedPlaylist) {
            PlaylistDetail detail;
            std::wstring detailError;
            if (m_client.GetPlaylist(likedPlaylist, abort, detail, detailError)) {
                for (auto& song : detail.songs)
                    if (song.id && liked.count(song.id) && have.insert(song.id).second)
                        songs.push_back(std::move(song));
            }
        }
        std::vector<int64_t> missing;
        for (const int64_t id : ids)
            if (!have.count(id)) missing.push_back(id);
        std::unordered_map<int64_t, Song> fetched;
        for (size_t start = 0; start < missing.size(); start += 200) {
            const size_t end = std::min(start + 200, missing.size());
            std::vector<int64_t> batch(missing.begin() + static_cast<std::ptrdiff_t>(start),
                                       missing.begin() + static_cast<std::ptrdiff_t>(end));
            std::vector<Song> part;
            if (!m_client.SongDetails(batch, abort, part, error)) break;
            for (auto& song : part)
                if (song.id) fetched.emplace(song.id, std::move(song));
        }
        for (const int64_t id : missing) {
            auto it = fetched.find(id);
            if (it != fetched.end()) songs.push_back(it->second);
        }

        {
            // The like state is the likelist itself, also for songs whose details failed to load.
            std::lock_guard likedLock(m_likedMutex);
            m_liked = std::move(liked);
        }
        {
            std::lock_guard lock(m_mutex);
            m_likedSongs = songs;
        }
        PostMain([done = std::move(done), songs = std::move(songs)]() mutable {
            Catalog::Get().PutAndHint(songs);
            if (done) done(std::move(songs), {});
        });
    });
}

void Session::FetchPlaylist(int64_t playlistId, DetailCallback done) {
    m_tasks.Post([this, playlistId, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!RequireLogin(error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        PlaylistDetail detail;
        if (!m_client.GetPlaylist(playlistId, abort, detail, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        Catalog::Get().Put(detail.info);
        {
            std::lock_guard lock(m_mutex);
            m_playlistSongs[playlistId] = detail.songs;
        }
        PostMain([done = std::move(done), detail = std::move(detail)]() mutable {
            Catalog::Get().PutAndHint(detail.songs);
            if (done) done(std::move(detail), {});
        });
    });
}

void Session::FetchArtistDetail(int64_t artistId, ArtistDetailCallback done) {
    m_tasks.Post([this, artistId, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        Artist artist;
        if (!m_client.GetArtistDetail(artistId, abort, artist, error)) {
            Log(L"artist: detail failed for " + std::to_wstring(artistId) + L": " + error);
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        m_client.GetArtistExtras(artistId, abort, artist);
        Catalog::Get().Put(artist);
        PostMain([done = std::move(done), artist] {
            if (done) done(artist, {});
        });
    });
}

void Session::FetchArtistDescription(int64_t artistId, ArtistDescriptionCallback done) {
    m_tasks.Post([this, artistId, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, {}, ErrorOf(error));
            });
            return;
        }
        std::wstring brief, full;
        if (!m_client.GetArtistDescription(artistId, abort, brief, full, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, {}, ErrorOf(error));
            });
            return;
        }
        PostMain([done = std::move(done), brief = std::move(brief), full = std::move(full)]() mutable {
            if (done) done(std::move(brief), std::move(full), {});
        });
    });
}

void Session::FetchArtistAlbumsPage(int64_t artistId, int offset, int limit, ArtistAlbumsCallback done) {
    m_tasks.Post([this, artistId, offset, limit, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, false, false, false, ErrorOf(error));
            });
            return;
        }
        std::vector<Album> albums;
        bool followed = false, more = false;
        if (!m_client.GetArtistAlbumsPage(artistId, offset, limit, abort, albums, &followed, &more, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, false, false, false, ErrorOf(error));
            });
            return;
        }
        Catalog::Get().Put(albums);
        PostMain([done = std::move(done), albums = std::move(albums), more, followed]() mutable {
            if (done) done(std::move(albums), more, true, followed, {});
        });
    });
}

void Session::FetchArtistSongsPage(int64_t artistId, int offset, int limit, SongsPageCallback done) {
    m_tasks.Post([this, artistId, offset, limit, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, false, ErrorOf(error));
            });
            return;
        }
        std::vector<Song> songs;
        bool more = false;
        if (!m_client.GetArtistSongsPage(artistId, offset, limit, L"hot", abort, songs, &more, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, false, ErrorOf(error));
            });
            return;
        }
        PostMain([done = std::move(done), songs = std::move(songs), more]() mutable {
            Catalog::Get().PutAndHint(songs);
            if (done) done(std::move(songs), more, {});
        });
    });
}

void Session::FetchAlbumDetail(int64_t albumId, AlbumDetailCallback done) {
    m_tasks.Post([this, albumId, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, {}, ErrorOf(error));
            });
            return;
        }
        Album info;
        std::vector<Song> songs;
        if (!m_client.GetAlbum(albumId, abort, info, songs, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, {}, ErrorOf(error));
            });
            return;
        }
        Catalog::Get().Put(info);
        {
            std::lock_guard lock(m_mutex);
            m_albumSongs[albumId] = songs;
        }
        PostMain([done = std::move(done), info, songs = std::move(songs)]() mutable {
            Catalog::Get().PutAndHint(songs);
            if (done) done(info, std::move(songs), {});
        });
    });
}

void Session::Search(std::wstring_view query, SongsCallback done) {
    m_tasks.Post([this, query = std::wstring(query), done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        std::vector<Song> songs;
        if (!m_client.SearchSongs(query, 100, 0, abort, songs, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        PostMain([done = std::move(done), songs = std::move(songs)]() mutable {
            Catalog::Get().PutAndHint(songs);
            if (done) done(std::move(songs), {});
        });
    });
}

void Session::ToggleLike(int64_t songId, bool like, DoneCallback done) {
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, loginError);
        return;
    }
    {
        std::lock_guard lock(m_likedMutex);
        if (like)
            m_liked.insert(songId);
        else
            m_liked.erase(songId);
    }
    m_tasks.Post([this, songId, like, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        bool ok = false;
        if (m_service.EnsureStarted(abort, &error)) ok = m_client.SetLike(songId, like, abort, error);
        if (!ok) {
            std::lock_guard lock(m_likedMutex);
            if (like)
                m_liked.erase(songId);
            else
                m_liked.insert(songId);
        }
        PostMain([done = std::move(done), ok, error] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
        NotifyLikesChanged();
    });
}

void Session::SetArtistFollow(int64_t artistId, bool follow, DoneCallback done) {
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, loginError);
        return;
    }
    m_tasks.Post([this, artistId, follow, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        bool ok = false;
        if (m_service.EnsureStarted(abort, &error))
            ok = m_client.SetArtistFollow(artistId, follow, abort, error);
        if (ok) {
            // Keep the followed-artists list (used by the navigation menus) in step.
            if (auto artist = Catalog::Get().FindArtist(artistId)) {
                artist->followed = follow;
                artist->followKnown = true;
                Catalog::Get().Put(*artist);  // FindArtist returns a copy
                std::lock_guard lock(m_mutex);
                auto it = std::find_if(m_followedArtists.begin(), m_followedArtists.end(),
                                       [artistId](const Artist& a) { return a.id == artistId; });
                if (follow && it == m_followedArtists.end())
                    m_followedArtists.insert(m_followedArtists.begin(), *artist);
                else if (!follow && it != m_followedArtists.end())
                    m_followedArtists.erase(it);
            }
        }
        PostMain([done = std::move(done), ok, error] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
    });
}

void Session::AddToPlaylist(int64_t playlistId, std::vector<int64_t> songIds, DoneCallback done) {
    if (songIds.empty()) {
        if (done) done(false, L"No songs selected.");
        return;
    }
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, loginError);
        return;
    }
    m_tasks.Post([this, playlistId, songIds = std::move(songIds), done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        bool ok = false;
        if (m_service.EnsureStarted(abort, &error)) {
            ok = m_client.AddToPlaylist(playlistId, songIds, abort, error);
            if (ok) {
                // The server regenerates the playlist cover and count after an edit; one
                // lightweight summary request keeps our tiles and navigation in step.
                Playlist info;
                std::wstring infoError;
                if (m_client.GetPlaylistInfo(playlistId, abort, info, infoError)) ApplyPlaylistInfo(info);
            }
        }
        PostMain([done = std::move(done), ok, error] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
        if (ok) NotifyLibraryChanged();
    });
}

void Session::RemoveFromPlaylist(int64_t playlistId, std::vector<int64_t> songIds, DoneCallback done) {
    if (songIds.empty()) {
        if (done) done(false, L"No songs selected.");
        return;
    }
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, loginError);
        return;
    }
    m_tasks.Post([this, playlistId, songIds = std::move(songIds), done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        bool ok = false;
        if (m_service.EnsureStarted(abort, &error)) {
            ok = m_client.RemoveFromPlaylist(playlistId, songIds, abort, error);
            if (ok) {
                Playlist info;
                std::wstring infoError;
                if (m_client.GetPlaylistInfo(playlistId, abort, info, infoError)) ApplyPlaylistInfo(info);
                SchedulePlaylistSummaryRefresh(playlistId);
            }
        }
        PostMain([done = std::move(done), ok, error] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
        if (ok) NotifyLibraryChanged();
    });
}

void Session::SetAlbumCollected(int64_t albumId, bool collected, DoneCallback done) {
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, loginError);
        return;
    }
    m_tasks.Post([this, albumId, collected, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        bool ok = false;
        if (m_service.EnsureStarted(abort, &error))
            ok = m_client.SetAlbumCollect(albumId, collected, abort, error);
        if (ok) {
            const auto album = Catalog::Get().FindAlbum(albumId);
            std::lock_guard lock(m_mutex);
            auto it = std::find_if(m_collectedAlbums.begin(), m_collectedAlbums.end(),
                                   [albumId](const Album& a) { return a.id == albumId; });
            if (!collected && it != m_collectedAlbums.end())
                m_collectedAlbums.erase(it);
            else if (collected && it == m_collectedAlbums.end() && album)
                m_collectedAlbums.insert(m_collectedAlbums.begin(), *album);  // newest first, like the service
        }
        PostMain([done = std::move(done), ok, error] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
        if (ok) NotifyLibraryChanged();
    });
}

void Session::SetPlaylistSubscribed(int64_t playlistId, bool subscribed, DoneCallback done) {
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, loginError);
        return;
    }
    m_tasks.Post([this, playlistId, subscribed, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        bool ok = false;
        if (m_service.EnsureStarted(abort, &error)) {
            ok = m_client.SetPlaylistSubscribed(playlistId, subscribed, abort, error);
            if (ok) {
                Playlist info;
                std::wstring infoError;
                if (m_client.GetPlaylistInfo(playlistId, abort, info, infoError)) {
                    info.subscribed = subscribed;
                    ApplyPlaylistInfo(info);
                }
                int64_t userId = 0;
                {
                    std::lock_guard lock(m_mutex);
                    userId = m_account.userId;
                }
                Playlist known;
                {
                    std::lock_guard lock(m_mutex);
                    for (const auto& playlist : m_collectedPlaylists)
                        if (playlist.id == playlistId) known = playlist;
                    if (!known.id)
                        for (const auto& playlist : m_recommendedPlaylists)
                            if (playlist.id == playlistId) known = playlist;
                }
                if (!known.id) known = Catalog::Get().FindPlaylist(playlistId).value_or(Playlist{});
                known.id = known.id ? known.id : playlistId;
                known.subscribed = subscribed;
                Catalog::Get().Put(known);
                std::lock_guard lock(m_mutex);
                auto it = std::find_if(m_collectedPlaylists.begin(), m_collectedPlaylists.end(),
                                       [playlistId](const Playlist& playlist) { return playlist.id == playlistId; });
                if (subscribed && it == m_collectedPlaylists.end() && known.creatorId != userId)
                    m_collectedPlaylists.insert(m_collectedPlaylists.begin(), known);  // the service puts new ones first
                else if (!subscribed && it != m_collectedPlaylists.end())
                    m_collectedPlaylists.erase(it);
            }
        }
        PostMain([done = std::move(done), ok, error] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
        if (ok) NotifyLibraryChanged();
    });
}

void Session::CreatePlaylist(std::wstring name, CreatePlaylistCallback done) {
    if (name.empty()) {
        if (done) done(false, 0, L"The playlist name is empty.");
        return;
    }
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, 0, loginError);
        return;
    }
    m_tasks.Post([this, name = std::move(name), done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done(false, 0, ErrorOf(error));
            });
            return;
        }
        int64_t userId = 0;
        if (!EnsureAccount(abort, userId, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done(false, 0, ErrorOf(error));
            });
            return;
        }
        int64_t id = 0;
        if (!m_client.CreatePlaylist(name, abort, id, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done(false, 0, ErrorOf(error));
            });
            return;
        }
        std::wstring listError;
        ReloadUserPlaylists(abort, userId, listError);  // a failed reload must not hide the created playlist
        PostMain([done = std::move(done), id] {
            if (done) done(true, id, {});
        });
        NotifyLibraryChanged();
    });
}

void Session::RenamePlaylist(int64_t playlistId, std::wstring name, DoneCallback done) {
    if (name.empty()) {
        if (done) done(false, L"The playlist name is empty.");
        return;
    }
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, loginError);
        return;
    }
    m_tasks.Post([this, playlistId, name = std::move(name), done = std::move(done)]() mutable {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        bool ok = false;
        if (m_service.EnsureStarted(abort, &error)) {
            ok = m_client.RenamePlaylist(playlistId, name, abort, error);
            if (ok) {
                Playlist info;
                std::wstring infoError;
                if (m_client.GetPlaylistInfo(playlistId, abort, info, infoError)) {
                    info.name = name;
                    ApplyPlaylistInfo(info);
                } else {
                    if (auto existing = Catalog::Get().FindPlaylist(playlistId)) {
                        Playlist updated = *existing;
                        updated.name = name;
                        Catalog::Get().Put(updated);
                    }
                    std::lock_guard lock(m_mutex);
                    auto rename = [&](std::vector<Playlist>& list) {
                        for (auto& playlist : list)
                            if (playlist.id == playlistId) playlist.name = name;
                    };
                    rename(m_createdPlaylists);
                    rename(m_collectedPlaylists);
                    rename(m_recommendedPlaylists);
                }
            }
        }
        PostMain([done = std::move(done), ok, error] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
        if (ok) NotifyLibraryChanged();
    });
}

void Session::DeletePlaylist(int64_t playlistId, DoneCallback done) {
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, loginError);
        return;
    }
    m_tasks.Post([this, playlistId, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        bool ok = false;
        if (m_service.EnsureStarted(abort, &error)) ok = m_client.DeletePlaylist(playlistId, abort, error);
        if (ok) {
            std::lock_guard lock(m_mutex);
            auto remove = [playlistId](std::vector<Playlist>& list) {
                list.erase(std::remove_if(list.begin(), list.end(),
                                          [playlistId](const Playlist& playlist) {
                                              return playlist.id == playlistId;
                                          }),
                           list.end());
            };
            remove(m_createdPlaylists);
            remove(m_collectedPlaylists);
            remove(m_recommendedPlaylists);
        }
        PostMain([done = std::move(done), ok, error] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
        if (ok) NotifyLibraryChanged();
    });
}

void Session::ReorderPlaylists(std::vector<int64_t> ids, DoneCallback done) {
    if (ids.empty()) {
        if (done) done(true, {});
        return;
    }
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, loginError);
        return;
    }
    ApplyPlaylistOrder(ids);  // the navigation shows the new order immediately
    m_tasks.Post([this, ids = std::move(ids), done = std::move(done)]() mutable {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        bool ok = false;
        if (m_service.EnsureStarted(abort, &error)) {
            ok = m_client.ReorderPlaylists(ids, abort, error);
            int64_t userId = 0;
            if (EnsureAccount(abort, userId, error)) {
                std::wstring listError;
                ReloadUserPlaylists(abort, userId, listError);  // authoritative order (or restores)
            }
        }
        if (!ok) Log(L"playlist: reorder failed: " + error);
        PostMain([done = std::move(done), ok, error] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
        NotifyLibraryChanged();
    });
}

void Session::OnLoginCookie(std::wstring cookie, DoneCallback done) {
    const std::wstring normalized = NormalizeCookie(cookie);
    if (!CookieHasMusicU(normalized)) {
        if (done) done(false, L"The login response did not contain a MUSIC_U session.");
        return;
    }
    SaveSessionCookie(normalized);
    RefreshAccount(std::move(done));
}

void Session::Logout() {
    ClearSessionCookie();
    ClearAccount();
    Catalog::Get().ClearPreferredCovers();
    {
        std::lock_guard lock(m_mutex);
        m_account = {};
        m_createdPlaylists.clear();
        m_collectedPlaylists.clear();
        m_recommendedPlaylists.clear();
        m_followedArtists.clear();
        m_collectedAlbums.clear();
        m_likedSongs.reset();
        m_playlistSongs.clear();
        m_albumSongs.clear();
        m_likedPlaylist = 0;
        std::lock_guard liked(m_likedMutex);
        m_liked.clear();
    }
    NotifyChanged();
}

}  // namespace glasswave::netease
