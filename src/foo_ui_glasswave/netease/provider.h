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

    TaskQueue& Tasks() { return m_tasks; }
    Client& Api() { return m_client; }
    ServiceManager& Service() { return m_service; }
    Catalog& Songs() { return Catalog::Get(); }

    bool LoggedIn() const;
    Account CurrentAccount() const;
    int64_t LikedPlaylistId() const;
    bool IsLiked(int64_t songId) const;
    std::vector<Playlist> MyPlaylists() const;

    using DoneCallback = std::function<void(bool ok, std::wstring error)>;
    using PlaylistsCallback = std::function<void(std::vector<Playlist> playlists, std::wstring error)>;
    using SongsCallback = std::function<void(std::vector<Song> songs, std::wstring error)>;
    using DetailCallback = std::function<void(PlaylistDetail detail, std::wstring error)>;

    void StartService(DoneCallback done = {});
    void RefreshAccount(DoneCallback done = {});
    void RefreshLikes(DoneCallback done = {});
    void FetchMyPlaylists(PlaylistsCallback done);
    void FetchRecommended(PlaylistsCallback done);
    void FetchLiked(SongsCallback done);
    void FetchPlaylist(int64_t playlistId, DetailCallback done);
    void Search(std::wstring_view query, SongsCallback done);
    void ToggleLike(int64_t songId, bool like, DoneCallback done = {});

    void OnLoginCookie(std::wstring cookie, DoneCallback done = {});
    void Logout();

private:
    Provider();
    ~Provider();
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;

    void PostMain(std::function<void()> fn);
    bool EnsureAccount(abort_callback& abort, int64_t& userId, std::wstring& error);
    void SetAccountLocked(const Account& account);

    ServiceManager& m_service;
    Client m_client;
    TaskQueue m_tasks;
    std::atomic_bool m_stopping{false};
    std::atomic_bool m_initialized{false};

    mutable std::mutex m_mutex;
    Account m_account;
    std::vector<Playlist> m_myPlaylists;
    int64_t m_likedPlaylist = 0;
    mutable std::mutex m_likedMutex;
    std::unordered_set<int64_t> m_liked;
};

}  // namespace glasswave::netease
