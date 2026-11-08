#include "pch.h"
#include "netease/client.h"
#include "netease/cookie.h"
#include "netease/http.h"
#include "netease/log.h"

#include <chrono>
#include <optional>

namespace glasswave::netease {

namespace {

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonValueType;

std::wstring Wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string UrlEncode(std::wstring_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    const std::string utf8 = Utf8(value);
    std::string out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

std::wstring MessageOf(const json::Object& body, int code) {
    std::wstring message = json::GetString(body, L"message");
    if (message.empty()) message = json::GetString(body, L"msg");
    if (message.empty()) message = L"NetEase API error " + std::to_wstring(code);
    return message;
}

std::vector<ArtistRef> ArtistRefs(const json::Object& object) {
    auto parse = [](const json::Array& array) {
        std::vector<ArtistRef> out;
        for (const auto& artist : json::Objects(array)) {
            ArtistRef ref;
            ref.id = json::GetInt64(artist, L"id");
            ref.name = json::GetString(artist, L"name");
            if (ref.id || !ref.name.empty()) out.push_back(std::move(ref));
        }
        return out;
    };
    if (auto artists = json::GetArray(object, L"ar")) {
        auto out = parse(*artists);
        if (!out.empty()) return out;
    }
    if (auto artists = json::GetArray(object, L"artists")) return parse(*artists);
    return {};
}

Album ParseAlbum(const json::Object& object) {
    Album album;
    album.id = json::GetInt64(object, L"id");
    album.name = json::GetString(object, L"name");
    album.coverUrl = json::GetString(object, L"picUrl",
                                     json::GetString(object, L"coverImgUrl", json::GetString(object, L"blurPicUrl")));
    album.trackCount = json::GetInt(object, L"size", json::GetInt(object, L"trackCount"));
    album.publishTime = json::GetInt64(object, L"publishTime");
    if (album.name.empty()) album.name = L"Unknown";
    return album;
}

Artist ParseArtist(const json::Object& object) {
    Artist artist;
    artist.id = json::GetInt64(object, L"id");
    artist.name = json::GetString(object, L"name");
    artist.avatarUrl = json::GetString(object, L"picUrl",
                                       json::GetString(object, L"avatar",
                                                       json::GetString(object, L"img1v1Url",
                                                                       json::GetString(object, L"cover"))));
    artist.brief = json::GetString(object, L"briefDesc");
    artist.albumCount = json::GetInt(object, L"albumSize");
    artist.songCount = json::GetInt(object, L"musicSize");
    if (artist.name.empty()) artist.name = L"Unknown";
    return artist;
}

std::vector<int64_t> IdsOf(const json::Array& array) {
    std::vector<int64_t> ids;
    if (!array) return ids;
    for (uint32_t i = 0, n = array.Size(); i < n; ++i) {
        const IJsonValue value = array.GetAt(i);
        if (!value) continue;
        if (value.ValueType() == JsonValueType::Object) {
            const int64_t id = json::GetInt64(value.GetObject(), L"id");
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

Song ParseSong(const json::Object& object) {
    Song song;
    song.id = json::GetInt64(object, L"id");
    song.title = json::GetString(object, L"name");
    song.artistRefs = ArtistRefs(object);
    for (const auto& ref : song.artistRefs)
        if (!ref.name.empty()) song.artists.push_back(ref.name);
    song.albumArtist = song.artists.empty() ? std::wstring() : song.artists.front();
    song.durationMs = json::GetInt(object, L"dt", json::GetInt(object, L"duration", 0));
    song.fee = json::GetInt(object, L"fee");

    if (auto album = json::GetObject(object, L"al")) {
        song.album = json::GetString(*album, L"name");
        song.albumId = json::GetInt64(*album, L"id");
        song.coverUrl = json::GetString(*album, L"picUrl");
    } else if (auto albumObject = json::GetObject(object, L"album")) {
        song.album = json::GetString(*albumObject, L"name");
        song.albumId = json::GetInt64(*albumObject, L"id");
        song.coverUrl = json::GetString(*albumObject, L"picUrl");
    }
    if (song.title.empty()) song.title = L"Unknown";

    const auto setQuality = [&song](const json::Object& quality, const wchar_t* codec, int bits) {
        const int bitrate = json::GetInt(quality, L"br");
        const int sampleRate = json::GetInt(quality, L"sr");
        if (bitrate > 0) {
            song.codec = codec;
            song.bitrate = bitrate;
            song.sampleRate = sampleRate;
            song.bitsPerSample = bits;
        }
    };
    if (auto hr = json::GetObject(object, L"hr"))
        setQuality(*hr, L"FLAC", 24);
    else if (auto sq = json::GetObject(object, L"sq"))
        setQuality(*sq, L"FLAC", 16);
    else if (auto high = json::GetObject(object, L"h"))
        setQuality(*high, L"MP3", 0);
    else if (auto medium = json::GetObject(object, L"m"))
        setQuality(*medium, L"MP3", 0);
    else if (auto low = json::GetObject(object, L"l"))
        setQuality(*low, L"MP3", 0);
    return song;
}

Playlist ParsePlaylist(const json::Object& object) {
    Playlist playlist;
    playlist.id = json::GetInt64(object, L"id");
    playlist.name = json::GetString(object, L"name");
    playlist.coverUrl = json::GetString(object, L"coverImgUrl", json::GetString(object, L"picUrl"));
    playlist.trackCount = json::GetInt(object, L"trackCount");
    playlist.subscribed = json::GetBool(object, L"subscribed");
    if (auto creator = json::GetObject(object, L"creator"))
        playlist.creatorId = json::GetInt64(*creator, L"userId");
    if (playlist.name.empty()) playlist.name = L"Unknown";
    return playlist;
}

bool Client::Request(std::wstring_view path, const std::vector<std::pair<std::wstring, std::wstring>>& params,
                     bool post, abort_callback& abort, json::Object& out, int& apiCode, std::wstring& error,
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
        query += UrlEncode(name);
        query += "=";
        query += UrlEncode(value);
    };
    for (const auto& [name, value] : params) append(name, value);
    // The Enhanced server caches successful responses for two minutes. A changing
    // parameter keeps account state, QR polls and short-lived playback URLs fresh.
    append(L"_t", std::to_wstring(GetTickCount64()));

    const std::wstring url = Wide(base) + std::wstring(path) + (query.empty() ? L"" : L"?" + Wide(query));
    const std::wstring method = post ? L"POST" : L"GET";
    const auto started = std::chrono::steady_clock::now();

    HttpOptions options;
    options.method = method;
    options.noProxy = true;
    options.timeoutMs = 25000;
    options.maxBodyBytes = 48u * 1024u * 1024u;
    options.headers = {{L"Accept", L"application/json"}};
    if (!cookie.empty()) options.headers.emplace_back(L"Cookie", cookie);
    const HttpResponse response = SendHttp(url, options, abort);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    LogHttp(method, path, response.status, elapsed);

    if (responseCookies) *responseCookies = response.setCookies;
    if (response.status == 0) {
        error = response.error.empty() ? L"Cannot reach the NetEase service." : response.error;
        return false;
    }
    std::wstring parseError;
    const auto parsed = json::Parse(response.body, &parseError);
    if (!parsed) {
        error = L"Invalid JSON from the NetEase service";
        if (!parseError.empty()) error += L": " + parseError;
        return false;
    }
    out = *parsed;
    apiCode = json::GetInt(out, L"code", static_cast<int>(response.status));
    return true;
}

bool Client::QrKey(abort_callback& abort, QrLogin& out, std::wstring& error) {
    json::Object body;
    int code = 0;
    if (!Request(L"/login/qr/key", {}, true, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto data = json::GetObject(body, L"data");
    if (!data) {
        error = L"The login service returned no QR key.";
        return false;
    }
    out.key = json::GetString(*data, L"unikey");
    if (out.key.empty()) {
        if (auto nested = json::GetObject(*data, L"data")) out.key = json::GetString(*nested, L"unikey");
    }
    if (out.key.empty()) {
        error = L"The login service returned an empty QR key.";
        return false;
    }
    return true;
}

bool Client::QrCreate(std::wstring_view key, abort_callback& abort, QrLogin& out, std::wstring& error) {
    json::Object body;
    int code = 0;
    if (!Request(L"/login/qr/create", {{L"key", std::wstring(key)}, {L"qrimg", L"true"}}, false, abort, body, code,
                 error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto data = json::GetObject(body, L"data");
    if (!data) {
        error = L"The login service returned no QR image.";
        return false;
    }
    out.url = json::GetString(*data, L"qrurl");
    out.imageDataUrl = json::GetString(*data, L"qrimg");
    if (out.imageDataUrl.empty()) {
        error = L"The login service returned an empty QR image.";
        return false;
    }
    return true;
}

QrStatus Client::QrCheck(std::wstring_view key, abort_callback& abort) {
    QrStatus status;
    json::Object body;
    int code = 0;
    std::wstring error;
    std::vector<std::wstring> responseCookies;
    if (!Request(L"/login/qr/check", {{L"key", std::wstring(key)}}, false, abort, body, code, error,
                 &responseCookies)) {
        status.message = error;
        return status;
    }
    status.code = code;
    status.message = json::GetString(body, L"message", json::GetString(body, L"msg"));
    std::wstring rawCookie = json::GetString(body, L"cookie");
    if (auto cookies = json::GetArray(body, L"cookie")) {
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
        Log(L"login: qr check code " + std::to_wstring(code) + L", json/header cookies " +
            std::to_wstring(responseCookies.size()) + L", normalized " + std::to_wstring(status.cookie.size()) +
            (CookieHasMusicU(status.cookie) ? L", MUSIC_U ok" : L", MUSIC_U missing") + L", names [" +
            CookieNames(status.cookie) + L"]");
    }
    return status;
}

bool Client::GetAccount(abort_callback& abort, Account& out, std::wstring& error) {
    json::Object body;
    int code = 0;
    if (!Request(L"/user/account", {}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto account = json::GetObject(body, L"account");
    auto profile = json::GetObject(body, L"profile");
    out = {};
    if (!account || account->Size() == 0 || !profile || json::GetBool(*account, L"anonimousUser")) return true;
    out.loggedIn = true;
    out.userId = json::GetInt64(*account, L"id");
    if (out.userId == 0) out.userId = json::GetInt64(*profile, L"userId");
    out.nickname = json::GetString(*profile, L"nickname");
    return true;
}

bool Client::UserPlaylists(int64_t userId, abort_callback& abort, std::vector<Playlist>& out,
                           int64_t& likedPlaylist, std::wstring& error) {
    out.clear();
    likedPlaylist = 0;
    int offset = 0;
    for (int page = 0; page < 20; ++page) {
        json::Object body;
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
        auto list = json::GetArray(body, L"playlist");
        if (!list) break;
        for (const auto& object : json::Objects(*list)) {
            const int special = json::GetInt(object, L"specialType");
            Playlist playlist = ParsePlaylist(object);
            if (special == 5) {
                const bool own = playlist.creatorId == userId;
                if (own || likedPlaylist == 0) likedPlaylist = playlist.id;
                continue;
            }
            if (special != 0) continue;
            if (playlist.id) out.push_back(std::move(playlist));
        }
        if (!json::GetBool(body, L"more")) break;
        offset += static_cast<int>(list->Size());
        if (list->Size() == 0) break;
    }
    return true;
}

bool Client::GetPlaylist(int64_t playlistId, abort_callback& abort, PlaylistDetail& out,
                         std::wstring& error) {
    json::Object body;
    int code = 0;
    if (!Request(L"/playlist/detail",
                 {{L"id", std::to_wstring(playlistId)}, {L"n", L"1000"}, {L"s", L"8"}}, false, abort, body, code,
                 error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto playlist = json::GetObject(body, L"playlist");
    if (!playlist) {
        error = L"The playlist response has no playlist object.";
        return false;
    }
    out.info = ParsePlaylist(*playlist);
    out.info.id = out.info.id ? out.info.id : playlistId;

    std::map<int64_t, Song> songs;
    if (auto tracks = json::GetArray(*playlist, L"tracks")) {
        for (const auto& object : json::Objects(*tracks)) {
            Song song = ParseSong(object);
            if (song.id) songs.emplace(song.id, std::move(song));
        }
    }
    std::vector<int64_t> order;
    if (auto trackIds = json::GetArray(*playlist, L"trackIds")) order = IdsOf(*trackIds);
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

bool Client::RecommendPlaylists(abort_callback& abort, std::vector<Playlist>& out, std::wstring& error) {
    out.clear();
    json::Object body;
    int code = 0;
    if (!Request(L"/recommend/resource", {}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (auto list = json::GetArray(body, L"recommend")) {
        for (const auto& object : json::Objects(*list)) {
            Playlist playlist = ParsePlaylist(object);
            if (playlist.id) out.push_back(std::move(playlist));
        }
    }
    return true;
}

bool Client::SearchSongs(std::wstring_view query, int limit, int offset, abort_callback& abort,
                         std::vector<Song>& out, std::wstring& error) {
    out.clear();
    json::Object body;
    int code = 0;
    if (!Request(L"/cloudsearch",
                 {{L"keywords", std::wstring(query)},
                  {L"type", L"1"},
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
    auto result = json::GetObject(body, L"result");
    std::optional<json::Array> songs;
    if (result) songs = json::GetArray(*result, L"songs");
    if (!songs) songs = json::GetArray(body, L"songs");
    if (songs) {
        for (const auto& object : json::Objects(*songs)) {
            Song song = ParseSong(object);
            if (song.id) out.push_back(std::move(song));
        }
    }
    return true;
}

bool Client::SongDetails(const std::vector<int64_t>& ids, abort_callback& abort, std::vector<Song>& out,
                         std::wstring& error) {
    out.clear();
    if (ids.empty()) return true;
    json::Object body;
    int code = 0;
    if (!Request(L"/song/detail", {{L"ids", JoinIds(ids)}}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (auto songs = json::GetArray(body, L"songs")) {
        for (const auto& object : json::Objects(*songs)) {
            Song song = ParseSong(object);
            if (song.id) out.push_back(std::move(song));
        }
    }
    return true;
}

bool Client::GetSongUrl(int64_t id, std::wstring_view level, abort_callback& abort, SongUrl& out,
                        std::wstring& error) {
    out = {};
    json::Object body;
    int code = 0;
    if (!Request(L"/song/url/v1", {{L"id", std::to_wstring(id)}, {L"level", std::wstring(level)}}, false, abort,
                 body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    auto data = json::GetArray(body, L"data");
    if (!data || data->Size() == 0) {
        error = L"The service returned no playable URL.";
        return false;
    }
    const IJsonValue first = data->GetAt(0);
    if (!first || first.ValueType() != JsonValueType::Object) {
        error = L"The service returned no playable URL.";
        return false;
    }
    const auto item = first.GetObject();
    out.url = json::GetString(item, L"url");
    out.type = json::GetString(item, L"type");
    out.level = json::GetString(item, L"level");
    out.bitrate = json::GetInt(item, L"br");
    out.code = json::GetInt(item, L"code");
    out.expireSeconds = json::GetInt(item, L"expi", 1200);
    out.trial = static_cast<bool>(json::GetObject(item, L"freeTrialInfo"));
    out.message = json::GetString(item, L"message");
    if (out.url.empty()) {
        error = out.message.empty() ? L"No playable source for this song." : out.message;
        return false;
    }
    return true;
}

bool Client::GetArtistDetail(int64_t artistId, abort_callback& abort, Artist& out, std::wstring& error) {
    out = {};
    json::Object body;
    int code = 0;
    if (!Request(L"/artist/detail", {{L"id", std::to_wstring(artistId)}}, false, abort, body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    std::optional<json::Object> artist;
    if (auto data = json::GetObject(body, L"data")) artist = json::GetObject(*data, L"artist");
    if (!artist) artist = json::GetObject(body, L"artist");
    if (!artist) {
        error = L"The artist response has no artist object.";
        return false;
    }
    out.id = json::GetInt64(*artist, L"id");
    if (!out.id) out.id = artistId;
    out.name = json::GetString(*artist, L"name");
    out.avatarUrl = json::GetString(*artist, L"avatar",
                                    json::GetString(*artist, L"cover", json::GetString(*artist, L"picUrl")));
    out.brief = json::GetString(*artist, L"briefDesc");
    out.albumCount = json::GetInt(*artist, L"albumSize");
    out.songCount = json::GetInt(*artist, L"musicSize");
    if (out.name.empty()) out.name = L"Unknown";
    return true;
}

bool Client::GetArtistDescription(int64_t artistId, abort_callback& abort, std::wstring& brief,
                                  std::wstring& full, std::wstring& error) {
    brief.clear();
    full.clear();
    json::Object body;
    int code = 0;
    if (!Request(L"/artist/desc", {{L"id", std::to_wstring(artistId)}}, false, abort, body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    brief = json::GetString(body, L"briefDesc");
    if (auto introduction = json::GetArray(body, L"introduction")) {
        for (const auto& section : json::Objects(*introduction)) {
            const std::wstring title = json::GetString(section, L"ti");
            const std::wstring text = json::GetString(section, L"txt");
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
                                 std::vector<Album>& out, bool* followed, bool* more, std::wstring& error) {
    out.clear();
    if (followed) *followed = false;
    if (more) *more = false;
    json::Object body;
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
        if (auto artist = json::GetObject(body, L"artist"); artist && artist->HasKey(L"followed"))
            *followed = json::GetBool(*artist, L"followed");
    }
    if (more) *more = json::GetBool(body, L"more");
    if (auto albums = json::GetArray(body, L"hotAlbums")) {
        for (const auto& object : json::Objects(*albums)) {
            Album album = ParseAlbum(object);
            if (album.id) out.push_back(std::move(album));
        }
    }
    return true;
}

bool Client::GetArtistTopSongs(int64_t artistId, abort_callback& abort, std::vector<Song>& out,
                               std::wstring& error) {
    out.clear();
    json::Object body;
    int code = 0;
    if (!Request(L"/artist/top/song", {{L"id", std::to_wstring(artistId)}}, false, abort, body, code, error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (auto songs = json::GetArray(body, L"songs")) {
        for (const auto& object : json::Objects(*songs)) {
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
    json::Object body;
    int code = 0;
    if (!Request(L"/album", {{L"id", std::to_wstring(albumId)}}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (auto album = json::GetObject(body, L"album")) info = ParseAlbum(*album);
    info.id = info.id ? info.id : albumId;
    if (auto list = json::GetArray(body, L"songs")) {
        for (const auto& object : json::Objects(*list)) {
            Song song = ParseSong(object);
            if (song.id) songs.push_back(std::move(song));
        }
    }
    return true;
}

bool Client::GetFollowedArtists(abort_callback& abort, std::vector<Artist>& out, std::wstring& error) {
    out.clear();
    int offset = 0;
    for (int page = 0; page < 50; ++page) {
        json::Object body;
        int code = 0;
        if (!Request(L"/artist/sublist",
                     {{L"limit", L"100"}, {L"offset", std::to_wstring(offset)}}, false, abort, body, code, error))
            return false;
        if (code == 301) {
            error = L"Please sign in to NetEase Cloud Music.";
            return false;
        }
        if (code != 200) {
            error = MessageOf(body, code);
            return false;
        }
        auto artists = json::GetArray(body, L"data");  // the helper returns followed artists here
        if (!artists) artists = json::GetArray(body, L"artists");
        if (!artists) break;
        for (const auto& object : json::Objects(*artists)) {
            Artist artist = ParseArtist(object);
            if (!artist.id) continue;
            artist.followed = true;
            artist.followKnown = true;
            out.push_back(std::move(artist));
        }
        if (!json::GetBool(body, L"hasMore") || artists->Size() == 0) break;
        offset += static_cast<int>(artists->Size());
    }
    return true;
}

bool Client::GetCollectedAlbums(abort_callback& abort, std::vector<Album>& out, std::wstring& error) {
    out.clear();
    int offset = 0;
    for (int page = 0; page < 50; ++page) {
        json::Object body;
        int code = 0;
        if (!Request(L"/album/sublist",
                     {{L"limit", L"100"}, {L"offset", std::to_wstring(offset)}}, false, abort, body, code, error))
            return false;
        if (code == 301) {
            error = L"Please sign in to NetEase Cloud Music.";
            return false;
        }
        if (code != 200) {
            error = MessageOf(body, code);
            return false;
        }
        auto albums = json::GetArray(body, L"data");
        if (!albums) break;
        for (const auto& object : json::Objects(*albums)) {
            Album album = ParseAlbum(object);
            if (album.id) out.push_back(std::move(album));
        }
        if (!json::GetBool(body, L"hasMore") || albums->Size() == 0) break;
        offset += static_cast<int>(albums->Size());
    }
    return true;
}

bool Client::SetArtistFollow(int64_t artistId, bool follow, abort_callback& abort, std::wstring& error) {
    json::Object body;
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
    json::Object body;
    int code = 0;
    if (!Request(L"/likelist", {{L"uid", std::to_wstring(userId)}}, false, abort, body, code, error)) return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    if (auto ids = json::GetArray(body, L"ids")) out = IdsOf(*ids);
    return true;
}

bool Client::SetLike(int64_t id, bool like, abort_callback& abort, std::wstring& error) {
    json::Object body;
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
    json::Object body;
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

bool Client::CreatePlaylist(std::wstring_view name, abort_callback& abort, int64_t& id, std::wstring& error) {
    id = 0;
    json::Object body;
    int code = 0;
    if (!Request(L"/playlist/create",
                 {{L"name", std::wstring(name)}, {L"privacy", L"0"}, {L"type", L"NORMAL"}}, true, abort, body, code,
                 error))
        return false;
    if (code != 200) {
        error = MessageOf(body, code);
        return false;
    }
    id = json::GetInt64(body, L"id");
    if (!id) {
        if (auto playlist = json::GetObject(body, L"playlist")) id = json::GetInt64(*playlist, L"id");
    }
    if (!id) {
        error = L"The service did not return the new playlist id.";
        return false;
    }
    return true;
}

}  // namespace glasswave::netease
