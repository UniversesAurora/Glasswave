#ifndef GLASSWAVE_LISTS_STANDALONE
#include "pch.h"
#endif
#include "cache/lists.h"
#include "core/hex.h"
#include "core/text.h"
#include "cache/store.h"

#include <windows.h>
#include <bcrypt.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace glasswave::cache::lists {
namespace {

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

// 4: playlists, albums and artists carry their details (stream::Playlist / Album / Artist).
constexpr std::string_view kPageSchema = "glasswave-list-page/6"; // 6: Track::badges
constexpr std::string_view kManifestSchema = "glasswave-list-manifest/3";
constexpr std::string_view kPageContentType = "application/vnd.glasswave.list-page+json";
constexpr std::string_view kManifestContentType = "application/vnd.glasswave.list-manifest+json";
constexpr int64_t kMaxExactJsonInteger = 9'007'199'254'740'991;
constexpr std::size_t kMaxPageBytes = 32ull * 1024 * 1024;
constexpr std::size_t kMaxManifestBytes = 8ull * 1024 * 1024;
constexpr std::size_t kMaxRows = 200'000;
constexpr std::size_t kMaxPages = 65'536;
constexpr std::size_t kMaxStringBytes = 1ull * 1024 * 1024;
constexpr std::size_t kMaxIdentityBytes = 48ull * 1024;
constexpr std::size_t kMaxArtists = 4'096;
constexpr std::size_t kMaxJsonDepth = 64;
constexpr std::size_t kMaxCursorOffset = 1ull << 40;
constexpr std::size_t kMaxGenerationStates = 2'048;

#ifdef GLASSWAVE_LISTS_TESTING
std::mutex testManifestPauseMutex;
std::function<void()> testManifestPause;
std::function<void()> testManifestCommitPause;
std::function<void()> testManifestComplete;

void RunTestManifestPause() {
    std::function<void()> pause;
    {
        std::lock_guard lock(testManifestPauseMutex);
        pause = std::move(testManifestPause);
        testManifestPause = {};
    }
    if (pause) pause();
}

void RunTestManifestCommitPause() {
    std::function<void()> pause;
    {
        std::lock_guard lock(testManifestPauseMutex);
        pause = std::move(testManifestCommitPause);
        testManifestCommitPause = {};
    }
    if (pause) pause();
}

void RunTestManifestComplete() {
    std::function<void()> complete;
    {
        std::lock_guard lock(testManifestPauseMutex);
        complete = std::move(testManifestComplete);
        testManifestComplete = {};
    }
    if (complete) complete();
}
#else
void RunTestManifestPause() {}
void RunTestManifestCommitPause() {}
void RunTestManifestComplete() {}
#endif

class InvalidData final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void Invalid(std::string_view message) {
    throw InvalidData(std::string(message));
}

int HexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

void AppendUtf8(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

class DuplicateKeyScanner {
  public:
    static bool Validate(std::string_view text) {
        DuplicateKeyScanner scanner(text);
        if (!scanner.Value(0) || !scanner.Whitespace()) return false;
        return scanner.m_at == text.size();
    }

  private:
    explicit DuplicateKeyScanner(std::string_view text) : m_text(text) {}

    bool Whitespace() {
        while (m_at < m_text.size()) {
            const char c = m_text[m_at];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++m_at;
        }
        return true;
    }
    bool Consume(char expected) {
        Whitespace();
        if (m_at >= m_text.size() || m_text[m_at] != expected) return false;
        ++m_at;
        return true;
    }
    bool Hex4(uint32_t& value) {
        if (m_text.size() - m_at < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = HexDigit(m_text[m_at++]);
            if (digit < 0) return false;
            value = (value << 4) | static_cast<uint32_t>(digit);
        }
        return true;
    }
    bool String(std::string& decoded) {
        Whitespace();
        if (m_at >= m_text.size() || m_text[m_at++] != '"') return false;
        decoded.clear();
        while (m_at < m_text.size()) {
            const unsigned char c = static_cast<unsigned char>(m_text[m_at++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                decoded.push_back(static_cast<char>(c));
                continue;
            }
            if (m_at >= m_text.size()) return false;
            const char escaped = m_text[m_at++];
            switch (escaped) {
            case '"':
            case '\\':
            case '/':
                decoded.push_back(escaped);
                break;
            case 'b':
                decoded.push_back('\b');
                break;
            case 'f':
                decoded.push_back('\f');
                break;
            case 'n':
                decoded.push_back('\n');
                break;
            case 'r':
                decoded.push_back('\r');
                break;
            case 't':
                decoded.push_back('\t');
                break;
            case 'u': {
                uint32_t codepoint = 0;
                if (!Hex4(codepoint)) return false;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (m_text.size() - m_at < 6 || m_text[m_at] != '\\' || m_text[m_at + 1] != 'u')
                        return false;
                    m_at += 2;
                    uint32_t low = 0;
                    if (!Hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    return false;
                }
                AppendUtf8(decoded, codepoint);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }
    bool Literal(std::string_view literal) {
        Whitespace();
        if (m_text.substr(m_at, literal.size()) != literal) return false;
        m_at += literal.size();
        return true;
    }
    bool Number() {
        Whitespace();
        const std::size_t start = m_at;
        if (m_at < m_text.size() && m_text[m_at] == '-') ++m_at;
        if (m_at >= m_text.size()) return false;
        if (m_text[m_at] == '0') {
            ++m_at;
        } else {
            if (m_text[m_at] < '1' || m_text[m_at] > '9') return false;
            while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9')
                ++m_at;
        }
        if (m_at < m_text.size() && m_text[m_at] == '.') {
            ++m_at;
            const std::size_t digits = m_at;
            while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9')
                ++m_at;
            if (digits == m_at) return false;
        }
        if (m_at < m_text.size() && (m_text[m_at] == 'e' || m_text[m_at] == 'E')) {
            ++m_at;
            if (m_at < m_text.size() && (m_text[m_at] == '+' || m_text[m_at] == '-')) ++m_at;
            const std::size_t digits = m_at;
            while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9')
                ++m_at;
            if (digits == m_at) return false;
        }
        return m_at > start;
    }
    bool Object(std::size_t depth) {
        if (depth > kMaxJsonDepth || !Consume('{')) return false;
        Whitespace();
        if (m_at < m_text.size() && m_text[m_at] == '}') {
            ++m_at;
            return true;
        }
        std::set<std::string> keys;
        for (;;) {
            std::string key;
            if (!String(key) || !keys.insert(std::move(key)).second || !Consume(':') || !Value(depth + 1))
                return false;
            Whitespace();
            if (m_at >= m_text.size()) return false;
            if (m_text[m_at] == '}') {
                ++m_at;
                return true;
            }
            if (m_text[m_at++] != ',') return false;
        }
    }
    bool Array(std::size_t depth) {
        if (depth > kMaxJsonDepth || !Consume('[')) return false;
        Whitespace();
        if (m_at < m_text.size() && m_text[m_at] == ']') {
            ++m_at;
            return true;
        }
        for (;;) {
            if (!Value(depth + 1)) return false;
            Whitespace();
            if (m_at >= m_text.size()) return false;
            if (m_text[m_at] == ']') {
                ++m_at;
                return true;
            }
            if (m_text[m_at++] != ',') return false;
        }
    }
    bool Value(std::size_t depth) {
        Whitespace();
        if (m_at >= m_text.size()) return false;
        switch (m_text[m_at]) {
        case '{':
            return Object(depth);
        case '[':
            return Array(depth);
        case '"': {
            std::string ignored;
            return String(ignored);
        }
        case 't':
            return Literal("true");
        case 'f':
            return Literal("false");
        case 'n':
            return Literal("null");
        default:
            return Number();
        }
    }

    std::string_view m_text;
    std::size_t m_at = 0;
};

using core::Wide;
using core::Utf8;

std::size_t StringBytes(std::wstring_view text) {
    return Utf8(text).size();
}

bool ChunkToken(std::string_view value) {
    return value.size() == 32 &&
           std::all_of(value.begin(), value.end(), [](char c) { return HexDigit(c) >= 0; });
}
std::string RandomToken() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        Invalid("Cannot create a list cache session token.");
    return core::HexLower(bytes.data(), bytes.size());
}

template <typename... Allowed> void CheckKeys(const JsonObject& object, Allowed... allowed) {
    const std::array<std::wstring_view, sizeof...(Allowed)> names{allowed...};
    for (const auto& entry : object.GetView()) {
        const std::wstring key(entry.Key().c_str());
        if (std::find(names.begin(), names.end(), std::wstring_view(key)) == names.end())
            Invalid("A list cache object contains an unknown field.");
    }
}

IJsonValue Field(const JsonObject& object, const wchar_t* key) {
    const winrt::hstring name(key);
    if (!object.HasKey(name)) Invalid("A required list cache field is missing.");
    const IJsonValue value = object.Lookup(name);
    if (!value) Invalid("A list cache field has no value.");
    return value;
}
void RequireType(const IJsonValue& value, JsonValueType type) {
    if (value.ValueType() != type) Invalid("A list cache field has the wrong type.");
}
std::wstring StringField(const JsonObject& object, const wchar_t* key) {
    const auto value = Field(object, key);
    RequireType(value, JsonValueType::String);
    std::wstring out(value.GetString().c_str());
    if (StringBytes(out) > kMaxStringBytes) Invalid("A list cache string is too large.");
    return out;
}
bool BoolField(const JsonObject& object, const wchar_t* key) {
    const auto value = Field(object, key);
    RequireType(value, JsonValueType::Boolean);
    return value.GetBoolean();
}
double NumberValue(const IJsonValue& value) {
    RequireType(value, JsonValueType::Number);
    const double number = value.GetNumber();
    if (!std::isfinite(number)) Invalid("A list cache number is not finite.");
    return number;
}
int64_t Int64Value(const IJsonValue& value, int64_t minimum, int64_t maximum) {
    const double number = NumberValue(value);
    if (std::floor(number) != number || number < static_cast<double>(minimum) ||
        number > static_cast<double>(maximum))
        Invalid("A list cache integer is out of bounds.");
    return static_cast<int64_t>(number);
}
int64_t Int64Field(const JsonObject& object, const wchar_t* key, int64_t minimum, int64_t maximum) {
    return Int64Value(Field(object, key), minimum, maximum);
}
int IntField(const JsonObject& object, const wchar_t* key, int minimum, int maximum) {
    return static_cast<int>(Int64Field(object, key, minimum, maximum));
}
uint64_t DecimalStringField(const JsonObject& object, const wchar_t* key) {
    const std::string text = Utf8(StringField(object, key));
    if (text.empty()) Invalid("A list cache decimal token is empty.");
    uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') Invalid("A list cache decimal token is malformed.");
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
            Invalid("A list cache decimal token is out of bounds.");
        value = value * 10 + digit;
    }
    return value;
}
JsonObject ObjectField(const JsonObject& object, const wchar_t* key) {
    const auto value = Field(object, key);
    RequireType(value, JsonValueType::Object);
    return value.GetObject();
}
JsonArray ArrayField(const JsonObject& object, const wchar_t* key) {
    const auto value = Field(object, key);
    RequireType(value, JsonValueType::Array);
    return value.GetArray();
}
bool Has(const JsonObject& object, const wchar_t* key) {
    return object.HasKey(winrt::hstring(key));
}

void SetString(JsonObject& object, const wchar_t* key, std::wstring_view value) {
    object.SetNamedValue(key, JsonValue::CreateStringValue(winrt::hstring(value)));
}
void SetDecodedString(JsonObject& object, const wchar_t* key, std::string_view value) {
    object.SetNamedValue(key, JsonValue::CreateStringValue(winrt::to_hstring(value)));
}
void SetNumber(JsonObject& object, const wchar_t* key, double value) {
    object.SetNamedValue(key, JsonValue::CreateNumberValue(value));
}
void SetBool(JsonObject& object, const wchar_t* key, bool value) {
    object.SetNamedValue(key, JsonValue::CreateBooleanValue(value));
}
void SetDecimal(JsonObject& object, const wchar_t* key, uint64_t value) {
    SetDecodedString(object, key, std::to_string(value));
}
void SetStrings(JsonObject& object, const wchar_t* key, const std::vector<std::wstring>& values) {
    if (values.size() > kMaxArtists) Invalid("A list cache row has too many names.");
    JsonArray array;
    for (const auto& value : values) array.Append(JsonValue::CreateStringValue(winrt::hstring(value)));
    object.SetNamedValue(key, array);
}
std::vector<std::wstring> StringsField(const JsonObject& object, const wchar_t* key) {
    const JsonArray array = ArrayField(object, key);
    if (array.Size() > kMaxArtists) Invalid("A cached row has too many names.");
    std::vector<std::wstring> out;
    out.reserve(array.Size());
    for (uint32_t i = 0; i < array.Size(); ++i) {
        const IJsonValue value = array.GetAt(i);
        RequireType(value, JsonValueType::String);
        out.emplace_back(value.GetString().c_str());
        if (StringBytes(out.back()) > kMaxStringBytes) Invalid("A list cache string is too large.");
    }
    return out;
}

const char* RowKindName(const stream::ListRows& rows) {
    switch (rows.index()) {
    case 0:
        return "track";
    case 1:
        return "playlist";
    case 2:
        return "album";
    case 3:
        return "artist";
    case 4:
        return "group";
    case 5:
        return "catalog-search";
    default:
        Invalid("A list cache row variant is unavailable.");
    }
}
bool IsRowKind(std::string_view value) {
    return value == "track" || value == "playlist" || value == "album" || value == "artist" ||
           value == "group" || value == "catalog-search";
}
std::size_t RowCount(const stream::ListRows& rows) {
    return std::visit([](const auto& values) { return values.size(); }, rows);
}

JsonObject ArtistJson(const stream::ArtistRef& artist) {
    // A credit is a name; its id only makes it navigable. Services credit artists they have
    // no page for (uploaded and matched library songs), and those rows are cached like any.
    if (artist.id.empty() && artist.name.empty()) Invalid("A list cache artist credit is empty.");
    JsonObject object;
    SetString(object, L"id", artist.id);
    SetString(object, L"name", artist.name);
    return object;
}
JsonArray ArtistsJson(const std::vector<stream::ArtistRef>& artists) {
    if (artists.size() > kMaxArtists) Invalid("A list cache row has too many artists.");
    JsonArray array;
    for (const auto& artist : artists)
        if (!artist.id.empty() || !artist.name.empty()) array.Append(ArtistJson(artist));
    return array;
}
JsonObject TrackJson(const stream::Track& track) {
    if (track.id.empty()) Invalid("A list cache track has no stable identity.");
    JsonObject object;
    SetString(object, L"id", track.id);
    SetString(object, L"title", track.title);
    SetString(object, L"note", track.note);
    SetString(object, L"album", track.album);
    SetString(object, L"albumId", track.albumId);
    object.SetNamedValue(L"artists", ArtistsJson(track.artists));
    SetNumber(object, L"durationMs", track.durationMs);
    SetNumber(object, L"popularity", track.popularity);
    SetStrings(object, L"badges", track.badges);
    return object;
}
JsonObject PlaylistJson(const stream::Playlist& playlist) {
    if (playlist.id.empty()) Invalid("A list cache playlist has no stable identity.");
    JsonObject object;
    SetString(object, L"id", playlist.id);
    SetString(object, L"name", playlist.name);
    SetString(object, L"coverUrl", playlist.coverUrl);
    SetString(object, L"creator", playlist.creator);
    SetString(object, L"description", playlist.description);
    SetNumber(object, L"trackCount", playlist.trackCount);
    SetBool(object, L"own", playlist.own);
    SetBool(object, L"saved", playlist.saved);
    SetString(object, L"tagline", playlist.tagline);
    SetString(object, L"type", playlist.type);
    SetString(object, L"created", playlist.created);
    SetString(object, L"updated", playlist.updated);
    SetString(object, L"updates", playlist.updates);
    SetStrings(object, L"tags", playlist.tags);
    SetStrings(object, L"aliases", playlist.aliases);
    SetStrings(object, L"traits", playlist.traits);
    return object;
}
JsonObject AlbumJson(const stream::Album& album) {
    if (album.id.empty()) Invalid("A list cache album has no stable identity.");
    JsonObject object;
    SetString(object, L"id", album.id);
    SetString(object, L"name", album.name);
    SetString(object, L"coverUrl", album.coverUrl);
    object.SetNamedValue(L"artists", ArtistsJson(album.artists));
    SetString(object, L"description", album.description);
    SetNumber(object, L"trackCount", album.trackCount);
    SetNumber(object, L"year", album.year);
    SetString(object, L"released", album.released);
    SetString(object, L"label", album.label);
    SetString(object, L"copyright", album.copyright);
    SetString(object, L"type", album.type);
    SetString(object, L"edition", album.edition);
    SetString(object, L"upc", album.upc);
    SetStrings(object, L"genres", album.genres);
    SetStrings(object, L"aliases", album.aliases);
    SetStrings(object, L"traits", album.traits);
    return object;
}
JsonObject ArtistRowJson(const stream::Artist& artist) {
    if (artist.id.empty()) Invalid("A list cache artist row has no stable identity.");
    JsonObject object;
    SetString(object, L"id", artist.id);
    SetString(object, L"name", artist.name);
    SetString(object, L"avatarUrl", artist.avatarUrl);
    SetString(object, L"bio", artist.bio);
    SetNumber(object, L"albumCount", artist.albumCount);
    SetNumber(object, L"trackCount", artist.trackCount);
    SetBool(object, L"followKnown", artist.followKnown);
    SetBool(object, L"followed", artist.followed);
    SetBool(object, L"profile", artist.profile);
    SetString(object, L"tagline", artist.tagline);
    SetString(object, L"born", artist.born);
    SetString(object, L"origin", artist.origin);
    SetString(object, L"area", artist.area);
    SetNumber(object, L"kind", static_cast<int>(artist.kind));
    SetStrings(object, L"genres", artist.genres);
    SetStrings(object, L"aliases", artist.aliases);
    SetStrings(object, L"roles", artist.roles);
    SetNumber(object, L"videoCount", artist.videoCount);
    return object;
}
JsonObject GroupJson(const stream::BrowseGroup& group) {
    if (group.id.empty()) Invalid("A list cache group has no stable identity.");
    JsonObject object;
    SetString(object, L"id", group.id);
    SetString(object, L"label", group.label);
    SetNumber(object, L"trackCount", static_cast<double>(group.trackCount));
    return object;
}

const char* CatalogSearchKindName(stream::CatalogSearchItemKind kind) {
    switch (kind) {
    case stream::CatalogSearchItemKind::Track:
        return "track";
    case stream::CatalogSearchItemKind::Artist:
        return "artist";
    case stream::CatalogSearchItemKind::Album:
        return "album";
    case stream::CatalogSearchItemKind::Playlist:
        return "playlist";
    }
    Invalid("A catalog search item kind is unavailable.");
}

JsonObject CatalogSearchItemJson(const stream::CatalogSearchItem& item) {
    JsonObject object;
    SetDecodedString(object, L"kind", CatalogSearchKindName(stream::CatalogSearchItemKindOf(item)));
    std::visit(
        [&object](const auto& row) {
            using Row = std::decay_t<decltype(row)>;
            if constexpr (std::is_same_v<Row, stream::Track>)
                object.SetNamedValue(L"data", TrackJson(row));
            else if constexpr (std::is_same_v<Row, stream::Artist>)
                object.SetNamedValue(L"data", ArtistRowJson(row));
            else if constexpr (std::is_same_v<Row, stream::Album>)
                object.SetNamedValue(L"data", AlbumJson(row));
            else if constexpr (std::is_same_v<Row, stream::Playlist>)
                object.SetNamedValue(L"data", PlaylistJson(row));
        },
        item);
    return object;
}
JsonObject CursorJson(const stream::ListCursor& cursor) {
    if (cursor.offset > kMaxCursorOffset) Invalid("A list cache cursor offset is out of bounds.");
    JsonObject object;
    SetString(object, L"value", cursor.value);
    SetNumber(object, L"offset", static_cast<double>(cursor.offset));
    return object;
}

stream::ArtistRef ParseArtist(const JsonObject& object) {
    CheckKeys(object, L"id", L"name");
    stream::ArtistRef artist;
    artist.id = StringField(object, L"id");
    artist.name = StringField(object, L"name");
    if (artist.id.empty() && artist.name.empty()) Invalid("A cached artist credit is empty.");
    return artist;
}
std::vector<stream::ArtistRef> ParseArtists(const JsonArray& array) {
    if (array.Size() > kMaxArtists) Invalid("A cached row has too many artists.");
    std::vector<stream::ArtistRef> artists;
    artists.reserve(array.Size());
    for (uint32_t i = 0; i < array.Size(); ++i) {
        const IJsonValue value = array.GetAt(i);
        RequireType(value, JsonValueType::Object);
        artists.push_back(ParseArtist(value.GetObject()));
    }
    return artists;
}
stream::Track ParseTrack(const JsonObject& object) {
    CheckKeys(object, L"id", L"title", L"note", L"album", L"albumId", L"artists", L"durationMs", L"popularity",
              L"badges");
    stream::Track track;
    track.id = StringField(object, L"id");
    track.title = StringField(object, L"title");
    track.note = StringField(object, L"note");
    track.album = StringField(object, L"album");
    track.albumId = StringField(object, L"albumId");
    track.artists = ParseArtists(ArrayField(object, L"artists"));
    track.durationMs =
        IntField(object, L"durationMs", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    track.popularity =
        IntField(object, L"popularity", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    track.badges = StringsField(object, L"badges");
    if (track.id.empty()) Invalid("A cached track has no stable identity.");
    return track;
}
stream::Playlist ParsePlaylist(const JsonObject& object) {
    CheckKeys(object, L"id", L"name", L"coverUrl", L"creator", L"description", L"trackCount", L"own",
              L"saved", L"tagline", L"type", L"created", L"updated", L"updates", L"tags", L"aliases", L"traits");
    stream::Playlist playlist;
    playlist.id = StringField(object, L"id");
    playlist.name = StringField(object, L"name");
    playlist.coverUrl = StringField(object, L"coverUrl");
    playlist.creator = StringField(object, L"creator");
    playlist.description = StringField(object, L"description");
    playlist.trackCount =
        IntField(object, L"trackCount", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    playlist.own = BoolField(object, L"own");
    playlist.saved = BoolField(object, L"saved");
    playlist.tagline = StringField(object, L"tagline");
    playlist.type = StringField(object, L"type");
    playlist.created = StringField(object, L"created");
    playlist.updated = StringField(object, L"updated");
    playlist.updates = StringField(object, L"updates");
    playlist.tags = StringsField(object, L"tags");
    playlist.aliases = StringsField(object, L"aliases");
    playlist.traits = StringsField(object, L"traits");
    if (playlist.id.empty()) Invalid("A cached playlist has no stable identity.");
    return playlist;
}
stream::Album ParseAlbum(const JsonObject& object) {
    CheckKeys(object, L"id", L"name", L"coverUrl", L"artists", L"description", L"trackCount", L"year", L"released",
              L"label", L"copyright", L"type", L"edition", L"upc", L"genres", L"aliases", L"traits");
    stream::Album album;
    album.id = StringField(object, L"id");
    album.name = StringField(object, L"name");
    album.coverUrl = StringField(object, L"coverUrl");
    album.artists = ParseArtists(ArrayField(object, L"artists"));
    album.description = StringField(object, L"description");
    album.trackCount =
        IntField(object, L"trackCount", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    album.year = IntField(object, L"year", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    album.released = StringField(object, L"released");
    album.label = StringField(object, L"label");
    album.copyright = StringField(object, L"copyright");
    album.type = StringField(object, L"type");
    album.edition = StringField(object, L"edition");
    album.upc = StringField(object, L"upc");
    album.genres = StringsField(object, L"genres");
    album.aliases = StringsField(object, L"aliases");
    album.traits = StringsField(object, L"traits");
    if (album.id.empty()) Invalid("A cached album has no stable identity.");
    return album;
}
stream::Artist ParseArtistRow(const JsonObject& object) {
    CheckKeys(object, L"id", L"name", L"avatarUrl", L"bio", L"albumCount", L"trackCount", L"followKnown",
              L"followed", L"profile", L"tagline", L"born", L"origin", L"area", L"kind", L"genres", L"aliases",
              L"roles", L"videoCount");
    stream::Artist artist;
    artist.id = StringField(object, L"id");
    artist.name = StringField(object, L"name");
    artist.avatarUrl = StringField(object, L"avatarUrl");
    artist.bio = StringField(object, L"bio");
    artist.albumCount =
        IntField(object, L"albumCount", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    artist.trackCount =
        IntField(object, L"trackCount", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    artist.followKnown = BoolField(object, L"followKnown");
    artist.followed = BoolField(object, L"followed");
    artist.profile = BoolField(object, L"profile");
    artist.tagline = StringField(object, L"tagline");
    artist.born = StringField(object, L"born");
    artist.origin = StringField(object, L"origin");
    artist.area = StringField(object, L"area");
    const int kind = IntField(object, L"kind", 0, static_cast<int>(stream::ArtistKind::Group));
    artist.kind = static_cast<stream::ArtistKind>(kind);
    artist.genres = StringsField(object, L"genres");
    artist.aliases = StringsField(object, L"aliases");
    artist.roles = StringsField(object, L"roles");
    artist.videoCount =
        IntField(object, L"videoCount", std::numeric_limits<int>::min(), std::numeric_limits<int>::max());    if (artist.id.empty()) Invalid("A cached artist row has no stable identity.");
    return artist;
}
stream::BrowseGroup ParseGroup(const JsonObject& object) {
    CheckKeys(object, L"id", L"label", L"trackCount");
    stream::BrowseGroup group;
    group.id = StringField(object, L"id");
    group.label = StringField(object, L"label");
    group.trackCount = static_cast<std::size_t>(Int64Field(object, L"trackCount", 0, kMaxCursorOffset));
    if (group.id.empty()) Invalid("A cached group has no stable identity.");
    return group;
}
stream::ListCursor ParseCursor(const JsonObject& object) {
    CheckKeys(object, L"value", L"offset");
    stream::ListCursor cursor;
    cursor.value = StringField(object, L"value");
    cursor.offset = static_cast<std::size_t>(Int64Field(object, L"offset", 0, kMaxCursorOffset));
    return cursor;
}
std::optional<stream::Playlist> OptionalPlaylist(const JsonObject& object) {
    return Has(object, L"playlist")
               ? std::optional<stream::Playlist>(ParsePlaylist(ObjectField(object, L"playlist")))
               : std::nullopt;
}
std::optional<stream::Album> OptionalAlbum(const JsonObject& object) {
    return Has(object, L"album") ? std::optional<stream::Album>(ParseAlbum(ObjectField(object, L"album")))
                                 : std::nullopt;
}
std::optional<stream::Artist> OptionalArtist(const JsonObject& object) {
    return Has(object, L"artist")
               ? std::optional<stream::Artist>(ParseArtistRow(ObjectField(object, L"artist")))
               : std::nullopt;
}
std::optional<std::size_t> OptionalTotal(const JsonObject& object) {
    if (!Has(object, L"total")) return std::nullopt;
    const auto value = Field(object, L"total");
    if (value.ValueType() == JsonValueType::Null) return std::nullopt;
    return static_cast<std::size_t>(Int64Value(value, 0, kMaxRows));
}

struct PageDocument {
    std::size_t ordinal = 0;
    stream::ListPage page;
    std::string body;
    std::string chunk;
};

struct SerializedPage {
    std::size_t ordinal = 0;
    std::string rowKind;
    std::size_t rowCount = 0;
    stream::ListCursor next;
    bool more = false;
    bool enrichmentPending = false;
    std::optional<std::size_t> total;
    std::wstring revision;
    std::string body;
    std::string chunk;
};

bool ValidIdentity(const Identity& identity) {
    return !identity.provider.empty() && identity.provider.size() <= kMaxIdentityBytes &&
           !identity.logical.empty() && identity.logical.size() <= kMaxIdentityBytes;
}

void ValidatePageData(const stream::ListPage& page) {
    const std::size_t rows = RowCount(page.rows);
    if (rows > kMaxRows) Invalid("A list cache page has too many rows.");
    if (page.total && *page.total > kMaxRows) Invalid("A list cache total is out of bounds.");
    if (page.next.offset > kMaxCursorOffset) Invalid("A list cache cursor offset is out of bounds.");
    if (page.more && page.next.value.empty() && page.next.offset == 0)
        Invalid("A non-final list cache page has no advancing cursor.");
    if (StringBytes(page.next.value) > kMaxStringBytes || StringBytes(page.revision) > kMaxStringBytes)
        Invalid("A list cache header is too large.");
    std::visit(
        [](const auto& values) {
            using Row = typename std::decay_t<decltype(values)>::value_type;
            (void)sizeof(Row);
            if (values.size() > kMaxRows) Invalid("A list cache page has too many rows.");
        },
        page.rows);
}

SerializedPage SerializePage(std::size_t ordinal, const stream::ListPage& page) {
    if (ordinal == 0) Invalid("A list cache page ordinal must be 1-based.");
    ValidatePageData(page);
    JsonObject object;
    SetDecodedString(object, L"schema", kPageSchema);
    SetNumber(object, L"ordinal", static_cast<double>(ordinal));
    SetDecodedString(object, L"rowsKind", RowKindName(page.rows));
    JsonArray rows;
    std::visit(
        [&rows](const auto& values) {
            using Rows = std::decay_t<decltype(values)>;
            if constexpr (std::is_same_v<Rows, std::vector<stream::Track>>) {
                for (const auto& row : values)
                    rows.Append(TrackJson(row));
            } else if constexpr (std::is_same_v<Rows, std::vector<stream::Playlist>>) {
                for (const auto& row : values)
                    rows.Append(PlaylistJson(row));
            } else if constexpr (std::is_same_v<Rows, std::vector<stream::Album>>) {
                for (const auto& row : values)
                    rows.Append(AlbumJson(row));
            } else if constexpr (std::is_same_v<Rows, std::vector<stream::Artist>>) {
                for (const auto& row : values)
                    rows.Append(ArtistRowJson(row));
            } else if constexpr (std::is_same_v<Rows, std::vector<stream::BrowseGroup>>) {
                for (const auto& row : values)
                    rows.Append(GroupJson(row));
            } else if constexpr (std::is_same_v<Rows, std::vector<stream::CatalogSearchItem>>) {
                for (const auto& row : values)
                    rows.Append(CatalogSearchItemJson(row));
            }
        },
        page.rows);
    object.SetNamedValue(L"rows", rows);
    if (page.playlist) object.SetNamedValue(L"playlist", PlaylistJson(*page.playlist));
    if (page.album) object.SetNamedValue(L"album", AlbumJson(*page.album));
    if (page.artist) object.SetNamedValue(L"artist", ArtistRowJson(*page.artist));
    object.SetNamedValue(L"next", CursorJson(page.next));
    SetBool(object, L"more", page.more);
    SetBool(object, L"enrichmentPending", page.enrichmentPending);
    if (page.total) SetNumber(object, L"total", static_cast<double>(*page.total));
    SetString(object, L"revision", page.revision);
    SerializedPage document;
    document.ordinal = ordinal;
    document.rowKind = RowKindName(page.rows);
    document.rowCount = RowCount(page.rows);
    document.next = page.next;
    document.more = page.more;
    document.enrichmentPending = page.enrichmentPending;
    document.total = page.total;
    document.revision = page.revision;
    document.body = Utf8(object.Stringify().c_str());
    if (document.body.size() > kMaxPageBytes) Invalid("A list cache page is too large.");
    document.chunk = RandomToken();
    return document;
}

stream::ListPage ParsePageBody(std::string_view body, std::size_t expectedOrdinal) {
    if (body.size() > kMaxPageBytes) Invalid("A cached list page is too large.");
    if (!DuplicateKeyScanner::Validate(body))
        Invalid("A cached list page is malformed or has duplicate keys.");
    JsonObject object;
    try {
        object = JsonObject::Parse(winrt::to_hstring(body));
    } catch (...) {
        Invalid("A cached list page is malformed.");
    }
    CheckKeys(object, L"schema", L"ordinal", L"rowsKind", L"rows", L"playlist", L"album", L"artist", L"next",
              L"more", L"enrichmentPending", L"total", L"revision");
    const auto schema = Utf8(StringField(object, L"schema"));
    if (schema != kPageSchema) Invalid("A cached page schema is unsupported.");
    const std::size_t ordinal =
        static_cast<std::size_t>(Int64Field(object, L"ordinal", 1, static_cast<int64_t>(kMaxPages)));
    if (ordinal != expectedOrdinal) Invalid("A cached page ordinal does not match its reference.");
    const std::string kind = Utf8(StringField(object, L"rowsKind"));
    if (!IsRowKind(kind)) Invalid("A cached page row kind is unsupported.");
    const JsonArray array = ArrayField(object, L"rows");
    if (array.Size() > kMaxRows) Invalid("A cached list page has too many rows.");
    stream::ListPage page;
    if (kind == "track") {
        std::vector<stream::Track> rows;
        rows.reserve(array.Size());
        for (uint32_t i = 0; i < array.Size(); ++i) {
            const IJsonValue value = array.GetAt(i);
            RequireType(value, JsonValueType::Object);
            rows.push_back(ParseTrack(value.GetObject()));
        }
        page.rows = std::move(rows);
    } else if (kind == "playlist") {
        std::vector<stream::Playlist> rows;
        rows.reserve(array.Size());
        for (uint32_t i = 0; i < array.Size(); ++i) {
            const IJsonValue value = array.GetAt(i);
            RequireType(value, JsonValueType::Object);
            rows.push_back(ParsePlaylist(value.GetObject()));
        }
        page.rows = std::move(rows);
    } else if (kind == "album") {
        std::vector<stream::Album> rows;
        rows.reserve(array.Size());
        for (uint32_t i = 0; i < array.Size(); ++i) {
            const IJsonValue value = array.GetAt(i);
            RequireType(value, JsonValueType::Object);
            rows.push_back(ParseAlbum(value.GetObject()));
        }
        page.rows = std::move(rows);
    } else if (kind == "artist") {
        std::vector<stream::Artist> rows;
        rows.reserve(array.Size());
        for (uint32_t i = 0; i < array.Size(); ++i) {
            const IJsonValue value = array.GetAt(i);
            RequireType(value, JsonValueType::Object);
            rows.push_back(ParseArtistRow(value.GetObject()));
        }
        page.rows = std::move(rows);
    } else if (kind == "group") {
        std::vector<stream::BrowseGroup> rows;
        rows.reserve(array.Size());
        for (uint32_t i = 0; i < array.Size(); ++i) {
            const IJsonValue value = array.GetAt(i);
            RequireType(value, JsonValueType::Object);
            rows.push_back(ParseGroup(value.GetObject()));
        }
        page.rows = std::move(rows);
    } else {
        std::vector<stream::CatalogSearchItem> rows;
        rows.reserve(array.Size());
        for (uint32_t i = 0; i < array.Size(); ++i) {
            const IJsonValue value = array.GetAt(i);
            RequireType(value, JsonValueType::Object);
            const JsonObject item = value.GetObject();
            CheckKeys(item, L"kind", L"data");
            const std::string itemKind = Utf8(StringField(item, L"kind"));
            const JsonObject data = ObjectField(item, L"data");
            if (itemKind == "track")
                rows.emplace_back(ParseTrack(data));
            else if (itemKind == "artist")
                rows.emplace_back(ParseArtistRow(data));
            else if (itemKind == "album")
                rows.emplace_back(ParseAlbum(data));
            else if (itemKind == "playlist")
                rows.emplace_back(ParsePlaylist(data));
            else
                Invalid("A catalog search item has an unsupported kind.");
        }
        page.rows = std::move(rows);
    }
    page.playlist = OptionalPlaylist(object);
    page.album = OptionalAlbum(object);
    page.artist = OptionalArtist(object);
    page.next = ParseCursor(ObjectField(object, L"next"));
    page.more = BoolField(object, L"more");
    page.enrichmentPending = BoolField(object, L"enrichmentPending");
    page.total = OptionalTotal(object);
    page.revision = StringField(object, L"revision");
    ValidatePageData(page);
    return page;
}

struct PageRef {
    std::size_t ordinal = 0;
    std::size_t rowCount = 0;
    bool more = false;
    bool enrichmentPending = false;
    std::wstring nextValue;
    std::size_t nextOffset = 0;
    std::wstring revision;
    std::optional<std::size_t> total;
    std::string chunk;
};

struct Manifest {
    std::string logical;
    std::string generation;
    uint64_t generationInput = 0;
    uint64_t saveTicket = 0;
    int64_t validatedAt = 0;
    bool complete = false;
    std::string rowKind;
    std::size_t rowCount = 0;
    std::wstring nextValue;
    std::size_t nextOffset = 0;
    std::optional<std::size_t> total;
    std::wstring revision;
    std::vector<PageRef> pages;
};

JsonObject RefJson(const PageRef& ref) {
    JsonObject object;
    SetNumber(object, L"ordinal", static_cast<double>(ref.ordinal));
    SetNumber(object, L"rowCount", static_cast<double>(ref.rowCount));
    SetBool(object, L"more", ref.more);
    SetBool(object, L"enrichmentPending", ref.enrichmentPending);
    SetString(object, L"nextValue", ref.nextValue);
    SetNumber(object, L"nextOffset", static_cast<double>(ref.nextOffset));
    SetString(object, L"revision", ref.revision);
    if (ref.total) SetNumber(object, L"total", static_cast<double>(*ref.total));
    SetDecodedString(object, L"chunk", ref.chunk);
    return object;
}

void ValidateManifest(const Manifest& manifest) {
    if (manifest.logical.empty() || manifest.logical.size() > kMaxIdentityBytes)
        Invalid("A list cache manifest has no valid semantic identity.");
    if (manifest.generation.empty() || manifest.generation.size() > 256)
        Invalid("A list cache manifest has no valid generation.");
    if (manifest.pages.empty() || manifest.pages.size() > kMaxPages)
        Invalid("A list cache manifest has an invalid page count.");
    if (!IsRowKind(manifest.rowKind)) Invalid("A list cache manifest has an unsupported row kind.");
    std::size_t rows = 0;
    std::set<std::wstring> cursors;
    std::set<std::string> chunks;
    std::size_t previousOffset = 0;
    std::optional<std::size_t> pageTotal;
    std::wstring pageRevision;
    for (std::size_t i = 0; i < manifest.pages.size(); ++i) {
        const auto& page = manifest.pages[i];
        if (page.ordinal != i + 1 || page.rowCount > kMaxRows || !ChunkToken(page.chunk))
            Invalid("A list cache manifest has a malformed page reference.");
        if (!chunks.insert(page.chunk).second)
            Invalid("A list cache manifest repeats an immutable page chunk.");
        if (rows > kMaxRows - page.rowCount) Invalid("A list cache manifest exceeds its row bound.");
        rows += page.rowCount;
        if (i + 1 < manifest.pages.size() && !page.more)
            Invalid("A list cache manifest places a page after a completed snapshot.");
        if (page.total && *page.total > kMaxRows) Invalid("A list cache manifest total is out of bounds.");
        if (page.total) {
            if (pageTotal && *pageTotal != *page.total)
                Invalid("A list cache manifest contains conflicting page totals.");
            pageTotal = page.total;
        }
        if (page.nextOffset > kMaxCursorOffset) Invalid("A list cache manifest cursor is out of bounds.");
        if (!page.revision.empty()) {
            if (!pageRevision.empty() && pageRevision != page.revision)
                Invalid("A list cache manifest contains conflicting page revisions.");
            pageRevision = page.revision;
        }
        if (page.more) {
            if (page.nextValue.empty() && page.nextOffset == 0)
                Invalid("A list cache manifest has a non-advancing cursor.");
            if (!page.nextValue.empty() && !cursors.insert(page.nextValue).second)
                Invalid("A list cache manifest repeats a cursor.");
            if (previousOffset != 0 && page.nextOffset != 0 && page.nextOffset <= previousOffset)
                Invalid("A list cache manifest cursor offset does not advance.");
            if (page.nextOffset != 0) previousOffset = page.nextOffset;
        }
    }
    if (rows != manifest.rowCount) Invalid("A list cache manifest row count is inconsistent.");
    if (manifest.total != pageTotal)
        Invalid("A list cache manifest aggregate total is inconsistent with its pages.");
    if (manifest.total && *manifest.total < manifest.rowCount)
        Invalid("A list cache manifest total is smaller than its loaded prefix.");
    if (manifest.revision != pageRevision)
        Invalid("A list cache manifest aggregate revision is inconsistent with its pages.");
    const auto& last = manifest.pages.back();
    if (manifest.complete != !last.more) Invalid("A list cache manifest completion flag is inconsistent.");
    if (manifest.nextValue != last.nextValue || manifest.nextOffset != last.nextOffset ||
        (!last.revision.empty() && manifest.revision != last.revision))
        Invalid("A list cache manifest trailing headers are inconsistent.");
    if (manifest.complete && manifest.total && *manifest.total != manifest.rowCount)
        Invalid("A complete list cache snapshot has an inconsistent total.");
}

std::string SerializeManifest(const Manifest& manifest) {
    ValidateManifest(manifest);
    JsonObject object;
    SetDecodedString(object, L"schema", kManifestSchema);
    SetDecodedString(object, L"logical", manifest.logical);
    SetDecodedString(object, L"generation", manifest.generation);
    SetDecimal(object, L"generationInput", manifest.generationInput);
    SetDecimal(object, L"saveTicket", manifest.saveTicket);
    SetNumber(object, L"validatedAt", static_cast<double>(manifest.validatedAt));
    SetBool(object, L"complete", manifest.complete);
    SetDecodedString(object, L"rowKind", manifest.rowKind);
    SetNumber(object, L"rowCount", static_cast<double>(manifest.rowCount));
    SetString(object, L"nextValue", manifest.nextValue);
    SetNumber(object, L"nextOffset", static_cast<double>(manifest.nextOffset));
    if (manifest.total) SetNumber(object, L"total", static_cast<double>(*manifest.total));
    SetString(object, L"revision", manifest.revision);
    JsonArray pages;
    for (const auto& page : manifest.pages)
        pages.Append(RefJson(page));
    object.SetNamedValue(L"pages", pages);
    const std::string body = Utf8(object.Stringify().c_str());
    if (body.size() > kMaxManifestBytes) Invalid("A list cache manifest is too large.");
    return body;
}

PageRef ParseRef(const JsonObject& object) {
    CheckKeys(object, L"ordinal", L"rowCount", L"more", L"nextValue", L"nextOffset", L"revision", L"total",
              L"chunk", L"enrichmentPending");
    PageRef ref;
    ref.ordinal =
        static_cast<std::size_t>(Int64Field(object, L"ordinal", 1, static_cast<int64_t>(kMaxPages)));
    ref.rowCount =
        static_cast<std::size_t>(Int64Field(object, L"rowCount", 0, static_cast<int64_t>(kMaxRows)));
    ref.more = BoolField(object, L"more");
    ref.enrichmentPending = BoolField(object, L"enrichmentPending");
    ref.nextValue = StringField(object, L"nextValue");
    ref.nextOffset = static_cast<std::size_t>(Int64Field(object, L"nextOffset", 0, kMaxCursorOffset));
    ref.revision = StringField(object, L"revision");
    ref.total = OptionalTotal(object);
    ref.chunk = Utf8(StringField(object, L"chunk"));
    return ref;
}

Manifest ParseManifestBody(std::string_view body, std::string_view expectedLogical) {
    if (body.size() > kMaxManifestBytes) Invalid("A cached list manifest is too large.");
    if (!DuplicateKeyScanner::Validate(body))
        Invalid("A cached list manifest is malformed or has duplicate keys.");
    JsonObject object;
    try {
        object = JsonObject::Parse(winrt::to_hstring(body));
    } catch (...) {
        Invalid("A cached list manifest is malformed.");
    }
    CheckKeys(object, L"schema", L"logical", L"generation", L"generationInput", L"saveTicket", L"validatedAt",
              L"complete", L"rowKind", L"rowCount", L"nextValue", L"nextOffset", L"total", L"revision",
              L"pages");
    const auto schema = Utf8(StringField(object, L"schema"));
    if (schema != kManifestSchema) Invalid("A cached list manifest schema is unsupported.");
    Manifest manifest;
    manifest.logical = Utf8(StringField(object, L"logical"));
    if (manifest.logical != expectedLogical) Invalid("A cached list manifest belongs to another query.");
    manifest.generation = Utf8(StringField(object, L"generation"));
    manifest.generationInput = DecimalStringField(object, L"generationInput");
    manifest.saveTicket = DecimalStringField(object, L"saveTicket");
    manifest.validatedAt = Int64Field(object, L"validatedAt", 0, kMaxExactJsonInteger);
    manifest.complete = BoolField(object, L"complete");
    manifest.rowKind = Utf8(StringField(object, L"rowKind"));
    manifest.rowCount =
        static_cast<std::size_t>(Int64Field(object, L"rowCount", 0, static_cast<int64_t>(kMaxRows)));
    manifest.nextValue = StringField(object, L"nextValue");
    manifest.nextOffset = static_cast<std::size_t>(Int64Field(object, L"nextOffset", 0, kMaxCursorOffset));
    manifest.total = OptionalTotal(object);
    manifest.revision = StringField(object, L"revision");
    const JsonArray pages = ArrayField(object, L"pages");
    if (pages.Size() > kMaxPages) Invalid("A cached list manifest has too many pages.");
    manifest.pages.reserve(pages.Size());
    for (uint32_t i = 0; i < pages.Size(); ++i) {
        const IJsonValue value = pages.GetAt(i);
        RequireType(value, JsonValueType::Object);
        manifest.pages.push_back(ParseRef(value.GetObject()));
    }
    ValidateManifest(manifest);
    return manifest;
}

void ValidateContinuity(const Manifest& current, const SerializedPage& next) {
    if (current.complete) Invalid("A completed list snapshot cannot accept another page.");
    if (next.ordinal != current.pages.size() + 1) Invalid("A list cache page ordinal is not contiguous.");
    if (current.rowKind != next.rowKind) Invalid("A list cache page changed its row schema.");
    if (current.total && next.total && *current.total != *next.total)
        Invalid("A list cache page changed its total.");
    if (!current.revision.empty() && !next.revision.empty() && current.revision != next.revision)
        Invalid("A list cache page changed its revision.");
    if (current.rowCount > kMaxRows - next.rowCount) Invalid("A list cache snapshot exceeds its row bound.");
    if (!current.pages.empty()) {
        const auto& previous = current.pages.back();
        if (!previous.more) Invalid("A list cache page follows a final page.");
    }
    if (next.more) {
        for (const auto& page : current.pages) {
            if (!page.more) continue;
            if (!next.next.value.empty() && page.nextValue == next.next.value)
                Invalid("A list cache snapshot repeats a cursor.");
            if (page.nextOffset != 0 && next.next.offset != 0 && next.next.offset <= page.nextOffset)
                Invalid("A list cache cursor offset does not advance.");
        }
    }
}

Key ManifestKey(const Identity& identity, const std::string& accountScope, const std::string& guardScope) {
    return Key{Kind::Data, identity.provider, accountScope, "lists-manifest/3/" + identity.logical,
               "guard/" + guardScope};
}
Key PageChunkKey(const Identity& identity, const std::string& accountScope, const std::string& guardScope,
                 const std::string& generation, std::size_t ordinal, std::string_view chunk) {
    return Key{Kind::Data, identity.provider, accountScope, "lists-page/3/" + identity.logical,
               "guard/" + guardScope + "/generation/" + generation + "/index/" + std::to_string(ordinal) +
                   "/chunk/" + std::string(chunk)};
}

std::optional<std::string> ReadSnapshot(const Snapshot& snapshot, std::size_t maximum) {
    if (snapshot.Size() > maximum) return std::nullopt;
    std::ifstream input(snapshot.Path(), std::ios::binary);
    if (!input) return std::nullopt;
    std::string body(static_cast<std::size_t>(snapshot.Size()), '\0');
    if (!body.empty()) input.read(body.data(), static_cast<std::streamsize>(body.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) return std::nullopt;
    return body;
}

bool WriteSnapshot(Store& store, const Key& key, std::string_view body, std::string_view contentType,
                   int64_t validatedAt, std::string extra, std::mutex* publicationGate = nullptr,
                   const std::function<bool()>* commitCurrent = nullptr) {
    try {
        std::unique_ptr<Writer> writer;
        if (publicationGate) {
            std::lock_guard lock(*publicationGate);
            if (!commitCurrent || !(*commitCurrent)()) return false;
            writer = store.Begin(key);
        } else {
            writer = store.Begin(key);
        }
        if (!writer) return false;
        {
            std::ofstream output(writer->Path(), std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output.write(body.data(), static_cast<std::streamsize>(body.size()));
            output.close();
            if (!output) return false;
        }
        Metadata metadata;
        metadata.contentType = std::string(contentType);
        metadata.etag = "\"" + RandomToken() + "\"";
        metadata.lastModified = std::to_string(Store::Now());
        metadata.extra = std::move(extra);
        metadata.validatedAt = validatedAt ? validatedAt : Store::Now();
        if (publicationGate) RunTestManifestCommitPause();
        if (publicationGate) {
            std::lock_guard lock(*publicationGate);
            if (!commitCurrent || !(*commitCurrent)()) return false;
            return writer->Commit(std::move(metadata)).has_value();
        }
        if (commitCurrent && !(*commitCurrent)()) return false;
        return writer->Commit(std::move(metadata)).has_value();
    } catch (...) {
        return false;
    }
}

std::optional<Manifest> ReadManifest(Store& store, const Identity& identity, const std::string& accountScope,
                                     const std::string& guardScope) {
    try {
        const auto snapshot = store.Lookup(ManifestKey(identity, accountScope, guardScope));
        if (!snapshot) return std::nullopt;
        auto body = ReadSnapshot(*snapshot, kMaxManifestBytes);
        if (!body) return std::nullopt;
        return ParseManifestBody(*body, identity.logical);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<PageDocument> ReadPage(Store& store, const Identity& identity, const std::string& accountScope,
                                     const std::string& guardScope, const Manifest& manifest,
                                     const PageRef& ref) {
    try {
        const auto snapshot = store.Lookup(
            PageChunkKey(identity, accountScope, guardScope, manifest.generation, ref.ordinal, ref.chunk));
        auto body = snapshot ? ReadSnapshot(*snapshot, kMaxPageBytes) : std::nullopt;
        if (!body) return std::nullopt;
        PageDocument document;
        document.ordinal = ref.ordinal;
        document.page = ParsePageBody(*body, ref.ordinal);
        document.body = std::move(*body);
        document.chunk = ref.chunk;
        if (manifest.rowKind != RowKindName(document.page.rows) ||
            RowCount(document.page.rows) != ref.rowCount || document.page.more != ref.more ||
            document.page.next.value != ref.nextValue || document.page.next.offset != ref.nextOffset ||
            document.page.enrichmentPending != ref.enrichmentPending || document.page.total != ref.total ||
            document.page.revision != ref.revision)
            return std::nullopt;
        return document;
    } catch (...) {
        return std::nullopt;
    }
}

void ErasePage(Store& store, const Identity& identity, const std::string& accountScope,
               const std::string& guardScope, const std::string& generation, const PageRef& ref) {
    try {
        store.Erase(PageChunkKey(identity, accountScope, guardScope, generation, ref.ordinal, ref.chunk));
    } catch (...) {
        // The manifest no longer references this immutable replacement.
    }
}

void AppendRows(stream::ListRows& target, stream::ListRows&& source) {
    if (target.index() != source.index()) Invalid("A cached list changed its row schema.");
    std::visit(
        [&source](auto& destination) {
            using Rows = std::decay_t<decltype(destination)>;
            auto* incoming = std::get_if<Rows>(&source);
            if (!incoming) Invalid("A cached list changed its row schema.");
            destination.insert(destination.end(), std::make_move_iterator(incoming->begin()),
                               std::make_move_iterator(incoming->end()));
        },
        target);
}

template <typename Value> void MergeOptional(std::optional<Value>& target, std::optional<Value>&& source) {
    if (!source) return;
    if (!target) {
        target = std::move(source);
    } else if (!(target->id == source->id)) {
        Invalid("A cached list has conflicting repeated metadata.");
    }
}

std::optional<Record> AssembleRecord(Store& store, const Identity& identity, const std::string& accountScope,
                                     const std::string& guardScope) {
    const auto manifest = ReadManifest(store, identity, accountScope, guardScope);
    if (!manifest) return std::nullopt;
    std::optional<stream::ListPage> combined;
    std::size_t rows = 0;
    std::size_t metadataReadyRows = 0;
    bool metadataContiguous = true;
    for (const auto& ref : manifest->pages) {
        auto page = ReadPage(store, identity, accountScope, guardScope, *manifest, ref);
        if (!page) return std::nullopt;
        const bool enrichmentPending = page->page.enrichmentPending;
        if (!combined) {
            combined = std::move(page->page);
        } else {
            AppendRows(combined->rows, std::move(page->page.rows));
            MergeOptional(combined->playlist, std::move(page->page.playlist));
            MergeOptional(combined->album, std::move(page->page.album));
            MergeOptional(combined->artist, std::move(page->page.artist));
            combined->enrichmentPending = combined->enrichmentPending || page->page.enrichmentPending;
        }
        rows += ref.rowCount;
        if (metadataContiguous) {
            if (enrichmentPending)
                metadataContiguous = false;
            else
                metadataReadyRows += ref.rowCount;
        }
    }
    if (!combined || rows != manifest->rowCount) return std::nullopt;
    combined->next.value = manifest->nextValue;
    combined->next.offset = manifest->nextOffset;
    combined->more = !manifest->complete;
    combined->total = manifest->total;
    combined->revision = manifest->revision;
    return Record{std::move(*combined), manifest->pages.size(), manifest->validatedAt, metadataReadyRows,
                  manifest->generation};
}

bool SameRowIdentities(const stream::ListRows& left, const stream::ListRows& right) {
    if (left.index() != right.index()) return false;
    return std::visit(
        [&right](const auto& leftRows) {
            using Rows = std::decay_t<decltype(leftRows)>;
            const auto* rightRows = std::get_if<Rows>(&right);
            if (!rightRows || leftRows.size() != rightRows->size()) return false;
            for (std::size_t i = 0; i < leftRows.size(); ++i)
                if constexpr (std::is_same_v<Rows, std::vector<stream::CatalogSearchItem>>) {
                    if (stream::CatalogSearchItemKey(leftRows[i]) !=
                        stream::CatalogSearchItemKey((*rightRows)[i]))
                        return false;
                } else {
                    if (leftRows[i].id != (*rightRows)[i].id) return false;
                }
            return true;
        },
        left);
}

template <typename Value>
bool SameOptionalIdentity(const std::optional<Value>& left, const std::optional<Value>& right) {
    if (left.has_value() != right.has_value()) return false;
    return !left || left->id == right->id;
}

bool SameLockedHeaders(const stream::ListPage& left, const stream::ListPage& right) {
    return left.next == right.next && left.more == right.more && left.total == right.total &&
           left.revision == right.revision;
}

bool IsValidPageUpdate(const stream::ListPage& current, const stream::ListPage& update) {
    if (RowKindName(current.rows) != RowKindName(update.rows) ||
        RowCount(current.rows) != RowCount(update.rows) || !SameRowIdentities(current.rows, update.rows) ||
        !SameOptionalIdentity(current.playlist, update.playlist) ||
        !SameOptionalIdentity(current.album, update.album) ||
        !SameOptionalIdentity(current.artist, update.artist) || !SameLockedHeaders(current, update))
        return false;
    return current.enrichmentPending || !update.enrichmentPending;
}

PageRef RefFromSerialized(const SerializedPage& page) {
    PageRef ref;
    ref.ordinal = page.ordinal;
    ref.rowCount = page.rowCount;
    ref.more = page.more;
    ref.enrichmentPending = page.enrichmentPending;
    ref.nextValue = page.next.value;
    ref.nextOffset = page.next.offset;
    ref.revision = page.revision;
    ref.total = page.total;
    ref.chunk = page.chunk;
    return ref;
}

bool SamePageClosure(const PageRef& ref, const SerializedPage& page) {
    return ref.ordinal == page.ordinal && ref.rowCount == page.rowCount && ref.more == page.more &&
           ref.enrichmentPending == page.enrichmentPending && ref.nextValue == page.next.value &&
           ref.nextOffset == page.next.offset && ref.revision == page.revision && ref.total == page.total;
}

std::mutex g_diagnosticsMutex;
std::function<void(std::string_view)> g_diagnostics;

// A page that is not persisted costs the user a full reload after the next start, so the
// reason is always recorded.
void Report(std::string_view reason) {
    std::function<void(std::string_view)> sink;
    {
        std::lock_guard lock(g_diagnosticsMutex);
        sink = g_diagnostics;
    }
    if (sink) sink(reason);
}

bool SavePageDocument(Store& store, const Identity& identity, const std::string& accountScope,
                      const std::string& guardScope, uint64_t generationInput, const std::string& generation,
                      uint64_t saveTicket, const Record& record, const std::function<bool()>& stillCurrent,
                      std::mutex* publicationGate = nullptr) {
    if (record.pages == 0) return false;
    SerializedPage next;
    try {
        next = SerializePage(record.pages, record.page);
    } catch (const std::exception& error) {
        Report(std::string("page ") + std::to_string(record.pages) + " cannot be serialized: " + error.what());
        return false;
    } catch (...) {
        Report("page cannot be serialized");
        return false;
    }
    auto existing = ReadManifest(store, identity, accountScope, guardScope);
    // A page that continues the snapshot its caller loaded is written into that snapshot's
    // generation; anything else written meanwhile replaced it, and the page is refused below.
    const bool continues = existing && !record.base.empty() && existing->generation == record.base;
    if (!record.base.empty() && !continues) {
        Report("page continues a snapshot that was replaced");
        return false;
    }
    const std::string token = continues ? existing->generation : generation; // copied: *existing moves
    if (existing && !continues) {
        const auto incomingEpoch = generation.substr(0, generation.find(':'));
        const auto existingEpoch = existing->generation.substr(0, existing->generation.find(':'));
        if (incomingEpoch == existingEpoch && generationInput < existing->generationInput) {
            Report("page belongs to an older generation than the persisted one");
            return false;
        }
    }
    const bool sameGeneration = existing && existing->generation == token;
    bool replaceCompletedPage = false;
    if (sameGeneration) {
        if (next.ordinal == existing->pages.size()) {
            const auto& final = existing->pages.back();
            if (existing->rowKind != next.rowKind || !SamePageClosure(final, next)) return false;
            if (!continues && saveTicket < existing->saveTicket) return false;
            const auto chunk = store.Lookup(
                PageChunkKey(identity, accountScope, guardScope, token, final.ordinal, final.chunk));
            if (chunk) {
                if (!stillCurrent()) return false;
                existing->saveTicket = saveTicket;
                existing->validatedAt = record.validatedAt ? record.validatedAt : Store::Now();
                try {
                    const auto manifest = SerializeManifest(*existing);
                    if (!stillCurrent()) return false;
                    RunTestManifestPause();
                    return WriteSnapshot(store, ManifestKey(identity, accountScope, guardScope), manifest,
                                         kManifestContentType, existing->validatedAt, "schema=3;reused=1",
                                         publicationGate, &stillCurrent);
                } catch (...) {
                    return false;
                }
            }
            replaceCompletedPage = true;
        }
        if (!replaceCompletedPage) {
            try {
                ValidateContinuity(*existing, next);
            } catch (const std::exception& error) {
                Report(std::string("page ") + std::to_string(next.ordinal) + " does not continue the snapshot: " +
                       error.what());
                return false;
            } catch (...) {
                return false;
            }
        }
    } else {
        if (next.ordinal != 1) {
            Report("page " + std::to_string(next.ordinal) + " has no persisted predecessor in its generation");
            return false;
        }
    }

    const auto pageKey = PageChunkKey(identity, accountScope, guardScope, token, next.ordinal, next.chunk);
    if (!stillCurrent()) return false;
    if (!WriteSnapshot(store, pageKey, next.body, kPageContentType,
                       record.validatedAt ? record.validatedAt : Store::Now(),
                       "schema=3;generation=" + token + ";index=" + std::to_string(next.ordinal) +
                           ";chunk=" + next.chunk)) {
        Report("page " + std::to_string(next.ordinal) + " could not be written");
        return false;
    }
    const PageRef nextRef = RefFromSerialized(next);

    Manifest manifest;
    if (sameGeneration) {
        manifest = std::move(*existing);
    } else {
        manifest.logical = identity.logical;
        manifest.generation = generation;
        manifest.generationInput = generationInput;
        manifest.rowKind = next.rowKind;
    }
    manifest.saveTicket = saveTicket;
    manifest.validatedAt = record.validatedAt ? record.validatedAt : Store::Now();
    manifest.complete = !next.more;
    manifest.rowCount += next.rowCount;
    manifest.nextValue = next.next.value;
    manifest.nextOffset = next.next.offset;
    manifest.revision = manifest.revision.empty() ? next.revision : manifest.revision;
    if (!manifest.total && next.total) manifest.total = next.total;
    if (replaceCompletedPage) {
        manifest.pages.back() = nextRef;
    } else {
        manifest.pages.push_back(nextRef);
    }
    std::string body;
    try {
        body = SerializeManifest(manifest);
    } catch (...) {
        ErasePage(store, identity, accountScope, guardScope, token, nextRef);
        return false;
    }
    if (!stillCurrent()) {
        ErasePage(store, identity, accountScope, guardScope, token, nextRef);
        return false;
    }
    RunTestManifestPause();
    const bool published = WriteSnapshot(store, ManifestKey(identity, accountScope, guardScope), body,
                                         kManifestContentType, manifest.validatedAt,
                                         "schema=3;generation=" + token, publicationGate, &stillCurrent);
    if (!published) {
        Report("manifest of page " + std::to_string(next.ordinal) + " could not be published");
        ErasePage(store, identity, accountScope, guardScope, token, nextRef);
        return false;
    }

    if (existing && !sameGeneration) {
        for (const auto& page : existing->pages)
            ErasePage(store, identity, accountScope, guardScope, existing->generation, page);
    }
    return true;
}

bool UpdatePageDocument(Store& store, const Identity& identity, const std::string& accountScope,
                        const std::string& guardScope, uint64_t generationInput,
                        const std::string& generation, uint64_t updateTicket, const Record& record,
                        const std::function<bool()>& stillCurrent, std::mutex* publicationGate = nullptr) {
    if (record.pages == 0) return false;
    SerializedPage replacement;
    try {
        replacement = SerializePage(record.pages, record.page);
    } catch (...) {
        return false;
    }
    auto existing = ReadManifest(store, identity, accountScope, guardScope);
    // A page the caller appended to a loaded snapshot lives in that snapshot's generation.
    const bool continues = !record.base.empty();
    const std::string token = continues ? record.base : generation;
    if (!existing || existing->generation != token ||
        (!continues && existing->generationInput != generationInput) || updateTicket < existing->saveTicket ||
        replacement.ordinal > existing->pages.size())
        return false;

    const PageRef previous = existing->pages[replacement.ordinal - 1];
    auto cached = ReadPage(store, identity, accountScope, guardScope, *existing, previous);
    if (!cached || !IsValidPageUpdate(cached->page, record.page)) return false;

    PageRef nextRef;
    nextRef = RefFromSerialized(replacement);
    const bool bodyChanged = cached->body != replacement.body;
    if (!bodyChanged) nextRef.chunk = previous.chunk;

    bool wroteReplacement = false;
    if (bodyChanged) {
        if (!stillCurrent()) return false;
        const auto key = PageChunkKey(identity, accountScope, guardScope, token, nextRef.ordinal, nextRef.chunk);
        if (!WriteSnapshot(store, key, replacement.body, kPageContentType,
                           record.validatedAt ? record.validatedAt : Store::Now(),
                           "schema=3;generation=" + token + ";index=" + std::to_string(nextRef.ordinal) +
                               ";chunk=" + nextRef.chunk))
            return false;
        wroteReplacement = true;
    }

    Manifest manifest = std::move(*existing);
    manifest.saveTicket = updateTicket;
    manifest.validatedAt = record.validatedAt ? record.validatedAt : Store::Now();
    manifest.pages[replacement.ordinal - 1] = nextRef;
    std::string body;
    try {
        body = SerializeManifest(manifest);
    } catch (...) {
        if (wroteReplacement) ErasePage(store, identity, accountScope, guardScope, token, nextRef);
        return false;
    }
    if (!stillCurrent()) {
        if (wroteReplacement) ErasePage(store, identity, accountScope, guardScope, token, nextRef);
        return false;
    }
    RunTestManifestPause();
    if (!WriteSnapshot(store, ManifestKey(identity, accountScope, guardScope), body, kManifestContentType,
                       manifest.validatedAt, "schema=3;generation=" + token, publicationGate, &stillCurrent)) {
        if (wroteReplacement) ErasePage(store, identity, accountScope, guardScope, token, nextRef);
        return false;
    }
    if (wroteReplacement) ErasePage(store, identity, accountScope, guardScope, token, previous);
    return true;
}

std::string InternalProvider(const std::string& provider) {
    return "lists:" + provider;
}

struct CapturedScope {
    std::string account;
    std::string guard;
};

bool CaptureScope(const Identity& identity, CapturedScope& scope) {
    try {
        if (AccountRevision(identity.provider) != identity.accountRevision) return false;
        scope.account = AccountScope(identity.provider);
        if (AccountRevision(identity.provider) != identity.accountRevision) return false;
        scope.guard = GetStore().Scope(InternalProvider(identity.provider));
        return AccountRevision(identity.provider) == identity.accountRevision;
    } catch (...) {
        return false;
    }
}

class WorkerPool {
  public:
    explicit WorkerPool(int count) {
        for (int i = 0; i < count; ++i)
            m_threads.emplace_back([this] { Worker(); });
    }
    ~WorkerPool() { Stop(); }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // urgent: a read someone is waiting to see; it overtakes queued publications of other
    // lists (its own chain still runs in order).
    bool Post(std::string chain, std::function<void()> task, bool drainOnStop = false, bool urgent = false) {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping || !task) return false;
            if (urgent) {
                // Ahead of other lists' work, but after everything queued for its own chain:
                // a read must see the pages that were saved before it was asked for.
                auto at = m_tasks.begin();
                if (!chain.empty())
                    for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it)
                        if (it->chain == chain) at = std::next(it);
                m_tasks.insert(at, Task{std::move(chain), std::move(task), drainOnStop});
            } else {
                m_tasks.push_back(Task{std::move(chain), std::move(task), drainOnStop});
            }
        }
        m_ready.notify_one();
        return true;
    }
    void Stop() {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) return;
            m_stopping = true;
            std::erase_if(m_tasks, [](const Task& task) { return !task.drainOnStop; });
        }
        m_ready.notify_all();
        {
            // Persisted list pages are small and bounded. Give already queued publications a
            // short chance to finish so closing the player does not discard the prefix that was
            // just visible. Existing epoch/account/guard checks still reject stale writes.
            std::unique_lock lock(m_mutex);
            constexpr auto kPersistenceDrain = std::chrono::seconds(2);
            m_ready.wait_until(lock, std::chrono::steady_clock::now() + kPersistenceDrain,
                               [this] { return m_tasks.empty() && m_busy.empty(); });
            m_tasks.clear();
        }
        m_ready.notify_all();
        for (auto& thread : m_threads)
            if (thread.joinable()) thread.join();
        m_threads.clear();
    }

  private:
    struct Task {
        std::string chain;
        std::function<void()> work;
        bool drainOnStop = false;
    };

    void Worker() {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
                if (m_tasks.empty()) {
                    if (m_stopping) return;
                    continue;
                }
                auto found = std::find_if(m_tasks.begin(), m_tasks.end(), [this](const Task& pending) {
                    if (m_stopping && !pending.drainOnStop) return false;
                    return pending.chain.empty() || !m_busy.contains(pending.chain);
                });
                if (found == m_tasks.end()) {
                    if (m_stopping) return;
                    m_ready.wait(lock);
                    continue;
                }
                if (!found->chain.empty()) m_busy.insert(found->chain);
                task = std::move(*found);
                m_tasks.erase(found);
            }
            try {
                task.work();
            } catch (...) { /* Cache failures are misses, never process failures. */
            }
            {
                std::lock_guard lock(m_mutex);
                if (!task.chain.empty()) m_busy.erase(task.chain);
            }
            m_ready.notify_all();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Task> m_tasks;
    std::set<std::string> m_busy;
    std::vector<std::thread> m_threads;
    bool m_stopping = false;
};

class Manager final : public std::enable_shared_from_this<Manager> {
  private:
    struct LogicalState {
        uint64_t generation = 0;
        uint64_t lastUsed = 0;
        bool hasGeneration = false;
    };

  public:
    Manager() : m_runEpoch(RandomToken()), m_pool(2) {}
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    void SavePage(Identity identity, Record record, uint64_t generation, Persisted persisted = {}) {
        const std::string logicalKey = StateKey(identity);
        uint64_t ticket = 0;
        uint64_t epoch = 0;
        uint64_t guard = 0;
        bool rejected = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                rejected = true;
            } else {
                auto& state = GenerationState(logicalKey);
                if (state.hasGeneration && generation < state.generation) {
                    rejected = true;
                } else {
                    state.hasGeneration = true;
                    state.generation = generation;
                    ticket = ++m_nextTicket;
                    epoch = PublisherEpoch(identity.provider);
                    guard = GuardRevision(identity.provider);
                }
            }
        }
        if (rejected) {
            CompletePersisted(std::move(persisted), false);
            return;
        }
        const std::string chain = ChainKey(identity, epoch, guard);
        auto self = shared_from_this();
        auto completion = std::make_shared<Persisted>(std::move(persisted));
        const bool queued =
            m_pool.Post(std::move(chain), [self, completion, identity = std::move(identity),
                                          record = std::move(record), generation, ticket, epoch,
                                          guard]() mutable {
                bool success = false;
                try {
                    success =
                        self->RunSave(std::move(identity), std::move(record), generation, ticket, epoch, guard);
                } catch (...) {
                }
                self->CompletePersisted(std::move(*completion), success);
            }, true);
        if (!queued) CompletePersisted(std::move(*completion), false);
    }

    void UpdatePage(Identity identity, Record record, uint64_t generation, Persisted persisted = {}) {
        const std::string logicalKey = StateKey(identity);
        uint64_t ticket = 0;
        uint64_t epoch = 0;
        uint64_t guard = 0;
        bool rejected = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                rejected = true;
            } else {
                const auto found = m_states.find(logicalKey);
                if (found != m_states.end()) {
                    found->second.lastUsed = ++m_stateClock;
                    if (found->second.hasGeneration && found->second.generation != generation)
                        rejected = true;
                }
                if (!rejected) {
                    ticket = ++m_nextTicket;
                    epoch = PublisherEpoch(identity.provider);
                    guard = GuardRevision(identity.provider);
                }
            }
        }
        if (rejected) {
            CompletePersisted(std::move(persisted), false);
            return;
        }
        const std::string chain = ChainKey(identity, epoch, guard);
        auto self = shared_from_this();
        auto completion = std::make_shared<Persisted>(std::move(persisted));
        const bool queued =
            m_pool.Post(std::move(chain), [self, completion, identity = std::move(identity),
                                          record = std::move(record), generation, ticket, epoch,
                                          guard]() mutable {
                bool success = false;
                try {
                    success = self->RunUpdate(std::move(identity), std::move(record), generation, ticket,
                                              epoch, guard);
                } catch (...) {
                }
                self->CompletePersisted(std::move(*completion), success);
            }, true);
        if (!queued) CompletePersisted(std::move(*completion), false);
    }

    void Load(Identity identity, Loaded done) {
        if (!done) return;
        uint64_t epoch = 0;
        uint64_t guard = 0;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) return;
            epoch = PublisherEpoch(identity.provider);
            guard = GuardRevision(identity.provider);
        }
        const std::string chain = ChainKey(identity, epoch, guard);
        auto self = shared_from_this();
        m_pool.Post(std::move(chain),
                    [self, identity = std::move(identity), done = std::move(done), epoch, guard]() mutable {
                        self->RunLoad(std::move(identity), std::move(done), epoch, guard);
                    },
                    false, true);
    }

    void InvalidateProvider(const std::string& provider) {
        uint64_t guard = 0;
        {
            std::lock_guard publication(m_publicationGate);
            std::lock_guard lock(m_mutex);
            if (m_stopping) return;
            ++m_publisherEpochs[provider];
            guard = ++m_guardRevisions[provider];
            m_pendingGuardRotations[provider] = guard;
            ClearGenerationStatesLocked(provider);
        }
        auto self = shared_from_this();
        m_pool.Post({}, [self, provider, guard] { self->EnsureGuardRotation(provider, guard); }, true);
    }

    void RetireProvider(const std::string& provider) {
        std::lock_guard publication(m_publicationGate);
        std::lock_guard lock(m_mutex);
        if (m_stopping) return;
        ++m_publisherEpochs[provider];
        ClearGenerationStatesLocked(provider);
    }

    void Shutdown() {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) return;
            m_stopping = true;
        }
        m_pool.Stop();
        std::vector<std::pair<std::string, uint64_t>> pending;
        {
            std::lock_guard lock(m_mutex);
            pending.assign(m_pendingGuardRotations.begin(), m_pendingGuardRotations.end());
        }
        for (const auto& [provider, target] : pending)
            (void)EnsureGuardRotation(provider, target);
    }

#ifdef GLASSWAVE_LISTS_TESTING
    void FillGenerationStatesForTest(std::size_t count) {
        std::lock_guard lock(m_mutex);
        for (std::size_t i = 0; i < count; ++i)
            (void)GenerationState("test-state/" + std::to_string(i));
    }
    std::size_t GenerationStateCountForTest() {
        std::lock_guard lock(m_mutex);
        return m_states.size();
    }
#endif

  private:
    static std::string StateKey(const Identity& identity) {
        return identity.provider + '\n' + std::to_string(identity.accountRevision) + '\n' + identity.logical;
    }
    static std::string ChainKey(const Identity& identity, uint64_t epoch, uint64_t guard) {
        return StateKey(identity) + '\n' + std::to_string(epoch) + '\n' + std::to_string(guard);
    }
    LogicalState& GenerationState(const std::string& key) {
        const auto found = m_states.find(key);
        if (found != m_states.end()) {
            found->second.lastUsed = ++m_stateClock;
            return found->second;
        }
        if (m_states.size() >= kMaxGenerationStates) {
            const auto oldest =
                std::min_element(m_states.begin(), m_states.end(), [](const auto& left, const auto& right) {
                    return left.second.lastUsed < right.second.lastUsed;
                });
            if (oldest != m_states.end()) m_states.erase(oldest);
        }
        auto [inserted, unused] = m_states.emplace(key, LogicalState{.lastUsed = ++m_stateClock});
        (void)unused;
        return inserted->second;
    }
    void ClearGenerationStatesLocked(const std::string& provider) {
        const std::string prefix = provider + '\n';
        for (auto it = m_states.begin(); it != m_states.end();) {
            if (it->first.starts_with(prefix))
                it = m_states.erase(it);
            else
                ++it;
        }
    }
    uint64_t PublisherEpoch(const std::string& provider) const {
        const auto found = m_publisherEpochs.find(provider);
        return found == m_publisherEpochs.end() ? 0 : found->second;
    }
    uint64_t GuardRevision(const std::string& provider) const {
        const auto found = m_guardRevisions.find(provider);
        return found == m_guardRevisions.end() ? 0 : found->second;
    }
    bool CurrentEpoch(const std::string& provider, uint64_t expected) {
        std::lock_guard lock(m_mutex);
        return !m_stopping && PublisherEpoch(provider) == expected;
    }
    bool EnsureGuardRotation(const std::string& provider, uint64_t requiredGuard) {
        uint64_t target = 0;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_pendingGuardRotations.find(provider);
            if (found == m_pendingGuardRotations.end()) return true;
            if (requiredGuard < found->second) return false;
            target = found->second;
        }
        std::lock_guard rotation(m_rotationMutex);
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_pendingGuardRotations.find(provider);
            if (found == m_pendingGuardRotations.end()) return true;
            target = std::max(target, found->second);
        }
        try {
            GetStore().RotateScope(InternalProvider(provider));
        } catch (...) {
            return false;
        }
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_pendingGuardRotations.find(provider);
            if (found != m_pendingGuardRotations.end() && found->second <= target)
                m_pendingGuardRotations.erase(found);
        }
        return true;
    }
    bool CurrentAccount(const Identity& identity) {
        std::lock_guard lock(m_mutex);
        return !m_stopping && AccountRevision(identity.provider) == identity.accountRevision;
    }
    std::string GenerationToken(uint64_t epoch, uint64_t generation) const {
        return m_runEpoch + '-' + std::to_string(epoch) + ':' + std::to_string(generation);
    }
    void Deliver(std::function<void()> callback) {
        if (!callback) return;
#ifdef GLASSWAVE_LISTS_STANDALONE
        callback();
#else
        auto self = shared_from_this();
        try {
            fb2k::inMainThread([self, callback = std::move(callback)]() mutable {
                {
                    std::lock_guard lock(self->m_mutex);
                    if (self->m_stopping) return;
                }
                callback();
            });
        } catch (...) { /* Shutdown may make the SDK queue unavailable. */
        }
#endif
    }
    void CompletePersisted(Persisted persisted, bool success) {
        if (!persisted) return;
        Deliver([persisted = std::move(persisted), success]() mutable { persisted(success); });
    }
    bool RunSave(Identity identity, Record record, uint64_t generation, uint64_t ticket, uint64_t epoch,
                 uint64_t guard) {
        if (!CurrentEpoch(identity.provider, epoch)) {
            Report("page publisher was retired before it ran");
            return false;
        }
        if (!EnsureGuardRotation(identity.provider, guard)) {
            Report("the list guard could not be rotated");
            return false;
        }
        CapturedScope scope;
        if (!CaptureScope(identity, scope)) {
            Report("the account scope is unavailable or changed");
            return false;
        }
        if (!CurrentEpoch(identity.provider, epoch) ||
            AccountRevision(identity.provider) != identity.accountRevision) {
            Report("page publisher was retired while it started");
            return false;
        }
        const std::string generationToken = GenerationToken(epoch, generation);
        const auto stillCurrent = [this, &identity, epoch, &scope] {
            if (!CurrentEpoch(identity.provider, epoch) ||
                AccountRevision(identity.provider) != identity.accountRevision)
                return false;
            try {
                return GetStore().Scope(InternalProvider(identity.provider)) == scope.guard;
            } catch (...) {
                return false;
            }
        };
        bool success = false;
        try {
            success =
                SavePageDocument(GetStore(), identity, scope.account, scope.guard, generation,
                                 generationToken, ticket, record, stillCurrent, &m_publicationGate);
        } catch (...) {
        }
        RunTestManifestComplete();
        return success;
    }
    bool RunUpdate(Identity identity, Record record, uint64_t generation, uint64_t ticket, uint64_t epoch,
                   uint64_t guard) {
        if (!CurrentEpoch(identity.provider, epoch)) return false;
        if (!EnsureGuardRotation(identity.provider, guard)) return false;
        CapturedScope scope;
        if (!CaptureScope(identity, scope)) return false;
        if (!CurrentEpoch(identity.provider, epoch) ||
            AccountRevision(identity.provider) != identity.accountRevision)
            return false;
        const std::string generationToken = GenerationToken(epoch, generation);
        const auto stillCurrent = [this, &identity, epoch, &scope] {
            if (!CurrentEpoch(identity.provider, epoch) ||
                AccountRevision(identity.provider) != identity.accountRevision)
                return false;
            try {
                return GetStore().Scope(InternalProvider(identity.provider)) == scope.guard;
            } catch (...) {
                return false;
            }
        };
        bool success = false;
        try {
            success =
                UpdatePageDocument(GetStore(), identity, scope.account, scope.guard, generation,
                                   generationToken, ticket, record, stillCurrent, &m_publicationGate);
        } catch (...) {
        }
        RunTestManifestComplete();
        return success;
    }
    void RunLoad(Identity identity, Loaded done, uint64_t epoch, uint64_t guard) {
        std::optional<Record> record;
        if (CurrentEpoch(identity.provider, epoch) && EnsureGuardRotation(identity.provider, guard)) {
            CapturedScope scope;
            if (CaptureScope(identity, scope)) {
                try {
                    record = AssembleRecord(GetStore(), identity, scope.account, scope.guard);
                } catch (...) {
                    record.reset();
                }
                if (record) {
                    try {
                        if (GetStore().Scope(InternalProvider(identity.provider)) != scope.guard) return;
                    } catch (...) {
                        return;
                    }
                }
            }
        }
        if (!CurrentEpoch(identity.provider, epoch) || !CurrentAccount(identity)) return;
        auto self = shared_from_this();
        Deliver([self, identity = std::move(identity), epoch, record = std::move(record),
                 done = std::move(done)]() mutable {
            if (!self->CurrentEpoch(identity.provider, epoch) || !self->CurrentAccount(identity)) return;
            done(std::move(record));
        });
    }

    std::string m_runEpoch;
    WorkerPool m_pool;
    std::mutex m_publicationGate;
    std::mutex m_mutex;
    std::mutex m_rotationMutex;
    bool m_stopping = false;
    uint64_t m_nextTicket = 0;
    uint64_t m_stateClock = 0;
    std::map<std::string, uint64_t> m_publisherEpochs;
    std::map<std::string, uint64_t> m_guardRevisions;
    std::map<std::string, uint64_t> m_pendingGuardRotations;
    std::map<std::string, LogicalState> m_states;
};

