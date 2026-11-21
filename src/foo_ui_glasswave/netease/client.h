#pragma once

#include "netease/json.h"
#include "netease/service.h"
#include "netease/types.h"
#include "stream/playback_source.h"

#include <cstdint>
#include <optional>
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

struct PlaylistSnapshot {
    Playlist info;
    std::vector<int64_t> trackIds;
    int64_t trackUpdateTime = 0;
};

// One page of a search category (/cloudsearch): the rows the service sent, its total and
// whether more follow.
struct SearchPage {
    size_t received = 0;
    std::optional<size_t> total;
    bool more = false;
};

// A song found by its lyrics: the line that matched and the two after it, and the lyrics whole
// (the service's highlight removed).
struct LyricHit {
    Song song;
    std::wstring excerpt;
    std::wstring text;
};

// One best match of a search (/search/multimatch): an artist, album, playlist or song.
using SearchHit = std::variant<Song, Artist, Album, Playlist>;

// What the service suggests while the user types: completions (/search/suggest?type=mobile),
// then its top songs, artists, albums and playlists (/search/suggest) in its own order.
struct SearchSuggestions {
    std::vector<std::wstring> terms;
    std::vector<SearchHit> items;
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
    // Playlist metadata and the complete ordered trackIds list, without hydrating every song.
    // NeteaseCloudMusicApi returns the complete trackIds even when tracks is only a prefix.
    bool GetPlaylistSnapshot(int64_t playlistId, abort_callback& abort, PlaylistSnapshot& out,
                             std::wstring& error);
    // The playlist's metadata only (name, cover, description, counts); also bypasses the
    // helper's GET cache so a cover that changed after an edit is seen immediately.
    bool GetPlaylistInfo(int64_t playlistId, abort_callback& abort, Playlist& out, std::wstring& error);
    bool SetPlaylistSubscribed(int64_t playlistId, bool subscribed, abort_callback& abort, std::wstring& error);
    bool RecommendPlaylists(abort_callback& abort, std::vector<Playlist>& out, std::wstring& error);
    bool SearchSongs(std::wstring_view query, int limit, int offset, abort_callback& abort, std::vector<Song>& out,
                     std::wstring& error);
    // The search categories, one page each.
    bool SearchSongsPage(std::wstring_view query, int limit, int offset, abort_callback& abort,
                         std::vector<Song>& out, SearchPage& page, std::wstring& error);
    bool SearchAlbumsPage(std::wstring_view query, int limit, int offset, abort_callback& abort,
                          std::vector<Album>& out, SearchPage& page, std::wstring& error);
    bool SearchArtistsPage(std::wstring_view query, int limit, int offset, abort_callback& abort,
                           std::vector<Artist>& out, SearchPage& page, std::wstring& error);
    bool SearchPlaylistsPage(std::wstring_view query, int limit, int offset, abort_callback& abort,
                             std::vector<Playlist>& out, SearchPage& page, std::wstring& error);
    bool SearchLyricsPage(std::wstring_view query, int limit, int offset, abort_callback& abort,
                          std::vector<LyricHit>& out, SearchPage& page, std::wstring& error);
    // The best matches the service puts above its results (empty when it has none).
    bool SearchBestMatches(std::wstring_view query, abort_callback& abort, std::vector<SearchHit>& out,
                           std::wstring& error);
    bool SearchSuggest(std::wstring_view query, abort_callback& abort, SearchSuggestions& out, std::wstring& error);
    bool SongDetails(const std::vector<int64_t>& ids, abort_callback& abort, std::vector<Song>& out,
                     std::wstring& error);
    stream::SourceStatus GetSongUrl(int64_t id, std::wstring_view level, abort_callback& abort, SongUrl& out,
                                   std::wstring& error);
    // Lyrics as LRC (/lyric/new): word-timed YRC becomes enhanced LRC; the translation is
    // added as reference lines. `lrc` stays empty when the song has none.
    bool GetLyrics(int64_t id, abort_callback& abort, std::wstring& lrc, std::wstring& error);
    bool GetArtistDetail(int64_t artistId, abort_callback& abort, Artist& out, std::wstring& error);
    // The song's detail and, best effort, its wiki (style, tags, language, BPM, similar songs).
    bool GetSongInfo(int64_t songId, abort_callback& abort, SongInfo& out, std::wstring& error);
    // The artist wiki's area and type, into `out` (best effort: a failed request leaves them
    // unknown).
    void GetArtistExtras(int64_t artistId, abort_callback& abort, Artist& out);
    bool GetArtistDescription(int64_t artistId, abort_callback& abort, std::wstring& brief, std::wstring& full,
                              std::wstring& error);
    // NetEase's similar artists (signed in only, as in its client).
    bool GetSimilarArtists(int64_t artistId, abort_callback& abort, std::vector<Artist>& out, std::wstring& error);
    bool GetArtistAlbumsPage(int64_t artistId, int offset, int limit, abort_callback& abort,
                             std::vector<Album>& out, bool* followed, bool* more, std::wstring& error,
                             size_t* received = nullptr);
    bool GetArtistSongsPage(int64_t artistId, int offset, int limit, std::wstring_view order,
                            abort_callback& abort, std::vector<Song>& out, bool* more, std::wstring& error,
                            size_t* received = nullptr);
    bool GetAlbum(int64_t albumId, abort_callback& abort, Album& info, std::vector<Song>& songs, std::wstring& error);
    bool GetFollowedArtistsPage(int offset, int limit, abort_callback& abort, std::vector<Artist>& out,
                                bool& more, size_t& received, std::wstring& error);
    bool GetFollowedArtists(abort_callback& abort, std::vector<Artist>& out, std::wstring& error);
    bool GetCollectedAlbums(abort_callback& abort, std::vector<Album>& out, std::wstring& error);
    bool GetCollectedAlbumsPage(int offset, int limit, abort_callback& abort, std::vector<Album>& out,
                                bool& more, std::wstring& error, size_t* received = nullptr);
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
    // The PC client's home page (read from its own bundle, 2026-11-15): the cards at its top and
    // its rows of playlists and songs. Podcasts and other kinds the UI cannot show are left out.
    bool HomeCards(abort_callback& abort, std::vector<HomeCard>& out, std::wstring& error);
    bool HomeBlocks(abort_callback& abort, std::vector<HomeBlock>& out, std::wstring& error);
    // Today's recommended songs (每日推荐, /recommend/songs).
    bool DailySongs(abort_callback& abort, std::vector<Song>& out, std::wstring& error);
    // A style's daily songs (华语流行日推, the PC client's /api/homepage/category/daily/song/list):
    // the card's category, tag and the songs it shows first (comma separated).
    bool StyleDailySongs(std::wstring_view category, std::wstring_view tag, std::wstring_view songs,
                         abort_callback& abort, std::vector<Song>& out, std::wstring& error);
    // The personal FM's next songs (私人漫游, /personal_fm: three at a time).
    bool PersonalFm(abort_callback& abort, std::vector<Song>& out, std::wstring& error);
    // The personal FM in another mode, seeded (the PC client's /api/v1/radio/get with mode and
    // sourceIds; 相似歌曲 is mode FAMILIAR seeded with songs, 相似艺人 with artists). sourceIds: a
    // JSON array of ids as text.
    bool FmMode(std::wstring_view mode, std::wstring_view sourceIds, abort_callback& abort, std::vector<Song>& out,
                std::wstring& error);
    // Heartbeat mode (心动模式, /playmode/intelligence/list): songs like `seed`, drawn from and
    // around the liked songs' playlist.
    bool Heartbeat(int64_t seed, int64_t likedPlaylist, abort_callback& abort, std::vector<Song>& out,
                   std::wstring& error);
    // 不感兴趣 for a personal FM song (/fm_trash).
    bool FmTrash(int64_t songId, abort_callback& abort, std::wstring& error);

private:
    // One of the service's own API paths through the helper's generic call
    // (/api?uri=&data=&crypto=eapi), for endpoints the helper has no route for.
    bool Generic(std::wstring_view uri, std::wstring_view data, abort_callback& abort, json::Object& out,
                 std::wstring& error);
    bool Request(std::wstring_view path, const std::vector<std::pair<std::wstring, std::wstring>>& params,
                 bool post, abort_callback& abort, json::Object& out, int& apiCode, std::wstring& error,
                 std::vector<std::wstring>* responseCookies = nullptr);
    // One page of /cloudsearch of the given type: the objects under result.<rowsKey>.
    bool CloudSearch(std::wstring_view query, int type, int limit, int offset, const wchar_t* rowsKey,
                     const wchar_t* countKey, abort_callback& abort, std::vector<json::Object>& rows,
                     SearchPage& page, std::wstring& error);

    ServiceManager& m_service;
};

// Parsing helpers shared with the catalog and tests.
Song ParseSong(const json::Object& object);
Playlist ParsePlaylist(const json::Object& object);

}  // namespace glasswave::netease
