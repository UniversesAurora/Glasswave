#pragma once

#include "netease/json.h"
#include "netease/service.h"
#include "netease/types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::netease {

struct QrLogin {
    std::wstring key;
    std::wstring url;
    std::wstring imageDataUrl;
};

struct QrStatus {
    int code = 0;  // 800 expired, 801 waiting, 802 scanned, 803 success; 0 = transport error
    std::wstring message;
    std::wstring cookie;
};

struct SongUrl {
    std::wstring url;
    std::wstring type;  // flac / mp3 / ...
    std::wstring level;
    int bitrate = 0;
    int code = 0;
    int expireSeconds = 1200;
    bool trial = false;
    std::wstring message;
};

struct PlaylistDetail {
    Playlist info;
    std::vector<Song> songs;
};

// Thin client for the local NeteaseCloudMusicApi Enhanced service. All methods block and
// are meant for worker threads; none of them touch UI state.
class Client {
public:
    explicit Client(ServiceManager& service) : m_service(service) {}

    bool QrKey(abort_callback& abort, QrLogin& out, std::wstring& error);
    bool QrCreate(std::wstring_view key, abort_callback& abort, QrLogin& out, std::wstring& error);
    QrStatus QrCheck(std::wstring_view key, abort_callback& abort);
    bool GetAccount(abort_callback& abort, Account& out, std::wstring& error);
    bool UserPlaylists(int64_t userId, abort_callback& abort, std::vector<Playlist>& out, int64_t& likedPlaylist,
                       std::wstring& error);
    bool GetPlaylist(int64_t playlistId, abort_callback& abort, PlaylistDetail& out, std::wstring& error);
    // The playlist's metadata only (name, cover, description, counts); also bypasses the
    // helper's GET cache so a cover that changed after an edit is seen immediately.
    bool GetPlaylistInfo(int64_t playlistId, abort_callback& abort, Playlist& out, std::wstring& error);
    bool SetPlaylistSubscribed(int64_t playlistId, bool subscribed, abort_callback& abort, std::wstring& error);
    bool RecommendPlaylists(abort_callback& abort, std::vector<Playlist>& out, std::wstring& error);
    bool SearchSongs(std::wstring_view query, int limit, int offset, abort_callback& abort, std::vector<Song>& out,
                     std::wstring& error);
    bool SongDetails(const std::vector<int64_t>& ids, abort_callback& abort, std::vector<Song>& out,
                     std::wstring& error);
    bool GetSongUrl(int64_t id, std::wstring_view level, abort_callback& abort, SongUrl& out, std::wstring& error);
    // Lyrics as LRC (/lyric/new): word-timed YRC becomes enhanced LRC; the translation is
    // added as reference lines. `lrc` stays empty when the song has none.
    bool GetLyrics(int64_t id, abort_callback& abort, std::wstring& lrc, std::wstring& error);
    bool GetArtistDetail(int64_t artistId, abort_callback& abort, Artist& out, std::wstring& error);
    bool GetArtistDescription(int64_t artistId, abort_callback& abort, std::wstring& brief, std::wstring& full,
                              std::wstring& error);
    bool GetArtistAlbumsPage(int64_t artistId, int offset, int limit, abort_callback& abort,
                             std::vector<Album>& out, bool* followed, bool* more, std::wstring& error);
    bool GetArtistSongsPage(int64_t artistId, int offset, int limit, std::wstring_view order,
                            abort_callback& abort, std::vector<Song>& out, bool* more, std::wstring& error);
    bool GetAlbum(int64_t albumId, abort_callback& abort, Album& info, std::vector<Song>& songs, std::wstring& error);
    bool GetFollowedArtists(abort_callback& abort, std::vector<Artist>& out, std::wstring& error);
    bool GetCollectedAlbums(abort_callback& abort, std::vector<Album>& out, std::wstring& error);
    bool GetCollectedAlbumsPage(int offset, int limit, abort_callback& abort, std::vector<Album>& out, bool& more,
                                std::wstring& error);
    bool SetArtistFollow(int64_t artistId, bool follow, abort_callback& abort, std::wstring& error);
    bool Likelist(int64_t userId, abort_callback& abort, std::vector<int64_t>& out, std::wstring& error);
    bool SetLike(int64_t id, bool like, abort_callback& abort, std::wstring& error);
    bool AddToPlaylist(int64_t playlistId, const std::vector<int64_t>& ids, abort_callback& abort,
                       std::wstring& error);
    bool RemoveFromPlaylist(int64_t playlistId, const std::vector<int64_t>& ids, abort_callback& abort,
                            std::wstring& error);
    bool SetAlbumCollect(int64_t albumId, bool collect, abort_callback& abort, std::wstring& error);
    bool CreatePlaylist(std::wstring_view name, abort_callback& abort, int64_t& id, std::wstring& error);
    bool RenamePlaylist(int64_t playlistId, std::wstring_view name, abort_callback& abort, std::wstring& error);
    bool DeletePlaylist(int64_t playlistId, abort_callback& abort, std::wstring& error);
    // Reorders the signed-in user's playlists to the given id sequence (created first, then
    // collected, matching what the navigation shows).
    bool ReorderPlaylists(const std::vector<int64_t>& ids, abort_callback& abort, std::wstring& error);

private:
    bool Request(std::wstring_view path, const std::vector<std::pair<std::wstring, std::wstring>>& params,
                 bool post, abort_callback& abort, json::Object& out, int& apiCode, std::wstring& error,
                 std::vector<std::wstring>* responseCookies = nullptr);

    ServiceManager& m_service;
};

// Parsing helpers shared with the catalog and tests.
Song ParseSong(const json::Object& object);
Playlist ParsePlaylist(const json::Object& object);

}  // namespace glasswave::netease