struct Lifetime {
    ~Lifetime() {
        if (manager) manager->Shutdown();
    }
    std::mutex mutex;
    bool stopped = false;
    std::shared_ptr<Manager> manager;
};

Lifetime& Life() {
    static Lifetime lifetime;
    return lifetime;
}
std::shared_ptr<Manager> Instance() {
    auto& life = Life();
    std::lock_guard lock(life.mutex);
    if (life.stopped) return {};
    if (!life.manager) life.manager = std::make_shared<Manager>();
    return life.manager;
}

} // namespace

void Load(Identity identity, Loaded done) {
    if (!done || !ValidIdentity(identity)) return;
    if (auto manager = Instance()) manager->Load(std::move(identity), std::move(done));
}

void SavePage(Identity identity, Record record, uint64_t generation, Persisted persisted) {
    if (!ValidIdentity(identity)) {
        if (persisted) persisted(false);
        return;
    }
    if (auto manager = Instance())
        manager->SavePage(std::move(identity), std::move(record), generation, std::move(persisted));
    else if (persisted)
        persisted(false);
}

void UpdatePage(Identity identity, Record record, uint64_t generation, Persisted persisted) {
    if (!ValidIdentity(identity)) {
        if (persisted) persisted(false);
        return;
    }
    if (auto manager = Instance())
        manager->UpdatePage(std::move(identity), std::move(record), generation, std::move(persisted));
    else if (persisted)
        persisted(false);
}

