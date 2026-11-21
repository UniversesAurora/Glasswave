#include "pch.h"
#include "netease/client.h"
#include "core/text.h"
#include "core/url.h"
#include "netease/cookie.h"
#include "net/http.h"
#include "core/log.h"
#include "lyrics/lyrics.h"

#include <chrono>
#include <limits>
#include <optional>

namespace glasswave::netease {

namespace {

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonValueType;

using core::Wide;
using core::Utf8;

std::wstring MessageOf(const core::json::Object& body, int code) {
    std::wstring message = core::json::GetString(body, L"message");
    if (message.empty()) message = core::json::GetString(body, L"msg");
    if (message.empty()) message = L"NetEase API error " + std::to_wstring(code);
    return message;
}

std::vector<ArtistRef> ArtistRefs(const core::json::Object& object) {
    auto parse = [](const core::json::Array& array) {
        std::vector<ArtistRef> out;
        for (const auto& artist : core::json::Objects(array)) {
            ArtistRef ref;
            ref.id = core::json::GetInt64(artist, L"id");
            ref.name = core::json::GetString(artist, L"name");
            if (ref.id || !ref.name.empty()) out.push_back(std::move(ref));
        }
        return out;
    };
    if (auto artists = core::json::GetArray(object, L"ar")) {
        auto out = parse(*artists);
        if (!out.empty()) return out;
    }
    if (auto artists = core::json::GetArray(object, L"artists")) return parse(*artists);
    return {};
}

Album ParseAlbum(const core::json::Object& object) {
    Album album;
    album.id = core::json::GetInt64(object, L"id");
    album.name = core::json::GetString(object, L"name");
    album.coverUrl = core::json::GetString(object, L"picUrl",
                                     core::json::GetString(object, L"coverImgUrl", core::json::GetString(object, L"blurPicUrl")));
    album.trackCount = core::json::GetInt(object, L"size", core::json::GetInt(object, L"trackCount"));
    album.publishTime = core::json::GetInt64(object, L"publishTime");
    album.artistRefs = ArtistRefs(object);
    album.description = core::json::GetString(object, L"description");
    album.company = core::json::GetString(object, L"company");
    album.type = core::json::GetString(object, L"type");
    album.subType = core::json::GetString(object, L"subType");
    album.tags = core::json::GetString(object, L"tags");
    for (const wchar_t* key : {L"alias", L"transNames"})
        if (const auto names = core::json::GetArray(object, key))
            for (uint32_t i = 0; i < names->Size(); ++i)
                if (const auto value = names->GetAt(i);
                    value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String) {
                    std::wstring name = value.GetString().c_str();
                    if (!name.empty() && name != album.name &&
                        std::find(album.aliases.begin(), album.aliases.end(), name) == album.aliases.end())
                        album.aliases.push_back(std::move(name));
                }
    if (album.name.empty()) album.name = L"Unknown";
    return album;
}

Artist ParseArtist(const core::json::Object& object) {
    Artist artist;
    artist.id = core::json::GetInt64(object, L"id");
    artist.name = core::json::GetString(object, L"name");
    artist.avatarUrl = core::json::GetString(object, L"picUrl",
                                       core::json::GetString(object, L"avatar",
                                                       core::json::GetString(object, L"img1v1Url",
                                                                       core::json::GetString(object, L"cover"))));
    artist.brief = core::json::GetString(object, L"briefDesc");
    artist.albumCount = core::json::GetInt(object, L"albumSize");
    artist.songCount = core::json::GetInt(object, L"musicSize");
    if (artist.name.empty()) artist.name = L"Unknown";
    return artist;
}

std::vector<int64_t> IdsOf(const core::json::Array& array) {
    std::vector<int64_t> ids;
    if (!array) return ids;
    for (uint32_t i = 0, n = array.Size(); i < n; ++i) {
        const IJsonValue value = array.GetAt(i);
        if (!value) continue;
        if (value.ValueType() == JsonValueType::Object) {
            const int64_t id = core::json::GetInt64(value.GetObject(), L"id");
            if (id) ids.push_back(id);
        } else if (value.ValueType() == JsonValueType::Number) {
            const int64_t id = static_cast<int64_t>(value.GetNumber());
            if (id) ids.push_back(id);
        }
    }
    return ids;
}

std::wstring JoinIds(const std::vector<int64_t>& ids) {
    std::wstring out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += L",";
        out += std::to_wstring(ids[i]);
    }
    return out;
}

std::wstring CookieNames(std::wstring_view cookie) {
    std::wstring out;
    size_t start = 0;
    while (start < cookie.size()) {
        const size_t end = cookie.find(L';', start);
        const std::wstring part(cookie.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos
                                                                                   : end - start));
        const size_t eq = part.find(L'=');
        if (eq != std::wstring::npos && eq > 0) {
            if (!out.empty()) out += L",";
            out += part.substr(0, eq);
        }
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return out;
}

}  // namespace

