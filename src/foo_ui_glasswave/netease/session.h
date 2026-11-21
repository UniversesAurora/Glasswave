#pragma once

#include "netease/catalog.h"
#include "netease/client.h"
#include "core/task_queue.h"
#include "netease/service.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <map>
#include <optional>
#include <vector>

namespace glasswave::netease {

// Account, session and asynchronous access to the NetEase service. All callbacks run on
// foobar2000's main thread; API work runs on `Tasks()`.
class Session {
public:
    static Session& Get();

    void Initialize();
    void Shutdown();
    // Called on the main thread when the account/session state changes.
    void SetStateCallback(std::function<void()> callback);
    // Called on the main thread after a playlist/album cache update that changes covers,
    // names or counts but does not need the open page to be re-read.
    void SetLibraryCallback(std::function<void()> callback);
    // A whole library read (RefreshAccount) takes `mark()` before its first request and, once
    // it has stored what it read, calls `read(mark)` on the main thread (before the state
    // callback that reports the refresh). The provider confirms its users' changes with it.
    void SetLibraryReadCallbacks(std::function<uint64_t()> mark, std::function<void(uint64_t)> read);
    // Called on the main thread after a like/unlike; only the favorite flags of the open
    // rows change, so the page must not be re-read.
    void SetLikesCallback(std::function<void()> callback);

    core::TaskQueue& Tasks() { return m_tasks; }
    Client& Api() { return m_client; }
    ServiceManager& Service() { return m_service; }
    bool LoggedIn() const;
    Account CurrentAccount() const;
    int64_t LikedPlaylistId() const;
    bool IsLiked(int64_t songId) const;
    // The account's lists, as stream:: records: a playlist's own / saved flags say which list it
    // belongs to, a followed artist is followKnown and followed.
    std::vector<stream::Playlist> CreatedPlaylists() const;
    std::vector<stream::Playlist> CollectedPlaylists() const;
    std::vector<stream::Playlist> RecommendedPlaylists() const;
    std::vector<stream::Artist> FollowedArtists() const;
    std::vector<stream::Album> CollectedAlbums() const;
    bool IsAlbumCollected(int64_t albumId) const; // no copy of the list
    using PlaylistScope = netease::PlaylistScope;
    PlaylistScope FindPlaylistScope(int64_t playlistId) const;

    using DoneCallback = std::function<void(bool ok, std::wstring error)>;
    using ArtistDetailCallback = std::function<void(stream::Artist artist, std::wstring error)>;
    using ArtistDescriptionCallback =
        std::function<void(std::wstring brief, std::wstring full, std::wstring error)>;
    using CreatePlaylistCallback = std::function<void(bool ok, int64_t id, std::wstring error)>;

    void StartService(DoneCallback done = {});
    void RefreshAccount(DoneCallback done = {});
    void RefreshLikes(DoneCallback done = {});
    void FetchArtistDetail(int64_t artistId, ArtistDetailCallback done);
    void FetchArtistDescription(int64_t artistId, ArtistDescriptionCallback done);
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
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void PostMain(std::function<void()> fn);
    void NotifyChanged();
    void NotifyLibraryChanged();
    void NotifyLikesChanged();
    // The service can regenerate a playlist cover a moment after an edit; one delayed
    // summary read catches it without polling.
    void SchedulePlaylistSummaryRefresh(int64_t playlistId);
    // Under m_mutex: the list a playlist is in.
    PlaylistScope ScopeLocked(const stream::Id& playlistId) const;
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
    core::TaskQueue m_tasks;
    std::atomic_bool m_stopping{false};
    // Bumped by Logout and a new sign-in: a task that read the account before a bump must not
    // write what it fetched (it belongs to the previous sign-in).
    std::atomic<uint64_t> m_accountGeneration{0};
    abort_callback_impl m_abort;  // every request of the session; signalled by Shutdown
    std::atomic_bool m_initialized{false};

    mutable std::mutex m_mutex;
    std::function<void()> m_stateCallback;
    std::function<void()> m_libraryCallback;
    std::function<uint64_t()> m_libraryReadMark;
    std::function<void(uint64_t)> m_libraryReadCallback;
    std::function<void()> m_likesCallback;
    Account m_account;
    std::vector<stream::Playlist> m_createdPlaylists;
    std::vector<stream::Playlist> m_collectedPlaylists;
    std::vector<stream::Playlist> m_recommendedPlaylists;
    std::vector<stream::Artist> m_followedArtists;
    std::vector<stream::Album> m_collectedAlbums;
    int64_t m_likedPlaylist = 0;
    mutable std::mutex m_likedMutex;
    std::unordered_set<int64_t> m_liked;
};

}  // namespace glasswave::netease
