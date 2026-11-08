#include "pch.h"
#include "media/favorites.h"
#include "netease/config.h"
#include "netease/cookie.h"
#include "netease/handle.h"
#include "netease/log.h"
#include "netease/provider.h"

namespace glasswave::netease {

namespace {

std::wstring ErrorOf(const std::wstring& error) {
    return error.empty() ? L"The NetEase service did not return a result." : error;
}

class NeteaseFavoriteHook : public media::FavoriteHook {
public:
    explicit NeteaseFavoriteHook(Provider& provider) : m_provider(provider) {}

    bool Handles(const metadb_handle_ptr& track) const override {
        int64_t id = 0;
        return track.is_valid() && ParseSongPath(track->get_path(), id);
    }
    bool IsFavorite(const metadb_handle_ptr& track) const override {
        int64_t id = 0;
        return ParseSongPath(track->get_path(), id) && m_provider.IsLiked(id);
    }
    void Toggle(const metadb_handle_ptr& track) override {
        int64_t id = 0;
        if (!ParseSongPath(track->get_path(), id)) return;
        m_provider.ToggleLike(id, !m_provider.IsLiked(id));
    }
    void Set(const metadb_handle_ptr& track, bool favorite) override {
        int64_t id = 0;
        if (!ParseSongPath(track->get_path(), id)) return;
        if (m_provider.IsLiked(id) != favorite) m_provider.ToggleLike(id, favorite);
    }

private:
    Provider& m_provider;
};

std::unique_ptr<NeteaseFavoriteHook> g_favoriteHook;

}  // namespace

Provider& Provider::Get() {
    static Provider provider;
    return provider;
}

Provider::Provider() : m_service(ServiceManager::Get()), m_client(m_service), m_tasks(2) {}

Provider::~Provider() { Shutdown(); }

void Provider::Initialize() {
    if (m_initialized.exchange(true)) return;
    m_stopping = false;
    LoadSessionCookie();
    if (!CookieHasMusicU(SessionCookie())) ClearSessionCookie();
    m_service.Initialize();
    Catalog::Get().Load();
    Log(L"art: extractor supports ncm://song/ = " +
        std::wstring(album_art_extractor::g_is_supported_path("ncm://song/1") ? L"yes" : L"no"));
    if (!g_favoriteHook) g_favoriteHook = std::make_unique<NeteaseFavoriteHook>(*this);
    media::SetFavoriteHook(g_favoriteHook.get());
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

void Provider::Shutdown() {
    if (!m_initialized.exchange(false)) return;
    media::SetFavoriteHook(nullptr);
    g_favoriteHook.reset();
    m_stopping = true;
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

bool Provider::LoggedIn() const { return CookieHasMusicU(SessionCookie()); }

Account Provider::CurrentAccount() const {
    std::lock_guard lock(m_mutex);
    return m_account;
}

int64_t Provider::LikedPlaylistId() const {
    std::lock_guard lock(m_mutex);
    return m_likedPlaylist;
}

bool Provider::IsLiked(int64_t songId) const {
    std::lock_guard lock(m_likedMutex);
    return m_liked.count(songId) != 0;
}

std::vector<Playlist> Provider::CreatedPlaylists() const {
    std::lock_guard lock(m_mutex);
    return m_createdPlaylists;
}

std::vector<Playlist> Provider::CollectedPlaylists() const {
    std::lock_guard lock(m_mutex);
    return m_collectedPlaylists;
}

std::vector<Playlist> Provider::RecommendedPlaylists() const {
    std::lock_guard lock(m_mutex);
    return m_recommendedPlaylists;
}

std::vector<Artist> Provider::FollowedArtists() const {
    std::lock_guard lock(m_mutex);
    return m_followedArtists;
}

std::vector<Album> Provider::CollectedAlbums() const {
    std::lock_guard lock(m_mutex);
    return m_collectedAlbums;
}

void Provider::SetAccountLocked(const Account& account) {
    m_account = account;
    SetAccount(account.userId, account.nickname);
}

void Provider::PostMain(std::function<void()> fn) {
    if (m_stopping || !fn) return;
    fb2k::inMainThread([this, fn = std::move(fn)] {
        if (!m_stopping) fn();
    });
}

void Provider::SetStateCallback(std::function<void()> callback) {
    std::lock_guard lock(m_mutex);
    m_stateCallback = std::move(callback);
}

void Provider::NotifyChanged() {
    std::function<void()> callback;
    {
        std::lock_guard lock(m_mutex);
        callback = m_stateCallback;
    }
    if (callback) PostMain([callback = std::move(callback)] { callback(); });
}

bool Provider::RequireLogin(std::wstring& error) const {
    if (LoggedIn()) return true;
    error = L"Please sign in to NetEase Cloud Music.";
    return false;
}

bool Provider::EnsureAccount(abort_callback& abort, int64_t& userId, std::wstring& error) {
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

void Provider::StartService(DoneCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
        std::wstring error;
        const bool ok = m_service.EnsureStarted(abort, &error);
        PostMain([done = std::move(done), ok, error = std::move(error)] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
    });
}

void Provider::RefreshAccount(DoneCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
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
            std::lock_guard liked(m_likedMutex);
            m_liked.clear();
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
        std::vector<Artist> artists;
        if (m_client.GetFollowedArtists(abort, artists, listError)) {
            for (const auto& artist : artists) Catalog::Get().Put(artist);
            std::lock_guard lock(m_mutex);
            m_followedArtists = std::move(artists);
        }
        std::vector<Album> albums;
        if (m_client.GetCollectedAlbums(abort, albums, listError)) {
            Catalog::Get().Put(albums);
            std::lock_guard lock(m_mutex);
            m_collectedAlbums = std::move(albums);
        }
        std::vector<Playlist> recommended;
        if (m_client.RecommendPlaylists(abort, recommended, listError)) {
            for (const auto& playlist : recommended) Catalog::Get().Put(playlist);
            std::lock_guard lock(m_mutex);
            m_recommendedPlaylists = std::move(recommended);
        }
        PostMain([done = std::move(done)] {
            if (done) done(true, {});
        });
        NotifyChanged();
    });
}

void Provider::RefreshLikes(DoneCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
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

void Provider::FetchMyPlaylists(PlaylistsCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
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

void Provider::FetchRecommended(PlaylistsCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
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
        for (const auto& playlist : playlists) Catalog::Get().Put(playlist);
        {
            std::lock_guard lock(m_mutex);
            m_recommendedPlaylists = playlists;
        }
        PostMain([done = std::move(done), playlists = std::move(playlists)] {
            if (done) done(std::move(playlists), {});
        });
    });
}

void Provider::FetchFollowedArtists(ArtistsCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
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

void Provider::FetchCollectedAlbums(AlbumsCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
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

void Provider::FetchLiked(SongsCallback done) {
    m_tasks.Post([this, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
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

        PlaylistDetail detail;
        bool haveDetail = false;
        if (likedPlaylist) haveDetail = m_client.GetPlaylist(likedPlaylist, abort, detail, error);

        std::vector<Song> songs;
        if (haveDetail) {
            songs = std::move(detail.songs);
        } else {
            std::vector<int64_t> ids;
            if (!m_client.Likelist(userId, abort, ids, error)) {
                PostMain([done = std::move(done), error] {
                    if (done) done({}, ErrorOf(error));
                });
                return;
            }
            for (size_t start = 0; start < ids.size(); start += 200) {
                const size_t end = std::min(start + 200, ids.size());
                std::vector<int64_t> batch(ids.begin() + static_cast<std::ptrdiff_t>(start),
                                           ids.begin() + static_cast<std::ptrdiff_t>(end));
                std::vector<Song> part;
                if (!m_client.SongDetails(batch, abort, part, error)) break;
                songs.insert(songs.end(), part.begin(), part.end());
            }
        }

        {
            std::lock_guard liked(m_likedMutex);
            m_liked.clear();
            for (const auto& song : songs) m_liked.insert(song.id);
        }
        PostMain([done = std::move(done), songs = std::move(songs)]() mutable {
            Catalog::Get().PutAndHint(songs);
            if (done) done(std::move(songs), {});
        });
    });
}

void Provider::FetchPlaylist(int64_t playlistId, DetailCallback done) {
    m_tasks.Post([this, playlistId, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
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
        PostMain([done = std::move(done), detail = std::move(detail)]() mutable {
            Catalog::Get().PutAndHint(detail.songs);
            if (done) done(std::move(detail), {});
        });
    });
}

void Provider::FetchArtist(int64_t artistId, ArtistCallback done) {
    m_tasks.Post([this, artistId, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        ArtistPage page;
        if (!m_client.GetArtistDetail(artistId, abort, page.info, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        std::wstring albumError;
        bool followed = false;
        if (m_client.GetArtistAlbums(artistId, abort, page.albums, &followed, albumError)) {
            page.info.followed = followed;
            page.info.followKnown = true;
        } else {
            Log(L"artist: album list failed for " + std::to_wstring(artistId) + L": " + albumError);
        }
        Catalog::Get().Put(page.info);
        Catalog::Get().Put(page.albums);
        PostMain([done = std::move(done), page = std::move(page), albumError = std::move(albumError)]() mutable {
            if (done) done(std::move(page), albumError);
        });
    });
}

void Provider::FetchArtistTopSongs(int64_t artistId, SongsCallback done) {
    m_tasks.Post([this, artistId, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        std::vector<Song> songs;
        if (!m_client.GetArtistTopSongs(artistId, abort, songs, error)) {
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

void Provider::FetchAlbumSongs(int64_t albumId, SongsCallback done) {
    m_tasks.Post([this, albumId, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
        std::wstring error;
        if (!m_service.EnsureStarted(abort, &error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        Album info;
        std::vector<Song> songs;
        if (!m_client.GetAlbum(albumId, abort, info, songs, error)) {
            PostMain([done = std::move(done), error] {
                if (done) done({}, ErrorOf(error));
            });
            return;
        }
        Catalog::Get().Put(info);
        PostMain([done = std::move(done), songs = std::move(songs)]() mutable {
            Catalog::Get().PutAndHint(songs);
            if (done) done(std::move(songs), {});
        });
    });
}

void Provider::Search(std::wstring_view query, SongsCallback done) {
    m_tasks.Post([this, query = std::wstring(query), done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
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

void Provider::ToggleLike(int64_t songId, bool like, DoneCallback done) {
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
        abort_callback_impl abort;
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
        NotifyChanged();
    });
}

void Provider::SetArtistFollow(int64_t artistId, bool follow, DoneCallback done) {
    std::wstring loginError;
    if (!RequireLogin(loginError)) {
        if (done) done(false, loginError);
        return;
    }
    m_tasks.Post([this, artistId, follow, done = std::move(done)] {
        if (m_stopping) return;
        abort_callback_impl abort;
        std::wstring error;
        bool ok = false;
        if (m_service.EnsureStarted(abort, &error))
            ok = m_client.SetArtistFollow(artistId, follow, abort, error);
        PostMain([done = std::move(done), ok, error] {
            if (done) done(ok, ok ? std::wstring() : ErrorOf(error));
        });
    });
}

void Provider::OnLoginCookie(std::wstring cookie, DoneCallback done) {
    const std::wstring normalized = NormalizeCookie(cookie);
    if (!CookieHasMusicU(normalized)) {
        if (done) done(false, L"The login response did not contain a MUSIC_U session.");
        return;
    }
    SaveSessionCookie(normalized);
    RefreshAccount(std::move(done));
}

void Provider::Logout() {
    ClearSessionCookie();
    ClearAccount();
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