Song ParseSong(const core::json::Object& object) {
    Song song;
    song.id = core::json::GetInt64(object, L"id");
    song.title = core::json::GetString(object, L"name");
    song.artistRefs = ArtistRefs(object);
    for (const auto& ref : song.artistRefs)
        if (!ref.name.empty()) song.artists.push_back(ref.name);
    song.durationMs = core::json::GetInt(object, L"dt", core::json::GetInt(object, L"duration", 0));
    song.fee = core::json::GetInt(object, L"fee");
    song.popularity = core::json::GetInt(object, L"pop");
    // 原唱: the song objects of /song/detail, playlists, albums and searches carry it.
    song.original = core::json::GetInt(object, L"originCoverType") == 1;
    // The grey note after the title in NetEase's lists: the song's alias, else its translated
    // title (older endpoints name them alias / transNames).
    for (const wchar_t* key : {L"alia", L"alias", L"tns", L"transNames"}) {
        if (!song.note.empty()) break;
        if (const auto names = core::json::GetArray(object, key))
            for (uint32_t i = 0; i < names->Size() && song.note.empty(); ++i)
                if (const auto value = names->GetAt(i);
                    value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                    song.note = value.GetString().c_str();
    }

    if (auto album = core::json::GetObject(object, L"al")) {
        song.album = core::json::GetString(*album, L"name");
        song.albumId = core::json::GetInt64(*album, L"id");
        song.coverUrl = core::json::GetString(*album, L"picUrl");
    } else if (auto albumObject = core::json::GetObject(object, L"album")) {
        song.album = core::json::GetString(*albumObject, L"name");
        song.albumId = core::json::GetInt64(*albumObject, L"id");
        song.coverUrl = core::json::GetString(*albumObject, L"picUrl");
    }
    if (song.title.empty()) song.title = L"Unknown";

    const auto setQuality = [&song](const core::json::Object& quality, const wchar_t* codec, int bits) {
        const int bitrate = core::json::GetInt(quality, L"br");
        const int sampleRate = core::json::GetInt(quality, L"sr");
        if (bitrate > 0) {
            song.codec = codec;
            song.bitrate = bitrate;
            song.sampleRate = sampleRate;
            song.bitsPerSample = bits;
        }
    };
    if (auto hr = core::json::GetObject(object, L"hr"))
        setQuality(*hr, L"FLAC", 24);
    else if (auto sq = core::json::GetObject(object, L"sq"))
        setQuality(*sq, L"FLAC", 16);
    else if (auto high = core::json::GetObject(object, L"h"))
        setQuality(*high, L"MP3", 0);
    else if (auto medium = core::json::GetObject(object, L"m"))
        setQuality(*medium, L"MP3", 0);
    else if (auto low = core::json::GetObject(object, L"l"))
        setQuality(*low, L"MP3", 0);
    return song;
}

Playlist ParsePlaylist(const core::json::Object& object) {
    Playlist playlist;
    playlist.id = core::json::GetInt64(object, L"id");
    playlist.name = core::json::GetString(object, L"name");
    playlist.coverUrl = core::json::GetString(object, L"coverImgUrl", core::json::GetString(object, L"picUrl"));
    playlist.trackCount = core::json::GetInt(object, L"trackCount");
    playlist.subscribed = core::json::GetBool(object, L"subscribed");
    playlist.description = core::json::GetString(object, L"description");
    if (auto creator = core::json::GetObject(object, L"creator")) {
        playlist.creatorId = core::json::GetInt64(*creator, L"userId");
        playlist.creatorName = core::json::GetString(*creator, L"nickname");
    }
    playlist.createTime = core::json::GetInt64(object, L"createTime");
    // trackNumberUpdateTime moves when songs are added or removed. trackUpdateTime also moves
    // whenever NetEase changes any listed song on its side (it reads "today" on lists nobody
    // edited for years), so it is only a cache revision, never the displayed date.
    playlist.updateTime =
        core::json::GetInt64(object, L"trackNumberUpdateTime", core::json::GetInt64(object, L"updateTime"));
    if (const auto tags = core::json::GetArray(object, L"tags"))
        for (uint32_t i = 0; i < tags->Size(); ++i)
            if (const auto value = tags->GetAt(i);
                value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                playlist.tags.emplace_back(value.GetString().c_str());
    playlist.updateFrequency = core::json::GetString(object, L"updateFrequency");
    playlist.englishTitle = core::json::GetString(object, L"englishTitle");
    playlist.chart = !core::json::GetString(object, L"ToplistType").empty();
    if (playlist.name.empty()) playlist.name = L"Unknown";
    return playlist;
}

bool Client::Request(std::wstring_view path, const std::vector<std::pair<std::wstring, std::wstring>>& params,
                     bool post, abort_callback& abort, core::json::Object& out, int& apiCode, std::wstring& error,
                     std::vector<std::wstring>* responseCookies) {
    const std::string base = m_service.BaseUrl();
    if (base.empty()) {
        error = L"The NetEase service is not running.";
        return false;
    }

    std::string query;
    const std::wstring cookie = SessionCookie();
    auto append = [&](std::wstring_view name, std::wstring_view value) {
        if (!query.empty()) query += "&";
        query += core::UrlEncode(name);
        query += "=";
        query += core::UrlEncode(value);
    };
    for (const auto& [name, value] : params) append(name, value);
    // The Enhanced server caches successful responses for two minutes. A changing
    // parameter keeps account state, QR polls and short-lived playback URLs fresh.
    append(L"_t", std::to_wstring(GetTickCount64()));

    const std::wstring url = Wide(base) + std::wstring(path) + (query.empty() ? L"" : L"?" + Wide(query));
    const std::wstring method = post ? L"POST" : L"GET";
    const auto started = std::chrono::steady_clock::now();

    net::HttpOptions options;
    options.method = method;
    options.noProxy = true;
    options.timeoutMs = 25000;
    options.maxBodyBytes = 48u * 1024u * 1024u;
    options.headers = {{L"Accept", L"application/json"}};
    if (!cookie.empty()) options.headers.emplace_back(L"Cookie", cookie);
    const net::HttpResponse response = net::SendHttp(url, options, abort);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    core::LogHttp(method, path, response.status, elapsed);

    if (responseCookies) *responseCookies = response.setCookies;
    if (response.status == 0) {
        error = response.error.empty() ? L"Cannot reach the NetEase service." : response.error;
        return false;
    }
    std::wstring parseError;
    const auto parsed = core::json::Parse(response.body, &parseError);
    if (!parsed) {
        error = L"Invalid JSON from the NetEase service";
        if (!parseError.empty()) error += L": " + parseError;
        return false;
    }
    out = *parsed;
    apiCode = core::json::GetInt(out, L"code", static_cast<int>(response.status));
    return true;
}

bool Client::QrKey(abort_callback& abort, QrLogin& out, std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/login/qr/key", {}, true, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto data = core::json::GetObject(body, L"data");
    if (!data) {
        error = L"The login service returned no QR key.";
        return false;
    }
    out.key = core::json::GetString(*data, L"unikey");
    if (out.key.empty()) {
        if (auto nested = core::json::GetObject(*data, L"data")) out.key = core::json::GetString(*nested, L"unikey");
    }
    if (out.key.empty()) {
        error = L"The login service returned an empty QR key.";
        return false;
    }
    return true;
}

bool Client::QrCreate(std::wstring_view key, abort_callback& abort, QrLogin& out, std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/login/qr/create", {{L"key", std::wstring(key)}, {L"qrimg", L"true"}}, false, abort, body, code,
                 error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto data = core::json::GetObject(body, L"data");
    if (!data) {
        error = L"The login service returned no QR image.";
        return false;
    }
    out.url = core::json::GetString(*data, L"qrurl");
    out.imageDataUrl = core::json::GetString(*data, L"qrimg");
    if (out.imageDataUrl.empty()) {
        error = L"The login service returned an empty QR image.";
        return false;
    }
    return true;
}

QrStatus Client::QrCheck(std::wstring_view key, abort_callback& abort) {
    QrStatus status;
    core::json::Object body;
    int code = 0;
    std::wstring error;
    std::vector<std::wstring> responseCookies;
    if (!Request(L"/login/qr/check", {{L"key", std::wstring(key)}}, false, abort, body, code, error,
                 &responseCookies)) {
        status.message = error;
        return status;
    }
    status.code = code;
    status.message = core::json::GetString(body, L"message", core::json::GetString(body, L"msg"));
    std::wstring rawCookie = core::json::GetString(body, L"cookie");
    if (auto cookies = core::json::GetArray(body, L"cookie")) {
        for (uint32_t i = 0, n = cookies->Size(); i < n; ++i) {
            const IJsonValue value = cookies->GetAt(i);
            if (value && value.ValueType() == JsonValueType::String) {
                if (!rawCookie.empty()) rawCookie += L"; ";
                rawCookie += value.GetString().c_str();
            }
        }
    }
    for (const auto& cookie : responseCookies) {
        if (!rawCookie.empty()) rawCookie += L"; ";
        rawCookie += cookie;
    }
    status.cookie = NormalizeCookie(rawCookie);
    if (code == 803 || !status.cookie.empty()) {
        core::Log(L"login: qr check code " + std::to_wstring(code) + L", json/header cookies " +
            std::to_wstring(responseCookies.size()) + L", normalized " + std::to_wstring(status.cookie.size()) +
            (CookieHasMusicU(status.cookie) ? L", MUSIC_U ok" : L", MUSIC_U missing") + L", names [" +
            CookieNames(status.cookie) + L"]");
    }
    return status;
}

bool Client::GetAccount(abort_callback& abort, Account& out, std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/user/account", {}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto account = core::json::GetObject(body, L"account");
    auto profile = core::json::GetObject(body, L"profile");
    out = {};
    if (!account || account->Size() == 0 || !profile || core::json::GetBool(*account, L"anonimousUser")) return true;
    out.loggedIn = true;
    out.userId = core::json::GetInt64(*account, L"id");
    if (out.userId == 0) out.userId = core::json::GetInt64(*profile, L"userId");
    out.nickname = core::json::GetString(*profile, L"nickname");
    return true;
}

bool Client::UserPlaylists(int64_t userId, abort_callback& abort, std::vector<Playlist>& out,
                           int64_t& likedPlaylist, std::wstring& error) {
    out.clear();
    likedPlaylist = 0;
    int offset = 0;
    for (int page = 0; page < 20; ++page) {
        core::json::Object body;
        int code = 0;
        if (!Request(L"/user/playlist",
                     {{L"uid", std::to_wstring(userId)},
                      {L"limit", L"1000"},
                      {L"offset", std::to_wstring(offset)}},
                     false, abort, body, code, error))
            return false;
        if (code != 200) {
            error = MessageOf(body, code);
            return false;
        }
        auto list = core::json::GetArray(body, L"playlist");
        if (!list) break;
        for (const auto& object : core::json::Objects(*list)) {
            const int special = core::json::GetInt(object, L"specialType");
            Playlist playlist = ParsePlaylist(object);
            if (special == 5) {
                const bool own = playlist.creatorId == userId;
                if (own || likedPlaylist == 0) likedPlaylist = playlist.id;
                continue;
            }
            // 100 is 私人雷达 (personal radar): an official dynamic playlist that users
            // collect like any other. Keep it in the account lists even though it is
            // marked special; the other special types (yearly reviews, ...) stay out.
            if (special != 0 && special != 100) continue;
            if (playlist.id) out.push_back(std::move(playlist));
        }
        if (!core::json::GetBool(body, L"more")) break;
        offset += static_cast<int>(list->Size());
        if (list->Size() == 0) break;
    }
    return true;
}

bool Client::GetPlaylist(int64_t playlistId, abort_callback& abort, PlaylistDetail& out,
                         std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playlist/detail",
                 {{L"id", std::to_wstring(playlistId)}, {L"n", L"1000"}, {L"s", L"8"}}, false, abort, body, code,
                 error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto playlist = core::json::GetObject(body, L"playlist");
    if (!playlist) {
        error = L"The playlist response has no playlist object.";
        return false;
    }
    out.info = ParsePlaylist(*playlist);
    out.info.id = out.info.id ? out.info.id : playlistId;

    std::map<int64_t, Song> songs;
    if (auto tracks = core::json::GetArray(*playlist, L"tracks")) {
        for (const auto& object : core::json::Objects(*tracks)) {
            Song song = ParseSong(object);
            if (song.id) songs.emplace(song.id, std::move(song));
        }
    }
    std::vector<int64_t> order;
    if (auto trackIds = core::json::GetArray(*playlist, L"trackIds")) order = IdsOf(*trackIds);
    if (order.empty()) {
        out.songs.reserve(songs.size());
        for (auto& [id, song] : songs) out.songs.push_back(std::move(song));
        return true;
    }

    std::vector<int64_t> missing;
    for (int64_t id : order)
        if (!songs.count(id)) missing.push_back(id);
    for (size_t start = 0; start < missing.size(); start += 200) {
        const size_t end = std::min(start + 200, missing.size());
        std::vector<int64_t> batch(missing.begin() + static_cast<std::ptrdiff_t>(start),
                                   missing.begin() + static_cast<std::ptrdiff_t>(end));
        std::vector<Song> details;
        if (!SongDetails(batch, abort, details, error)) return false;
        for (auto& song : details)
            if (song.id) songs.emplace(song.id, std::move(song));
    }

    out.songs.reserve(order.size());
    for (int64_t id : order) {
        auto it = songs.find(id);
        if (it != songs.end()) out.songs.push_back(it->second);
    }
    return true;
}

bool Client::GetPlaylistSnapshot(int64_t playlistId, abort_callback& abort, PlaylistSnapshot& out,
                                 std::wstring& error) {
    out = {};
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playlist/detail", {{L"id", std::to_wstring(playlistId)}, {L"n", L"1"}, {L"s", L"8"}},
                 false, abort, body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto playlist = core::json::GetObject(body, L"playlist");
    if (!playlist) {
        error = L"The playlist response has no playlist object.";
        return false;
    }
    out.info = ParsePlaylist(*playlist);
    out.info.id = out.info.id ? out.info.id : playlistId;
    out.trackUpdateTime =
        core::json::GetInt64(*playlist, L"trackUpdateTime", core::json::GetInt64(*playlist, L"updateTime"));
    if (auto trackIds = core::json::GetArray(*playlist, L"trackIds")) out.trackIds = IdsOf(*trackIds);
    if (out.trackIds.empty() && out.info.trackCount != 0) {
        error = L"The playlist response has no complete trackIds list.";
        return false;
    }
    return true;
}

bool Client::GetPlaylistInfo(int64_t playlistId, abort_callback& abort, Playlist& out, std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playlist/detail",
                 {{L"id", std::to_wstring(playlistId)},
                  {L"n", L"1"},
                  {L"_t", std::to_wstring(GetTickCount64())}},
                 false, abort, body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto playlist = core::json::GetObject(body, L"playlist");
    if (!playlist) {
        error = L"The playlist response has no playlist object.";
        return false;
    }
    out = ParsePlaylist(*playlist);
    out.id = out.id ? out.id : playlistId;
    return true;
}

bool Client::RecommendPlaylists(abort_callback& abort, std::vector<Playlist>& out, std::wstring& error) {
    out.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/recommend/resource", {}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (auto list = core::json::GetArray(body, L"recommend")) {
        for (const auto& object : core::json::Objects(*list)) {
            Playlist playlist = ParsePlaylist(object);
            if (playlist.id) out.push_back(std::move(playlist));
        }
    }
    return true;
}

bool Client::Generic(std::wstring_view uri, std::wstring_view data, abort_callback& abort, core::json::Object& out,
                     std::wstring& error) {
    int code = 0;
    if (!Request(L"/api", {{L"uri", std::wstring(uri)}, {L"data", std::wstring(data)}, {L"crypto", L"eapi"}}, false,
                 abort, out, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(out, code);
        return false;
    }
    return true;
}

namespace {

// The local time as the PC client sends it ("2026-11-15 14:03:00").
std::wstring ClientTime() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t text[32];
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return text;
}

// A title object's text ({"title": "..."}), or the first of a list of them.
std::wstring TitleOf(const core::json::Object& element, const wchar_t* key) {
    if (auto title = core::json::GetObject(element, key)) return core::json::GetString(*title, L"title");
    if (auto titles = core::json::GetArray(element, key))
        for (const auto& title : core::json::Objects(*titles)) return core::json::GetString(title, L"title");
    return {};
}

// A picture address without the service's processing parameters (the radar's watermark).
std::wstring PlainImage(std::wstring url) {
    if (const size_t q = url.find(L'?'); q != std::wstring::npos) url.resize(q);
    return url;
}

// The rows the PC client asks for by name, in its order; a block the account has none of is not
// returned. The request's cursor and extInfo are JSON texts inside the JSON body.
std::wstring BlockPageData() {
    static constexpr const wchar_t* kBlocks[] = {
        L"HOMEPAGE_BLOCK_PLAYLIST_RCMD", L"HOMEPAGE_BLOCK_STYLE_RCMD", L"HOMEPAGE_BLOCK_RED_SIMILAR_SONG",
        L"HOMPAGE_BLOCK_VIP_RCMD", L"HOMEPAGE_BLOCK_OFFICIAL_PLAYLIST"};
    std::wstring order;
    for (const wchar_t* block : kBlocks) order += (order.empty() ? L"\\\"" : L",\\\"") + std::wstring(block) + L"\\\"";
    return L"{\"refresh\":false,\"cursor\":\"{\\\"offset\\\":0,\\\"blockCodeOrderList\\\":[" + order +
           L"]}\",\"extInfo\":\"{\\\"abInfo\\\":{\\\"hp-new-homepageV3.1\\\":\\\"t3\\\"}}\",\"newStyle\":true}";
}

} // namespace

bool Client::HomeCards(abort_callback& abort, std::vector<HomeCard>& out, std::wstring& error) {
    out.clear();
    core::json::Object body;
    if (!Generic(L"/api/pc/daily/rcmd/block", L"{\"clientTime\":\"" + ClientTime() + L"\"}", abort, body, error))
        return false;
    const auto data = core::json::GetObject(body, L"data");
    const auto items = data ? core::json::GetArray(*data, L"items") : std::nullopt;
    if (!items) return true;
    for (const auto& item : core::json::Objects(*items)) {
        HomeCard card;
        card.label = core::json::GetString(item, L"coverText");
        card.title = core::json::GetString(item, L"title");
        card.subtitle = core::json::GetString(item, L"simplifiedTitle");
        card.coverUrl = core::json::GetString(item, L"coverUrl");
        card.resourceType = core::json::GetString(item, L"resourceType");
        card.subResourceType = core::json::GetString(item, L"subResourceType");
        card.moduleType = core::json::GetString(item, L"moduleType");
        card.resourceId = core::json::GetString(item, L"resourceId");
        card.targetUrl = core::json::GetString(item, L"targetUrl");
        if (card.title.empty() || card.coverUrl.empty()) continue;
        out.push_back(std::move(card));
    }
    return true;
}

bool Client::HomeBlocks(abort_callback& abort, std::vector<HomeBlock>& out, std::wstring& error) {
    out.clear();
    core::json::Object body;
    if (!Generic(L"/api/homepage/block/page", BlockPageData(), abort, body, error)) return false;
    const auto data = core::json::GetObject(body, L"data");
    const auto blocks = data ? core::json::GetArray(*data, L"blocks") : std::nullopt;
    if (blocks) {
        for (const auto& block : core::json::Objects(*blocks)) {
            HomeBlock row;
            row.code = core::json::GetString(block, L"blockCode");
            if (auto element = core::json::GetObject(block, L"uiElement")) {
                row.title = TitleOf(*element, L"mainTitle");
                if (row.title.empty()) row.title = TitleOf(*element, L"subTitle");
            }
            const auto creatives = core::json::GetArray(block, L"creatives");
            if (!creatives) continue;
            for (const auto& creative : core::json::Objects(*creatives)) {
                const auto resources = core::json::GetArray(creative, L"resources");
                if (!resources) continue;
                for (const auto& resource : core::json::Objects(*resources)) {
                    const std::wstring type = core::json::GetString(resource, L"resourceType");
                    if (type == L"song") {
                        const auto ext = core::json::GetObject(resource, L"resourceExtInfo");
                        const auto song = ext ? core::json::GetObject(*ext, L"songData") : std::nullopt;
                        if (!song) continue;
                        Song parsed = ParseSong(*song);
                        if (parsed.coverUrl.empty())
                            if (auto album = core::json::GetObject(*song, L"album"))
                                parsed.coverUrl = core::json::GetString(*album, L"blurPicUrl");
                        if (parsed.id) row.songs.push_back(std::move(parsed));
                    } else if (type == L"list") {
                        Playlist playlist;
                        playlist.id = _wtoi64(core::json::GetString(resource, L"resourceId").c_str());
                        if (auto element = core::json::GetObject(resource, L"uiElement")) {
                            playlist.name = TitleOf(*element, L"mainTitle");
                            if (auto image = core::json::GetObject(*element, L"image"))
                                playlist.coverUrl = core::json::GetString(*image, L"imageUrl");
                        }
                        if (playlist.id && !playlist.name.empty()) row.playlists.push_back(std::move(playlist));
                    }
                }
            }
            if (!row.songs.empty() || !row.playlists.empty()) out.push_back(std::move(row));
        }
    }
    // The radar playlists, rows of their own on the PC client (best effort).
    core::json::Object custom;
    std::wstring customError;
    if (!Generic(L"/api/pc/customize/block/page", L"{\"newStyle\":true}", abort, custom, customError)) return true;
    const auto customData = core::json::GetObject(custom, L"data");
    const auto customBlocks = customData ? core::json::GetArray(*customData, L"blocks") : std::nullopt;
    if (!customBlocks) return true;
    for (const auto& block : core::json::Objects(*customBlocks)) {
        HomeBlock row;
        row.code = core::json::GetString(block, L"code");
        if (auto element = core::json::GetObject(block, L"uiElement")) row.title = TitleOf(*element, L"mainTitle");
        const auto creatives = core::json::GetArray(block, L"creatives");
        if (!creatives) continue;
        for (const auto& creative : core::json::Objects(*creatives)) {
            if (core::json::GetString(creative, L"creativeType") != L"list") continue;
            Playlist playlist;
            playlist.id = _wtoi64(core::json::GetString(creative, L"creativeId").c_str());
            if (auto element = core::json::GetObject(creative, L"uiElement")) {
                playlist.name = TitleOf(*element, L"mainTitle");
                if (auto images = core::json::GetArray(*element, L"images"))
                    for (const auto& image : core::json::Objects(*images)) {
                        playlist.coverUrl = PlainImage(core::json::GetString(image, L"imageUrl"));
                        break;
                    }
            }
            if (playlist.id && !playlist.name.empty()) row.playlists.push_back(std::move(playlist));
        }
        if (!row.playlists.empty()) out.push_back(std::move(row));
    }
    return true;
}

bool Client::StyleDailySongs(std::wstring_view category, std::wstring_view tag, std::wstring_view songs,
                             abort_callback& abort, std::vector<Song>& out, std::wstring& error) {
    out.clear();
    core::json::Object body;
    const std::wstring data = L"{\"source\":\"homepage\",\"categoryId\":\"" + std::wstring(category) +
                              L"\",\"tagId\":\"" + std::wstring(tag) + L"\",\"songId\":\"" + std::wstring(songs) +
                              L"\"}";
    if (!Generic(L"/api/homepage/category/daily/song/list", data, abort, body, error)) return false;
    const auto payload = core::json::GetObject(body, L"data");
    const auto list = payload ? core::json::GetArray(*payload, L"dailySongs") : std::nullopt;
    if (list)
        for (const auto& object : core::json::Objects(*list)) {
            Song song = ParseSong(object);
            if (song.id) out.push_back(std::move(song));
        }
    return true;
}

bool Client::PersonalFm(abort_callback& abort, std::vector<Song>& out, std::wstring& error) {
    out.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/personal_fm", {}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (const auto list = core::json::GetArray(body, L"data"))
        for (const auto& object : core::json::Objects(*list)) {
            Song song = ParseSong(object);
            if (song.id) out.push_back(std::move(song));
        }
    return true;
}

bool Client::FmMode(std::wstring_view mode, std::wstring_view sourceIds, abort_callback& abort,
                    std::vector<Song>& out, std::wstring& error) {
    out.clear();
    std::wstring escaped; // the ids' JSON text inside the JSON body
    for (const wchar_t c : sourceIds) {
        if (c == L'"' || c == L'\\') escaped += L'\\';
        escaped += c;
    }
    core::json::Object body;
    if (!Generic(L"/api/v1/radio/get",
                 L"{\"imageFm\":1,\"mode\":\"" + std::wstring(mode) + L"\",\"sourceIds\":\"" + escaped + L"\"}",
                 abort, body, error))
        return false;
    if (const auto list = core::json::GetArray(body, L"data"))
        for (const auto& object : core::json::Objects(*list)) {
            Song song = ParseSong(object);
            if (song.id) out.push_back(std::move(song));
        }
    return true;
}

bool Client::Heartbeat(int64_t seed, int64_t likedPlaylist, abort_callback& abort, std::vector<Song>& out,
                       std::wstring& error) {
    out.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playmode/intelligence/list",
                 {{L"id", std::to_wstring(seed)}, {L"pid", std::to_wstring(likedPlaylist)}}, false, abort, body,
                 code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (const auto list = core::json::GetArray(body, L"data"))
        for (const auto& object : core::json::Objects(*list))
            if (const auto info = core::json::GetObject(object, L"songInfo")) {
                Song song = ParseSong(*info);
                if (song.id) out.push_back(std::move(song));
            }
    return true;
}

bool Client::FmTrash(int64_t songId, abort_callback& abort, std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/fm_trash", {{L"id", std::to_wstring(songId)}}, true, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    return true;
}

bool Client::DailySongs(abort_callback& abort, std::vector<Song>& out, std::wstring& error) {
    out.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/recommend/songs", {}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    const auto data = core::json::GetObject(body, L"data");
    const auto songs = data ? core::json::GetArray(*data, L"dailySongs") : std::nullopt;
    if (songs)
        for (const auto& object : core::json::Objects(*songs)) {
            Song song = ParseSong(object);
            if (song.id) out.push_back(std::move(song));
        }
    return true;
}

bool Client::SearchSongs(std::wstring_view query, int limit, int offset, abort_callback& abort,
                         std::vector<Song>& out, std::wstring& error) {
    SearchPage page;
    return SearchSongsPage(query, limit, offset, abort, out, page, error);
}

bool Client::CloudSearch(std::wstring_view query, int type, int limit, int offset, const wchar_t* rowsKey,
                         const wchar_t* countKey, abort_callback& abort, std::vector<core::json::Object>& rows,
                         SearchPage& page, std::wstring& error) {
    rows.clear();
    page = {};
    core::json::Object body;
    int code = 0;
    if (!Request(L"/cloudsearch",
                 {{L"keywords", std::wstring(query)},
                  {L"type", std::to_wstring(type)},
                  {L"limit", std::to_wstring(limit)},
                  {L"offset", std::to_wstring(offset)}},
                 false, abort, body, code, error))
        return false;
    if (code == 301) {
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto result = core::json::GetObject(body, L"result");
    std::optional<core::json::Array> array;
    if (result) array = core::json::GetArray(*result, rowsKey);
    if (!array) array = core::json::GetArray(body, rowsKey);
    if (array) {
        page.received = array->Size();
        rows = core::json::Objects(*array);
    }
    const auto setTotal = [&page](int64_t count) {
        if (count >= 0 && static_cast<uint64_t>(count) <= std::numeric_limits<size_t>::max())
            page.total = static_cast<size_t>(count);
    };
    if (result && result->HasKey(countKey))
        setTotal(core::json::GetInt64(*result, countKey));
    else if (body.HasKey(countKey))
        setTotal(core::json::GetInt64(body, countKey));
    if (page.total) {
        page.more = static_cast<uint64_t>(offset) + page.received < *page.total;
    } else {
        // The endpoint normally reports its count. A full page is the conservative fallback;
        // an empty page always terminates.
        page.more = page.received != 0 && limit > 0 && page.received >= static_cast<size_t>(limit);
    }
    return true;
}

bool Client::SearchSongsPage(std::wstring_view query, int limit, int offset, abort_callback& abort,
                             std::vector<Song>& out, SearchPage& page, std::wstring& error) {
    out.clear();
    std::vector<core::json::Object> rows;
    if (!CloudSearch(query, 1, limit, offset, L"songs", L"songCount", abort, rows, page, error)) return false;
    for (const auto& object : rows)
        if (Song song = ParseSong(object); song.id) out.push_back(std::move(song));
    return true;
}

bool Client::SearchAlbumsPage(std::wstring_view query, int limit, int offset, abort_callback& abort,
                              std::vector<Album>& out, SearchPage& page, std::wstring& error) {
    out.clear();
    std::vector<core::json::Object> rows;
    if (!CloudSearch(query, 10, limit, offset, L"albums", L"albumCount", abort, rows, page, error)) return false;
    for (const auto& object : rows)
        if (Album album = ParseAlbum(object); album.id) out.push_back(std::move(album));
    return true;
}

bool Client::SearchArtistsPage(std::wstring_view query, int limit, int offset, abort_callback& abort,
                               std::vector<Artist>& out, SearchPage& page, std::wstring& error) {
    out.clear();
    std::vector<core::json::Object> rows;
    if (!CloudSearch(query, 100, limit, offset, L"artists", L"artistCount", abort, rows, page, error))
        return false;
    for (const auto& object : rows)
        if (Artist artist = ParseArtist(object); artist.id) out.push_back(std::move(artist));
    return true;
}

bool Client::SearchPlaylistsPage(std::wstring_view query, int limit, int offset, abort_callback& abort,
                                 std::vector<Playlist>& out, SearchPage& page, std::wstring& error) {
    out.clear();
    std::vector<core::json::Object> rows;
    if (!CloudSearch(query, 1000, limit, offset, L"playlists", L"playlistCount", abort, rows, page, error))
        return false;
    for (const auto& object : rows)
        if (Playlist playlist = ParsePlaylist(object); playlist.id) out.push_back(std::move(playlist));
    return true;
}

bool Client::SearchLyricsPage(std::wstring_view query, int limit, int offset, abort_callback& abort,
                              std::vector<LyricHit>& out, SearchPage& page, std::wstring& error) {
    out.clear();
    std::vector<core::json::Object> rows;
    if (!CloudSearch(query, 1006, limit, offset, L"songs", L"songCount", abort, rows, page, error)) return false;
    for (const auto& object : rows) {
        LyricHit hit;
        hit.song = ParseSong(object);
        if (!hit.song.id) continue;
        // `lyrics` is the song's lyrics, one line per entry; the service marks the match with <b>.
        if (const auto lines = core::json::GetArray(object, L"lyrics")) {
            std::vector<std::wstring> text;
            size_t match = SIZE_MAX;
            for (uint32_t i = 0; i < lines->Size(); ++i) {
                const auto value = lines->GetAt(i);
                if (!value || value.ValueType() != JsonValueType::String) continue;
                std::wstring line = value.GetString().c_str();
                if (match == SIZE_MAX && line.find(L"<b>") != std::wstring::npos) match = text.size();
                for (const wchar_t* tag : {L"<b>", L"</b>"})
                    for (size_t at; (at = line.find(tag)) != std::wstring::npos;)
                        line.erase(at, wcslen(tag));
                text.push_back(std::move(line));
            }
            if (match == SIZE_MAX) match = 0;
            // The matched line and the two after it, as NetEase's results show them.
            for (size_t i = match; i < text.size() && i < match + 3; ++i)
                hit.excerpt += (hit.excerpt.empty() ? L"" : L" / ") + text[i];
            for (const auto& line : text) hit.text += line + L"\r\n";
        }
        out.push_back(std::move(hit));
    }
    return true;
}

namespace {

// A search result object as the record its kind names ("artist", "albums", ...).
std::optional<SearchHit> ParseHit(std::wstring_view kind, const core::json::Object& object) {
    if (kind.starts_with(L"song")) {
        if (Song song = ParseSong(object); song.id) return song;
    } else if (kind.starts_with(L"artist")) {
        if (Artist artist = ParseArtist(object); artist.id) return artist;
    } else if (kind.starts_with(L"album")) {
        if (Album album = ParseAlbum(object); album.id) return album;
    } else if (kind.starts_with(L"playlist")) {
        if (Playlist playlist = ParsePlaylist(object); playlist.id) return playlist;
    }
    return std::nullopt;
}

// The objects of result.<kind> for each kind the result lists under `orderKey`, in that order.
std::vector<SearchHit> HitsInOrder(const core::json::Object& result, const wchar_t* orderKey) {
    std::vector<SearchHit> hits;
    const auto order = core::json::GetArray(result, orderKey);
    if (!order) return hits;
    for (uint32_t i = 0; i < order->Size(); ++i) {
        const auto name = order->GetAt(i);
        if (!name || name.ValueType() != JsonValueType::String) continue;
        const std::wstring kind = name.GetString().c_str();
        if (const auto rows = core::json::GetArray(result, kind.c_str()))
            for (const auto& object : core::json::Objects(*rows))
                if (auto hit = ParseHit(kind, object)) hits.push_back(std::move(*hit));
    }
    return hits;
}

}  // namespace

bool Client::SearchBestMatches(std::wstring_view query, abort_callback& abort, std::vector<SearchHit>& out,
                               std::wstring& error) {
    out.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/search/multimatch", {{L"keywords", std::wstring(query)}}, false, abort, body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (const auto result = core::json::GetObject(body, L"result")) out = HitsInOrder(*result, L"orders");
    return true;
}

bool Client::SearchSuggest(std::wstring_view query, abort_callback& abort, SearchSuggestions& out,
                           std::wstring& error) {
    out = {};
    core::json::Object body;
    int code = 0;
    if (!Request(L"/search/suggest", {{L"keywords", std::wstring(query)}, {L"type", L"mobile"}}, false, abort,
                 body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (const auto result = core::json::GetObject(body, L"result"))
        if (const auto matches = core::json::GetArray(*result, L"allMatch"))
            for (const auto& match : core::json::Objects(*matches))
                if (std::wstring term = core::json::GetString(match, L"keyword"); !term.empty())
                    out.terms.push_back(std::move(term));
    // The top items are a second request; without them the completions still help.
    core::json::Object items;
    std::wstring itemsError;
    if (Request(L"/search/suggest", {{L"keywords", std::wstring(query)}}, false, abort, items, code, itemsError) &&
        code == 200)
        if (const auto result = core::json::GetObject(items, L"result")) out.items = HitsInOrder(*result, L"order");
    return true;
}

bool Client::SongDetails(const std::vector<int64_t>& ids, abort_callback& abort, std::vector<Song>& out,
                         std::wstring& error) {
    out.clear();
    if (ids.empty()) return true;
    core::json::Object body;
    int code = 0;
    if (!Request(L"/song/detail", {{L"ids", JoinIds(ids)}}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (auto songs = core::json::GetArray(body, L"songs")) {
        for (const auto& object : core::json::Objects(*songs)) {
            Song song = ParseSong(object);
            if (song.id) out.push_back(std::move(song));
        }
    }
    return true;
}

namespace {

std::wstring LrcTime(long long ms) {
    if (ms < 0) ms = 0;
    wchar_t buf[32];
    swprintf_s(buf, L"%02lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return buf;
}

// YRC: "[start,duration](start,duration,0)word(start,duration,0)word..." per line (times in
// ms), plus JSON credit lines. Becomes "[mm:ss.xxx]<mm:ss.xxx>word<mm:ss.xxx>word<end>".
std::wstring YrcToLrc(std::wstring_view yrc, const glasswave::lyrics::Lyrics& translation) {
    std::wstring out;
    size_t pos = 0;
    while (pos < yrc.size()) {
        size_t end = yrc.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = yrc.size();
        std::wstring_view line = yrc.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.remove_suffix(1);
        if (line.size() < 3 || line[0] != L'[') continue;
        long long start = 0, duration = 0;
        if (swscanf_s(std::wstring(line).c_str(), L"[%lld,%lld]", &start, &duration) != 2) continue;
        size_t at = line.find(L']');
        if (at == std::wstring_view::npos) continue;
        ++at;
        std::wstring text;
        long long lastEnd = start + duration;
        bool any = false;
        while (at < line.size()) {
            if (line[at] != L'(') {
                ++at;
                continue;
            }
            const size_t close = line.find(L')', at);
            if (close == std::wstring_view::npos) break;
            long long ws = 0, wd = 0, unused = 0;
            if (swscanf_s(std::wstring(line.substr(at, close - at + 1)).c_str(), L"(%lld,%lld,%lld)", &ws, &wd, &unused) < 2) {
                at = close + 1;
                continue;
            }
            size_t next = line.find(L'(', close + 1);
            // A '(' inside the lyric text is not a time tag unless "(digits," follows.
            while (next != std::wstring_view::npos &&
                   !(next + 1 < line.size() && iswdigit(line[next + 1]) && line.find(L',', next) < line.find(L')', next)))
                next = line.find(L'(', next + 1);
            const std::wstring_view word = line.substr(close + 1, (next == std::wstring_view::npos ? line.size() : next) - close - 1);
            text += L'<' + LrcTime(ws) + L'>' + std::wstring(word);
            lastEnd = ws + wd;
            any = true;
            at = next == std::wstring_view::npos ? line.size() : next;
        }
        if (!any) continue;
        const std::wstring stamp = L'[' + LrcTime(start) + L']';
        out += stamp + text + L'<' + LrcTime(lastEnd) + L">\n";
        // The translation line closest in time (NetEase times them like the line-level LRC).
        const glasswave::lyrics::Line* best = nullptr;
        long long bestDiff = 1500;
        for (const auto& t : translation.lines) {
            const long long diff = std::llabs(t.start - start);
            if (diff < bestDiff) {
                bestDiff = diff;
                best = &t;
            }
        }
        if (best && !best->text.empty() && best->text != L"//") out += stamp + best->text + L'\n';
    }
    return out;
}

std::wstring LyricText(const core::json::Object& body, const wchar_t* key) {
    if (auto o = core::json::GetObject(body, key)) return core::json::GetString(*o, L"lyric");
    return {};
}

}  // namespace

bool Client::GetLyrics(int64_t id, abort_callback& abort, std::wstring& lrc, std::wstring& error) {
    lrc.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/lyric/new", {{L"id", std::to_wstring(id)}}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    const std::wstring yrc = LyricText(body, L"yrc");
    const std::wstring line = LyricText(body, L"lrc");
    std::wstring translation = LyricText(body, L"ytlrc");
    if (translation.empty()) translation = LyricText(body, L"tlyric");
    if (!yrc.empty()) {
        lrc = YrcToLrc(yrc, glasswave::lyrics::ParseLrc(translation));
        if (!lrc.empty()) return true;
    }
    lrc = line;
    if (!lrc.empty() && !translation.empty()) lrc += L"\n" + translation;
    return true;
}

stream::SourceStatus Client::GetSongUrl(int64_t id, std::wstring_view level, abort_callback& abort, SongUrl& out,
                                       std::wstring& error) {
    out = {};
    error.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/song/url/v1", {{L"id", std::to_wstring(id)}, {L"level", std::wstring(level)}}, false, abort,
                 body, code, error))
        return stream::SourceStatus::Failed;
    if (code != 200) {
        error = MessageOf(body, code);
        return stream::SourceStatus::Failed;
    }
    auto data = core::json::GetArray(body, L"data");
    if (!data || data->Size() == 0) {
        error = L"The service returned an invalid song URL response.";
        return stream::SourceStatus::Failed;
    }
    const IJsonValue first = data->GetAt(0);
    if (!first || first.ValueType() != JsonValueType::Object) {
        error = L"The service returned an invalid song URL response.";
        return stream::SourceStatus::Failed;
    }
    const auto item = first.GetObject();
    if (core::json::GetInt64(item, L"id") != id || !item.HasKey(L"url")) {
        error = L"The service returned an invalid song URL response.";
        return stream::SourceStatus::Failed;
    }
    out.url = core::json::GetString(item, L"url");
    out.type = core::json::GetString(item, L"type");
    out.level = core::json::GetString(item, L"level");
    out.bitrate = core::json::GetInt(item, L"br");
    out.code = core::json::GetInt(item, L"code");
    out.expireSeconds = core::json::GetInt(item, L"expi", 1200);
    out.trial = static_cast<bool>(core::json::GetObject(item, L"freeTrialInfo"));
    out.message = core::json::GetString(item, L"message");
    if (out.code >= 500 || out.code == 301 || out.code == 401) {
        error = out.message.empty() ? L"NetEase stream request failed (" + std::to_wstring(out.code) + L")."
                                    : out.message;
        return stream::SourceStatus::Failed;
    }
    const auto status = stream::CheckSourceUrl(out.url);
    if (status == stream::SourceStatus::Unavailable) {
        error = out.message.empty() ? L"No playable source for this song." : out.message;
    } else if (status == stream::SourceStatus::Failed) {
        error = L"The service returned an invalid stream URL.";
    }
    return status;
}

bool Client::GetArtistDetail(int64_t artistId, abort_callback& abort, Artist& out, std::wstring& error) {
    out = {};
    core::json::Object body;
    int code = 0;
    if (!Request(L"/artist/detail", {{L"id", std::to_wstring(artistId)}}, false, abort, body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    std::optional<core::json::Object> artist;
    if (auto data = core::json::GetObject(body, L"data")) artist = core::json::GetObject(*data, L"artist");
    if (!artist) artist = core::json::GetObject(body, L"artist");
    if (!artist) {
        error = L"The artist response has no artist object.";
        return false;
    }
    out.id = core::json::GetInt64(*artist, L"id");
    if (!out.id) out.id = artistId;
    out.name = core::json::GetString(*artist, L"name");
    out.avatarUrl = core::json::GetString(*artist, L"avatar",
                                    core::json::GetString(*artist, L"cover", core::json::GetString(*artist, L"picUrl")));
    out.brief = core::json::GetString(*artist, L"briefDesc");
    out.albumCount = core::json::GetInt(*artist, L"albumSize");
    out.songCount = core::json::GetInt(*artist, L"musicSize");
    out.videoCount = core::json::GetInt(*artist, L"mvSize", -1);
    const auto addAlias = [&out](std::wstring name) {
        if (!name.empty() && name != out.name &&
            std::find(out.aliases.begin(), out.aliases.end(), name) == out.aliases.end())
            out.aliases.push_back(std::move(name));
    };
    for (const wchar_t* key : {L"alias", L"transNames"})
        if (const auto names = core::json::GetArray(*artist, key))
            for (uint32_t i = 0; i < names->Size(); ++i)
                if (const auto value = names->GetAt(i);
                    value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                    addAlias(value.GetString().c_str());
    // "网易音乐人、歌手、作词、作曲": what the artist does, as the client's profile shows it,
    // without the platform's own badges (the owner wants the music, not the service's social
    // side).
    if (auto data = core::json::GetObject(body, L"data"))
        if (auto identify = core::json::GetObject(*data, L"identify")) {
            const std::wstring roles = core::json::GetString(*identify, L"imageDesc");
            size_t start = 0;
            while (start < roles.size()) {
                size_t end = roles.find(L'、', start);
                if (end == std::wstring::npos) end = roles.size();
                const std::wstring role = roles.substr(start, end - start);
                if (!role.empty() && role.find(L"网易") == std::wstring::npos) out.roles.push_back(role);
                start = end + 1;
            }
        }
    out.profile = true;
    if (out.name.empty()) out.name = L"Unknown";
    return true;
}

bool Client::GetSongInfo(int64_t songId, abort_callback& abort, SongInfo& out, std::wstring& error) {
    out = {};
    core::json::Object body;
    int code = 0;
    if (!Request(L"/song/detail", {{L"ids", std::to_wstring(songId)}}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    const auto songs = core::json::GetArray(body, L"songs");
    const auto list = songs ? core::json::Objects(*songs) : std::vector<core::json::Object>{};
    if (list.empty()) {
        error = L"NetEase returned no song.";
        return false;
    }
    const core::json::Object& object = list.front();
    out.song = ParseSong(object);
    out.disc = _wtoi(core::json::GetString(object, L"cd").c_str());
    out.number = core::json::GetInt(object, L"no");
    out.publishTime = core::json::GetInt64(object, L"publishTime");
    out.originCoverType = core::json::GetInt(object, L"originCoverType");
    out.hiRes = static_cast<bool>(core::json::GetObject(object, L"hr"));
    out.lossless = out.hiRes || static_cast<bool>(core::json::GetObject(object, L"sq"));
    for (const wchar_t* key : {L"alia", L"tns"})
        if (const auto names = core::json::GetArray(object, key))
            for (uint32_t i = 0; i < names->Size(); ++i)
                if (const auto value = names->GetAt(i);
                    value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String) {
                    std::wstring name = value.GetString().c_str();
                    if (!name.empty() && std::find(out.aliases.begin(), out.aliases.end(), name) == out.aliases.end())
                        out.aliases.push_back(std::move(name));
                }

    // The song wiki: blocks of "creatives" (2026-11-14, through the local service).
    std::wstring ignored;
    if (!Request(L"/song/wiki/summary", {{L"id", std::to_wstring(songId)}}, false, abort, body, code, ignored) ||
        code != 200)
        return true;
    const auto data = core::json::GetObject(body, L"data");
    const auto blocks = data ? core::json::GetArray(*data, L"blocks") : std::nullopt;
    if (!blocks) return true;
    const auto title = [](const core::json::Object& item) {
        const auto ui = core::json::GetObject(item, L"uiElement");
        const auto main = ui ? core::json::GetObject(*ui, L"mainTitle") : std::nullopt;
        return main ? core::json::GetString(*main, L"title") : std::wstring();
    };
    const auto links = [](const core::json::Object& item) {
        std::vector<std::wstring> out;
        const auto ui = core::json::GetObject(item, L"uiElement");
        if (const auto texts = ui ? core::json::GetArray(*ui, L"textLinks") : std::nullopt)
            for (const auto& link : core::json::Objects(*texts))
                if (auto text = core::json::GetString(link, L"text"); !text.empty()) out.push_back(std::move(text));
        return out;
    };
    for (const auto& block : core::json::Objects(*blocks)) {
        const std::wstring kind = core::json::GetString(block, L"code");
        const auto creatives = core::json::GetArray(block, L"creatives");
        if (!creatives) continue;
        for (const auto& creative : core::json::Objects(*creatives)) {
            const std::wstring type = core::json::GetString(creative, L"creativeType");
            std::vector<std::wstring> names; // the resources' titles (style and tag words)
            const auto resources = core::json::GetArray(creative, L"resources");
            const auto items = resources ? core::json::Objects(*resources) : std::vector<core::json::Object>{};
            for (const auto& resource : items)
                if (auto name = title(resource); !name.empty()) names.push_back(std::move(name));
            if (kind == L"SONG_PLAY_ABOUT_SIMILAR_SONG") {
                for (const auto& resource : items)
                    if (core::json::GetString(resource, L"resourceType") == L"SONG")
                        if (const int64_t id = _wtoi64(core::json::GetString(resource, L"resourceId").c_str()))
                            out.similar.push_back(id);
            } else if (type == L"songTag") {
                out.styles = std::move(names);
            } else if (type == L"songBizTag") {
                out.tags = std::move(names);
            } else if (type == L"language") {
                const auto values = links(creative);
                if (!values.empty()) out.language = values.front();
            } else if (type == L"bpm") {
                const auto values = links(creative);
                if (!values.empty()) out.bpm = _wtoi(values.front().c_str());
            }
        }
    }
    return true;
}

void Client::GetArtistExtras(int64_t artistId, abort_callback& abort, Artist& out) {
    core::json::Object body;
    int code = 0;
    std::wstring error;
    const std::wstring id = std::to_wstring(artistId);
    // The artist wiki (as the client's artist page edits it): area and type.
    if (Request(L"/ugc/artist/get", {{L"id", id}}, false, abort, body, code, error) && code == 200) {
        if (const auto data = core::json::GetObject(body, L"data")) {
            out.area = core::json::GetInt(*data, L"area", -1);
            out.type = core::json::GetInt(*data, L"type", 0);
        }
    }
}

bool Client::GetArtistDescription(int64_t artistId, abort_callback& abort, std::wstring& brief,
                                  std::wstring& full, std::wstring& error) {
    brief.clear();
    full.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/artist/desc", {{L"id", std::to_wstring(artistId)}}, false, abort, body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    brief = core::json::GetString(body, L"briefDesc");
    if (auto introduction = core::json::GetArray(body, L"introduction")) {
        for (const auto& section : core::json::Objects(*introduction)) {
            const std::wstring title = core::json::GetString(section, L"ti");
            const std::wstring text = core::json::GetString(section, L"txt");
            if (title.empty() && text.empty()) continue;
            if (!full.empty()) full += L"\n\n";
            if (!title.empty()) full += title + L"\n";
            full += text;
        }
    }
    if (full.empty()) full = brief;
    return true;
}

bool Client::GetArtistAlbumsPage(int64_t artistId, int offset, int limit, abort_callback& abort,
                                 std::vector<Album>& out, bool* followed, bool* more, std::wstring& error,
                                 size_t* received) {
    out.clear();
    if (followed) *followed = false;
    if (more) *more = false;
    if (received) *received = 0;
    core::json::Object body;
    int code = 0;
    if (!Request(L"/artist/album",
                 {{L"id", std::to_wstring(artistId)},
                  {L"limit", std::to_wstring(limit)},
                  {L"offset", std::to_wstring(offset)}},
                 false, abort, body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (followed) {
        if (auto artist = core::json::GetObject(body, L"artist"); artist && artist->HasKey(L"followed"))
            *followed = core::json::GetBool(*artist, L"followed");
    }
    if (more) *more = core::json::GetBool(body, L"more");
    if (auto albums = core::json::GetArray(body, L"hotAlbums")) {
        if (received) *received = albums->Size();
        for (const auto& object : core::json::Objects(*albums)) {
            Album album = ParseAlbum(object);
            if (album.id) out.push_back(std::move(album));
        }
    }
    return true;
}

bool Client::GetArtistSongsPage(int64_t artistId, int offset, int limit, std::wstring_view order,
                                abort_callback& abort, std::vector<Song>& out, bool* more,
                                std::wstring& error, size_t* received) {
    out.clear();
    if (more) *more = false;
    if (received) *received = 0;
    core::json::Object body;
    int code = 0;
    if (!Request(L"/artist/songs",
                 {{L"id", std::to_wstring(artistId)},
                  {L"limit", std::to_wstring(limit)},
                  {L"offset", std::to_wstring(offset)},
                  {L"order", std::wstring(order)}},
                 false, abort, body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (more) *more = core::json::GetBool(body, L"more");
    if (auto songs = core::json::GetArray(body, L"songs")) {
        if (received) *received = songs->Size();
        for (const auto& object : core::json::Objects(*songs)) {
            Song song = ParseSong(object);
            if (song.id) out.push_back(std::move(song));
        }
    }
    return true;
}

bool Client::GetAlbum(int64_t albumId, abort_callback& abort, Album& info, std::vector<Song>& songs,
                      std::wstring& error) {
    info = {};
    songs.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/album", {{L"id", std::to_wstring(albumId)}}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (auto album = core::json::GetObject(body, L"album")) info = ParseAlbum(*album);
    info.id = info.id ? info.id : albumId;
    if (auto list = core::json::GetArray(body, L"songs")) {
        for (const auto& object : core::json::Objects(*list)) {
            Song song = ParseSong(object);
            if (song.id) songs.push_back(std::move(song));
        }
    }
    return true;
}

bool Client::GetFollowedArtistsPage(int offset, int limit, abort_callback& abort, std::vector<Artist>& out,
                                    bool& more, size_t& received, std::wstring& error) {
    out.clear();
    more = false;
    received = 0;
    core::json::Object body;
    int code = 0;
    if (!Request(L"/artist/sublist",
                 {{L"limit", std::to_wstring(limit)}, {L"offset", std::to_wstring(offset)}}, false, abort,
                 body, code, error))
        return false;
    if (code == 301) {
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto artists = core::json::GetArray(body, L"data"); // the helper returns followed artists here
    if (!artists) artists = core::json::GetArray(body, L"artists");
    if (artists) {
        received = artists->Size();
        for (const auto& object : core::json::Objects(*artists)) {
            Artist artist = ParseArtist(object);
            if (!artist.id) continue;
            artist.followed = true;
            artist.followKnown = true;
            out.push_back(std::move(artist));
        }
    }
    more = core::json::GetBool(body, L"hasMore") && received != 0;
    return true;
}

bool Client::GetSimilarArtists(int64_t artistId, abort_callback& abort, std::vector<Artist>& out,
                               std::wstring& error) {
    out.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/simi/artist", {{L"id", std::to_wstring(artistId)}}, false, abort, body, code, error))
        return false;
    if (code == 301) {
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (const auto artists = core::json::GetArray(body, L"artists"))
        for (const auto& object : core::json::Objects(*artists))
            if (Artist artist = ParseArtist(object); artist.id) out.push_back(std::move(artist));
    return true;
}

bool Client::GetFollowedArtists(abort_callback& abort, std::vector<Artist>& out, std::wstring& error) {
    out.clear();
    int offset = 0;
    for (int page = 0; page < 50; ++page) {
        std::vector<Artist> artists;
        bool more = false;
        size_t received = 0;
        if (!GetFollowedArtistsPage(offset, 100, abort, artists, more, received, error)) return false;
        out.insert(out.end(), std::make_move_iterator(artists.begin()),
                   std::make_move_iterator(artists.end()));
        if (!more || received == 0) break;
        offset += static_cast<int>(received);
    }
    return true;
}

bool Client::GetCollectedAlbumsPage(int offset, int limit, abort_callback& abort, std::vector<Album>& out,
                                    bool& more, std::wstring& error, size_t* received) {
    out.clear();
    more = false;
    if (received) *received = 0;
    core::json::Object body;
    int code = 0;
    if (!Request(L"/album/sublist",
                 {{L"limit", std::to_wstring(limit)}, {L"offset", std::to_wstring(offset)}}, false, abort, body, code,
                 error))
        return false;
    if (code == 301) {
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto albums = core::json::GetArray(body, L"data");
    if (!albums) return true;
    const size_t receivedCount = albums->Size();
    if (received) *received = receivedCount;
    for (const auto& object : core::json::Objects(*albums)) {
        Album album = ParseAlbum(object);
        if (album.id) out.push_back(std::move(album));
    }
    more = core::json::GetBool(body, L"hasMore") && receivedCount != 0;
    return true;
}

bool Client::GetCollectedAlbums(abort_callback& abort, std::vector<Album>& out, std::wstring& error) {
    out.clear();
    int offset = 0;
    for (int page = 0; page < 50; ++page) {
        std::vector<Album> albums;
        bool more = false;
        size_t received = 0;
        if (!GetCollectedAlbumsPage(offset, 100, abort, albums, more, error, &received)) return false;
        if (received == 0) break;
        out.insert(out.end(), std::make_move_iterator(albums.begin()), std::make_move_iterator(albums.end()));
        if (!more) break;
        offset += static_cast<int>(received);
    }
    return true;
}

bool Client::SetArtistFollow(int64_t artistId, bool follow, abort_callback& abort, std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/artist/sub", {{L"id", std::to_wstring(artistId)}, {L"t", follow ? L"1" : L"0"}}, true, abort,
                 body, code, error))
        return false;
    if (code == 301) {
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    return true;
}

bool Client::Likelist(int64_t userId, abort_callback& abort, std::vector<int64_t>& out, std::wstring& error) {
    out.clear();
    core::json::Object body;
    int code = 0;
    if (!Request(L"/likelist", {{L"uid", std::to_wstring(userId)}}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (auto ids = core::json::GetArray(body, L"ids")) out = IdsOf(*ids);
    return true;
}

bool Client::SetLike(int64_t id, bool like, abort_callback& abort, std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/like", {{L"id", std::to_wstring(id)}, {L"like", like ? L"true" : L"false"}}, true, abort, body,
                 code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    return true;
}

bool Client::AddToPlaylist(int64_t playlistId, const std::vector<int64_t>& ids, abort_callback& abort,
                           std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playlist/tracks",
                 {{L"op", L"add"}, {L"pid", std::to_wstring(playlistId)}, {L"tracks", JoinIds(ids)}}, true, abort,
                 body, code, error))
        return false;
    if (code != 200 && code != 502) {  // 502: every track was already in the playlist
        error = MessageOf(body, code);
        return false;
    }
    return true;
}

bool Client::RemoveFromPlaylist(int64_t playlistId, const std::vector<int64_t>& ids, abort_callback& abort,
                                std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playlist/tracks",
                 {{L"op", L"del"}, {L"pid", std::to_wstring(playlistId)}, {L"tracks", JoinIds(ids)}}, true, abort,
                 body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    return true;
}

bool Client::SetAlbumCollect(int64_t albumId, bool collect, abort_callback& abort, std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/album/sub", {{L"id", std::to_wstring(albumId)}, {L"t", collect ? L"1" : L"0"}}, true, abort,
                 body, code, error))
        return false;
    if (code == 301) {
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    return true;
}

bool Client::SetPlaylistSubscribed(int64_t playlistId, bool subscribed, abort_callback& abort,
                                   std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playlist/subscribe",
                 {{L"id", std::to_wstring(playlistId)}, {L"t", subscribed ? L"1" : L"0"}}, true, abort, body,
                 code, error))
        return false;
    if (code == 301) {
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    return true;
}

bool Client::RenamePlaylist(int64_t playlistId, std::wstring_view name, abort_callback& abort,
                            std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playlist/name/update",
                 {{L"id", std::to_wstring(playlistId)}, {L"name", std::wstring(name)}}, true, abort, body, code,
                 error))
        return false;
    if (code == 301) {
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    return true;
}

bool Client::DeletePlaylist(int64_t playlistId, abort_callback& abort, std::wstring& error) {
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playlist/delete", {{L"id", std::to_wstring(playlistId)}}, true, abort, body, code, error))
        return false;
    if (code == 301) {
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    return true;
}

bool Client::ReorderPlaylists(const std::vector<int64_t>& ids, abort_callback& abort, std::wstring& error) {
    if (ids.empty()) return true;
    std::wstring list = L"[";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) list += L',';
        list += std::to_wstring(ids[i]);
    }
    list += L']';
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playlist/order/update", {{L"ids", list}}, true, abort, body, code, error)) return false;
    if (code == 301) {
        error = L"Please sign in to NetEase Cloud Music.";
        return false;
    }
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    return true;
}

bool Client::CreatePlaylist(std::wstring_view name, abort_callback& abort, int64_t& id, std::wstring& error) {
    id = 0;
    core::json::Object body;
    int code = 0;
    if (!Request(L"/playlist/create",
                 {{L"name", std::wstring(name)}, {L"privacy", L"0"}, {L"type", L"NORMAL"}}, true, abort, body, code,
                 error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    id = core::json::GetInt64(body, L"id");
    if (!id) {
        if (auto playlist = core::json::GetObject(body, L"playlist")) id = core::json::GetInt64(*playlist, L"id");
    }
    if (!id) {
        error = L"The service did not return the new playlist id.";
        return false;
    }
    return true;
}

}  // namespace glasswave::netease
