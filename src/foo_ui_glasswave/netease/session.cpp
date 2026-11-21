#include "pch.h"
#include "netease/config.h"
#include "netease/cookie.h"
#include "core/log.h"
#include "netease/session.h"
#include "stream/provider.h"

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

// The playlists as the account's lists hold them; every one is also put in the catalog.
std::vector<stream::Playlist> PutPlaylists(const std::vector<Playlist>& playlists, int64_t userId,
                                           PlaylistScope scope) {
    std::vector<stream::Playlist> converted;
    converted.reserve(playlists.size());
    for (const auto& playlist : playlists) {
        converted.push_back(ToPlaylist(playlist, userId, scope));
        Catalog::Get().Records().Put(converted.back());
        if (scope == PlaylistScope::Recommended && playlist.id == kPersonalRadarId)
            Catalog::Get().Records().PreferCover(stream::ArtKind::Playlist, converted.back().id,
                                                 playlist.coverUrl);
    }
    return converted;
}

std::vector<stream::Playlist>::iterator FindById(std::vector<stream::Playlist>& list, const stream::Id& id) {
    return std::find_if(list.begin(), list.end(),
                        [&id](const stream::Playlist& playlist) { return playlist.id == id; });
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
    if (!SessionCookieHasMusicU()) ClearSessionCookie();
    m_service.Initialize();
    Catalog::Get().Load();
    core::Log(L"art: extractor supports ncm://song/ = " +
              std::wstring(album_art_extractor::g_is_supported_path("ncm://song/1") ? L"yes" : L"no"));
    std::lock_guard lock(m_mutex);
    m_account = {};
    m_account.nickname = GetAccountName();
    m_account.userId = GetAccountId();
    m_account.loggedIn = SessionCookieHasMusicU();
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

bool Session::LoggedIn() const {
    return SessionCookieHasMusicU();
}

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

std::vector<stream::Playlist> Session::CreatedPlaylists() const {
    std::lock_guard lock(m_mutex);
    return m_createdPlaylists;
}

std::vector<stream::Playlist> Session::CollectedPlaylists() const {
    std::lock_guard lock(m_mutex);
    return m_collectedPlaylists;
}

std::vector<stream::Playlist> Session::RecommendedPlaylists() const {
    std::lock_guard lock(m_mutex);
    return m_recommendedPlaylists;
}

std::vector<stream::Artist> Session::FollowedArtists() const {
    std::lock_guard lock(m_mutex);
    return m_followedArtists;
}

bool Session::IsAlbumCollected(int64_t albumId) const {
    std::lock_guard lock(m_mutex);
    const stream::Id id = IdText(albumId);
    return std::any_of(m_collectedAlbums.begin(), m_collectedAlbums.end(),
                       [&id](const stream::Album& album) { return album.id == id; });
}

std::vector<stream::Album> Session::CollectedAlbums() const {
    std::lock_guard lock(m_mutex);
    return m_collectedAlbums;
}

Session::PlaylistScope Session::FindPlaylistScope(int64_t playlistId) const {
    std::lock_guard lock(m_mutex);
    return ScopeLocked(IdText(playlistId));
}

Session::PlaylistScope Session::ScopeLocked(const stream::Id& id) const {
    const auto in = [&id](const std::vector<stream::Playlist>& list) {
        return std::any_of(list.begin(), list.end(),
                           [&id](const stream::Playlist& playlist) { return playlist.id == id; });
    };
    if (in(m_createdPlaylists)) return PlaylistScope::Created;
    if (in(m_collectedPlaylists)) return PlaylistScope::Collected;
    if (in(m_recommendedPlaylists)) return PlaylistScope::Recommended;
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

void Session::SetLibraryReadCallbacks(std::function<uint64_t()> mark, std::function<void(uint64_t)> read) {
    std::lock_guard lock(m_mutex);
    m_libraryReadMark = std::move(mark);
    m_libraryReadCallback = std::move(read);
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
    const uint64_t generation = m_accountGeneration;
    {
        std::lock_guard lock(m_mutex);
        if (m_account.loggedIn && m_account.userId) {
            userId = m_account.userId;
            return true;
        }
    }
    Account account;
    if (!m_client.GetAccount(abort, account, error)) return false;
    if (generation != m_accountGeneration) {
        error = stream::kRequestDropped;
        return false;
    }
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
        if (generation != m_accountGeneration) {
            error = stream::kRequestDropped;
            return false;
        }
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
    std::vector<Playlist> made, saved;
    for (auto& playlist : playlists)
        (playlist.creatorId == userId ? made : saved).push_back(std::move(playlist));
    auto created = PutPlaylists(made, userId, PlaylistScope::Created);
    auto collected = PutPlaylists(saved, userId, PlaylistScope::Collected);
    std::lock_guard lock(m_mutex);
    m_createdPlaylists = std::move(created);
    m_collectedPlaylists = std::move(collected);
    m_likedPlaylist = likedPlaylist;
    return true;
}

void Session::ApplyPlaylistInfo(const Playlist& info) {
    if (!info.id) return;
    std::lock_guard lock(m_mutex);
    const stream::Playlist record = ToPlaylist(info, m_account.userId, ScopeLocked(IdText(info.id)));
    Catalog::Get().Records().Put(record);
    auto update = [&record](std::vector<stream::Playlist>& list) {
        for (auto& playlist : list) {
            if (playlist.id != record.id) continue;
            playlist.name = record.name;
            playlist.coverUrl = record.coverUrl;
            playlist.trackCount = record.trackCount;
            playlist.creator = record.creator;
            playlist.description = record.description;
            playlist.own = record.own;
            playlist.saved = record.saved;
            return;
        }
    };
    update(m_createdPlaylists);
    update(m_collectedPlaylists);
    update(m_recommendedPlaylists);
}

void Session::ApplyPlaylistOrder(const std::vector<int64_t>& ids) {
    std::lock_guard lock(m_mutex);
    auto apply = [&ids](std::vector<stream::Playlist>& list) {
        std::map<stream::Id, stream::Playlist> byId;
        for (auto& playlist : list)
            byId.emplace(playlist.id, std::move(playlist));
        std::vector<stream::Playlist> ordered;
        ordered.reserve(list.size());
        for (int64_t number : ids) {
            auto it = byId.find(IdText(number));
            if (it == byId.end()) continue;
            ordered.push_back(std::move(it->second));
            byId.erase(it);
        }
        for (auto& [id, playlist] : byId)
            ordered.push_back(std::move(playlist));
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
    const uint64_t generation = m_accountGeneration;
    m_tasks.Post([this, generation, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback& abort = m_abort;
        std::wstring error;
        // Before the first request: the users' changes acknowledged after this are not in it.
        std::function<uint64_t()> markOf;
        std::function<void(uint64_t)> readCallback;
        {
            std::lock_guard lock(m_mutex);
            markOf = m_libraryReadMark;
            readCallback = m_libraryReadCallback;
        }
        const uint64_t readMark = markOf ? markOf() : 0;
        // A sign-out or a new sign-in while this ran: what was read belongs to the old account.
        const auto superseded = [&] {
            PostMain([done] {
                if (done) done(false, stream::kRequestDropped);
            });
        };
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
        if (generation != m_accountGeneration) return superseded();
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
            if (generation != m_accountGeneration) {
                superseded();
                return;
            }
            SetAccountLocked(account);
        }
        std::vector<int64_t> ids;
        std::wstring likeError;
        if (m_client.Likelist(account.userId, abort, ids, likeError)) {
            std::lock_guard lock(m_mutex); // the order Logout takes them in
            if (generation != m_accountGeneration) return superseded();
            std::lock_guard liked(m_likedMutex);
            m_liked.clear();
            for (int64_t id : ids)
                m_liked.insert(id);
        }
        std::vector<Playlist> playlists;
        int64_t likedPlaylist = 0;
        std::wstring playlistError;
        if (m_client.UserPlaylists(account.userId, abort, playlists, likedPlaylist, playlistError)) {
            std::vector<Playlist> made, saved;
            for (auto& playlist : playlists)
                (playlist.creatorId == account.userId ? made : saved).push_back(std::move(playlist));
            auto created = PutPlaylists(made, account.userId, PlaylistScope::Created);
            auto collected = PutPlaylists(saved, account.userId, PlaylistScope::Collected);
            std::lock_guard lock(m_mutex);
            if (generation != m_accountGeneration) return superseded();
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
            std::vector<stream::Artist> followed;
            followed.reserve(artists.size());
            for (const auto& artist : artists) {
                followed.push_back(ToArtist(artist));
                Catalog::Get().Records().Put(followed.back());
                followed.back().followKnown = followed.back().followed = true;
            }
            std::lock_guard lock(m_mutex);
            if (generation != m_accountGeneration) return superseded();
            m_followedArtists = std::move(followed);
        }
        std::vector<Album> albums;
        if (m_client.GetCollectedAlbums(abort, albums, listError)) {
            albumCount = albums.size();
            auto collected = ToAlbums(albums);
            Catalog::Get().Records().Put(collected);
            std::lock_guard lock(m_mutex);
            if (generation != m_accountGeneration) return superseded();
            m_collectedAlbums = std::move(collected);
        }
        std::vector<Playlist> recommended;
        if (m_client.RecommendPlaylists(abort, recommended, listError)) {
            recommendedCount = recommended.size();
            auto converted = PutPlaylists(recommended, account.userId, PlaylistScope::Recommended);
            std::lock_guard lock(m_mutex);
            if (generation != m_accountGeneration) return superseded();
            m_recommendedPlaylists = std::move(converted);
        }
        core::Log(L"account: saved lists: " + std::to_wstring(followedCount) + L" artists, " +
                  std::to_wstring(albumCount) + L" albums, " + std::to_wstring(recommendedCount) +
                  L" recommended");
        if (readCallback) PostMain([readCallback = std::move(readCallback), readMark] { readCallback(readMark); });
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
            core::Log(L"artist: detail failed for " + std::to_wstring(artistId) + L": " + error);
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        m_client.GetArtistExtras(artistId, abort, artist);
        stream::Artist converted = ToArtist(artist);
        Catalog::Get().Records().Put(converted);
        {
            std::lock_guard lock(m_mutex);
            MarkFollowed(converted, m_followedArtists);
        }
        PostMain([done = std::move(done), converted = std::move(converted)] {
            if (done) done(converted, {});
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
            const stream::Id id = IdText(artistId);
            if (auto artist = Catalog::Get().Records().FindArtist(id)) {
                artist->followed = follow;
                artist->followKnown = true;
                Catalog::Get().Records().Put(*artist); // FindArtist returns a copy
                std::lock_guard lock(m_mutex);
                auto it = std::find_if(m_followedArtists.begin(), m_followedArtists.end(),
                                       [&id](const stream::Artist& a) { return a.id == id; });
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
            const stream::Id id = IdText(albumId);
            const auto album = Catalog::Get().Records().FindAlbum(id);
            std::lock_guard lock(m_mutex);
            auto it = std::find_if(m_collectedAlbums.begin(), m_collectedAlbums.end(),
                                   [&id](const stream::Album& a) { return a.id == id; });
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
                const stream::Id id = IdText(playlistId);
                stream::Playlist known;
                {
                    std::lock_guard lock(m_mutex);
                    for (const auto& playlist : m_collectedPlaylists)
                        if (playlist.id == id) known = playlist;
                    if (known.id.empty())
                        for (const auto& playlist : m_recommendedPlaylists)
                            if (playlist.id == id) known = playlist;
                }
                if (known.id.empty())
                    known = Catalog::Get().Records().FindPlaylist(id).value_or(stream::Playlist{});
                known.id = id;
                known.saved = subscribed && !known.own;
                Catalog::Get().Records().Put(known);
                std::lock_guard lock(m_mutex);
                auto it = FindById(m_collectedPlaylists, id);
                if (subscribed && it == m_collectedPlaylists.end() && !known.own)
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
                    const stream::Id id = IdText(playlistId);
                    if (auto existing = Catalog::Get().Records().FindPlaylist(id)) {
                        existing->name = name;
                        Catalog::Get().Records().Put(*existing);
                    }
                    std::lock_guard lock(m_mutex);
                    auto rename = [&](std::vector<stream::Playlist>& list) {
                        for (auto& playlist : list)
                            if (playlist.id == id) playlist.name = name;
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
            const stream::Id id = IdText(playlistId);
            auto remove = [&id](std::vector<stream::Playlist>& list) {
                list.erase(
                    std::remove_if(list.begin(), list.end(),
                                   [&id](const stream::Playlist& playlist) { return playlist.id == id; }),
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
        if (!ok) core::Log(L"playlist: reorder failed: " + error);
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
    ++m_accountGeneration;
    SaveSessionCookie(normalized);
    RefreshAccount(std::move(done));
}

void Session::Logout() {
    ++m_accountGeneration;
    ClearSessionCookie();
    ClearAccount();
    Catalog::Get().Records().ClearPreferredCovers();
    {
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
    NotifyChanged();
}

}  // namespace glasswave::netease