void RetireProvider(std::string provider) {
    if (provider.empty() || provider.size() > kMaxIdentityBytes) return;
    if (auto manager = Instance()) manager->RetireProvider(provider);
}

void InvalidateProvider(std::string provider) {
    if (provider.empty() || provider.size() > kMaxIdentityBytes) return;
    if (auto manager = Instance()) manager->InvalidateProvider(provider);
}

void SetDiagnostics(std::function<void(std::string_view)> sink) {
    std::lock_guard lock(g_diagnosticsMutex);
    g_diagnostics = std::move(sink);
}

void Shutdown() {
    auto& life = Life();
    std::shared_ptr<Manager> manager;
    {
        std::lock_guard lock(life.mutex);
        if (life.stopped) return;
        life.stopped = true;
        manager = life.manager;
    }
    if (manager) manager->Shutdown();
}

#ifdef GLASSWAVE_LISTS_TESTING
namespace detail {

void SetManifestPauseForTest(std::function<void()> pause) {
    std::lock_guard lock(testManifestPauseMutex);
    testManifestPause = std::move(pause);
}
void SetManifestCommitPauseForTest(std::function<void()> pause) {
    std::lock_guard lock(testManifestPauseMutex);
    testManifestCommitPause = std::move(pause);
}
void SetManifestCompleteForTest(std::function<void()> complete) {
    std::lock_guard lock(testManifestPauseMutex);
    testManifestComplete = std::move(complete);
}
std::string SerializePageForTest(std::size_t ordinal, const stream::ListPage& page) {
    return SerializePage(ordinal, page).body;
}
stream::ListPage ParsePageForTest(std::string_view body, std::size_t ordinal) {
    return ParsePageBody(body, ordinal);
}
std::string SerializeManifestForTest(const Manifest& manifest) {
    return SerializeManifest(manifest);
}
Manifest ParseManifestForTest(std::string_view body, std::string_view logical) {
    return ParseManifestBody(body, logical);
}
void ValidateContinuityForTest(const Manifest& current, std::size_t ordinal, const stream::ListPage& page) {
    ValidateContinuity(current, SerializePage(ordinal, page));
}

} // namespace detail
#endif

} // namespace glasswave::cache::lists
