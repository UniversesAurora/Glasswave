#pragma once

#include "netease/catalog.h"
#include "netease/client.h"
#include "netease/task_queue.h"
#include "netease/service.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace glasswave::netease {

// Account, session and asynchronous access to the NetEase service. All callbacks run on
// foobar2000's main thread; API work runs on `Tasks()`.
class Provider {
public:
    static Provider& Get();

    void Initialize();
    void Shutdown();
    // Called on the main thread when the account/session state changes.
    void SetStateCallback(std::function<void()> callback);

    TaskQueue& Tasks() { return m_tasks; }
    Client& Api() { return m_client; }
    ServiceManager& Service() { return m_service; }
    Catalog& Songs() { return Catalog::Get(); }

    bool LoggedIn() const;
    Account CurrentAccount() const;
    int64_t LikedPlaylistId() const;
    bool IsLiked(int64_t songId) const;
    std::vector<Playlist> CreatedPlaylists() const;
    std::vector<Playlist> CollectedPlaylists() const;
    std::vector<Playlist> RecommendedPlaylists() const;
    std::vector<Artist> FollowedArtists() const;
    std::vector<Album> CollectedAlbums() const;
    enum class PlaylistScope { Unknown, Created, Collected, Recommended };
    PlaylistScope FindPlaylistScope(int64_t playlistId) const;

    using DoneCallback = std::function<void(bool ok, std::wstring error)>;
    using PlaylistsCallback = std::function<void(std::vector<Playlist> playlists, std::wstring error)>;
    using SongsCallback = std::function<void(std::vector<Song> songs, std::wstring error)>;
    using ArtistsCallback = std::function<void(std::vector<Artist> artists, std::wstring error)>;
    using AlbumsCallback = std::function<void(std::vector<Album> albums, std::wstring error)>;
    using DetailCallback = std::function<void(PlaylistDetail detail, std::wstring error)>;
    struct ArtistPage {
        Artist info;
        std::vector<Album> albums;
    };
    using ArtistCallback = std::function<void(ArtistPage page, std::wstring error)>;

    void StartService(DoneCallback done = {});
    void RefreshAccount(DoneCallback done = {});
    void RefreshLikes(DoneCallback done = {});
    void FetchMyPlaylists(PlaylistsCallback done);
    void FetchRecommended(PlaylistsCallback done);
    void FetchFollowedArtists(ArtistsCallback done);
    void FetchCollectedAlbums(AlbumsCallback done);
    void FetchCollectedAlbumSongs(SongsCallback done);
    void FetchLiked(SongsCallback done);
    void FetchPlaylist(int64_t playlistId, DetailCallback done);
    void FetchArtist(int64_t artistId, std::function<void(Artist artist)> header, ArtistCallback done);
    void FetchArtistTopSongs(int64_t artistId, SongsCallback done);
    void FetchAlbumSongs(int64_t albumId, SongsCallback done);
    void FetchAlbumTracks(std::vector<Album> albums, SongsCallback done);
    void Search(std::wstring_view query, SongsCallback done);
    void ToggleLike(int64_t songId, bool like, DoneCallback done = {});
    void SetArtistFollow(int64_t artistId, bool follow, DoneCallback done = {});

    void OnLoginCookie(std::wstring cookie, DoneCallback done = {});
    void Logout();

private:
    Provider();
    ~Provider();
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;

    void PostMain(std::function<void()> fn);
    void NotifyChanged();
    bool RequireLogin(std::wstring& error) const;
    bool EnsureAccount(abort_callback& abort, int64_t& userId, std::wstring& error);
    void SetAccountLocked(const Account& account);

    ServiceManager& m_service;
    Client m_client;
    TaskQueue m_tasks;
    std::atomic_bool m_stopping{false};
    std::atomic_bool m_initialized{false};

    mutable std::mutex m_mutex;
    std::function<void()> m_stateCallback;
    Account m_account;
    std::vector<Playlist> m_createdPlaylists;
    std::vector<Playlist> m_collectedPlaylists;
    std::vector<Playlist> m_recommendedPlaylists;
    std::vector<Artist> m_followedArtists;
    std::vector<Album> m_collectedAlbums;
    int64_t m_likedPlaylist = 0;
    mutable std::mutex m_likedMutex;
    std::unordered_set<int64_t> m_liked;
};

}  // namespace glasswave::netease
