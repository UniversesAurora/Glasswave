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
    // Called on the main thread after a playlist/album cache update that changes covers,
    // names or counts but does not need the open page to be re-read.
    void SetLibraryCallback(std::function<void()> callback);
    // Called on the main thread after a like/unlike; only the favorite flags of the open
    // rows change, so the page must not be re-read.
    void SetLikesCallback(std::function<void()> callback);

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
    using SongsPageCallback = std::function<void(std::vector<Song> songs, bool more, std::wstring error)>;
    using ArtistsCallback = std::function<void(std::vector<Artist> artists, std::wstring error)>;
    using AlbumsCallback = std::function<void(std::vector<Album> albums, std::wstring error)>;
    using AlbumsPageCallback = std::function<void(std::vector<Album> albums, bool more, std::wstring error)>;
    using DetailCallback = std::function<void(PlaylistDetail detail, std::wstring error)>;
    using ArtistDetailCallback = std::function<void(Artist artist, std::wstring error)>;
    using ArtistDescriptionCallback =
        std::function<void(std::wstring brief, std::wstring full, std::wstring error)>;
    using ArtistAlbumsCallback =
        std::function<void(std::vector<Album> albums, bool more, bool followedKnown, bool followed, std::wstring error)>;
    using AlbumDetailCallback =
        std::function<void(Album album, std::vector<Song> songs, std::wstring error)>;
    using CreatePlaylistCallback = std::function<void(bool ok, int64_t id, std::wstring error)>;

    void StartService(DoneCallback done = {});
    void RefreshAccount(DoneCallback done = {});
    void RefreshLikes(DoneCallback done = {});
    void FetchMyPlaylists(PlaylistsCallback done);
    void FetchRecommended(PlaylistsCallback done);
    void FetchFollowedArtists(ArtistsCallback done);
    void FetchCollectedAlbums(AlbumsCallback done);
    void FetchCollectedAlbumsPage(int offset, int limit, AlbumsPageCallback done);
    void FetchLiked(SongsCallback done);
    void FetchPlaylist(int64_t playlistId, DetailCallback done);
    void FetchArtistDetail(int64_t artistId, ArtistDetailCallback done);
    void FetchArtistDescription(int64_t artistId, ArtistDescriptionCallback done);
    void FetchArtistAlbumsPage(int64_t artistId, int offset, int limit, ArtistAlbumsCallback done);
    void FetchArtistSongsPage(int64_t artistId, int offset, int limit, SongsPageCallback done);
    void FetchAlbumDetail(int64_t albumId, AlbumDetailCallback done);
    void FetchAlbumTracks(std::vector<Album> albums, SongsCallback done);
    void Search(std::wstring_view query, SongsCallback done);
    void ToggleLike(int64_t songId, bool like, DoneCallback done = {});
    void SetArtistFollow(int64_t artistId, bool follow, DoneCallback done = {});
    void AddToPlaylist(int64_t playlistId, std::vector<int64_t> songIds, DoneCallback done = {});
    void RemoveFromPlaylist(int64_t playlistId, std::vector<int64_t> songIds, DoneCallback done = {});
    void SetAlbumCollected(int64_t albumId, bool collected, DoneCallback done = {});
    void SetPlaylistSubscribed(int64_t playlistId, bool subscribed, DoneCallback done = {});
    void CreatePlaylist(std::wstring name, CreatePlaylistCallback done);
    void RenamePlaylist(int64_t playlistId, std::wstring name, DoneCallback done = {});
    void DeletePlaylist(int64_t playlistId, DoneCallback done = {});
    // Reorders created/collected playlists to the given id sequence (created first, then
    // collected), updates the caches immediately and confirms with the service.
    void ReorderPlaylists(std::vector<int64_t> ids, DoneCallback done = {});

    void OnLoginCookie(std::wstring cookie, DoneCallback done = {});
    void Logout();

private:
    Provider();
    ~Provider();
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;

    void PostMain(std::function<void()> fn);
    void NotifyChanged();
    void NotifyLibraryChanged();
    void NotifyLikesChanged();
    // The service can regenerate a playlist cover a moment after an edit; one delayed
    // summary read catches it without polling.
    void SchedulePlaylistSummaryRefresh(int64_t playlistId);
    bool RequireLogin(std::wstring& error) const;
    bool EnsureAccount(abort_callback& abort, int64_t& userId, std::wstring& error);
    void SetAccountLocked(const Account& account);
    // Updates the catalog and the cached playlists with a fresh summary (cover, name,
    // count). Worker thread; the caller posts NotifyChanged afterwards.
    void ApplyPlaylistInfo(const Playlist& info);
    void ApplyPlaylistOrder(const std::vector<int64_t>& ids);
    // Re-reads the user's playlists after a create/delete so the navigation lists stay exact.
    bool ReloadUserPlaylists(abort_callback& abort, int64_t userId, std::wstring& error);

    ServiceManager& m_service;
    Client m_client;
    TaskQueue m_tasks;
    std::atomic_bool m_stopping{false};
    std::atomic_bool m_initialized{false};

    mutable std::mutex m_mutex;
    std::function<void()> m_stateCallback;
    std::function<void()> m_libraryCallback;
    std::function<void()> m_likesCallback;
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
