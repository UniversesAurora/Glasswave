#include "pch.h"
#include "applemusic/stream_provider.h"
#include "applemusic/artist_preference.h"
#include "applemusic/atmos_renderer.h"
#include "applemusic/config.h"
#include "applemusic/display_metadata.h"
#include "applemusic/handle.h"
#include "applemusic/lyrics_protocol.h"
#include "applemusic/login.h"
#include "cache/provider_snapshot.h"
#include "cache/store.h"
#include "netease/json.h"
#include "netease/log.h"
#include "ui/i18n.h"

#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <future>
#include <limits>
#include <type_traits>

namespace glasswave::applemusic {

namespace {

namespace json = netease::json;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_utf8_from_wide(text.data(), text.size()).get_ptr();
}

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    const std::string copy(text);
    return pfc::stringcvt::string_wide_from_utf8(copy.c_str()).get_ptr();
}

void AppendListContextPart(std::string& key, std::string_view value) {
    key += std::to_string(value.size());
    key.push_back(':');
    key.append(value);
}

std::string AppleListCacheContext() {
    const AppleMusicDisplayRegion region = GetAppleMusicDisplayRegion();
    const auto target = DisplayTargetFor(region);
    std::string key = "apple-list-v1;";
    AppendListContextPart(key, Utf8(DisplayMetadata::Get().SourceStorefront()));
    AppendListContextPart(key, std::to_string(static_cast<int>(region)));
    AppendListContextPart(key, target ? Utf8(target->storefront) : std::string());
    AppendListContextPart(key, target ? Utf8(target->locale) : std::string());
    return key;
}

std::wstring TextAttribute(const Object& attributes, const wchar_t* name) {
    if (const auto value = json::GetObject(attributes, name)) return json::GetString(*value, L"standard");
    return json::GetString(attributes, name);
}

constexpr wchar_t kFavoritesSection[] = L"favorites";
constexpr wchar_t kSongsSection[] = L"library-songs";
constexpr wchar_t kArtistsSection[] = L"library-artists";
constexpr wchar_t kAlbumsSection[] = L"library-albums";
constexpr wchar_t kGenresSection[] = L"genres";
constexpr wchar_t kComposersSection[] = L"composers";
constexpr wchar_t kCreatedSection[] = L"my-playlists";
constexpr wchar_t kSavedSection[] = L"saved-playlists";
constexpr wchar_t kGenreGroupPrefix[] = L"genre:";
constexpr wchar_t kComposerGroupPrefix[] = L"composer:";
constexpr auto kLibrarySnapshotTtl = std::chrono::minutes(5);
constexpr size_t kProviderCollectionPageLimit = 256;
constexpr size_t kProviderCollectionItemLimit = 10'000;
constexpr size_t kSongAlbumHydrationBatch = 20;
constexpr size_t kSongAlbumHydrationConcurrency = 4;
constexpr size_t kDisplayMetadataBatch = 25;
constexpr size_t kCatalogSearchApiLimit = 25;

bool IsArtistAlbumView(std::wstring_view id) {
    return id == L"featured-albums" || id == L"full-albums" || id == L"singles" || id == L"live-albums" ||
           id == L"appears-on-albums" || id == L"compilation-albums";
}

bool IsArtistPlaylistView(std::wstring_view id) {
    return id == L"featured-playlists";
}

bool ListCancelled(const std::shared_ptr<std::atomic_bool>& cancelled) {
    return cancelled && cancelled->load(std::memory_order_acquire);
}

// The service answered that this request cannot succeed as asked (no such resource, or a
// request it rejects as malformed). Asking again gives the same answer, so the caller records
// the absence instead of repeating the request for every page that shows the item.
bool DefiniteNotFound(const std::exception& exception) {
    const std::string_view message(exception.what());
    return message.find("HTTP 404") != std::string_view::npos ||
           message.find("HTTP 400") != std::string_view::npos;
}

bool EnrichableList(stream::ListKind kind) {
    switch (kind) {
    case stream::ListKind::PlaylistTracks:
    case stream::ListKind::Favorites:
    case stream::ListKind::AlbumTracks:
    case stream::ListKind::SectionTracks:
    case stream::ListKind::ArtistTopTracks:
    case stream::ListKind::CatalogSearch:
    case stream::ListKind::ArtistAlbums:
    case stream::ListKind::ArtistPlaylists:
    case stream::ListKind::SavedAlbums:
    case stream::ListKind::FollowedArtists:
    case stream::ListKind::Playlists:
    case stream::ListKind::Groups:
    case stream::ListKind::CatalogSearchArtists:
    case stream::ListKind::CatalogSearchAlbums:
    case stream::ListKind::CatalogSearchPlaylists:
    case stream::ListKind::CatalogSearchTopResults:
        return true;
    default:
        return false;
    }
}

// Combines the provider lifetime abort with a list lease. A polling watcher bridges the
// lease's atomic token to WinHTTP's event-based cancellation path.
class ListHydrationAbort final : public abort_callback {
  public:
    ListHydrationAbort(abort_callback& providerAbort, std::shared_ptr<std::atomic_bool> cancelled)
        : providerAbort_(providerAbort), cancelled_(std::move(cancelled)) {
        if (IsAborting()) {
            signal_.abort();
        } else {
            watcher_ = std::thread([this] {
                while (!stopping_.load(std::memory_order_acquire)) {
                    if (IsAborting() || !providerAbort_.sleep_ex(0.025)) {
                        signal_.abort();
                        return;
                    }
                }
            });
        }
    }
    ~ListHydrationAbort() {
        stopping_.store(true, std::memory_order_release);
        if (watcher_.joinable()) watcher_.join();
    }

    bool is_aborting() const override { return IsAborting(); }
    abort_callback_event get_abort_event() const override { return signal_.get_abort_event(); }

  private:
    bool IsAborting() const {
        return providerAbort_.is_aborting() || ListCancelled(cancelled_) || signal_.is_aborting();
    }

    abort_callback& providerAbort_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    abort_callback_impl signal_;
    std::atomic_bool stopping_ = false;
    std::thread watcher_;
};

struct PlaylistLoadMetrics {
    long long primaryMs = 0;
    long long pagesMs = 0;
    long long hydrationMs = 0;
    size_t fallbackBatches = 0;
};

long long ElapsedMs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

std::wstring CurrentLocale() {
    return ui::UsingChinese() ? L"zh-Hans-CN" : L"en-US";
}

std::string SnapshotContext(AppleMusicDisplayRegion region, std::wstring_view locale) {
    const std::string regionKey = std::to_string(static_cast<int>(region));
    const std::string localeKey = Utf8(locale);
    const std::array<std::string_view, 3> parts{"apple-bootstrap-v1", regionKey, localeKey};
    return cache::provider_snapshot::Context(parts);
}

bool EqualAscii(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t left = a[i], right = b[i];
        if (left >= L'A' && left <= L'Z') left = static_cast<wchar_t>(left + L'a' - L'A');
        if (right >= L'A' && right <= L'Z') right = static_cast<wchar_t>(right + L'a' - L'A');
        if (left != right) return false;
    }
    return true;
}

bool SameAccountState(const AccountState& left, const AccountState& right) {
    if (left.signedIn != right.signedIn) return false;
    if (!left.signedIn) return true;
    return !left.accountId.empty() && left.accountId == right.accountId;
}

bool StartsWith(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::vector<Object> EmbeddedRelationship(const Object& item, const wchar_t* name);

bool IsFavoriteSystemPlaylist(const Object& item) {
    const auto attributes = json::GetObject(item, L"attributes");
    if (!attributes) return false;
    if (const auto tags = json::GetArray(*attributes, L"tags")) {
        for (uint32_t i = 0, count = tags->Size(); i < count; ++i) {
            const auto tag = tags->GetAt(i);
            if (tag && tag.ValueType() == winrt::Windows::Data::Json::JsonValueType::String &&
                EqualAscii(tag.GetString().c_str(), L"favorited")) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::wstring> StringArray(const Object& object, const wchar_t* key) {
    std::vector<std::wstring> out;
    if (const auto array = json::GetArray(object, key)) {
        for (uint32_t i = 0, count = array->Size(); i < count; ++i) {
            const auto value = array->GetAt(i);
            if (value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String) {
                std::wstring text = value.GetString().c_str();
                if (!text.empty()) out.push_back(std::move(text));
            }
        }
    }
    return out;
}

std::vector<std::wstring> ComposerNames(const Object& item, const Object& attributes) {
    std::vector<std::wstring> names;
    for (const auto& composer : EmbeddedRelationship(item, L"composers")) {
        std::wstring name;
        if (const auto composerAttributes = json::GetObject(composer, L"attributes"))
            name = json::GetString(*composerAttributes, L"name");
        if (name.empty()) name = json::GetString(composer, L"name");
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(std::move(name));
    }
    if (names.empty()) {
        const std::wstring fallback = json::GetString(attributes, L"composerName");
        if (!fallback.empty()) names.push_back(fallback);
    }
    return names;
}

std::optional<stream::Id> CatalogIdFrom(const Object& item) {
    for (const auto& catalog : EmbeddedRelationship(item, L"catalog")) {
        const std::wstring id = json::GetString(catalog, L"id");
        if (!id.empty()) return id;
    }
    return std::nullopt;
}

std::wstring AlbumIdFrom(const Object& item, bool preferLibrary) {
    std::wstring fallback;
    for (const auto& album : EmbeddedRelationship(item, L"albums")) {
        const std::wstring id = json::GetString(album, L"id");
        if (id.empty()) continue;
        if (fallback.empty()) fallback = id;
        const std::wstring type = json::GetString(album, L"type");
        if ((preferLibrary && type == L"library-albums") || (!preferLibrary && type == L"albums")) return id;
    }
    return fallback;
}

bool AttributeFlag(const Object& attributes, const wchar_t* name, bool fallback) {
    if (!attributes.HasKey(name)) return fallback;
    return json::GetBool(attributes, name, false);
}

std::wstring Artwork(const Object& item) {
    if (const auto attributes = json::GetObject(item, L"attributes")) {
        for (const wchar_t* name : {L"artwork", L"editorialArtwork"}) {
            if (const auto artwork = json::GetObject(*attributes, name)) {
                const std::wstring url = json::GetString(*artwork, L"url");
                if (!url.empty()) return url;
            }
        }
    }
    if (const auto relationships = json::GetObject(item, L"relationships")) {
        if (const auto catalog = json::GetObject(*relationships, L"catalog")) {
            if (const auto data = json::GetArray(*catalog, L"data")) {
                for (const auto& related : json::Objects(*data)) {
                    const std::wstring url = Artwork(related);
                    if (!url.empty()) return url;
                }
            }
        }
    }
    return {};
}

std::optional<Object> Relationship(const Object& item, const wchar_t* name) {
    const auto relationships = json::GetObject(item, L"relationships");
    return relationships ? json::GetObject(*relationships, name) : std::nullopt;
}

std::vector<Object> EmbeddedRelationship(const Object& item, const wchar_t* name) {
    const auto relationship = Relationship(item, name);
    if (!relationship) return {};
    const auto data = json::GetArray(*relationship, L"data");
    return data ? json::Objects(*data) : std::vector<Object>();
}

std::optional<Object> FirstData(const Object& response) {
    const auto data = json::GetArray(response, L"data");
    if (!data) return std::nullopt;
    const auto items = json::Objects(*data);
    return items.empty() ? std::nullopt : std::optional<Object>(items.front());
}

std::wstring AppendQuery(std::wstring path, std::wstring_view query) {
    if (query.empty()) return path;
    path += path.find(L'?') == std::wstring::npos ? L'?' : L'&';
    path += query;
    return path;
}

bool HasQueryParameter(std::wstring_view path, std::wstring_view name) {
    const size_t query = path.find(L'?');
    if (query == std::wstring_view::npos) return false;
    size_t start = query + 1;
    while (start <= path.size()) {
        const size_t end = path.find(L'&', start);
        const std::wstring_view parameter =
            path.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
        if (parameter.substr(0, parameter.find(L'=')) == name) return true;
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return false;
}

std::wstring EnsureQuery(std::wstring path, std::wstring_view query) {
    size_t start = 0;
    while (start < query.size()) {
        const size_t end = query.find(L'&', start);
        const std::wstring_view parameter =
            query.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
        const std::wstring_view name = parameter.substr(0, parameter.find(L'='));
        if (!name.empty() && !HasQueryParameter(path, name)) path = AppendQuery(std::move(path), parameter);
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return path;
}

std::wstring WithQueryValue(std::wstring path, std::wstring_view name, std::wstring_view value) {
    std::wstring out = path.substr(0, path.find(L'?'));
    const size_t query = path.find(L'?');
    if (query != std::wstring::npos) {
        size_t start = query + 1;
        while (start <= path.size()) {
            const size_t end = path.find(L'&', start);
            const std::wstring parameterText =
                path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            const std::wstring_view parameter(parameterText);
            const std::wstring_view parameterName = parameter.substr(0, parameter.find(L'='));
            if (!parameter.empty() && parameterName != name) {
                out += out.find(L'?') == std::wstring::npos ? L'?' : L'&';
                out += parameter;
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
    }
    if (!value.empty()) {
        out += out.find(L'?') == std::wstring::npos ? L'?' : L'&';
        out += name;
        out += L'=';
        out += value;
    }
    return out;
}

std::wstring RelationshipPagePath(std::wstring path, bool library, size_t limit = 100) {
    path = WithQueryValue(std::move(path), L"limit", std::to_wstring(std::clamp<size_t>(limit, 1, 100)));
    path = WithQueryValue(std::move(path), L"include", L"catalog,artists");
    path = WithQueryValue(std::move(path), library ? L"include[library-songs]" : L"include[songs]",
                          library ? L"catalog,artists,albums" : L"albums");
    if (library) path = WithQueryValue(std::move(path), L"include[songs]", L"albums");
    return path;
}

std::wstring ListPathOnly(std::wstring_view path) {
    return std::wstring(path.substr(0, path.find(L'?')));
}

std::wstring LowerListAscii(std::wstring_view text) {
    std::wstring out(text);
    for (auto& c : out)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + L'a' - L'A');
    return out;
}

bool ValidListApiPath(std::wstring_view path) {
    return path.size() > 4 && StartsWith(path, L"/v1/") &&
           std::none_of(path.begin(), path.end(), [](wchar_t c) { return c < 0x20 || c == 0x7f; }) &&
           path.find(L'#') == std::wstring_view::npos && path.find(L'\\') == std::wstring_view::npos;
}

std::wstring NormalizeListNext(std::wstring_view expectedPath, std::wstring_view next) {
    if (next.empty() || next.find(L'#') != std::wstring_view::npos ||
        next.find(L'\\') != std::wstring_view::npos ||
        std::any_of(next.begin(), next.end(), [](wchar_t c) { return c < 0x20 || c == 0x7f; })) {
        throw std::runtime_error("Apple Music returned an invalid list continuation.");
    }

    std::wstring path;
    if (next[0] == L'/') {
        path.assign(next);
    } else {
        const size_t scheme = next.find(L"://");
        const size_t pathStart =
            scheme == std::wstring_view::npos ? std::wstring_view::npos : next.find(L'/', scheme + 3);
        if (scheme == std::wstring_view::npos || pathStart == std::wstring_view::npos ||
            LowerListAscii(next.substr(0, scheme)) != L"https") {
            throw std::runtime_error("Apple Music returned a list continuation from another origin.");
        }
        const std::wstring host = LowerListAscii(next.substr(scheme + 3, pathStart - scheme - 3));
        if (host != L"amp-api.music.apple.com" && host != L"api.music.apple.com") {
            throw std::runtime_error("Apple Music returned a list continuation from another origin.");
        }
        path.assign(next.substr(pathStart));
    }
    if (!ValidListApiPath(path)) {
        throw std::runtime_error("Apple Music returned an invalid list continuation.");
    }
    if (!EqualAscii(ListPathOnly(path), ListPathOnly(expectedPath))) {
        throw std::runtime_error("Apple Music changed the API path during list pagination.");
    }
    return path;
}

uint64_t Fnv1a(std::string_view bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

void HashFingerprint(uint64_t& hash, std::wstring_view value) {
    const std::string bytes = Utf8(value);
    uint64_t size = bytes.size();
    for (size_t i = 0; i < sizeof(size); ++i) {
        hash ^= static_cast<unsigned char>(size & 0xff);
        hash *= 1099511628211ull;
        size >>= 8;
    }
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
}

std::wstring Hex64(uint64_t value) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring out(16, L'0');
    for (size_t i = out.size(); i != 0; --i) {
        out[i - 1] = digits[value & 0xf];
        value >>= 4;
    }
    return out;
}

// Cursors are saved with the list's rows and a list read in part goes on from them after a
// restart, so the fingerprint names only what the query is, never the provider's in-process
// generation: stale generations are dropped where work is posted, list sessions stop on a
// context change, and an account change rotates the cached lists away.
uint64_t ListQueryFingerprint(const stream::ListQuery& query, std::wstring_view storefront,
                              std::wstring_view locale) {
    uint64_t hash = 1469598103934665603ull;
    HashFingerprint(hash, std::to_wstring(static_cast<int>(query.kind)));
    HashFingerprint(hash, storefront);
    HashFingerprint(hash, locale);
    HashFingerprint(hash, query.id);
    HashFingerprint(hash, query.section);
    HashFingerprint(hash, std::to_wstring(static_cast<int>(query.shelf)));
    HashFingerprint(hash, query.search);
    HashFingerprint(hash, query.cache ? L"1" : L"0");
    if (query.cache) HashFingerprint(hash, std::to_wstring(static_cast<int>(*query.cache)));
    // query.cancelled is transport lifetime only and must not bind persisted cursors.
    return hash;
}

bool ParseUnsigned(std::wstring_view text, size_t& value) {
    if (text.empty()) return false;
    size_t parsed = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        const size_t digit = static_cast<size_t>(c - L'0');
        if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

bool ParseHex64(std::wstring_view text, uint64_t& value) {
    if (text.empty() || text.size() > 16) return false;
    uint64_t parsed = 0;
    for (const wchar_t c : text) {
        unsigned digit = 0;
        if (c >= L'0' && c <= L'9')
            digit = static_cast<unsigned>(c - L'0');
        else if (c >= L'a' && c <= L'f')
            digit = static_cast<unsigned>(c - L'a') + 10;
        else if (c >= L'A' && c <= L'F')
            digit = static_cast<unsigned>(c - L'A') + 10;
        else
            return false;
        parsed = (parsed << 4) | digit;
    }
    value = parsed;
    return true;
}

struct ListCursorState {
    size_t offset = 0;
    uint64_t fingerprint = 0;
    std::vector<uint64_t> history;
    std::wstring path;
};

bool DecodeListCursor(const stream::ListCursor& cursor, stream::ListKind kind, uint64_t expectedFingerprint,
                      ListCursorState& state, std::wstring& error) {
    if (cursor.value.empty()) {
        if (cursor.offset != 0) {
            error = L"The list cursor is missing its continuation.";
            return false;
        }
        state.offset = 0;
        state.fingerprint = expectedFingerprint;
        return true;
    }

    std::vector<std::wstring_view> fields;
    size_t start = 0;
    for (size_t i = 0; i < 5; ++i) {
        const size_t end = cursor.value.find(L'|', start);
        if (end == std::wstring::npos) {
            error = L"The list cursor is invalid.";
            return false;
        }
        fields.emplace_back(cursor.value.data() + start, end - start);
        start = end + 1;
    }
    fields.emplace_back(cursor.value.data() + start, cursor.value.size() - start);
    if (fields.size() != 6 || fields[0] != L"gw-list-v1") {
        error = L"The list cursor is invalid.";
        return false;
    }

    size_t encodedKind = 0;
    size_t offset = 0;
    uint64_t fingerprint = 0;
    if (!ParseUnsigned(fields[1], encodedKind) || encodedKind != static_cast<size_t>(kind) ||
        !ParseUnsigned(fields[2], offset) || !ParseHex64(fields[3], fingerprint) ||
        fingerprint != expectedFingerprint || offset != cursor.offset || !ValidListApiPath(fields[5])) {
        error = L"The list cursor does not belong to this query.";
        return false;
    }

    std::vector<uint64_t> history;
    if (fields[4] != L"-") {
        size_t historyStart = 0;
        while (historyStart <= fields[4].size()) {
            const size_t historyEnd = fields[4].find(L',', historyStart);
            uint64_t value = 0;
            if (!ParseHex64(fields[4].substr(historyStart, historyEnd == std::wstring_view::npos
                                                               ? std::wstring_view::npos
                                                               : historyEnd - historyStart),
                            value)) {
                error = L"The list cursor history is invalid.";
                return false;
            }
            history.push_back(value);
            if (history.size() > kProviderCollectionPageLimit) {
                error = L"The list cursor history is too long.";
                return false;
            }
            if (historyEnd == std::wstring_view::npos) break;
            historyStart = historyEnd + 1;
        }
    }

    state.offset = offset;
    state.fingerprint = fingerprint;
    state.history = std::move(history);
    state.path.assign(fields[5].data(), fields[5].size());
    return true;
}

bool HistoryContains(const std::vector<uint64_t>& history, uint64_t value) {
    return std::find(history.begin(), history.end(), value) != history.end();
}

bool BuildListCursor(const ListCursorState& current, stream::ListKind kind, std::wstring_view fingerprint,
                     std::wstring_view currentRequest, size_t nextOffset, std::wstring_view expectedPath,
                     std::wstring_view nextPath, std::wstring_view nextRequest, stream::ListCursor& next,
                     std::wstring& error) {
    const std::wstring normalized = NormalizeListNext(expectedPath, nextPath);
    if (!ValidListApiPath(nextRequest) || !EqualAscii(ListPathOnly(nextRequest), ListPathOnly(normalized))) {
        error = L"The list continuation changed the API path.";
        return false;
    }

    std::vector<uint64_t> history = current.history;
    const uint64_t currentHash = Fnv1a(Utf8(currentRequest));
    if (HistoryContains(history, currentHash)) {
        error = L"Apple Music returned a cyclic list continuation.";
        return false;
    }
    history.push_back(currentHash);
    if (HistoryContains(history, Fnv1a(Utf8(nextRequest)))) {
        error = L"Apple Music returned a cyclic list continuation.";
        return false;
    }
    if (history.size() > kProviderCollectionPageLimit) {
        error = L"Apple Music returned too many list pages.";
        return false;
    }

    std::wstring encodedHistory;
    for (const uint64_t value : history) {
        if (!encodedHistory.empty()) encodedHistory.push_back(L',');
        encodedHistory += Hex64(value);
    }
    next.value = L"gw-list-v1|" + std::to_wstring(static_cast<int>(kind)) + L'|' +
                 std::to_wstring(nextOffset) + L'|' + std::wstring(fingerprint) + L'|' + encodedHistory +
                 L'|' + normalized;
    next.offset = nextOffset;
    return true;
}

std::optional<size_t> OptionalCount(const Object& object, const wchar_t* key) {
    if (!object.HasKey(key)) return std::nullopt;
    const auto value = object.GetNamedValue(key);
    if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number) return std::nullopt;
    const double number = value.GetNumber();
    if (!std::isfinite(number) || number < 0 ||
        number >= static_cast<double>(std::numeric_limits<size_t>::max()) || std::floor(number) != number) {
        return std::nullopt;
    }
    return static_cast<size_t>(number);
}

std::optional<size_t> CollectionTotal(const Object& collection) {
    if (const auto total = OptionalCount(collection, L"total")) return total;
    if (const auto meta = json::GetObject(collection, L"meta"))
        if (const auto total = OptionalCount(*meta, L"total")) return total;
    return std::nullopt;
}

std::vector<Object> CollectionWithQuery(std::wstring_view firstPath, std::wstring_view query,
                                        abort_callback& abort) {
    std::vector<Object> out;
    std::set<std::wstring> seen;
    std::wstring path = EnsureQuery(std::wstring(firstPath), query);
    const std::wstring pathOnly = path.substr(0, path.find(L'?'));
    for (size_t page = 0; !path.empty(); ++page) {
        abort.check();
        if (page >= kProviderCollectionPageLimit || !seen.emplace(path).second)
            throw std::runtime_error("Apple Music pagination did not terminate.");
        const Object response = Apple(path, abort);
        if (const auto data = json::GetArray(response, L"data")) {
            const auto objects = json::Objects(*data);
            if (out.size() + objects.size() > kProviderCollectionItemLimit)
                throw std::runtime_error("Apple Music returned too many items.");
            out.insert(out.end(), objects.begin(), objects.end());
        }
        const std::wstring next = json::GetString(response, L"next");
        if (next.empty()) break;
        if (!StartsWith(next, L"/v1/") || next.find(L"://") != std::wstring::npos)
            throw std::runtime_error("Apple Music pagination left the API origin.");
        path = EnsureQuery(next, query);
        if (path.substr(0, path.find(L'?')) != pathOnly)
            throw std::runtime_error("Apple Music pagination changed the API path.");
    }
    return out;
}

std::vector<Object> RelationshipData(const Object& item, const wchar_t* name, bool library,
                                     abort_callback& abort) {
    const auto relationship = Relationship(item, name);
    if (!relationship) return {};
    // The primary resource normally embeds the first relationship page. Reusing it avoids
    // downloading the same 100 playlist rows a second time.
    std::vector<Object> out = EmbeddedRelationship(item, name);
    const std::wstring href = json::GetString(*relationship, L"href");
    std::wstring next = json::GetString(*relationship, L"next");
    if (!out.empty() && next.empty()) return out;
    if (out.empty()) {
        if (!href.empty())
            next = href;
        else if (next.empty())
            return out;
    }
    const std::wstring expectedPath =
        (href.empty() ? next : href).substr(0, (href.empty() ? next : href).find(L'?'));
    std::set<std::wstring> seen;
    for (size_t page = 0; !next.empty(); ++page) {
        abort.check();
        if (!StartsWith(next, L"/v1/") || next.find(L"://") != std::wstring::npos ||
            page >= kProviderCollectionPageLimit)
            throw std::runtime_error("Apple Music relationship pagination did not terminate.");
        next = RelationshipPagePath(std::move(next), library);
        if (!seen.emplace(next).second || next.substr(0, next.find(L'?')) != expectedPath)
            throw std::runtime_error("Apple Music relationship pagination changed the API path.");
        const Object response = Apple(next, abort);
        if (const auto data = json::GetArray(response, L"data")) {
            const auto objects = json::Objects(*data);
            if (out.size() + objects.size() > kProviderCollectionItemLimit)
                throw std::runtime_error("Apple Music returned too many items.");
            out.insert(out.end(), objects.begin(), objects.end());
        }
        next = json::GetString(response, L"next");
    }
    return out;
}

bool IsCatalogResourceId(std::wstring_view id) {
    return !id.empty() && !IsLibraryId(id) &&
           std::all_of(id.begin(), id.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

std::vector<stream::ArtistRef> ArtistRefs(const Object& item, std::wstring_view fallback) {
    std::vector<stream::ArtistRef> out;
    for (const auto& artist : EmbeddedRelationship(item, L"artists")) {
        stream::ArtistRef ref;
        ref.id = json::GetString(artist, L"id");
        if (const auto attributes = json::GetObject(artist, L"attributes"))
            ref.name = json::GetString(*attributes, L"name");
        if (ref.name.empty()) ref.name = json::GetString(artist, L"name");
        if (IsLibraryId(ref.id) && ref.id[0] == L'r') {
            if (const auto catalogId = VerifiedArtistCatalogId(artist, ref.id))
                Catalog::Get().PutArtistCatalogId(ref.id, catalogId->Value());
        }
        if (!ref.id.empty() || !ref.name.empty()) out.push_back(std::move(ref));
    }
    const bool hasName =
        std::any_of(out.begin(), out.end(), [](const stream::ArtistRef& ref) { return !ref.name.empty(); });
    if (!hasName && !out.empty() && !fallback.empty()) out.front().name = std::wstring(fallback);
    if (out.empty() && !fallback.empty()) out.push_back({{}, std::wstring(fallback)});
    return out;
}

int Year(std::wstring_view date) {
    if (date.size() < 4) return 0;
    const std::wstring year(date.substr(0, 4));
    const std::string utf8Year = Utf8(year);
    char* end = nullptr;
    const long value = std::strtol(utf8Year.c_str(), &end, 10);
    return end && *end == '\0' && value > 0 && value <= 9999 ? static_cast<int>(value) : 0;
}

std::optional<Song> ParseSong(const Object& item) {
    const auto attributes = json::GetObject(item, L"attributes");
    if (!attributes) return std::nullopt;
    const std::wstring objectId = json::GetString(item, L"id");
    const std::wstring type = json::GetString(item, L"type");
    if (objectId.empty()) return std::nullopt;

    Song song;
    song.libraryId = type == L"library-songs" ? objectId : std::wstring();
    if (const auto play = json::GetObject(*attributes, L"playParams")) {
        song.catalogId = json::GetString(*play, L"catalogId");
        song.reportingId = json::GetString(*play, L"reportingId");
        if (song.catalogId.empty() && type == L"songs") song.catalogId = json::GetString(*play, L"id");
    }
    if (!song.reportingId.empty() && !IsCatalogResourceId(song.reportingId)) song.reportingId.clear();
    if (song.catalogId.empty() && type == L"songs") song.catalogId = objectId;
    if (song.catalogId.empty()) {
        for (const auto& catalog : EmbeddedRelationship(item, L"catalog")) {
            song.catalogId = json::GetString(catalog, L"id");
            if (!song.catalogId.empty()) break;
        }
    }
    song.track.id = song.catalogId.empty() ? objectId : song.catalogId;
    song.track.title = json::GetString(*attributes, L"name");
    if (song.track.title.empty()) song.track.title = L"Unknown";
    song.track.album = json::GetString(*attributes, L"albumName");
    const std::wstring sourceAlbumId = AlbumIdFrom(item, !song.libraryId.empty());
    song.track.albumId = sourceAlbumId;
    if (IsLibraryId(sourceAlbumId) && sourceAlbumId[0] == L'l') song.libraryAlbumId = sourceAlbumId;
    song.track.artists = ArtistRefs(item, json::GetString(*attributes, L"artistName"));
    song.track.durationMs = json::GetInt(*attributes, L"durationInMillis");
    song.isrc = json::GetString(*attributes, L"isrc");
    song.contentRating = json::GetString(*attributes, L"contentRating");
    song.genreNames = StringArray(*attributes, L"genreNames");
    song.composerName = json::GetString(*attributes, L"composerName");
    song.composerNames = ComposerNames(item, *attributes);
    std::wstring catalogName;
    std::wstring catalogAlbumName;
    std::wstring catalogAlbumId;
    for (const auto& catalog : EmbeddedRelationship(item, L"catalog")) {
        if (const auto catalogAttributes = json::GetObject(catalog, L"attributes")) {
            if (catalogName.empty()) catalogName = json::GetString(*catalogAttributes, L"name");
            if (catalogAlbumName.empty())
                catalogAlbumName = json::GetString(*catalogAttributes, L"albumName");
            if (song.composerName.empty())
                song.composerName = json::GetString(*catalogAttributes, L"composerName");
            if (song.composerNames.empty()) song.composerNames = ComposerNames(catalog, *catalogAttributes);
        }
        if (catalogAlbumId.empty()) {
            const std::wstring candidate = AlbumIdFrom(catalog, false);
            if (IsCatalogResourceId(candidate)) catalogAlbumId = candidate;
        }
    }
    if (!catalogAlbumId.empty()) {
        song.track.albumId = catalogAlbumId;
        if (!song.libraryAlbumId.empty())
            Catalog::Get().PutAlbumLibraryId(catalogAlbumId, song.libraryAlbumId);
    }
    if (type == L"library-songs") {
        song.customTitle = !catalogName.empty() && catalogName != song.track.title;
        song.customAlbum = !catalogAlbumName.empty() && catalogAlbumName != song.track.album;
    }
    song.coverUrl = Artwork(item);
    if (song.coverUrl.empty()) {
        for (const auto& album : EmbeddedRelationship(item, L"albums")) {
            song.coverUrl = Artwork(album);
            if (!song.coverUrl.empty()) break;
        }
    }
    if (song.coverUrl.empty()) {
        for (const auto& catalog : EmbeddedRelationship(item, L"catalog")) {
            song.coverUrl = Artwork(catalog);
            if (!song.coverUrl.empty()) break;
        }
    }
    return song;
}

std::optional<stream::Playlist> ParsePlaylist(const Object& item) {
    const auto attributes = json::GetObject(item, L"attributes");
    if (!attributes) return std::nullopt;
    stream::Playlist playlist;
    playlist.id = json::GetString(item, L"id");
    if (playlist.id.empty()) return std::nullopt;
    playlist.name = json::GetString(*attributes, L"name", L"Unknown");
    playlist.coverUrl = Artwork(item);
    playlist.creator = json::GetString(*attributes, L"curatorName");
    playlist.description = TextAttribute(*attributes, L"description");
    playlist.trackCount = json::GetInt(*attributes, L"trackCount", stream::kUnknownCount);
    playlist.own = json::GetBool(*attributes, L"canEdit");
    playlist.saved = !playlist.own;
    return playlist;
}

std::optional<stream::Album> ParseAlbum(const Object& item) {
    const auto attributes = json::GetObject(item, L"attributes");
    if (!attributes) return std::nullopt;
    stream::Album album;
    album.id = json::GetString(item, L"id");
    if (album.id.empty()) return std::nullopt;
    album.name = json::GetString(*attributes, L"name", L"Unknown");
    album.coverUrl = Artwork(item);
    album.artists = ArtistRefs(item, json::GetString(*attributes, L"artistName"));
    album.description = TextAttribute(*attributes, L"editorialNotes");
    album.trackCount = json::GetInt(*attributes, L"trackCount", stream::kUnknownCount);
    album.year = Year(json::GetString(*attributes, L"releaseDate"));
    return album;
}

std::optional<stream::Artist> ParseArtist(const Object& item) {
    const auto attributes = json::GetObject(item, L"attributes");
    if (!attributes) return std::nullopt;
    stream::Artist artist;
    artist.id = json::GetString(item, L"id");
    if (artist.id.empty()) return std::nullopt;
    if (IsLibraryId(artist.id) && artist.id[0] == L'r') {
        if (const auto catalogId = VerifiedArtistCatalogId(item, artist.id))
            Catalog::Get().PutArtistCatalogId(artist.id, catalogId->Value());
    }
    artist.name = json::GetString(*attributes, L"name", L"Unknown");
    artist.avatarUrl = Artwork(item);
    artist.bio = TextAttribute(*attributes, L"editorialNotes");
    for (const auto& catalog : EmbeddedRelationship(item, L"catalog")) {
        if (artist.avatarUrl.empty()) artist.avatarUrl = Artwork(catalog);
        if (artist.bio.empty())
            if (const auto catalogAttributes = json::GetObject(catalog, L"attributes"))
                artist.bio = TextAttribute(*catalogAttributes, L"editorialNotes");
        if (const auto catalogAttributes = json::GetObject(catalog, L"attributes")) {
            if (artist.albumCount == stream::kUnknownCount)
                artist.albumCount = json::GetInt(*catalogAttributes, L"albumCount", stream::kUnknownCount);
            if (artist.trackCount == stream::kUnknownCount)
                artist.trackCount = json::GetInt(*catalogAttributes, L"trackCount", stream::kUnknownCount);
        }
    }
    const ArtistPreference preference = ParseArtistPreference(item);
    artist.followKnown = preference != ArtistPreference::Unknown;
    artist.followed = preference == ArtistPreference::Favorite;
    return artist;
}

std::vector<Song> ParseSongs(const std::vector<Object>& items) {
    std::vector<Song> songs;
    songs.reserve(items.size());
    for (const auto& item : items)
        if (auto song = ParseSong(item)) songs.push_back(std::move(*song));
    return songs;
}

void MergeHydratedSong(Song& target, const Song& hydrated) {
    if (target.catalogId.empty()) target.catalogId = hydrated.catalogId;
    if (target.libraryId.empty()) target.libraryId = hydrated.libraryId;
    if (target.libraryAlbumId.empty()) target.libraryAlbumId = hydrated.libraryAlbumId;
    if (target.reportingId.empty()) target.reportingId = hydrated.reportingId;
    if (target.isrc.empty()) target.isrc = hydrated.isrc;
    if (target.contentRating.empty()) target.contentRating = hydrated.contentRating;
    target.customTitle = target.customTitle || hydrated.customTitle;
    target.customAlbum = target.customAlbum || hydrated.customAlbum;
    const bool hydratedCatalogAlbum = !hydrated.track.albumId.empty() && !IsLibraryId(hydrated.track.albumId);
    const bool targetCatalogAlbum = !target.track.albumId.empty() && !IsLibraryId(target.track.albumId);
    if (!hydrated.track.albumId.empty() &&
        (target.track.albumId.empty() ||
         (!target.libraryId.empty() && hydratedCatalogAlbum && !targetCatalogAlbum)))
        target.track.albumId = hydrated.track.albumId;
    if (target.track.album.empty()) target.track.album = hydrated.track.album;
    if (target.track.durationMs <= 0) target.track.durationMs = hydrated.track.durationMs;
    if (target.coverUrl.empty()) target.coverUrl = hydrated.coverUrl;
    if (target.genreNames.empty()) target.genreNames = hydrated.genreNames;
    if (target.composerName.empty()) target.composerName = hydrated.composerName;
    if (target.composerNames.empty()) target.composerNames = hydrated.composerNames;
    if (target.track.artists.empty()) {
        target.track.artists = hydrated.track.artists;
        return;
    }
    for (auto& targetArtist : target.track.artists) {
        if (!targetArtist.id.empty() || targetArtist.name.empty()) continue;
        const auto found = std::find_if(hydrated.track.artists.begin(), hydrated.track.artists.end(),
                                        [&targetArtist](const stream::ArtistRef& hydratedArtist) {
                                            return !hydratedArtist.id.empty() &&
                                                   (hydratedArtist.name == targetArtist.name ||
                                                    EqualAscii(hydratedArtist.name, targetArtist.name));
                                        });
        if (found != hydrated.track.artists.end()) targetArtist.id = found->id;
    }
}

bool NeedsSongAlbumHydration(const Song& song) {
    const bool hasLibraryAlbum = IsLibraryId(song.track.albumId) && song.track.albumId[0] == L'l';
    const bool hasAlbumId = !song.track.albumId.empty() && (song.libraryId.empty() || hasLibraryAlbum);
    const bool hasArtistIds = !song.track.artists.empty() &&
                              std::all_of(song.track.artists.begin(), song.track.artists.end(),
                                          [](const stream::ArtistRef& artist) { return !artist.id.empty(); });
    return !hasAlbumId || !hasArtistIds;
}

bool HasSongAlbumHydrationWork(const std::vector<Song>& songs) {
    return std::any_of(songs.begin(), songs.end(), [](const Song& song) {
        return NeedsSongAlbumHydration(song) && (!song.libraryId.empty() || !song.catalogId.empty());
    });
}

Song BaselineSong(const stream::Track& track) {
    Song song;
    song.track = track;
    if (IsLibraryId(track.id) && track.id[0] == L'i')
        song.libraryId = track.id;
    else
        song.catalogId = track.id;
    return song;
}

// Track projections normally supply album and artist IDs. This fallback handles only
// rows where Apple omitted one of those relations.
size_t HydrateSongAlbums(std::vector<Song>& songs, const std::wstring& storefront, abort_callback& abort) {
    std::vector<stream::Id> libraryIds;
    std::vector<stream::Id> catalogIds;
    std::set<stream::Id> seenLibrary;
    std::set<stream::Id> seenCatalog;
    for (const auto& song : songs) {
        if (!NeedsSongAlbumHydration(song)) continue;
        if (!song.libraryId.empty() && seenLibrary.emplace(song.libraryId).second)
            libraryIds.push_back(song.libraryId);
        else if (song.libraryId.empty() && !song.catalogId.empty() &&
                 seenCatalog.emplace(song.catalogId).second)
            catalogIds.push_back(song.catalogId);
    }

    struct Batch {
        std::vector<stream::Id> ids;
        bool library = false;
    };
    struct BatchResult {
        std::vector<Song> songs;
        bool library = false;
    };

    std::vector<Batch> batches;
    const auto appendBatches = [&batches](const std::vector<stream::Id>& ids, bool library) {
        for (size_t first = 0; first < ids.size(); first += kSongAlbumHydrationBatch) {
            const size_t last = std::min(ids.size(), first + kSongAlbumHydrationBatch);
            Batch batch;
            batch.library = library;
            batch.ids.assign(ids.begin() + first, ids.begin() + last);
            batches.push_back(std::move(batch));
        }
    };
    appendBatches(libraryIds, true);
    appendBatches(catalogIds, false);

    // The bridge permits concurrent reads. Keep a small fixed ceiling so large playlists do
    // not create one request per track or starve the ordinary task queue.
    std::vector<std::future<BatchResult>> pending;
    size_t nextBatch = 0;
    while (nextBatch < batches.size() || !pending.empty()) {
        abort.check();
        while (pending.size() < kSongAlbumHydrationConcurrency && nextBatch < batches.size()) {
            auto batch = std::move(batches[nextBatch++]);
            pending.push_back(
                std::async(std::launch::async, [&storefront, &abort, batch = std::move(batch)]() mutable {
                    BatchResult result;
                    result.library = batch.library;
                    std::wstring joined;
                    for (const auto& id : batch.ids) {
                        if (!joined.empty()) joined += L',';
                        joined += id;
                    }
                    const std::wstring path =
                        batch.library ? L"/v1/me/library/songs?ids=" + Encode(joined) +
                                            L"&include=catalog,albums,artists&include[songs]=albums"
                                            L"&platform=web"
                                      : L"/v1/catalog/" + storefront + L"/songs?ids=" + Encode(joined) +
                                            L"&include=albums,artists&platform=web";
                    try {
                        const Object response = Apple(path, abort);
                        const auto data = json::GetArray(response, L"data");
                        if (data) result.songs = ParseSongs(json::Objects(*data));
                    } catch (const exception_aborted&) {
                        throw;
                    } catch (...) {
                    }
                    return result;
                }));
        }
        if (pending.empty()) break;

        BatchResult result = pending.front().get();
        pending.erase(pending.begin());
        abort.check();
        for (const auto& hydrated : result.songs) {
            const stream::Id key = result.library ? hydrated.libraryId : hydrated.catalogId;
            if (key.empty()) continue;
            for (auto& song : songs) {
                if ((result.library ? song.libraryId : song.catalogId) == key)
                    MergeHydratedSong(song, hydrated);
            }
        }
    }
    return batches.size();
}

std::vector<Song> LoadLibrarySongs(abort_callback& abort) {
    return ParseSongs(CollectionWithQuery(
        L"/v1/me/library/songs?limit=100&include=catalog,albums,artists&include[songs]=albums"
        L"&extend=editorialVideo",
        L"limit=100&include=catalog,albums,artists&include[songs]=albums&extend=editorialVideo", abort));
}

std::vector<stream::Album> ParseAlbums(const std::vector<Object>& items) {
    std::vector<stream::Album> albums;
    albums.reserve(items.size());
    for (const auto& item : items)
        if (auto album = ParseAlbum(item)) albums.push_back(std::move(*album));
    return albums;
}

std::vector<stream::Artist> ParseArtists(const std::vector<Object>& items) {
    std::vector<stream::Artist> artists;
    artists.reserve(items.size());
    for (const auto& item : items)
        if (auto artist = ParseArtist(item)) artists.push_back(std::move(*artist));
    return artists;
}

std::vector<stream::CatalogSearchItem> ParseCatalogSearchItems(const std::vector<Object>& items) {
    std::vector<stream::CatalogSearchItem> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        const std::wstring type = json::GetString(item, L"type");
        if (type == L"songs") {
            auto songs = ParseSongs({item});
            if (!songs.empty()) rows.emplace_back(std::move(songs.front().track));
        } else if (type == L"artists") {
            auto artists = ParseArtists({item});
            if (!artists.empty()) rows.emplace_back(std::move(artists.front()));
        } else if (type == L"albums") {
            auto albums = ParseAlbums({item});
            if (!albums.empty()) rows.emplace_back(std::move(albums.front()));
        } else if (type == L"playlists") {
            if (auto playlist = ParsePlaylist(item)) rows.emplace_back(std::move(*playlist));
        }
    }
    return rows;
}

bool RegionalDisplayEnabled() {
    return GetAppleMusicDisplayRegion() != AppleMusicDisplayRegion::Account;
}

std::wstring JoinedIds(const std::vector<stream::Id>& ids) {
    std::wstring joined;
    for (const auto& id : ids) {
        if (id.empty()) continue;
        if (!joined.empty()) joined.push_back(L',');
        joined += id;
    }
    return joined;
}

std::wstring LocalizedArtistName(const stream::ArtistRef& source,
                                 const std::vector<stream::ArtistRef>& localized, size_t index,
                                 bool sameCardinality) {
    const auto byId =
        std::find_if(localized.begin(), localized.end(), [&source](const stream::ArtistRef& candidate) {
            return !source.id.empty() && candidate.id == source.id && !candidate.name.empty();
        });
    if (byId != localized.end()) return byId->name;
    if (sameCardinality && index < localized.size()) return localized[index].name;
    return {};
}

bool HasCompleteArtistNames(const std::vector<stream::ArtistRef>& artists) {
    return !artists.empty() &&
           std::all_of(artists.begin(), artists.end(), [](const stream::ArtistRef& artist) {
               return !artist.id.empty() && !artist.name.empty();
           });
}

void StoreSongDisplayNames(AppleMusicDisplayRegion region, uint64_t displayEpoch, const Song& source,
                           const Song& localized, std::wstring_view sourceStorefront) {
    SongDisplayNames names;
    if (!source.customTitle) names.title = localized.track.title;
    if (!source.customAlbum) {
        names.album = localized.track.album;
        if (IsCatalogResourceId(localized.track.albumId)) names.albumId = localized.track.albumId;
    }
    names.coverUrl = localized.coverUrl;
    names.composerName = localized.composerName;
    names.composerNames = localized.composerNames;
    names.genreNames = localized.genreNames;
    if (HasCompleteArtistNames(localized.track.artists)) {
        names.artists = localized.track.artists;
        names.artistsAuthoritative = true;
        for (const auto& artist : names.artists) {
            DisplayMetadata::Get().PutArtistFor(region, sourceStorefront, artist.id, {artist.name}, false,
                                                displayEpoch);
        }
    } else {
        const bool sameCardinality = source.track.artists.size() == localized.track.artists.size();
        for (size_t i = 0; i < source.track.artists.size(); ++i) {
            stream::ArtistRef artist = source.track.artists[i];
            const std::wstring name =
                LocalizedArtistName(artist, localized.track.artists, i, sameCardinality);
            if (!name.empty()) {
                artist.name = name;
                DisplayMetadata::Get().PutArtistFor(region, sourceStorefront, artist.id, {artist.name}, false,
                                                    displayEpoch);
            }
            names.artists.push_back(std::move(artist));
        }
    }
    DisplayMetadata::Get().PutSongFor(region, sourceStorefront, source.track.id, std::move(names), true,
                                      displayEpoch);

    if (!source.track.albumId.empty() && (!localized.track.album.empty() || !source.track.artists.empty())) {
        AlbumDisplayNames album;
        if (!source.customAlbum) album.name = localized.track.album;
        album.coverUrl = localized.coverUrl;
        if (HasCompleteArtistNames(localized.track.artists)) {
            album.artists = localized.track.artists;
            album.artistsAuthoritative = true;
        } else {
            const bool sameCardinality = source.track.artists.size() == localized.track.artists.size();
            for (size_t i = 0; i < source.track.artists.size(); ++i) {
                stream::ArtistRef artist = source.track.artists[i];
                const std::wstring name =
                    LocalizedArtistName(artist, localized.track.artists, i, sameCardinality);
                if (!name.empty()) artist.name = name;
                album.artists.push_back(std::move(artist));
            }
        }
        DisplayMetadata::Get().PutAlbumFor(region, sourceStorefront, source.track.albumId, std::move(album),
                                           false, displayEpoch);
        if (!localized.track.albumId.empty() && localized.track.albumId != source.track.albumId) {
            AlbumDisplayNames displayAlbum;
            if (!source.customAlbum) displayAlbum.name = localized.track.album;
            displayAlbum.coverUrl = localized.coverUrl;
            displayAlbum.artists = localized.track.artists;
            displayAlbum.artistsAuthoritative = HasCompleteArtistNames(localized.track.artists);
            DisplayMetadata::Get().PutAlbumFor(region, sourceStorefront, localized.track.albumId,
                                               std::move(displayAlbum), false, displayEpoch);
        }
    }
}

void StoreAlbumDisplayNames(AppleMusicDisplayRegion region, uint64_t displayEpoch,
                            const stream::Album& source, const stream::Album& localized,
                            std::wstring_view sourceStorefront) {
    AlbumDisplayNames names;
    names.name = localized.name;
    names.description = localized.description;
    names.coverUrl = localized.coverUrl;
    if (HasCompleteArtistNames(localized.artists)) {
        names.artists = localized.artists;
        names.artistsAuthoritative = true;
        for (const auto& artist : names.artists) {
            DisplayMetadata::Get().PutArtistFor(region, sourceStorefront, artist.id, {artist.name}, false,
                                                displayEpoch);
        }
    } else {
        const bool sameCardinality = source.artists.size() == localized.artists.size();
        for (size_t i = 0; i < source.artists.size(); ++i) {
            stream::ArtistRef artist = source.artists[i];
            const std::wstring name = LocalizedArtistName(artist, localized.artists, i, sameCardinality);
            if (!name.empty()) {
                artist.name = name;
                DisplayMetadata::Get().PutArtistFor(region, sourceStorefront, artist.id, {artist.name}, false,
                                                    displayEpoch);
            }
            names.artists.push_back(std::move(artist));
        }
    }
    DisplayMetadata::Get().PutAlbumFor(region, sourceStorefront, source.id, std::move(names), true,
                                       displayEpoch);
}

void StoreArtistDisplayNames(AppleMusicDisplayRegion region, uint64_t displayEpoch,
                             const stream::Artist& source, const stream::Artist& localized,
                             std::wstring_view sourceStorefront) {
    ArtistDisplayNames names;
    names.name = localized.name;
    names.bio = localized.bio;
    names.avatarUrl = localized.avatarUrl;
    DisplayMetadata::Get().PutArtistFor(region, sourceStorefront, source.id, std::move(names), true,
                                        displayEpoch);
}

void ApplyPageDisplayNames(stream::ListPage& page, bool applyTracks = true) {
    if (!RegionalDisplayEnabled()) return;
    const std::wstring storefront = DisplayMetadata::Get().SourceStorefront();
    std::visit(
        [storefront, applyTracks](auto& rows) {
            using Rows = std::decay_t<decltype(rows)>;
            for (auto& row : rows) {
                if constexpr (std::is_same_v<Rows, std::vector<stream::Track>>) {
                    if (applyTracks) DisplayMetadata::Get().Apply(row, storefront);
                } else if constexpr (std::is_same_v<Rows, std::vector<stream::Album>> ||
                                     std::is_same_v<Rows, std::vector<stream::Artist>> ||
                                     std::is_same_v<Rows, std::vector<stream::Playlist>>) {
                    DisplayMetadata::Get().Apply(row, storefront);
                } else if constexpr (std::is_same_v<Rows, std::vector<stream::CatalogSearchItem>>) {
                    std::visit(
                        [&storefront, applyTracks](auto& item) {
                            using Item = std::decay_t<decltype(item)>;
                            if constexpr (std::is_same_v<Item, stream::Track>) {
                                if (applyTracks) DisplayMetadata::Get().Apply(item, storefront);
                            } else {
                                DisplayMetadata::Get().Apply(item, storefront);
                            }
                        },
                        row);
                }
            }
        },
        page.rows);
    if (page.playlist) DisplayMetadata::Get().Apply(*page.playlist, storefront);
    if (page.album) DisplayMetadata::Get().Apply(*page.album, storefront);
    if (page.artist) DisplayMetadata::Get().Apply(*page.artist, storefront);
}

bool PageNeedsDisplayNames(const stream::ListPage& page) {
    if (!RegionalDisplayEnabled()) return false;
    const std::wstring storefront = DisplayMetadata::Get().SourceStorefront();
    return std::visit(
        [&storefront, &page](const auto& rows) {
            using Rows = std::decay_t<decltype(rows)>;
            for (const auto& row : rows) {
                if constexpr (std::is_same_v<Rows, std::vector<stream::Track>>) {
                    if (!row.id.empty() && !DisplayMetadata::Get().IsSongComplete(
                                               GetAppleMusicDisplayRegion(), storefront, row.id))
                        return true;
                } else if constexpr (std::is_same_v<Rows, std::vector<stream::Album>>) {
                    if (!row.id.empty() && !DisplayMetadata::Get().IsAlbumComplete(
                                               GetAppleMusicDisplayRegion(), storefront, row.id))
                        return true;
                } else if constexpr (std::is_same_v<Rows, std::vector<stream::Artist>>) {
                    if (!row.id.empty() && !DisplayMetadata::Get().IsArtistComplete(
                                               GetAppleMusicDisplayRegion(), storefront, row.id))
                        return true;
                } else if constexpr (std::is_same_v<Rows, std::vector<stream::Playlist>>) {
                    if (!row.own && !row.id.empty() &&
                        !DisplayMetadata::Get().IsPlaylistComplete(GetAppleMusicDisplayRegion(), storefront,
                                                                   row.id))
                        return true;
                } else if constexpr (std::is_same_v<Rows, std::vector<stream::CatalogSearchItem>>) {
                    const bool needs = std::visit(
                        [&storefront](const auto& item) {
                            using Item = std::decay_t<decltype(item)>;
                            if (item.id.empty()) return false;
                            if constexpr (std::is_same_v<Item, stream::Track>) {
                                return !DisplayMetadata::Get().IsSongComplete(GetAppleMusicDisplayRegion(),
                                                                              storefront, item.id);
                            } else if constexpr (std::is_same_v<Item, stream::Album>) {
                                return !DisplayMetadata::Get().IsAlbumComplete(GetAppleMusicDisplayRegion(),
                                                                               storefront, item.id);
                            } else if constexpr (std::is_same_v<Item, stream::Artist>) {
                                return !DisplayMetadata::Get().IsArtistComplete(GetAppleMusicDisplayRegion(),
                                                                                storefront, item.id);
                            } else {
                                return !item.own && !DisplayMetadata::Get().IsPlaylistComplete(
                                                        GetAppleMusicDisplayRegion(), storefront, item.id);
                            }
                        },
                        row);
                    if (needs) return true;
                }
            }
            if (page.playlist && !page.playlist->own && !page.playlist->id.empty() &&
                !DisplayMetadata::Get().IsPlaylistComplete(GetAppleMusicDisplayRegion(), storefront,
                                                           page.playlist->id))
                return true;
            if (page.album && !page.album->id.empty() &&
                !DisplayMetadata::Get().IsAlbumComplete(GetAppleMusicDisplayRegion(), storefront,
                                                        page.album->id))
                return true;
            return page.artist && !page.artist->id.empty() &&
                   !DisplayMetadata::Get().IsArtistComplete(GetAppleMusicDisplayRegion(), storefront,
                                                            page.artist->id);
        },
        page.rows);
}

std::wstring PreferText(const std::wstring& primary, const std::wstring& fallback) {
    return primary.empty() ? fallback : primary;
}

int PreferCount(int primary, int fallback) {
    return primary == stream::kUnknownCount ? fallback : primary;
}

std::vector<stream::ArtistRef> MergeArtistRefs(std::vector<stream::ArtistRef> primary,
                                               const std::vector<stream::ArtistRef>& fallback) {
    for (const auto& candidate : fallback) {
        auto found =
            std::find_if(primary.begin(), primary.end(), [&candidate](const stream::ArtistRef& item) {
                if (!candidate.id.empty() && item.id == candidate.id) return true;
                return !candidate.name.empty() && !item.name.empty() &&
                       (candidate.name == item.name || EqualAscii(candidate.name, item.name));
            });
        if (found == primary.end()) {
            if (!candidate.id.empty() || !candidate.name.empty()) primary.push_back(candidate);
            continue;
        }
        if (found->id.empty()) found->id = candidate.id;
        if (found->name.empty()) found->name = candidate.name;
    }
    return primary;
}

Song MergeSongRecord(const Song& primary, const Song& fallback) {
    Song merged = primary;
    MergeHydratedSong(merged, fallback);
    if (merged.track.id.empty()) merged.track.id = fallback.track.id;
    if (merged.track.title.empty() || merged.track.title == L"Unknown")
        if (!fallback.track.title.empty() && fallback.track.title != L"Unknown")
            merged.track.title = fallback.track.title;
    merged.track.artists = MergeArtistRefs(std::move(merged.track.artists), fallback.track.artists);
    if (merged.track.popularity == 0) merged.track.popularity = fallback.track.popularity;
    if (merged.genreNames.empty()) {
        merged.genreNames = fallback.genreNames;
    } else {
        for (const auto& value : fallback.genreNames)
            if (std::find(merged.genreNames.begin(), merged.genreNames.end(), value) ==
                merged.genreNames.end())
                merged.genreNames.push_back(value);
    }
    if (merged.composerNames.empty()) {
        merged.composerNames = fallback.composerNames;
    } else {
        for (const auto& value : fallback.composerNames)
            if (std::find(merged.composerNames.begin(), merged.composerNames.end(), value) ==
                merged.composerNames.end())
                merged.composerNames.push_back(value);
    }
    return merged;
}

stream::Playlist MergePlaylistRecord(const stream::Playlist& primary, const stream::Playlist& fallback,
                                     bool mergeFlags) {
    stream::Playlist merged = primary;
    if (merged.id.empty()) merged.id = fallback.id;
    merged.name = PreferText(merged.name, fallback.name);
    merged.coverUrl = PreferText(merged.coverUrl, fallback.coverUrl);
    merged.creator = PreferText(merged.creator, fallback.creator);
    merged.description = PreferText(merged.description, fallback.description);
    merged.trackCount = PreferCount(merged.trackCount, fallback.trackCount);
    if (mergeFlags) {
        merged.own = merged.own || fallback.own;
        merged.saved = merged.saved || fallback.saved;
    }
    return merged;
}

stream::Album MergeAlbumRecord(const stream::Album& primary, const stream::Album& fallback) {
    stream::Album merged = primary;
    if (merged.id.empty()) merged.id = fallback.id;
    merged.name = PreferText(merged.name, fallback.name);
    merged.coverUrl = PreferText(merged.coverUrl, fallback.coverUrl);
    merged.description = PreferText(merged.description, fallback.description);
    merged.artists = MergeArtistRefs(std::move(merged.artists), fallback.artists);
    merged.trackCount = PreferCount(merged.trackCount, fallback.trackCount);
    if (merged.year == 0) merged.year = fallback.year;
    return merged;
}

stream::Artist MergeArtistRecord(const stream::Artist& primary, const stream::Artist& fallback,
                                 bool mergeFollowState) {
    stream::Artist merged = primary;
    if (merged.id.empty()) merged.id = fallback.id;
    merged.name = PreferText(merged.name, fallback.name);
    merged.avatarUrl = PreferText(merged.avatarUrl, fallback.avatarUrl);
    merged.bio = PreferText(merged.bio, fallback.bio);
    merged.albumCount = PreferCount(merged.albumCount, fallback.albumCount);
    merged.trackCount = PreferCount(merged.trackCount, fallback.trackCount);
    if (mergeFollowState || !merged.followKnown) {
        if (fallback.followKnown) {
            merged.followKnown = true;
            merged.followed = fallback.followed;
        }
    }
    return merged;
}

std::vector<Song> MergeSongRecordsWithCatalog(std::vector<Song> incoming, bool preferIncoming) {
    for (auto& record : incoming) {
        if (record.track.id.empty()) continue;
        const auto existing = Catalog::Get().FindSongRaw(record.track.id);
        if (!existing) continue;
        record = preferIncoming ? MergeSongRecord(record, *existing) : MergeSongRecord(*existing, record);
    }
    return incoming;
}

void ApplyAlbumContext(Song& song, const stream::Album& album) {
    if (song.track.album.empty()) song.track.album = album.name;
    if (song.track.albumId.empty()) {
        song.track.albumId = album.id;
    } else if (IsLibraryId(song.track.albumId) && song.track.albumId[0] == L'l' && !IsLibraryId(album.id)) {
        song.libraryAlbumId = song.track.albumId;
        song.track.albumId = album.id;
        Catalog::Get().PutAlbumLibraryId(album.id, song.libraryAlbumId);
    }
    if (song.coverUrl.empty()) song.coverUrl = album.coverUrl;
    if (song.track.artists.empty()) return;
    for (auto& trackArtist : song.track.artists) {
        if (!trackArtist.id.empty() || trackArtist.name.empty()) continue;
        const auto found = std::find_if(
            album.artists.begin(), album.artists.end(), [&trackArtist](const stream::ArtistRef& albumArtist) {
                return !albumArtist.id.empty() && (albumArtist.name == trackArtist.name ||
                                                   EqualAscii(albumArtist.name, trackArtist.name));
            });
        if (found != album.artists.end()) trackArtist.id = found->id;
    }
}

bool TrackCreditsArtist(const Song& song, const stream::Id& artistId, std::wstring_view artistName) {
    for (const auto& credit : song.track.artists)
        if (!artistId.empty() && credit.id == artistId) return true;
    if (artistName.empty()) return false;
    for (const auto& credit : song.track.artists)
        if (credit.name == artistName || EqualAscii(credit.name, artistName)) return true;
    return false;
}

struct LibraryArtistData {
    std::vector<stream::Album> albums;
    std::vector<Song> songs;
};

LibraryArtistData LoadLibraryArtistData(const stream::Id& artist, std::wstring_view artistName,
                                        abort_callback& abort) {
    LibraryArtistData data;
    const std::wstring path =
        L"/v1/me/library/artists/" + Encode(artist) + L"/albums?limit=100&include=catalog,artists";
    for (const auto& albumObject : CollectionWithQuery(path, L"limit=100&include=catalog,artists", abort)) {
        const auto album = ParseAlbum(albumObject);
        if (!album) continue;
        data.albums.push_back(*album);
        const std::wstring tracksPath = L"/v1/me/library/albums/" + Encode(album->id) +
                                        L"/tracks?limit=100&include[library-songs]=catalog,artists,albums"
                                        L"&include[songs]=albums";
        for (auto song :
             ParseSongs(CollectionWithQuery(tracksPath,
                                            L"limit=100&include[library-songs]=catalog,artists,albums"
                                            L"&include[songs]=albums",
                                            abort))) {
            ApplyAlbumContext(song, *album);
            if (!TrackCreditsArtist(song, artist, artistName)) continue;
            data.songs.push_back(std::move(song));
        }
    }

    std::set<stream::Id> seen;
    data.songs.erase(std::remove_if(data.songs.begin(), data.songs.end(),
                                    [&seen](const Song& song) {
                                        return song.track.id.empty() || !seen.emplace(song.track.id).second;
                                    }),
                     data.songs.end());
    return data;
}

std::vector<stream::Track> Tracks(const std::vector<Song>& songs) {
    std::vector<stream::Track> tracks;
    tracks.reserve(songs.size());
    for (const auto& song : songs)
        tracks.push_back(song.track);
    return tracks;
}

std::vector<stream::Track> DisplayTracks(std::vector<stream::Track> tracks) {
    const std::wstring storefront = DisplayMetadata::Get().SourceStorefront();
    for (auto& track : tracks) {
        if (auto song = Catalog::Get().FindSongRaw(track.id)) {
            song->track = track;
            DisplayMetadata::Get().Apply(*song, storefront);
            track = std::move(song->track);
        } else {
            DisplayMetadata::Get().Apply(track, storefront);
        }
    }
    return tracks;
}

std::vector<stream::Track> DisplayTracks(const std::vector<Song>& songs) {
    const std::wstring storefront = DisplayMetadata::Get().SourceStorefront();
    std::vector<stream::Track> tracks;
    tracks.reserve(songs.size());
    for (Song song : songs) {
        DisplayMetadata::Get().Apply(song, storefront);
        tracks.push_back(std::move(song.track));
    }
    return tracks;
}

std::vector<Song> DisplaySongs(std::vector<Song> songs) {
    const std::wstring storefront = DisplayMetadata::Get().SourceStorefront();
    for (auto& song : songs)
        DisplayMetadata::Get().Apply(song, storefront);
    return songs;
}

std::vector<stream::Album> DisplayAlbums(std::vector<stream::Album> albums) {
    const std::wstring storefront = DisplayMetadata::Get().SourceStorefront();
    for (auto& album : albums)
        DisplayMetadata::Get().Apply(album, storefront);
    return albums;
}

std::vector<stream::Artist> DisplayArtists(std::vector<stream::Artist> artists) {
    const std::wstring storefront = DisplayMetadata::Get().SourceStorefront();
    for (auto& artist : artists)
        DisplayMetadata::Get().Apply(artist, storefront);
    return artists;
}

std::vector<stream::Playlist> DisplayPlaylists(std::vector<stream::Playlist> playlists) {
    const std::wstring storefront = DisplayMetadata::Get().SourceStorefront();
    for (auto& playlist : playlists)
        DisplayMetadata::Get().Apply(playlist, storefront);
    return playlists;
}

void WarmDisplayCacheFromPage(const stream::ListPage& page, const DisplayTarget& target,
                              std::wstring_view sourceStorefront) {
    std::visit(
        [&target, sourceStorefront](const auto& rows) {
            using Rows = std::decay_t<decltype(rows)>;
            for (const auto& row : rows) {
                if constexpr (std::is_same_v<Rows, std::vector<stream::Track>>) {
                    SongDisplayNames names;
                    names.title = row.title;
                    names.album = row.album;
                    names.albumId = row.albumId;
                    names.artists = row.artists;
                    names.artistsAuthoritative = !row.artists.empty();
                    DisplayMetadata::Get().PutSongFor(target.region, sourceStorefront, row.id,
                                                      std::move(names));
                    if (!row.albumId.empty())
                        DisplayMetadata::Get().PutAlbumFor(target.region, sourceStorefront, row.albumId,
                                                           {row.album, {}, row.artists});
                    for (const auto& artist : row.artists)
                        DisplayMetadata::Get().PutArtistFor(target.region, sourceStorefront, artist.id,
                                                            {artist.name});
                } else if constexpr (std::is_same_v<Rows, std::vector<stream::Album>>) {
                    AlbumDisplayNames names;
                    names.name = row.name;
                    names.description = row.description;
                    names.artists = row.artists;
                    names.coverUrl = row.coverUrl;
                    names.artistsAuthoritative = !row.artists.empty();
                    DisplayMetadata::Get().PutAlbumFor(target.region, sourceStorefront, row.id,
                                                       std::move(names), false);
                } else if constexpr (std::is_same_v<Rows, std::vector<stream::Artist>>) {
                    DisplayMetadata::Get().PutArtistFor(target.region, sourceStorefront, row.id,
                                                        {row.name, row.bio, row.avatarUrl}, false);
                } else if constexpr (std::is_same_v<Rows, std::vector<stream::Playlist>>) {
                    if (!row.own)
                        DisplayMetadata::Get().PutPlaylistFor(target.region, sourceStorefront, row.id,
                                                              {row.name, row.description, row.coverUrl},
                                                              false);
                } else if constexpr (std::is_same_v<Rows, std::vector<stream::CatalogSearchItem>>) {
                    std::visit(
                        [&target, sourceStorefront](const auto& item) {
                            using Item = std::decay_t<decltype(item)>;
                            if constexpr (std::is_same_v<Item, stream::Track>) {
                                SongDisplayNames names;
                                names.title = item.title;
                                names.album = item.album;
                                names.albumId = item.albumId;
                                names.artists = item.artists;
                                names.artistsAuthoritative = !item.artists.empty();
                                DisplayMetadata::Get().PutSongFor(target.region, sourceStorefront, item.id,
                                                                  std::move(names));
                            } else if constexpr (std::is_same_v<Item, stream::Album>) {
                                DisplayMetadata::Get().PutAlbumFor(
                                    target.region, sourceStorefront, item.id,
                                    {item.name, item.description, item.artists, item.coverUrl}, false);
                            } else if constexpr (std::is_same_v<Item, stream::Artist>) {
                                DisplayMetadata::Get().PutArtistFor(target.region, sourceStorefront, item.id,
                                                                    {item.name, item.bio, item.avatarUrl},
                                                                    false);
                            } else if (!item.own) {
                                DisplayMetadata::Get().PutPlaylistFor(
                                    target.region, sourceStorefront, item.id,
                                    {item.name, item.description, item.coverUrl}, false);
                            }
                        },
                        row);
                }
            }
        },
        page.rows);
    if (page.playlist && !page.playlist->own)
        DisplayMetadata::Get().PutPlaylistFor(
            target.region, sourceStorefront, page.playlist->id,
            {page.playlist->name, page.playlist->description, page.playlist->coverUrl}, false);
    if (page.album)
        DisplayMetadata::Get().PutAlbumFor(target.region, sourceStorefront, page.album->id,
                                           {page.album->name, page.album->description, page.album->artists},
                                           false);
    if (page.artist)
        DisplayMetadata::Get().PutArtistFor(target.region, sourceStorefront, page.artist->id,
                                            {page.artist->name, page.artist->bio, page.artist->avatarUrl},
                                            false);
}

std::vector<std::wstring> GroupValues(const Song& song, bool genres) {
    if (genres) return song.genreNames;
    if (!song.composerNames.empty()) return song.composerNames;
    if (!song.composerName.empty()) return {song.composerName};
    return {};
}

std::vector<stream::BrowseGroup> DerivedGroups(const std::vector<Song>& songs,
                                               const std::vector<Song>& displaySongs, bool genres) {
    std::map<std::wstring, size_t> counts;
    std::map<std::wstring, std::wstring> labels;
    for (size_t i = 0; i < songs.size(); ++i) {
        const auto values = GroupValues(songs[i], genres);
        const auto displayValues =
            i < displaySongs.size() ? GroupValues(displaySongs[i], genres) : std::vector<std::wstring>();
        std::set<std::wstring> seen;
        for (size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex) {
            const std::wstring& value = values[valueIndex];
            if (seen.emplace(value).second) ++counts[value];
            if (!labels.contains(value) && values.size() == displayValues.size() &&
                valueIndex < displayValues.size() && !displayValues[valueIndex].empty())
                labels[value] = displayValues[valueIndex];
        }
    }

    std::vector<stream::BrowseGroup> out;
    out.reserve(counts.size());
    for (const auto& [name, count] : counts) {
        stream::BrowseGroup group;
        group.id = std::wstring(genres ? kGenreGroupPrefix : kComposerGroupPrefix) + name;
        const auto label = labels.find(name);
        group.label = label == labels.end() ? name : label->second;
        group.trackCount = count;
        out.push_back(std::move(group));
    }
    return out;
}

std::vector<stream::Track> TracksInGroup(const std::vector<Song>& songs, std::wstring_view group,
                                         bool genres) {
    const std::wstring_view prefix = genres ? kGenreGroupPrefix : kComposerGroupPrefix;
    if (!StartsWith(group, prefix)) return {};
    const std::wstring_view value = group.substr(prefix.size());
    if (value.empty()) return {};
    const std::wstring selected(value);

    std::vector<stream::Track> out;
    for (const auto& song : songs) {
        const bool match = genres ? std::find(song.genreNames.begin(), song.genreNames.end(), selected) !=
                                        song.genreNames.end()
                                  : std::find(song.composerNames.begin(), song.composerNames.end(),
                                              selected) != song.composerNames.end() ||
                                        (song.composerNames.empty() && song.composerName == selected);
        if (match) out.push_back(song.track);
    }
    return out;
}

std::vector<Song> SongsInGroup(const std::vector<Song>& songs, std::wstring_view group, bool genres) {
    const std::wstring_view prefix = genres ? kGenreGroupPrefix : kComposerGroupPrefix;
    if (!StartsWith(group, prefix)) return {};
    const std::wstring_view value = group.substr(prefix.size());
    if (value.empty()) return {};
    const std::wstring selected(value);

    std::vector<Song> out;
    for (const auto& song : songs) {
        const bool match = genres ? std::find(song.genreNames.begin(), song.genreNames.end(), selected) !=
                                        song.genreNames.end()
                                  : std::find(song.composerNames.begin(), song.composerNames.end(),
                                              selected) != song.composerNames.end() ||
                                        (song.composerNames.empty() && song.composerName == selected);
        if (match) out.push_back(song);
    }
    return out;
}

size_t PageLimit(size_t limit) {
    return std::clamp<size_t>(limit, 1, 100);
}

std::vector<stream::Track> TrackPage(const std::vector<stream::Track>& tracks, size_t offset, size_t limit,
                                     bool& more) {
    const size_t begin = std::min(offset, tracks.size());
    const size_t end = std::min(tracks.size(), begin + PageLimit(limit));
    more = end < tracks.size();
    return std::vector<stream::Track>(tracks.begin() + begin, tracks.begin() + end);
}

std::wstring PlaylistPath(const std::wstring& id, const std::wstring& storefront, bool library,
                          bool tracks = true) {
    if (!library)
        return L"/v1/catalog/" + storefront + L"/playlists/" + Encode(id) +
               L"?include=tracks,catalog,artists&include[songs]=albums";
    std::wstring path = L"/v1/me/library/playlists/" + Encode(id);
    path += tracks ? L"?include=tracks&include[library-songs]=catalog,artists,albums"
                     L"&include[songs]=albums&extend[library-playlists]=tags"
                   : L"?extend[library-playlists]=tags";
    return path;
}

std::wstring AlbumPath(const std::wstring& id, const std::wstring& storefront, bool library) {
    return library ? L"/v1/me/library/albums/" + Encode(id) +
                         L"?include=tracks,catalog,artists&include[library-songs]=catalog,artists,albums"
                   : L"/v1/catalog/" + storefront + L"/albums/" + Encode(id) + L"?include=tracks";
}

std::wstring PlaylistTracksPath(const std::wstring& id, const std::wstring& storefront, bool library) {
    return library ? L"/v1/me/library/playlists/" + Encode(id) + L"/tracks"
                   : L"/v1/catalog/" + storefront + L"/playlists/" + Encode(id) + L"/tracks";
}

std::wstring AlbumTracksPath(const std::wstring& id, const std::wstring& storefront, bool library) {
    return library ? L"/v1/me/library/albums/" + Encode(id) + L"/tracks"
                   : L"/v1/catalog/" + storefront + L"/albums/" + Encode(id) + L"/tracks";
}

std::wstring ArtistPath(const std::wstring& id, const std::wstring& storefront, bool library) {
    return library
               ? L"/v1/me/library/artists?ids=" + Encode(id) +
                     L"&include=catalog&extend[library-artists]=inFavorites&extend=inFavorites&platform=web"
               : L"/v1/catalog/" + storefront + L"/artists/" + Encode(id) +
                     L"?extend=inFavorites&platform=web";
}

struct PlaylistData {
    stream::Playlist info;
    std::vector<Song> songs;
    PlaylistLoadMetrics metrics;
    stream::Id catalogId;
    bool canRename = false;
    bool canDelete = false;
    bool favorite = false;
};

PlaylistData LoadPlaylist(const std::wstring& id, const std::wstring& storefront, bool library,
                          abort_callback& abort, bool tracks = true) {
    const auto primaryStarted = std::chrono::steady_clock::now();
    const Object response = Apple(PlaylistPath(id, storefront, library, tracks), abort);
    const auto item = FirstData(response);
    if (!item) throw std::runtime_error("Apple Music returned no playlist.");
    const auto info = ParsePlaylist(*item);
    if (!info) throw std::runtime_error("Apple Music returned an invalid playlist.");
    PlaylistData data;
    data.info = *info;
    data.catalogId = CatalogIdFrom(*item).value_or(std::wstring());
    if (const auto attributes = json::GetObject(*item, L"attributes")) {
        data.canRename = data.info.own && AttributeFlag(*attributes, L"canRename", true);
        data.canDelete = AttributeFlag(*attributes, L"canDelete", true);
    }
    data.favorite = IsFavoriteSystemPlaylist(*item);
    data.metrics.primaryMs = ElapsedMs(primaryStarted, std::chrono::steady_clock::now());
    if (tracks) {
        const auto pagesStarted = std::chrono::steady_clock::now();
        const auto pageItems = RelationshipData(*item, L"tracks", library, abort);
        data.songs = ParseSongs(pageItems);
        data.metrics.pagesMs = ElapsedMs(pagesStarted, std::chrono::steady_clock::now());
        const auto hydrationStarted = std::chrono::steady_clock::now();
        data.metrics.fallbackBatches = HydrateSongAlbums(data.songs, storefront, abort);
        data.metrics.hydrationMs = ElapsedMs(hydrationStarted, std::chrono::steady_clock::now());
        if (data.info.trackCount == stream::kUnknownCount)
            data.info.trackCount =
                static_cast<int>(std::min(data.songs.size(), static_cast<size_t>(INT_MAX)));
    }
    return data;
}

struct AlbumData {
    stream::Album info;
    std::vector<Song> songs;
    stream::Id catalogId;
};

AlbumData LoadAlbum(const std::wstring& id, const std::wstring& storefront, bool library,
                    abort_callback& abort) {
    const Object response = Apple(AlbumPath(id, storefront, library), abort);
    const auto item = FirstData(response);
    if (!item) throw std::runtime_error("Apple Music returned no album.");
    const auto info = ParseAlbum(*item);
    if (!info) throw std::runtime_error("Apple Music returned an invalid album.");
    AlbumData data;
    data.info = *info;
    if (library) {
        const std::wstring tracksPath = L"/v1/me/library/albums/" + Encode(id) +
                                        L"/tracks?limit=100&include[library-songs]=catalog,artists,albums"
                                        L"&include[songs]=albums";
        data.songs = ParseSongs(CollectionWithQuery(tracksPath,
                                                    L"limit=100&include[library-songs]=catalog,artists,albums"
                                                    L"&include[songs]=albums",
                                                    abort));
    } else {
        data.songs = ParseSongs(RelationshipData(*item, L"tracks", false, abort));
    }
    for (auto& song : data.songs)
        ApplyAlbumContext(song, data.info);
    if (data.info.trackCount == stream::kUnknownCount)
        data.info.trackCount = static_cast<int>(std::min(data.songs.size(), static_cast<size_t>(INT_MAX)));
    data.catalogId = CatalogIdFrom(*item).value_or(std::wstring());
    return data;
}

struct ArtistData {
    stream::Artist info;
    stream::Id catalogId;
};

std::optional<ArtistData> LoadArtist(const std::wstring& id, const std::wstring& storefront, bool library,
                                     abort_callback& abort) {
    std::optional<Object> item;
    stream::Id catalogId;
    if (library) {
        catalogId = Catalog::Get().FindArtistCatalogId(id);
        if (!catalogId.empty()) {
            try {
                item = FirstData(Apple(ArtistPath(catalogId, storefront, false), abort));
                if (item && !VerifiedArtistCatalogId(*item, catalogId)) item.reset();
            } catch (const exception_aborted&) {
                throw;
            } catch (const std::exception& exception) {
                if (!DefiniteNotFound(exception)) throw;
            }
        }
    }
    if (!item) {
        item = FirstData(Apple(ArtistPath(id, storefront, library), abort));
        if (library) catalogId = item ? CatalogIdFrom(*item).value_or(std::wstring()) : stream::Id();
    } else {
        // Present-catalog data under the stable library identity used by the caller.
    }
    if (!item) return std::nullopt;
    auto info = ParseArtist(*item);
    if (!info) return std::nullopt;
    if (library) info->id = id;
    if (!library) catalogId = id;
    return ArtistData{std::move(*info), catalogId};
}

std::optional<CatalogArtistId> ResolveVerifiedArtistCatalogId(const stream::Id& artist,
                                                              const stream::Id& catalogId,
                                                              const std::wstring& storefront, bool library,
                                                              abort_callback& abort) {
    if (library && !catalogId.empty()) {
        const auto item = FirstData(Apple(ArtistPath(catalogId, storefront, false), abort));
        return item ? VerifiedArtistCatalogId(*item, catalogId) : std::nullopt;
    }
    const auto item = FirstData(Apple(ArtistPath(artist, storefront, library), abort));
    return item ? VerifiedArtistCatalogId(*item, artist) : std::nullopt;
}

stream::Id ResolveLibraryArtistCatalogId(const stream::Id& artist, abort_callback& abort) {
    const auto loaded = LoadArtist(artist, {}, true, abort);
    return loaded ? loaded->catalogId : stream::Id();
}

std::wstring ArtistAlbumsPath(const std::wstring& id, const std::wstring& catalogId,
                              const std::wstring& storefront, bool library, size_t offset, size_t limit) {
    if (library && catalogId.empty()) {
        return L"/v1/me/library/artists/" + Encode(id) + L"/albums?limit=" +
               std::to_wstring(PageLimit(limit)) + L"&offset=" + std::to_wstring(offset) +
               L"&include=catalog,artists&platform=web";
    }
    return L"/v1/catalog/" + storefront + L"/artists/" + Encode(catalogId) + L"/albums?limit=" +
           std::to_wstring(PageLimit(limit)) + L"&offset=" + std::to_wstring(offset);
}

int CountArtistAlbums(const std::wstring& id, const std::wstring& catalogId, const std::wstring& storefront,
                      bool library, abort_callback& abort) {
    const std::wstring path = ArtistAlbumsPath(id, catalogId, storefront, library, 0, 100);
    const size_t count = ParseAlbums(Collection(path, abort)).size();
    return static_cast<int>(std::min(count, static_cast<size_t>(INT_MAX)));
}

Object CreatePlaylistBody(const std::wstring& name) {
    Object attributes;
    attributes.Insert(L"name", JsonValue::CreateStringValue(name));
    Object body;
    body.Insert(L"attributes", attributes);
    return body;
}

Object RenamePlaylistBody(const std::wstring& name) {
    return CreatePlaylistBody(name);
}

Object AppendTracksBody(const std::vector<stream::Id>& tracks) {
    JsonArray data;
    for (const auto& track : tracks) {
        Object item;
        item.Insert(L"id", JsonValue::CreateStringValue(track));
        item.Insert(L"type", JsonValue::CreateStringValue(TrackResourceType(track)));
        data.Append(item);
    }
    Object body;
    body.Insert(L"data", data);
    return body;
}

std::optional<stream::Id> ResolvedLibraryId(const Object& response, const wchar_t* type,
                                            const stream::Id& catalogId) {
    const auto resources = json::GetObject(response, L"resources");
    if (!resources) return std::nullopt;
    const auto typed = json::GetObject(*resources, type);
    if (!typed) return std::nullopt;
    const auto resource = json::GetObject(*typed, catalogId.c_str());
    if (!resource) return std::nullopt;
    for (const auto& library : EmbeddedRelationship(*resource, L"library")) {
        const std::wstring id = json::GetString(library, L"id");
        if (!id.empty()) return id;
    }
    return std::nullopt;
}

stream::Id ResolveLibraryIdFromCatalog(const wchar_t* type, wchar_t libraryPrefix,
                                       const stream::Id& catalogId, const std::wstring& storefront,
                                       abort_callback& abort) {
    const std::wstring path = L"/v1/catalog/" + storefront + L"?relate=library&ids[" + type + L"]=" +
                              Encode(catalogId) + L"&fields[" + type +
                              L"]=inLibrary,personalRating&platform=web&omit[resource]=autos"
                              L"&format[resources]=map";
    const std::wstring libraryId =
        ResolvedLibraryId(Apple(path, abort), type, catalogId).value_or(std::wstring());
    if (!IsLibraryId(libraryId) || libraryId[0] != libraryPrefix)
        throw std::runtime_error("The Apple Music item is not in the library.");
    return libraryId;
}

bool SameArtworkBase(std::wstring_view a, std::wstring_view b) {
    const auto base = [](std::wstring_view value) {
        const size_t query = value.find(L'?');
        value = value.substr(0, query);
        std::wstring copy(value);
        for (std::wstring_view token : {L"{w}", L"{h}"}) {
            size_t at = 0;
            while ((at = copy.find(token, at)) != std::wstring::npos) {
                copy.replace(at, token.size(), L"*");
                at += 1;
            }
        }
        return copy;
    };
    return base(a) == base(b);
}

const bool g_registered = [] {
    stream::Register(StreamProvider::Get());
    return true;
}();

class StreamProviderInit : public initquit {
  public:
    void on_init() override { StreamProvider::Get().Start(); }
    void on_quit() override { StreamProvider::Get().Shutdown(); }
};
FB2K_SERVICE_FACTORY(StreamProviderInit);

} // namespace

StreamProvider& StreamProvider::Get() {
    static StreamProvider provider;
    return provider;
}

StreamProvider::~StreamProvider() {
    Shutdown();
}

void StreamProvider::Post(uint64_t generation, std::function<void()> callback) {
    netease::TaskQueue* tasks = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || generation != m_generation || !m_tasks || !m_abort) return;
        tasks = m_tasks.get();
    }
    tasks->Post([this, generation, callback = std::move(callback)] {
        {
            std::lock_guard lock(m_mutex);
            if (!m_started || generation != m_generation || !m_abort) return;
        }
        callback();
    });
}

void StreamProvider::PostMutation(uint64_t generation, std::function<void()> callback) {
    netease::TaskQueue* tasks = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || generation != m_generation || !m_mutationTasks || !m_abort) return;
        tasks = m_mutationTasks.get();
    }
    tasks->Post([this, generation, callback = std::move(callback)] {
        {
            std::lock_guard lock(m_mutex);
            if (!m_started || generation != m_generation || !m_abort) return;
        }
        callback();
    });
}

bool StreamProvider::PostDetail(uint64_t generation, std::function<void()> callback) {
    netease::TaskQueue* tasks = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || generation != m_generation || !m_detailTasks || !m_abort) return false;
        tasks = m_detailTasks.get();
    }
    tasks->Post([this, generation, callback = std::move(callback)] {
        {
            std::lock_guard lock(m_mutex);
            if (!m_started || generation != m_generation || !m_abort) return;
        }
        callback();
    });
    return true;
}

void StreamProvider::PostMain(uint64_t generation, std::function<void()> callback) {
    if (!callback) return;
    fb2k::inMainThread([this, generation, callback = std::move(callback)] {
        {
            std::lock_guard lock(m_mutex);
            if (!m_started || generation != m_generation) return;
        }
        callback();
    });
}

void StreamProvider::ClearLibraryLocked() {
    m_createdPlaylists.clear();
    m_savedPlaylists.clear();
    m_savedAlbums.clear();
    m_libraryArtists.clear();
    m_playlistTracks.clear();
    m_playlistSongs.clear();
    m_albumTracks.clear();
    m_artistLibraryTracks.clear();
    m_playlistLibraryIds.clear();
    m_albumLibraryIds.clear();
    m_artistCatalogIds.clear();
    m_savedAlbumIds.clear();
    m_editablePlaylists.clear();
    m_renameablePlaylists.clear();
    m_deletablePlaylists.clear();
    m_favoritePlaylistId.clear();
    m_favoriteBindingState = FavoriteBindingState::Unknown;
    m_favoriteBindingReadInFlight = false;
    m_librarySongs.clear();
    m_favoriteSongs.clear();
    m_favoriteIds.clear();
    m_librarySongsLoaded = false;
    m_favoriteSongsLoaded = false;
    m_librarySnapshotLoaded = false;
    m_librarySnapshotValidUntil = {};
}

bool StreamProvider::VerifyCatalogArtist(uint64_t generation, std::wstring_view storefront,
                                         std::wstring_view catalogId, abort_callback& abort) {
    constexpr size_t kMaximumExistenceEntries = 256;
    constexpr auto kExistenceTtl = std::chrono::minutes(5);
    const std::string storefrontKey = Utf8(storefront);
    const std::string catalogKey = Utf8(catalogId);
    const std::array<std::string_view, 2> identity{storefrontKey, catalogKey};
    const std::wstring key = Wide(cache::provider_snapshot::Context(identity));
    std::unique_lock lock(m_mutex);
    for (;;) {
        abort.check();
        if (!m_started || generation != m_generation) throw exception_aborted();
        const auto found = m_artistExistence.find(key);
        if (found == m_artistExistence.end()) {
            if (m_artistExistence.size() >= kMaximumExistenceEntries) {
                std::erase_if(m_artistExistence, [generation](const auto& entry) {
                    return entry.second.known && entry.second.generation != generation;
                });
            }
            if (m_artistExistence.size() >= kMaximumExistenceEntries) {
                const auto oldest =
                    std::min_element(m_artistExistence.begin(), m_artistExistence.end(),
                                     [](const auto& left, const auto& right) {
                                         if (left.second.known != right.second.known)
                                             return left.second.known;
                                         return left.second.validUntil < right.second.validUntil;
                                     });
                if (oldest != m_artistExistence.end() && oldest->second.known) {
                    m_artistExistence.erase(oldest);
                } else {
                    m_artistVerificationCondition.wait_for(lock, std::chrono::milliseconds(100));
                    continue;
                }
            }
            m_artistExistence.emplace(key, ArtistExistenceEntry{generation});
            break;
        }
        if (found->second.generation == generation) {
            if (found->second.known && std::chrono::steady_clock::now() < found->second.validUntil)
                return found->second.exists;
            if (!found->second.known) {
                m_artistVerificationCondition.wait_for(lock, std::chrono::milliseconds(100));
                continue;
            }
        }
        found->second = ArtistExistenceEntry{generation};
        break;
    }
    lock.unlock();

    bool exists = false;
    std::exception_ptr failure;
    try {
        Apple(L"/v1/catalog/" + std::wstring(storefront) + L"/artists/" + Encode(catalogId), abort);
        exists = true;
    } catch (const BridgeHttpError& exception) {
        if (exception.status() != 404) failure = std::current_exception();
    } catch (...) {
        failure = std::current_exception();
    }

    lock.lock();
    const bool current = m_started && generation == m_generation;
    if (current) {
        if (failure) {
            m_artistExistence.erase(key);
        } else {
            ArtistExistenceEntry entry;
            entry.generation = generation;
            entry.known = true;
            entry.exists = exists;
            entry.validUntil = std::chrono::steady_clock::now() + kExistenceTtl;
            m_artistExistence[key] = entry;
        }
    }
    m_artistVerificationCondition.notify_all();
    lock.unlock();
    if (failure) std::rethrow_exception(failure);
    if (!current) throw exception_aborted();
    return exists;
}

void StreamProvider::ClearPlaylistDetailsLocked() {
    ++m_playlistDetailEpoch;
    m_playlistTracks.clear();
    m_playlistSongs.clear();
}

void StreamProvider::CancelPlaylistFetchesLocked() {
    m_playlistFetches.clear();
}

bool StreamProvider::PutIfCurrent(uint64_t generation, const Song& song) {
    std::lock_guard lock(m_mutex);
    if (!m_started || generation != m_generation) return false;
    Catalog::Get().Put(song);
    return true;
}

bool StreamProvider::PutIfCurrent(uint64_t generation, const std::vector<Song>& songs) {
    std::lock_guard lock(m_mutex);
    if (!m_started || generation != m_generation) return false;
    Catalog::Get().Put(songs);
    return true;
}

bool StreamProvider::PutIfCurrent(uint64_t generation, const stream::Playlist& playlist) {
    std::lock_guard lock(m_mutex);
    if (!m_started || generation != m_generation) return false;
    Catalog::Get().Put(playlist);
    return true;
}

bool StreamProvider::PutIfCurrent(uint64_t generation, const stream::Album& album) {
    std::lock_guard lock(m_mutex);
    if (!m_started || generation != m_generation) return false;
    Catalog::Get().Put(album);
    return true;
}

bool StreamProvider::PutIfCurrent(uint64_t generation, const std::vector<stream::Album>& albums) {
    std::lock_guard lock(m_mutex);
    if (!m_started || generation != m_generation) return false;
    Catalog::Get().Put(albums);
    return true;
}

bool StreamProvider::PutIfCurrent(uint64_t generation, const stream::Artist& artist) {
    std::lock_guard lock(m_mutex);
    if (!m_started || generation != m_generation) return false;
    Catalog::Get().Put(artist);
    return true;
}

bool StreamProvider::PutIfCurrent(uint64_t generation, const std::vector<stream::Artist>& artists) {
    std::lock_guard lock(m_mutex);
    if (!m_started || generation != m_generation) return false;
    Catalog::Get().Put(artists);
    return true;
}

void StreamProvider::Start() {
    uint64_t generation = 0;
    {
        std::lock_guard lock(m_mutex);
        if (m_started) return;
        m_started = true;
        ++m_generation;
        generation = m_generation;
        m_tasks = std::make_unique<netease::TaskQueue>(3);
        m_mutationTasks = std::make_unique<netease::TaskQueue>(1);
        m_detailTasks = std::make_unique<netease::TaskQueue>(2);
        m_enrichTasks = std::make_unique<netease::TaskQueue>(1);
        m_abort = std::make_unique<abort_callback_impl>();
        m_locale = CurrentLocale();
        m_account = {};
        m_accountVerified = false;
        m_authPending = true;
        m_sessionRestoring = false;
        m_lastAuthError.clear();
        m_cachedAccountBound = false;
        m_cachedAccountId.clear();
        m_cachedAccountName.clear();
        m_cachedStorefront.clear();
        m_snapshotSavePosted = false;
        m_snapshotSaveDirty = false;
        m_drm = {};
        m_refreshInFlight = false;
        m_refreshGeneration = 0;
        ++m_refreshSequence;
        m_refreshPreservePlaylistDetails = false;
        m_refreshInvalidatesLists = false;
        m_displayChangePosted = false;
        m_refreshWaiters.clear();
        m_artistCountsInFlight = false;
        m_librarySnapshotLoaded = false;
        m_librarySnapshotValidUntil = {};
        m_playlistDetailEpoch = 0;
        m_libraryMutationEpoch = 0;
        m_libraryMutationSequence = 0;
        m_appliedLibraryMutationSequence = 0;
        m_albumMutations.clear();
        m_playlistMutations.clear();
        m_artistPreferenceMutations.clear();
        m_artistPreferenceEpoch = 0;
        m_artistPreferenceSequence = 0;
        m_appliedArtistPreferenceSequence = 0;
        CancelPlaylistFetchesLocked();
        ClearLibraryLocked();
        m_favoriteBindingState = FavoriteBindingState::Unknown;
        ++m_favoriteBindingEpoch;
        m_favoriteBindingReadInFlight = false;
        m_favoritePageWaiters.clear();
    }
    SetAppleLocale(ui::UsingChinese());
    DisplayMetadata::Get().SetSourceStorefront({});
    LoadBootstrapSnapshot(generation);
    try {
        std::string catalogAccountScope = cache::AccountScope("apple");
        const uint64_t catalogAccountRevision = cache::AccountRevision("apple");
        Post(generation, [this, generation, catalogAccountScope = std::move(catalogAccountScope),
                          catalogAccountRevision]() mutable {
            auto songs = Catalog::Get().ReadCacheFile();
            if (songs.empty()) return;
            PostMain(generation, [catalogAccountScope = std::move(catalogAccountScope),
                                  catalogAccountRevision, songs = std::move(songs)]() mutable {
                try {
                    if (cache::AccountRevision("apple") != catalogAccountRevision ||
                        cache::AccountScope("apple") != catalogAccountScope)
                        return;
                } catch (...) {
                    return;
                }
                Catalog::Get().InstallCached(std::move(songs));
            });
        });
    } catch (...) {
    }
    RefreshAccountAsync(generation, {});
}

void StreamProvider::Shutdown() {
    std::optional<cache::provider_snapshot::Snapshot> finalSnapshot;
    std::string finalAccountScope;
    uint64_t finalAccountRevision = 0;
    {
        cache::provider_snapshot::Snapshot payload;
        AppleMusicDisplayRegion region = AppleMusicDisplayRegion::Account;
        std::wstring locale;
        bool save = false;
        {
            std::lock_guard lock(m_mutex);
            save = m_started && m_accountVerified && m_account.signedIn && !m_account.accountId.empty() &&
                   !m_sessionRestoring;
            if (save) {
                region = GetAppleMusicDisplayRegion();
                locale = m_locale;
                payload.provider = "apple";
                payload.locale = Utf8(m_locale);
                payload.accountId = Utf8(m_account.accountId);
                payload.accountName = Utf8(m_account.name);
                payload.sourceStorefront = Utf8(m_account.storefront);
                payload.displayRegion = static_cast<int>(region);
                if (!m_favoritePlaylistId.empty())
                    payload.bindings.push_back({"favorites", Utf8(m_favoritePlaylistId)});
            }
        }
        if (save) {
            try {
                payload.sourceOrigin = BridgeSourceIdentity();
                payload.context = SnapshotContext(region, locale);
                payload.created = RawPlaylists(stream::Shelf::Created);
                payload.saved = RawPlaylists(stream::Shelf::Saved);
                finalAccountScope = cache::AccountScope("apple");
                finalAccountRevision = cache::AccountRevision("apple");
                finalSnapshot = std::move(payload);
            } catch (...) {
            }
        }
    }

    netease::TaskQueue* tasks = nullptr;
    netease::TaskQueue* mutations = nullptr;
    netease::TaskQueue* details = nullptr;
    netease::TaskQueue* enrichment = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started && !m_tasks && !m_mutationTasks && !m_detailTasks && !m_enrichTasks && !m_abort)
            return;
        m_started = false;
        ++m_generation;
        if (m_abort) m_abort->abort();
        m_account = {};
        m_accountVerified = false;
        m_authPending = false;
        m_sessionRestoring = false;
        m_lastAuthError.clear();
        m_cachedAccountBound = false;
        m_snapshotSavePosted = false;
        m_snapshotSaveDirty = false;
        m_cachedAccountId.clear();
        m_cachedAccountName.clear();
        m_cachedStorefront.clear();
        m_drm = {};
        m_refreshInFlight = false;
        m_refreshGeneration = 0;
        ++m_refreshSequence;
        m_refreshPreservePlaylistDetails = false;
        m_refreshInvalidatesLists = false;
        m_displayChangePosted = false;
        m_refreshWaiters.clear();
        m_artistCountsInFlight = false;
        m_libraryMutationEpoch = 0;
        m_libraryMutationSequence = 0;
        m_appliedLibraryMutationSequence = 0;
        m_albumMutations.clear();
        m_playlistMutations.clear();
        m_artistPreferenceMutations.clear();
        m_artistPreferenceEpoch = 0;
        m_artistPreferenceSequence = 0;
        m_appliedArtistPreferenceSequence = 0;
        m_librarySnapshotLoaded = false;
        m_librarySnapshotValidUntil = {};
        CancelPlaylistFetchesLocked();
        ClearLibraryLocked();
        m_favoriteBindingState = FavoriteBindingState::Unknown;
        ++m_favoriteBindingEpoch;
        m_favoriteBindingReadInFlight = false;
        m_favoritePageWaiters.clear();
        tasks = m_tasks.get();
        mutations = m_mutationTasks.get();
        details = m_detailTasks.get();
        enrichment = m_enrichTasks.get();
    }
    m_artistVerificationCondition.notify_all();
    if (tasks) tasks->Stop();
    if (mutations) mutations->Stop();
    if (details) details->Stop();
    if (enrichment) enrichment->Stop();
    {
        std::lock_guard lock(m_mutex);
        m_enrichTasks.reset();
        m_detailTasks.reset();
        m_tasks.reset();
        m_mutationTasks.reset();
        m_abort.reset();
        m_artistExistence.clear();
    }
    if (finalSnapshot) {
        try {
            if (cache::AccountRevision("apple") == finalAccountRevision &&
                cache::AccountScope("apple") == finalAccountScope) {
                cache::provider_snapshot::Save(cache::GetStore(), finalAccountScope, *finalSnapshot);
            }
        } catch (...) {
        }
    }
}

namespace {

// The ids of the favorite songs, kept across runs so the stars are right from the first
// paint and the Favorite Songs playlist is not downloaded at every start just to draw them.
constexpr int64_t kFavoriteIdsFreshSeconds = 6 * 60 * 60;
constexpr char kFavoriteIdsSchema[] = "favorite-song-ids/1";

cache::Key FavoriteIdsKey(std::string accountScope) {
    return {cache::Kind::Data, "apple", std::move(accountScope), "favorite-song-ids", "1"};
}

// Returns when the ids were last confirmed with the service, 0 when there is no record.
int64_t LoadFavoriteIds(const std::string& accountScope, std::set<stream::Id>& ids) {
    try {
        const auto record = cache::GetStore().Lookup(FavoriteIdsKey(accountScope));
        if (!record || record->Info().contentType != kFavoriteIdsSchema) return 0;
        std::ifstream input(record->Path(), std::ios::binary);
        std::string id;
        while (std::getline(input, id))
            if (!id.empty()) ids.insert(Wide(id));
        return record->Info().validatedAt;
    } catch (...) {
        ids.clear();
        return 0;
    }
}

void SaveFavoriteIds(const std::string& accountScope, const std::set<stream::Id>& ids, int64_t checkedAt) {
    try {
        auto writer = cache::GetStore().Begin(FavoriteIdsKey(accountScope));
        if (!writer) return;
        {
            std::ofstream output(writer->Path(), std::ios::binary | std::ios::trunc);
            for (const auto& id : ids)
                if (!id.empty() && id.find_first_of(L"\r\n") == std::wstring::npos) output << Utf8(id) << '\n';
            output.close();
            if (!output) return;
        }
        cache::Metadata metadata;
        metadata.contentType = kFavoriteIdsSchema;
        metadata.validatedAt = checkedAt > 0 ? checkedAt : 1;
        writer->Commit(std::move(metadata));
    } catch (...) {
        // Only saved requests depend on this record.
    }
}

} // namespace

void StreamProvider::SaveFavoriteIdsAsync() {
    uint64_t generation = 0;
    std::set<stream::Id> ids;
    int64_t checkedAt = 0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || !m_accountVerified) return;
        generation = m_generation;
        ids = m_favoriteIds;
        checkedAt = m_favoriteIdsCheckedAt;
    }
    const uint64_t accountRevision = cache::AccountRevision("apple");
    Post(generation, [ids = std::move(ids), checkedAt, accountRevision] {
        try {
            const std::string scope = cache::AccountScope("apple");
            if (cache::AccountRevision("apple") == accountRevision) SaveFavoriteIds(scope, ids, checkedAt);
        } catch (...) {
        }
    });
}

void StreamProvider::LoadBootstrapSnapshot(uint64_t generation) {
    std::string sourceOrigin;
    std::string accountScope;
    std::string context;
    try {
        sourceOrigin = BridgeSourceIdentity();
        accountScope = cache::AccountScope("apple");
        std::wstring locale;
        {
            std::lock_guard lock(m_mutex);
            if (!m_started || generation != m_generation || m_accountVerified) return;
            locale = m_locale;
        }
        context = SnapshotContext(GetAppleMusicDisplayRegion(), locale);
    } catch (...) {
        return;
    }

    const auto cached =
        cache::provider_snapshot::Load(cache::GetStore(), accountScope, "apple", sourceOrigin, context);
    if (!cached || cached->accountId.empty() || cached->sourceStorefront.size() != 2) return;

    std::wstring accountId = Wide(cached->accountId);
    std::wstring accountName = Wide(cached->accountName);
    std::wstring storefront = Wide(cached->sourceStorefront);
    if (accountId.empty() || storefront.size() != 2) return;
    stream::Id favoritePlaylist;
    for (const auto& binding : cached->bindings) {
        if (binding.key != "favorites") continue;
        const stream::Id candidate = Wide(binding.resourceId);
        if (IsLibraryId(candidate) && !candidate.empty() && candidate[0] == L'p')
            favoritePlaylist = candidate;
        break;
    }

    std::set<stream::Id> favoriteIds;
    const int64_t favoriteIdsCheckedAt = LoadFavoriteIds(accountScope, favoriteIds);

    std::vector<stream::Playlist> created;
    std::vector<stream::Playlist> saved;
    std::wstring sourceStorefront;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || generation != m_generation || m_accountVerified) return;
        m_favoriteIds = std::move(favoriteIds);
        m_favoriteIdsCheckedAt = favoriteIdsCheckedAt;
        m_cachedAccountId = std::move(accountId);
        m_cachedAccountName = std::move(accountName);
        m_cachedStorefront = std::move(storefront);
        sourceStorefront = m_cachedStorefront;
        m_cachedAccountBound = true;
        m_sessionRestoring = true;
        m_createdPlaylists = cached->created;
        m_savedPlaylists = cached->saved;
        m_favoritePlaylistId = std::move(favoritePlaylist);
        m_favoriteBindingState =
            m_favoritePlaylistId.empty() ? FavoriteBindingState::Unknown : FavoriteBindingState::Ready;
        m_librarySnapshotLoaded = false;
        created = m_createdPlaylists;
        saved = m_savedPlaylists;
    }

    DisplayMetadata::Get().SetSourceStorefront(std::move(sourceStorefront));
    Catalog::Get().Put(created);
    Catalog::Get().Put(saved);
    NotifyLibraryChanged();
}

void StreamProvider::QueueSnapshotSave() {
    cache::provider_snapshot::Snapshot payload;
    uint64_t generation = 0;
    uint64_t accountRevision = 0;
    std::string accountScope;
    AppleMusicDisplayRegion region = AppleMusicDisplayRegion::Account;
    std::wstring locale;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || !m_accountVerified || !m_account.signedIn || m_account.accountId.empty() ||
            m_sessionRestoring)
            return;
        m_snapshotSaveDirty = true;
        if (m_snapshotSavePosted) return;
        m_snapshotSavePosted = true;
        m_snapshotSaveDirty = false;
        generation = m_generation;
        locale = m_locale;
        region = GetAppleMusicDisplayRegion();
        payload.provider = "apple";
        payload.locale = Utf8(m_locale);
        payload.accountId = Utf8(m_account.accountId);
        payload.accountName = Utf8(m_account.name);
        payload.sourceStorefront = Utf8(m_account.storefront);
        payload.displayRegion = static_cast<int>(region);
        if (!m_favoritePlaylistId.empty())
            payload.bindings.push_back({"favorites", Utf8(m_favoritePlaylistId)});
    }

    try {
        accountScope = cache::AccountScope("apple");
        accountRevision = cache::AccountRevision("apple");
        payload.sourceOrigin = BridgeSourceIdentity();
    } catch (...) {
        std::lock_guard lock(m_mutex);
        if (generation == m_generation) m_snapshotSavePosted = false;
        return;
    }
    payload.context = SnapshotContext(region, locale);
    payload.created = RawPlaylists(stream::Shelf::Created);
    payload.saved = RawPlaylists(stream::Shelf::Saved);

    Post(generation, [this, generation, accountRevision, accountScope = std::move(accountScope),
                      payload = std::move(payload)]() mutable {
        bool allowed = false;
        {
            std::lock_guard lock(m_mutex);
            allowed = m_started && generation == m_generation && m_accountVerified && m_account.signedIn &&
                      !m_sessionRestoring && cache::AccountRevision("apple") == accountRevision;
        }
        if (allowed) {
            try {
                if (cache::AccountScope("apple") == accountScope)
                    cache::provider_snapshot::Save(cache::GetStore(), accountScope, payload);
            } catch (...) {
            }
        }
        fb2k::inMainThread([this, generation] { CompleteSnapshotSave(generation); });
    });
}

void StreamProvider::CompleteSnapshotSave(uint64_t generation) {
    bool rerun = false;
    {
        std::lock_guard lock(m_mutex);
        if (generation != m_generation) return;
        m_snapshotSavePosted = false;
        rerun = m_snapshotSaveDirty && m_started && m_accountVerified && m_account.signedIn &&
                !m_sessionRestoring;
        if (!rerun) m_snapshotSaveDirty = false;
    }
    if (rerun) QueueSnapshotSave();
}

void StreamProvider::StartFavoriteBindingRead(uint64_t generation, uint64_t bindingEpoch) {
    Post(generation, [this, generation, bindingEpoch] {
        std::vector<Object> playlists;
        std::wstring error;
        try {
            playlists = Collection(L"/v1/me/library/playlists?limit=100&include=catalog"
                                   L"&extend[library-playlists]=tags&fields[library-playlists]=name,artwork,"
                                   L"editorialArtwork,tags,canEdit,canDelete,canRename,trackCount,playParams"
                                   L"&platform=web",
                                   *m_abort);
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        stream::Id favorite;
        if (error.empty()) {
            for (const auto& object : playlists) {
                if (!IsFavoriteSystemPlaylist(object)) continue;
                favorite = json::GetString(object, L"id");
                break;
            }
        }
        fb2k::inMainThread([this, generation, bindingEpoch, favorite = std::move(favorite),
                            complete = error.empty(), error = std::move(error)]() mutable {
            CompleteFavoriteBinding(generation, bindingEpoch, std::move(favorite), complete,
                                    std::move(error));
        });
    });
}

void StreamProvider::CompleteFavoriteBinding(uint64_t generation, uint64_t bindingEpoch, stream::Id playlist,
                                             bool complete, std::wstring error) {
    std::vector<FavoritePageWaiter> waiters;
    FavoriteBindingState state = FavoriteBindingState::Unknown;
    stream::Id favorite;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || generation != m_generation || bindingEpoch != m_favoriteBindingEpoch) return;
        m_favoriteBindingReadInFlight = false;
        if (!complete) {
            const bool knownBinding =
                m_favoriteBindingState == FavoriteBindingState::Empty ||
                (m_favoriteBindingState == FavoriteBindingState::Ready && !m_favoritePlaylistId.empty());
            if (!knownBinding) {
                m_favoriteBindingState = FavoriteBindingState::Unknown;
                m_favoritePlaylistId.clear();
            }
        } else if (IsLibraryId(playlist) && !playlist.empty() && playlist[0] == L'p') {
            m_favoriteBindingState = FavoriteBindingState::Ready;
            m_favoritePlaylistId = std::move(playlist);
        } else {
            m_favoriteBindingState = FavoriteBindingState::Empty;
            m_favoritePlaylistId.clear();
        }
        state = m_favoriteBindingState;
        favorite = m_favoritePlaylistId;
        waiters.swap(m_favoritePageWaiters);
    }

    for (auto& waiter : waiters) {
        {
            std::lock_guard lock(m_mutex);
            if (!m_started || waiter.generation != m_generation) continue;
        }
        if (ListCancelled(waiter.query.cancelled) || !waiter.done) continue;
        if (!error.empty()) {
            waiter.done({}, error);
        } else if (state == FavoriteBindingState::Ready) {
            waiter.query.id = favorite;
            FetchListPage(waiter.query, std::move(waiter.cursor), waiter.limit, std::move(waiter.done));
        } else {
            stream::ListPage empty;
            empty.rows = std::vector<stream::Track>{};
            empty.total = 0;
            waiter.done(std::move(empty), {});
        }
    }
    if (complete) QueueSnapshotSave();
}

stream::Capabilities StreamProvider::Offers() const {
    using enum stream::Capability;
    return {ReadFavorites,     AddFavorite,     RemoveFavorite,       LibraryArtists,
            FavoriteArtists,   LibraryAlbums,   SaveAlbums,           LibraryPlaylists,
            SavePlaylists,     CreatePlaylists, AppendPlaylistTracks, RemovePlaylistTracks,
            RenamePlaylists,   DeletePlaylists, CatalogSearch,        SearchSuggestions,
            NativeLyrics};
}

// Apple Music keeps things in a "library" and marks artists as favorites; its playlists are
// translated in its own context (播放列表 rather than 歌单).
std::wstring StreamProvider::Wording(std::wstring_view english) const {
    static const std::map<std::wstring_view, std::wstring_view> terms{
        {L"Collect album", L"Add to Library"},     {L"Uncollect album", L"Remove from Library"},
        {L"Collect", L"Add to Library"},           {L"Collected", L"Added to Library"},
        {L"Follow artist", L"Favorite artist"},    {L"Unfollow artist", L"Unfavorite artist"},
    };
    const auto found = terms.find(english);
    return std::wstring(found == terms.end() ? english : found->second);
}

std::wstring StreamProvider::ArtistTracksLabel(const stream::Id& artist) const {
    const bool libraryOnly = IsLibraryId(artist) && artist[0] == L'r' && ArtistCatalogId(artist).empty();
    return libraryOnly ? L"Songs" : L"Hot songs";
}

std::vector<stream::ArtistSection> StreamProvider::ArtistSections(const stream::Id& artist) const {
    if (artist.empty()) return {};
    using Kind = stream::ArtistSectionRowKind;
    return {
        {L"top-songs", L"Hot songs", Kind::Tracks},
        {L"featured-albums", L"Featured albums", Kind::Albums},
        {L"full-albums", L"Full albums", Kind::Albums},
        {L"singles", L"Singles", Kind::Albums},
        {L"live-albums", L"Live albums", Kind::Albums},
        {L"appears-on-albums", L"Appears on", Kind::Albums},
        {L"compilation-albums", L"Compilation albums", Kind::Albums},
        {L"featured-playlists", L"Featured playlists", Kind::Playlists},
    };
}

std::vector<stream::ListKind> StreamProvider::CatalogSearchKinds() const {
    return {
        stream::ListKind::CatalogSearchTopResults, stream::ListKind::CatalogSearch,
        stream::ListKind::CatalogSearchArtists,    stream::ListKind::CatalogSearchAlbums,
        stream::ListKind::CatalogSearchPlaylists,
    };
}

std::vector<stream::NavigationSection> StreamProvider::Sections() const {
    using Kind = stream::SectionKind;
    using Shelf = stream::Shelf;
    return {
        {kFavoritesSection, L"Favorite songs", Kind::Favorites, Shelf::Created, false},
        {kSongsSection, L"Songs", Kind::Tracks, Shelf::Created, false},
        {kArtistsSection, L"Artists", Kind::Artists, Shelf::Created, false},
        {kAlbumsSection, L"Albums", Kind::Albums, Shelf::Created, false},
        {kGenresSection, L"Genres", Kind::Groups, Shelf::Created, false},
        {kComposersSection, L"Composers", Kind::Groups, Shelf::Created, false, stream::GroupFacet::Composer},
        {kCreatedSection, L"My playlists", Kind::PlaylistShelf, Shelf::Created, false},
        {kSavedSection, L"Saved playlists", Kind::PlaylistShelf, Shelf::Saved, false},
    };
}

void StreamProvider::OnLanguageChanged() {
    const bool chinese = ui::UsingChinese();
    SetAppleLocale(chinese);
    const std::wstring locale = CurrentLocale();
    uint64_t generation = 0;
    {
        std::lock_guard lock(m_mutex);
        if (locale == m_locale) return;
        m_locale = locale;
        if (!m_started) return;
        ++m_generation;
        generation = m_generation;
        m_refreshInvalidatesLists = false;
        CancelPlaylistFetchesLocked();
        m_artistVerificationCondition.notify_all();
    }
    Catalog::Get().RefreshHints();
    Catalog::Get().ClearMemory();
    NotifyLibraryChanged();
    NotifyLanguageChanged();
    RefreshAccountAsync(generation, {});
}

std::vector<settings::Field> StreamProvider::SettingFields() {
    using Field = settings::Field;
    SetAtmosRendererReady(atmos::Renderer::Available());
    std::vector<Field> fields;

    Field account;
    account.kind = Field::Kind::Note;
    account.label = L"Account";
    account.note = [this] {
        if (!SignedIn()) return std::wstring(L"Not signed in");
        const std::wstring name = AccountName();
        return name.empty() ? std::wstring(L"Signed in") : L"Signed in as " + name;
    };
    fields.push_back(std::move(account));

    Field displayNames;
    displayNames.kind = Field::Kind::Choice;
    displayNames.label = L"Apple Music metadata region";
    displayNames.choices = {L"Account region", L"Japan", L"China"};
    displayNames.defaultValue = static_cast<int>(AppleMusicDisplayRegion::Account);
    displayNames.get = [] { return static_cast<int>(GetAppleMusicDisplayRegion()); };
    displayNames.set = [this](int value) {
        const auto region = static_cast<AppleMusicDisplayRegion>(value);
        if (GetAppleMusicDisplayRegion() == region) return;
        SetAppleMusicDisplayRegion(region);
        OnDisplayMetadataChanged();
    };
    fields.push_back(std::move(displayNames));

    Field playback;
    playback.kind = Field::Kind::Note;
    playback.label = L"Playback authorization";
    playback.note = [this] {
        std::lock_guard lock(m_mutex);
        if (!m_drm.available) return std::wstring(L"DRM helper unavailable");
        if (m_drm.authentication == L"logged_in" && m_drm.fairplay == L"ready") return std::wstring(L"Ready");
        if (!m_drm.authentication.empty()) return L"Account: " + m_drm.authentication;
        return std::wstring(L"Checking");
    };
    fields.push_back(std::move(playback));

    Field signIn;
    signIn.kind = Field::Kind::Button;
    signIn.label = L"Sign in...";
    signIn.press = [this](HWND owner) { SignIn(owner); };
    fields.push_back(std::move(signIn));

    Field signOut;
    signOut.kind = Field::Kind::Button;
    signOut.label = L"Sign out";
    signOut.press = [this](HWND) {
        if (SignedIn()) SignOut();
    };
    fields.push_back(std::move(signOut));

    Field renderer;
    renderer.kind = Field::Kind::Note;
    renderer.label = L"Dolby Atmos renderer";
    renderer.note = [] {
        return IsAtmosRendererReady() ? ui::Tr(L"Ready") : ui::Tr(L"Renderer unavailable; using stereo.");
    };
    fields.push_back(std::move(renderer));

    Field atmos;
    atmos.kind = Field::Kind::Choice;
    atmos.label = L"Dolby Atmos";
    atmos.choices = {L"Off", L"On"};
    atmos.defaultValue = 0;
    atmos.get = [] { return GetAtmosEnabled() ? 1 : 0; };
    atmos.set = [](int value) { SetAtmosEnabled(value != 0); };
    fields.push_back(std::move(atmos));

    Field output;
    output.kind = Field::Kind::Choice;
    output.label = L"Render output";
    output.choices = {L"Stereo speakers", L"Headphones", L"5.1 speakers", L"7.1 speakers"};
    output.defaultValue = static_cast<int>(AtmosOutput::StereoSpeakers);
    output.get = [] { return static_cast<int>(GetAtmosOutput()); };
    output.set = [](int value) { SetAtmosOutput(static_cast<AtmosOutput>(value)); };
    fields.push_back(std::move(output));

    Field atmosMix;
    atmosMix.kind = Field::Kind::Note;
    atmosMix.label = L"Dolby Atmos mix";
    atmosMix.note = [] { return ui::Tr(L"Off: highest lossless; on: Atmos if available."); };
    fields.push_back(std::move(atmosMix));

    Field speakerLimit;
    speakerLimit.kind = Field::Kind::Note;
    speakerLimit.label = L"Output details";
    speakerLimit.note = [] {
        switch (GetAtmosOutput()) {
        case AtmosOutput::Headphones:
            return ui::Tr(L"Headphones: HRTF with 6 dB headroom.");
        case AtmosOutput::Speakers51:
            return ui::Tr(L"5.1 speakers: six output channels.");
        case AtmosOutput::Speakers71:
            return ui::Tr(L"7.1 speakers: eight output channels.");
        default:
            return ui::Tr(L"Stereo speakers: two output channels.");
        }
    };
    fields.push_back(std::move(speakerLimit));

    Field applies;
    applies.kind = Field::Kind::Note;
    applies.label = L"When settings apply";
    applies.note = [] { return ui::Tr(L"The next track uses the new settings."); };
    fields.push_back(std::move(applies));
    return fields;
}

bool StreamProvider::SignedIn() const {
    std::lock_guard lock(m_mutex);
    return m_account.signedIn;
}

bool StreamProvider::SessionRestoring() const {
    std::lock_guard lock(m_mutex);
    return m_sessionRestoring || m_authPending;
}

bool StreamProvider::AccountVerified() const {
    std::lock_guard lock(m_mutex);
    return m_accountVerified;
}

std::wstring StreamProvider::SessionRestoreError() const {
    std::lock_guard lock(m_mutex);
    return m_accountVerified ? std::wstring() : m_lastAuthError;
}

bool StreamProvider::RetrySessionRestore(Done done) {
    uint64_t generation = 0;
    bool started = false;
    {
        std::lock_guard lock(m_mutex);
        started = m_started && m_abort;
        generation = m_generation;
    }
    if (!started) {
        if (done) done(L"Apple Music is not running.");
        return false;
    }
    // RefreshAccountAsync joins the generation's active request through m_refreshInFlight.
    // That makes every foreground caller share one account HTTP attempt.
    RefreshAccountAsync(generation, std::move(done), true);
    return true;
}

std::wstring StreamProvider::AccountName() const {
    std::lock_guard lock(m_mutex);
    if (!m_account.name.empty()) return m_account.name;
    if (m_sessionRestoring) return m_cachedAccountName;
    return m_account.storefront.empty() ? std::wstring() : m_account.storefront;
}

bool StreamProvider::RequireSignedIn(std::wstring& error) const {
    std::lock_guard lock(m_mutex);
    if (m_account.signedIn) return true;
    error = L"Please sign in to Apple Music.";
    return false;
}

std::wstring StreamProvider::Storefront() const {
    std::lock_guard lock(m_mutex);
    return m_account.signedIn && m_account.storefront.size() == 2 ? m_account.storefront
                                                                  : std::wstring(L"US");
}

stream::Id StreamProvider::CatalogIdForAlbum(const stream::Id& id) const {
    if (!IsLibraryId(id) || id.empty() || id[0] != L'l') return id;
    {
        std::lock_guard lock(m_mutex);
        const auto found = std::find_if(m_albumLibraryIds.begin(), m_albumLibraryIds.end(),
                                        [&id](const auto& item) { return item.second == id; });
        if (found != m_albumLibraryIds.end()) return found->first;
    }
    return Catalog::Get().FindAlbumCatalogId(id);
}

StreamProvider::AlbumRequest StreamProvider::ResolveAlbumRequest(const stream::Id& id) const {
    if (!IsLibraryId(id) || id.empty() || id[0] != L'l') return {id, false};
    const stream::Id catalogId = CatalogIdForAlbum(id);
    return catalogId.empty() ? AlbumRequest{id, true} : AlbumRequest{catalogId, false};
}

stream::Id StreamProvider::CatalogIdForArtist(const stream::Id& id) const {
    if (!IsLibraryId(id) || id.empty() || id[0] != L'r') return id;
    std::lock_guard lock(m_mutex);
    const auto found = m_artistCatalogIds.find(id);
    return found == m_artistCatalogIds.end() ? stream::Id() : found->second;
}

stream::Id StreamProvider::CatalogIdForPlaylist(const stream::Id& id) const {
    if (!IsLibraryId(id) || id.empty() || id[0] != L'p') return id;
    std::lock_guard lock(m_mutex);
    const auto found = std::find_if(m_playlistLibraryIds.begin(), m_playlistLibraryIds.end(),
                                    [&id](const auto& item) { return item.second == id; });
    return found == m_playlistLibraryIds.end() ? stream::Id() : found->first;
}

void StreamProvider::ResolveSongDisplayNames(std::vector<Song>& songs, uint64_t generation,
                                             abort_callback& abort) {
    const AppleMusicDisplayRegion region = GetAppleMusicDisplayRegion();
    const auto target = DisplayTargetFor(region);
    if (!target) return;
    const uint64_t displayEpoch = DisplayMetadata::Get().Epoch();
    const std::wstring sourceStorefront = DisplayMetadata::Get().SourceStorefront();
    const auto canCommit = [this, generation, region] {
        std::lock_guard lock(m_mutex);
        return m_started && generation == m_generation && GetAppleMusicDisplayRegion() == region;
    };

    struct Pending {
        Song* source = nullptr;
        stream::Id metadataId;
    };
    std::vector<Pending> pending;
    pending.reserve(songs.size());
    for (auto& song : songs) {
        if (song.track.id.empty() ||
            DisplayMetadata::Get().IsSongComplete(region, sourceStorefront, song.track.id))
            continue;
        const stream::Id metadataId = !song.reportingId.empty() ? song.reportingId : song.catalogId;
        if (metadataId.empty()) {
            if (canCommit())
                DisplayMetadata::Get().PutSongFor(region, sourceStorefront, song.track.id, {}, true,
                                                  displayEpoch);
            continue;
        }
        pending.push_back({&song, metadataId});
    }

    for (size_t first = 0; first < pending.size(); first += kDisplayMetadataBatch) {
        abort.check();
        if (!canCommit()) return;
        const size_t last = std::min(pending.size(), first + kDisplayMetadataBatch);
        std::vector<stream::Id> ids;
        ids.reserve(last - first);
        for (size_t i = first; i < last; ++i)
            ids.push_back(pending[i].metadataId);
        try {
            const Object response =
                Apple(L"/v1/catalog/" + target->storefront + L"/songs?ids=" + Encode(JoinedIds(ids)) +
                          L"&include=albums,artists&platform=web&l=" + target->locale,
                      abort);
            std::unordered_map<stream::Id, Song> localized;
            if (const auto data = json::GetArray(response, L"data"))
                for (auto& song : ParseSongs(json::Objects(*data)))
                    localized[song.track.id] = song;
            if (!canCommit()) return;
            for (size_t i = first; i < last; ++i) {
                if (!canCommit()) return;
                Song* source = pending[i].source;
                const auto found = localized.find(pending[i].metadataId);
                if (found == localized.end()) {
                    DisplayMetadata::Get().PutSongFor(region, sourceStorefront, source->track.id, {}, true,
                                                      displayEpoch);
                } else {
                    StoreSongDisplayNames(region, displayEpoch, *source, found->second, sourceStorefront);
                }
            }
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& exception) {
            netease::Log(L"Apple Music regional song names failed: " +
                         Wide(std::string_view(exception.what())));
            if (DefiniteNotFound(exception) && canCommit())
                for (size_t i = first; i < last; ++i)
                    DisplayMetadata::Get().PutSongFor(region, sourceStorefront, pending[i].source->track.id,
                                                      {}, true, displayEpoch);
        } catch (...) {
            netease::Log(L"Apple Music regional song names failed.");
        }
    }
}

void StreamProvider::ResolveAlbumDisplayNames(std::vector<stream::Album>& albums, uint64_t generation,
                                              abort_callback& abort) {
    const AppleMusicDisplayRegion region = GetAppleMusicDisplayRegion();
    const auto target = DisplayTargetFor(region);
    if (!target) return;
    const uint64_t displayEpoch = DisplayMetadata::Get().Epoch();
    const std::wstring sourceStorefront = DisplayMetadata::Get().SourceStorefront();
    const auto canCommit = [this, generation, region] {
        std::lock_guard lock(m_mutex);
        return m_started && generation == m_generation && GetAppleMusicDisplayRegion() == region;
    };

    struct Pending {
        stream::Album* source = nullptr;
        stream::Id catalogId;
    };
    std::vector<Pending> pending;
    for (auto& album : albums) {
        if (album.id.empty() || DisplayMetadata::Get().IsAlbumComplete(region, sourceStorefront, album.id))
            continue;
        const stream::Id catalogId = CatalogIdForAlbum(album.id);
        if (catalogId.empty()) {
            if (canCommit())
                DisplayMetadata::Get().PutAlbumFor(region, sourceStorefront, album.id, {}, true,
                                                   displayEpoch);
        } else {
            pending.push_back({&album, catalogId});
        }
    }

    for (size_t first = 0; first < pending.size(); first += kDisplayMetadataBatch) {
        abort.check();
        if (!canCommit()) return;
        const size_t last = std::min(pending.size(), first + kDisplayMetadataBatch);
        std::vector<stream::Id> ids;
        ids.reserve(last - first);
        for (size_t i = first; i < last; ++i)
            ids.push_back(pending[i].catalogId);
        try {
            const Object response =
                Apple(L"/v1/catalog/" + target->storefront + L"/albums?ids=" + Encode(JoinedIds(ids)) +
                          L"&include=artists&platform=web&l=" + target->locale,
                      abort);
            std::unordered_map<stream::Id, stream::Album> localized;
            if (const auto data = json::GetArray(response, L"data"))
                for (auto& album : ParseAlbums(json::Objects(*data)))
                    localized[album.id] = album;
            if (!canCommit()) return;
            for (size_t i = first; i < last; ++i) {
                if (!canCommit()) return;
                const Pending& item = pending[i];
                const auto found = localized.find(item.catalogId);
                if (found == localized.end()) {
                    DisplayMetadata::Get().PutAlbumFor(region, sourceStorefront, item.source->id, {}, true,
                                                       displayEpoch);
                } else {
                    StoreAlbumDisplayNames(region, displayEpoch, *item.source, found->second,
                                           sourceStorefront);
                }
            }
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& exception) {
            netease::Log(L"Apple Music regional album names failed: " +
                         Wide(std::string_view(exception.what())));
            if (DefiniteNotFound(exception) && canCommit())
                for (size_t i = first; i < last; ++i)
                    DisplayMetadata::Get().PutAlbumFor(region, sourceStorefront, pending[i].source->id, {},
                                                       true, displayEpoch);
        } catch (...) {
            netease::Log(L"Apple Music regional album names failed.");
        }
    }
}

void StreamProvider::ResolveArtistDisplayNames(std::vector<stream::Artist>& artists, uint64_t generation,
                                               abort_callback& abort) {
    const AppleMusicDisplayRegion region = GetAppleMusicDisplayRegion();
    const auto target = DisplayTargetFor(region);
    if (!target) return;
    const uint64_t displayEpoch = DisplayMetadata::Get().Epoch();
    const std::wstring sourceStorefront = DisplayMetadata::Get().SourceStorefront();
    const auto canCommit = [this, generation, region] {
        std::lock_guard lock(m_mutex);
        return m_started && generation == m_generation && GetAppleMusicDisplayRegion() == region;
    };

    struct Pending {
        stream::Artist* source = nullptr;
        stream::Id catalogId;
    };
    std::vector<Pending> pending;
    for (auto& artist : artists) {
        if (artist.id.empty() || DisplayMetadata::Get().IsArtistComplete(region, sourceStorefront, artist.id))
            continue;
        const stream::Id catalogId = CatalogIdForArtist(artist.id);
        if (catalogId.empty()) {
            if (canCommit())
                DisplayMetadata::Get().PutArtistFor(region, sourceStorefront, artist.id, {}, true,
                                                    displayEpoch);
        } else {
            pending.push_back({&artist, catalogId});
        }
    }

    for (size_t first = 0; first < pending.size(); first += kDisplayMetadataBatch) {
        abort.check();
        if (!canCommit()) return;
        const size_t last = std::min(pending.size(), first + kDisplayMetadataBatch);
        std::vector<stream::Id> ids;
        ids.reserve(last - first);
        for (size_t i = first; i < last; ++i)
            ids.push_back(pending[i].catalogId);
        try {
            const Object response = Apple(L"/v1/catalog/" + target->storefront + L"/artists?ids=" +
                                              Encode(JoinedIds(ids)) + L"&platform=web&l=" + target->locale,
                                          abort);
            std::unordered_map<stream::Id, stream::Artist> localized;
            if (const auto data = json::GetArray(response, L"data"))
                for (auto& artist : ParseArtists(json::Objects(*data)))
                    localized[artist.id] = artist;
            if (!canCommit()) return;
            for (size_t i = first; i < last; ++i) {
                if (!canCommit()) return;
                const Pending& item = pending[i];
                const auto found = localized.find(item.catalogId);
                if (found == localized.end()) {
                    DisplayMetadata::Get().PutArtistFor(region, sourceStorefront, item.source->id, {}, true,
                                                        displayEpoch);
                } else {
                    StoreArtistDisplayNames(region, displayEpoch, *item.source, found->second,
                                            sourceStorefront);
                }
            }
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& exception) {
            netease::Log(L"Apple Music regional artist names failed: " +
                         Wide(std::string_view(exception.what())));
            if (DefiniteNotFound(exception) && canCommit())
                for (size_t i = first; i < last; ++i)
                    DisplayMetadata::Get().PutArtistFor(region, sourceStorefront, pending[i].source->id, {},
                                                        true, displayEpoch);
        } catch (...) {
            netease::Log(L"Apple Music regional artist names failed.");
        }
    }
}

void StreamProvider::ResolvePlaylistDisplayNames(std::vector<stream::Playlist>& playlists,
                                                 uint64_t generation, abort_callback& abort) {
    const AppleMusicDisplayRegion region = GetAppleMusicDisplayRegion();
    const auto target = DisplayTargetFor(region);
    if (!target) return;
    const uint64_t displayEpoch = DisplayMetadata::Get().Epoch();
    const std::wstring sourceStorefront = DisplayMetadata::Get().SourceStorefront();
    const auto canCommit = [this, generation, region] {
        std::lock_guard lock(m_mutex);
        return m_started && generation == m_generation && GetAppleMusicDisplayRegion() == region;
    };

    struct Pending {
        stream::Playlist* source = nullptr;
        stream::Id catalogId;
    };
    std::vector<Pending> pending;
    for (auto& playlist : playlists) {
        if (playlist.id.empty() || playlist.own ||
            DisplayMetadata::Get().IsPlaylistComplete(region, sourceStorefront, playlist.id))
            continue;
        const stream::Id catalogId = CatalogIdForPlaylist(playlist.id);
        if (catalogId.empty()) {
            if (canCommit())
                DisplayMetadata::Get().PutPlaylistFor(region, sourceStorefront, playlist.id, {}, true,
                                                      displayEpoch);
        } else {
            pending.push_back({&playlist, catalogId});
        }
    }

    for (const Pending& item : pending) {
        abort.check();
        if (!canCommit()) return;
        try {
            const Object response = Apple(L"/v1/catalog/" + target->storefront + L"/playlists/" +
                                              Encode(item.catalogId) + L"?platform=web&l=" + target->locale,
                                          abort);
            if (!canCommit()) return;
            std::optional<stream::Playlist> localized;
            if (const auto data = json::GetArray(response, L"data")) {
                const auto objects = json::Objects(*data);
                if (!objects.empty() && json::GetString(objects.front(), L"id") == item.catalogId)
                    localized = ParsePlaylist(objects.front());
            }
            if (!localized) {
                DisplayMetadata::Get().PutPlaylistFor(region, sourceStorefront, item.source->id, {}, true,
                                                      displayEpoch);
            } else {
                PlaylistDisplayNames names;
                names.name = localized->name;
                names.description = localized->description;
                names.coverUrl = localized->coverUrl;
                DisplayMetadata::Get().PutPlaylistFor(region, sourceStorefront, item.source->id,
                                                      std::move(names), true, displayEpoch);
            }
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& exception) {
            netease::Log(L"Apple Music regional playlist names failed: " +
                         Wide(std::string_view(exception.what())));
            if (DefiniteNotFound(exception) && canCommit())
                DisplayMetadata::Get().PutPlaylistFor(region, sourceStorefront, item.source->id, {}, true,
                                                      displayEpoch);
        } catch (...) {
            netease::Log(L"Apple Music regional playlist names failed.");
        }
    }
}

void StreamProvider::ApplyArtistPreferenceOverlay(stream::Artist& artist, uint64_t capturedEpoch) const {
    std::lock_guard lock(m_mutex);
    ApplyArtistPreferenceOverlayLocked(artist, capturedEpoch);
}

void StreamProvider::ApplyArtistPreferenceOverlayLocked(stream::Artist& artist,
                                                        uint64_t capturedEpoch) const {
    const auto mutation = std::find_if(
        m_artistPreferenceMutations.begin(), m_artistPreferenceMutations.end(), [&artist](const auto& entry) {
            return entry.first == artist.id || entry.second.sourceId == artist.id ||
                   entry.second.catalogId == artist.id;
        });
    if (mutation == m_artistPreferenceMutations.end()) return;
    if (mutation->second.pending && capturedEpoch >= mutation->second.epoch && artist.followKnown) {
        if (artist.followed == mutation->second.favorite) {
            // A successful read started after ACK confirms the intent. Later reads no longer
            // need the overlay and may expose a subsequent change made on another device.
            mutation->second.pending = false;
            mutation->second.epoch = ++m_artistPreferenceEpoch;
            return;
        }
    }
    if (!mutation->second.pending && capturedEpoch >= mutation->second.epoch) return;
    artist.followKnown = true;
    artist.followed = mutation->second.favorite;
}

void StreamProvider::OnDisplayMetadataChanged() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || m_displayChangePosted) return;
        m_displayChangePosted = true;
    }
    // Coalesce rapid region changes and let the settings field finish its current event.
    fb2k::inMainThread([this] { ApplyDisplayMetadataChange(); });
}

void StreamProvider::ApplyDisplayMetadataChange() {
    {
        std::lock_guard lock(m_mutex);
        m_displayChangePosted = false;
        if (!m_started) return;
        CancelPlaylistFetchesLocked();
    }

    // The account snapshot, library records, IDs and detail records are region-independent.
    // Rotate only the display epoch so old async jobs cannot publish into the new projection.
    DisplayMetadata::Get().BeginTransition();
    NotifyDisplayChanged();
    QueueSnapshotSave();
}

void StreamProvider::SignIn(HWND owner) {
    ShowLogin(owner, [this] {
        uint64_t generation = 0;
        {
            std::lock_guard lock(m_mutex);
            if (!m_started) return;
            generation = m_generation;
            m_authPending = true;
            m_lastAuthError.clear();
            ++m_libraryMutationEpoch;
            m_libraryMutationSequence = 0;
            m_appliedLibraryMutationSequence = 0;
            m_albumMutations.clear();
            m_playlistMutations.clear();
            m_artistPreferenceMutations.clear();
            ++m_artistPreferenceEpoch;
            m_artistPreferenceSequence = 0;
            m_appliedArtistPreferenceSequence = 0;
        }
        // A completed sign-in starts a fresh opaque account generation for provider caches.
        cache::RotateAccountScope(Utf8(Key()));
        RefreshAccountAsync(generation, {});
    });
}

void StreamProvider::SignOut() {
    uint64_t generation = 0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started) return;
        ++m_generation;
        generation = m_generation;
        m_account = {};
        m_accountVerified = false;
        m_authPending = false;
        m_sessionRestoring = false;
        m_lastAuthError.clear();
        m_drm = {};
        m_refreshInFlight = false;
        m_refreshGeneration = 0;
        ++m_refreshSequence;
        m_refreshPreservePlaylistDetails = false;
        m_refreshInvalidatesLists = false;
        m_refreshWaiters.clear();
        m_artistCountsInFlight = false;
        m_artistVerificationCondition.notify_all();
        ++m_libraryMutationEpoch;
        m_libraryMutationSequence = 0;
        m_appliedLibraryMutationSequence = 0;
        m_albumMutations.clear();
        m_playlistMutations.clear();
        m_artistPreferenceMutations.clear();
        ++m_artistPreferenceEpoch;
        m_artistPreferenceSequence = 0;
        m_appliedArtistPreferenceSequence = 0;
        CancelPlaylistFetchesLocked();
        ClearLibraryLocked();
        m_favoritePageWaiters.clear();
    }
    cache::RotateAccountScope(Utf8(Key()));
    Catalog::Get().Clear();
    DisplayMetadata::Get().Clear();
    NotifyAccountChanged();
    NotifyLibraryChanged();
    Post(generation, [this, generation] {
        try {
            abort_callback& abort = *m_abort;
            Request(L"/api/v1/drm/logout", abort, L"POST", "{}");
        } catch (const exception_aborted&) {
        } catch (...) {
        }
        PostMain(generation, [] {});
    });
}

void StreamProvider::RefreshAccountAsync(uint64_t generation, Done done, bool preservePlaylistDetails) {
    std::vector<Done> superseded;
    std::wstring startError;
    uint64_t mutationEpoch = 0;
    uint64_t artistPreferenceEpoch = 0;
    uint64_t refreshToken = 0;
    uint64_t favoriteBindingEpoch = 0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || generation != m_generation || !m_abort) {
            startError = L"Apple Music is not running.";
        } else if (m_refreshInFlight) {
            if (m_refreshGeneration == generation) {
                m_refreshPreservePlaylistDetails |= preservePlaylistDetails;
                if (done) m_refreshWaiters.push_back(std::move(done));
                return;
            }
            superseded.swap(m_refreshWaiters);
            m_refreshInFlight = false;
        }
        if (startError.empty()) {
            m_refreshInFlight = true;
            m_refreshGeneration = generation;
            refreshToken = ++m_refreshSequence;
            favoriteBindingEpoch = ++m_favoriteBindingEpoch;
            if (m_favoriteBindingState == FavoriteBindingState::Unknown)
                m_favoriteBindingState = FavoriteBindingState::Pending;
            m_favoriteBindingReadInFlight = true;
            m_refreshPreservePlaylistDetails = preservePlaylistDetails;
            mutationEpoch = m_libraryMutationEpoch;
            artistPreferenceEpoch = m_artistPreferenceEpoch;
            if (done) m_refreshWaiters.push_back(std::move(done));
        }
    }
    if (!startError.empty()) {
        if (done) done(std::move(startError));
        return;
    }
    for (auto& waiter : superseded)
        if (waiter) waiter(L"The Apple Music refresh was superseded.");

    Post(generation, [this, generation, mutationEpoch, artistPreferenceEpoch, refreshToken,
                      favoriteBindingEpoch]() mutable {
        Snapshot snapshot;
        snapshot.libraryMutationEpoch = mutationEpoch;
        snapshot.artistPreferenceEpoch = artistPreferenceEpoch;
        snapshot.refreshToken = refreshToken;
        std::wstring error;
        bool favoriteBindingPublished = false;
        try {
            abort_callback& abort = *m_abort;
            snapshot.account = GetAccount(abort);
            snapshot.accountVerified = true;

            generation = ApplyVerifiedAccount(generation, refreshToken, snapshot.account);
            if (!generation) return;

            if (snapshot.account.signedIn) {
                auto playlistsFuture = std::async(std::launch::async, [&abort] {
                    return Collection(
                        L"/v1/me/library/playlists?limit=100&include=catalog&extend[library-playlists]=tags"
                        L"&fields[library-playlists]=name,artwork,editorialArtwork,tags,canEdit,canDelete,"
                        L"canRename,trackCount,playParams&platform=web",
                        abort);
                });
                auto albumsFuture = std::async(std::launch::async, [&abort] {
                    return Collection(
                        L"/v1/me/library/albums?limit=100&include=catalog,artists&platform=web"
                        L"&fields[library-albums]=name,artwork,artistName,genreNames,trackCount,"
                        L"playParams,releaseDate",
                        abort);
                });
                auto artistsFuture = std::async(std::launch::async, [&abort] {
                    return Collection(L"/v1/me/library/artists?limit=100&include=catalog"
                                      L"&extend[library-artists]=inFavorites&extend=inFavorites&platform=web",
                                      abort);
                });
                const auto drain = [](auto& future) {
                    try {
                        (void)future.get();
                    } catch (...) {
                    }
                };
                std::vector<Object> playlists;
                try {
                    playlists = playlistsFuture.get();
                } catch (const exception_aborted&) {
                    fb2k::inMainThread([this, generation, favoriteBindingEpoch] {
                        CompleteFavoriteBinding(generation, favoriteBindingEpoch, {}, false,
                                                L"The Apple Music favorite binding refresh was cancelled.");
                    });
                    favoriteBindingPublished = true;
                    drain(albumsFuture);
                    drain(artistsFuture);
                    throw;
                } catch (const std::exception& exception) {
                    const std::wstring playlistError = Wide(exception.what());
                    fb2k::inMainThread([this, generation, favoriteBindingEpoch, playlistError] {
                        CompleteFavoriteBinding(generation, favoriteBindingEpoch, {}, false, playlistError);
                    });
                    favoriteBindingPublished = true;
                    drain(albumsFuture);
                    drain(artistsFuture);
                    throw std::runtime_error(Utf8(playlistError));
                }

                for (const auto& object : playlists) {
                    if (IsFavoriteSystemPlaylist(object)) {
                        if (snapshot.favoritePlaylistId.empty())
                            snapshot.favoritePlaylistId = json::GetString(object, L"id");
                        continue;
                    }
                    if (auto playlist = ParsePlaylist(object)) {
                        if (playlist->own)
                            snapshot.createdPlaylists.push_back(*playlist);
                        else
                            snapshot.savedPlaylists.push_back(*playlist);
                        if (playlist->own) {
                            snapshot.editablePlaylists.emplace(playlist->id);
                            if (const auto attributes = json::GetObject(object, L"attributes");
                                !attributes || AttributeFlag(*attributes, L"canRename", true))
                                snapshot.renameablePlaylists.emplace(playlist->id);
                        }
                        const auto attributes = json::GetObject(object, L"attributes");
                        if (attributes ? AttributeFlag(*attributes, L"canDelete", true) : true)
                            snapshot.deletablePlaylists.emplace(playlist->id);
                        if (const auto catalogId = CatalogIdFrom(object))
                            snapshot.playlistLibraryIds[*catalogId] = playlist->id;
                        if (!PutIfCurrent(generation, *playlist)) return;
                    }
                }
                fb2k::inMainThread(
                    [this, generation, favoriteBindingEpoch, favoritePlaylist = snapshot.favoritePlaylistId] {
                        CompleteFavoriteBinding(generation, favoriteBindingEpoch, favoritePlaylist, true, {});
                    });
                favoriteBindingPublished = true;

                try {
                    snapshot.drm = GetDrmState(abort);
                } catch (const exception_aborted&) {
                    throw;
                } catch (...) {
                }

                const auto albums = albumsFuture.get();
                const auto artists = artistsFuture.get();
                for (const auto& object : albums) {
                    if (auto album = ParseAlbum(object)) {
                        snapshot.savedAlbumIds.emplace(album->id);
                        if (const auto catalogId = CatalogIdFrom(object))
                            snapshot.albumLibraryIds[*catalogId] = album->id;
                        if (!PutIfCurrent(generation, *album)) return;
                        snapshot.savedAlbums.push_back(std::move(*album));
                    }
                }
                snapshot.libraryArtists = ParseArtists(artists);
                for (const auto& object : artists) {
                    const std::wstring artistId = json::GetString(object, L"id");
                    if (const auto catalogId = CatalogIdFrom(object); !artistId.empty() && catalogId)
                        snapshot.artistCatalogIds[artistId] = *catalogId;
                }
                if (!PutIfCurrent(generation, snapshot.libraryArtists)) return;
            } else {
                error = L"Please sign in to Apple Music.";
                fb2k::inMainThread([this, generation, favoriteBindingEpoch] {
                    CompleteFavoriteBinding(generation, favoriteBindingEpoch, {}, true, {});
                });
                favoriteBindingPublished = true;
            }
        } catch (const exception_aborted&) {
            error = L"The Apple Music refresh was cancelled.";
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        if (!favoriteBindingPublished && !error.empty()) {
            fb2k::inMainThread([this, generation, favoriteBindingEpoch, error] {
                CompleteFavoriteBinding(generation, favoriteBindingEpoch, {}, false, error);
            });
        }

        fb2k::inMainThread(
            [this, generation, snapshot = std::move(snapshot), error = std::move(error)]() mutable {
                CompleteRefresh(generation, std::move(snapshot), std::move(error));
            });
    });
}

uint64_t StreamProvider::ApplyVerifiedAccount(uint64_t generation, uint64_t refreshToken,
                                              const AccountState& account) {
    bool accountChanged = false;
    bool accountObserved = false;
    uint64_t currentGeneration = 0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || generation != m_generation || !m_refreshInFlight ||
            m_refreshGeneration != generation || refreshToken != m_refreshSequence)
            return 0;

        const AccountState previous = m_account;
        const bool firstVerification = !m_accountVerified;
        const bool restoringCachedAccount = m_sessionRestoring && m_cachedAccountBound;
        const bool sameCachedAccount = firstVerification && restoringCachedAccount &&
                                       !m_cachedAccountId.empty() && account.signedIn &&
                                       account.accountId == m_cachedAccountId;
        const bool cachedIdentityMismatch = firstVerification && restoringCachedAccount && !sameCachedAccount;
        const bool verifiedIdentityChanged =
            m_accountVerified && previous.signedIn && !SameAccountState(previous, account);
        const bool identityChanged = cachedIdentityMismatch || verifiedIdentityChanged;
        accountChanged = !sameCachedAccount && (firstVerification || previous.signedIn != account.signedIn ||
                                                (m_accountVerified && !SameAccountState(previous, account)));
        const bool storefrontChanged =
            previous.signedIn && account.signedIn && previous.storefront != account.storefront;
        accountObserved = firstVerification || storefrontChanged;

        currentGeneration = generation;
        if (accountChanged || identityChanged) {
            // Keep the refresh-owned binding epoch. The current worker will fetch B's playlist
            // collection after this phase, while older targeted reads fail the generation guard.
            Catalog::Get().Clear();
            DisplayMetadata::Get().Clear();
            cache::RotateAccountScope("apple");
            m_snapshotSavePosted = false;
            m_snapshotSaveDirty = false;
            m_albumMutations.clear();
            m_playlistMutations.clear();
            currentGeneration = ++m_generation;
            m_refreshGeneration = currentGeneration;
            CancelPlaylistFetchesLocked();
            ClearLibraryLocked();
            m_favoritePageWaiters.clear();
            m_artistVerificationCondition.notify_all();
        }

        m_account = account;
        DisplayMetadata::Get().SetSourceStorefront(account.signedIn ? account.storefront : std::wstring());
        m_accountVerified = true;
        m_authPending = false;
        m_sessionRestoring = false;
        m_lastAuthError.clear();
        m_cachedAccountBound = false;
        m_cachedAccountId.clear();
        m_cachedAccountName.clear();
        m_cachedStorefront.clear();
        if (!account.signedIn) {
            m_drm = {};
            m_favoriteBindingState = FavoriteBindingState::Empty;
            m_favoriteBindingReadInFlight = false;
        } else {
            if (m_favoriteBindingState == FavoriteBindingState::Unknown)
                m_favoriteBindingState = FavoriteBindingState::Pending;
            m_favoriteBindingReadInFlight = true;
        }
    }

    if (accountChanged || accountObserved) {
        fb2k::inMainThread([this, observedGeneration = currentGeneration, accountChanged, accountObserved] {
            {
                std::lock_guard lock(m_mutex);
                if (!m_started || observedGeneration != m_generation) return;
            }
            if (accountChanged)
                NotifyAccountChanged();
            else if (accountObserved)
                NotifyAccountObserved();
            NotifyLibraryChanged();
        });
    }
    return currentGeneration;
}

void StreamProvider::CompleteRefresh(uint64_t generation, Snapshot snapshot, std::wstring error) {
    bool accountChanged = false;
    bool accountObserved = false;
    bool favoritesChanged = false;
    bool invalidateLists = false;
    bool snapshotReady = false;
    bool storefrontChanged = false;
    std::vector<Done> waiters;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || generation != m_generation || !m_refreshInFlight ||
            m_refreshGeneration != generation || snapshot.refreshToken != m_refreshSequence)
            return;
        m_refreshInFlight = false;
        m_refreshGeneration = 0;
        const bool preservePlaylistDetails = m_refreshPreservePlaylistDetails;
        m_refreshPreservePlaylistDetails = false;
        invalidateLists = m_refreshInvalidatesLists;
        m_refreshInvalidatesLists = false;
        waiters.swap(m_refreshWaiters);
        const AccountState previous = m_account;
        const bool previouslyVerified = m_accountVerified && previous.signedIn;
        const bool restoringCachedAccount = m_sessionRestoring && m_cachedAccountBound;
        const bool firstVerification = snapshot.accountVerified && !m_accountVerified;
        const bool sameCachedAccount = firstVerification && restoringCachedAccount &&
                                       !m_cachedAccountId.empty() && snapshot.account.signedIn &&
                                       snapshot.account.accountId == m_cachedAccountId;
        const bool cachedIdentityMismatch = firstVerification && restoringCachedAccount && !sameCachedAccount;
        const bool verifiedIdentityChanged = m_accountVerified && snapshot.accountVerified &&
                                             previous.signedIn &&
                                             !SameAccountState(previous, snapshot.account);
        accountChanged = snapshot.accountVerified && !sameCachedAccount &&
                         (firstVerification || previouslyVerified != snapshot.account.signedIn ||
                          (previouslyVerified && !SameAccountState(previous, snapshot.account)));
        storefrontChanged = snapshot.accountVerified && previous.signedIn && snapshot.account.signedIn &&
                            previous.storefront != snapshot.account.storefront;
        const bool identityChanged = cachedIdentityMismatch || verifiedIdentityChanged;
        const bool staleMutation = snapshot.libraryMutationEpoch < m_libraryMutationEpoch;
        if (staleMutation && error.empty())
            error = L"The Apple Music refresh was superseded by a library change.";
        if (!snapshot.accountVerified) {
            m_lastAuthError =
                error.empty() ? std::wstring(L"Apple Music account identity is temporarily unavailable.")
                              : error;
        } else {
            m_lastAuthError.clear();
        }
        if (accountChanged || identityChanged) {
            Catalog::Get().Clear();
            DisplayMetadata::Get().Clear();
            cache::RotateAccountScope("apple");
            m_snapshotSavePosted = false;
            m_snapshotSaveDirty = false;
            m_albumMutations.clear();
            m_playlistMutations.clear();
            ++m_generation;
            CancelPlaylistFetchesLocked();
            m_artistVerificationCondition.notify_all();
        }
        snapshotReady =
            error.empty() && !staleMutation && snapshot.accountVerified && snapshot.account.signedIn;
        accountObserved =
            sameCachedAccount || (snapshotReady && previouslyVerified &&
                                  SameAccountState(previous, snapshot.account) && storefrontChanged);
        const bool verifiedSignOut = snapshot.accountVerified && !snapshot.account.signedIn;
        const bool preserveLibrary =
            (restoringCachedAccount && !identityChanged && !verifiedSignOut && !snapshotReady) ||
            (m_accountVerified && previous.signedIn && !verifiedSignOut && !identityChanged &&
             !snapshotReady);
        const bool keepPlaylistDetails = preservePlaylistDetails && snapshotReady && !identityChanged &&
                                         m_accountVerified && previous.signedIn &&
                                         previous.name == snapshot.account.name &&
                                         previous.storefront == snapshot.account.storefront;
        std::unordered_map<stream::Id, std::vector<stream::Track>> previousPlaylistTracks;
        std::unordered_map<stream::Id, std::vector<Song>> previousPlaylistSongs;
        if (keepPlaylistDetails) {
            previousPlaylistTracks = std::move(m_playlistTracks);
            previousPlaylistSongs = std::move(m_playlistSongs);
        }
        const bool previousFavoritesLoaded = m_favoriteSongsLoaded;
        const std::set<stream::Id> previousFavoriteIds = m_favoriteIds;
        std::vector<Song> previousFavoriteSongs = m_favoriteSongs;
        if (!preserveLibrary) {
            m_account = snapshot.account;
            DisplayMetadata::Get().SetSourceStorefront(snapshot.account.signedIn ? snapshot.account.storefront
                                                                                 : std::wstring());
            m_accountVerified = snapshot.accountVerified;
            m_authPending = !snapshot.accountVerified;
            m_sessionRestoring = false;
            m_cachedAccountBound = false;
            m_cachedAccountId.clear();
            m_cachedAccountName.clear();
            m_cachedStorefront.clear();
            m_drm = snapshot.drm;
            ClearLibraryLocked();
        } else {
            accountChanged = false;
            if (snapshot.accountVerified) {
                m_account = snapshot.account;
                DisplayMetadata::Get().SetSourceStorefront(
                    snapshot.account.signedIn ? snapshot.account.storefront : std::wstring());
                m_accountVerified = true;
                m_authPending = false;
                m_sessionRestoring = false;
                m_cachedAccountBound = false;
                m_cachedAccountId.clear();
                m_cachedAccountName.clear();
                m_cachedStorefront.clear();
                m_drm = snapshot.drm;
            }
        }
        if (!preserveLibrary && snapshot.account.signedIn) {
            m_createdPlaylists = std::move(snapshot.createdPlaylists);
            m_savedPlaylists = std::move(snapshot.savedPlaylists);
            m_savedAlbums = std::move(snapshot.savedAlbums);
            m_libraryArtists = std::move(snapshot.libraryArtists);
            for (auto& artist : m_libraryArtists)
                ApplyArtistPreferenceOverlayLocked(artist, snapshot.artistPreferenceEpoch);
            m_playlistTracks = std::move(snapshot.playlistTracks);
            m_playlistSongs = std::move(snapshot.playlistSongs);
            m_albumTracks = std::move(snapshot.albumTracks);
            m_playlistLibraryIds = std::move(snapshot.playlistLibraryIds);
            m_albumLibraryIds = std::move(snapshot.albumLibraryIds);
            for (const auto& [key, mutation] : m_albumMutations) {
                (void)key;
                if (!mutation.catalogId.empty() && !mutation.libraryId.empty())
                    m_albumLibraryIds[mutation.catalogId] = mutation.libraryId;
            }
            for (const auto& [key, mutation] : m_playlistMutations) {
                (void)key;
                if (!mutation.catalogId.empty() && !mutation.libraryId.empty())
                    m_playlistLibraryIds[mutation.catalogId] = mutation.libraryId;
            }
            m_artistCatalogIds = std::move(snapshot.artistCatalogIds);
            m_savedAlbumIds = std::move(snapshot.savedAlbumIds);
            m_editablePlaylists = std::move(snapshot.editablePlaylists);
            m_renameablePlaylists = std::move(snapshot.renameablePlaylists);
            m_deletablePlaylists = std::move(snapshot.deletablePlaylists);
            m_favoritePlaylistId = std::move(snapshot.favoritePlaylistId);
            m_favoriteBindingState =
                m_favoritePlaylistId.empty() ? FavoriteBindingState::Empty : FavoriteBindingState::Ready;
            m_librarySongs = std::move(snapshot.librarySongs);
            m_librarySongsLoaded = snapshot.librarySongsLoaded;
            if (m_librarySongsLoaded) m_librarySongsAt = std::chrono::steady_clock::now();
            // A library refresh does not read the favorite songs. The same account keeps the
            // ones it has (also the ids restored at start); dropping them here made every
            // refresh download the whole Favorite Songs playlist again.
            if (accountChanged) {
                m_favoriteSongs.clear();
                m_favoriteIds.clear();
                m_favoriteSongsLoaded = false;
                m_favoriteIdsCheckedAt = 0;
            } else {
                m_favoriteSongs = std::move(previousFavoriteSongs);
                m_favoriteIds = previousFavoriteIds;
                m_favoriteSongsLoaded = previousFavoritesLoaded;
            }
            if (keepPlaylistDetails) {
                // List refreshes do not contain track details. Preserve only details whose
                // playlist is still present in this verified same-account snapshot.
                const auto playlistStillPresent = [this](const stream::Id& id) {
                    return id == m_favoritePlaylistId ||
                           std::any_of(
                               m_createdPlaylists.begin(), m_createdPlaylists.end(),
                               [&id](const stream::Playlist& playlist) { return playlist.id == id; }) ||
                           std::any_of(m_savedPlaylists.begin(), m_savedPlaylists.end(),
                                       [&id](const stream::Playlist& playlist) { return playlist.id == id; });
                };
                for (auto& [id, tracks] : previousPlaylistTracks) {
                    if (playlistStillPresent(id)) m_playlistTracks.try_emplace(id, std::move(tracks));
                }
                for (auto& [id, songs] : previousPlaylistSongs) {
                    if (playlistStillPresent(id)) m_playlistSongs.try_emplace(id, std::move(songs));
                }
            }
        }
        if (!preserveLibrary) m_librarySnapshotLoaded = snapshotReady;
        if (snapshotReady)
            m_librarySnapshotValidUntil = std::chrono::steady_clock::now() + kLibrarySnapshotTtl;
        if (snapshotReady) {
            const auto canonical = [this](const stream::Id& id) {
                if (!IsLibraryId(id) || id.empty() || id[0] != L'l') return id;
                const auto found = std::find_if(m_albumLibraryIds.begin(), m_albumLibraryIds.end(),
                                                [&id](const auto& item) { return item.second == id; });
                return found == m_albumLibraryIds.end() ? id : found->first;
            };
            for (auto it = m_albumMutations.begin(); it != m_albumMutations.end();) {
                AlbumMutation& mutation = it->second;
                if (mutation.epoch > snapshot.libraryMutationEpoch) {
                    ++it;
                    continue;
                }
                if (mutation.libraryId.empty()) {
                    const auto mapping = m_albumLibraryIds.find(mutation.catalogId);
                    if (mapping != m_albumLibraryIds.end()) mutation.libraryId = mapping->second;
                }
                const bool present =
                    std::any_of(m_savedAlbums.begin(), m_savedAlbums.end(), [&](const stream::Album& album) {
                        return album.id == mutation.libraryId || canonical(album.id) == mutation.catalogId;
                    });
                if (present == mutation.saved)
                    it = m_albumMutations.erase(it);
                else
                    ++it;
            }
            const auto canonicalPlaylist = [this](const stream::Id& id) {
                if (!IsLibraryId(id) || id.empty() || id[0] != L'p') return id;
                const auto found = std::find_if(m_playlistLibraryIds.begin(), m_playlistLibraryIds.end(),
                                                [&id](const auto& item) { return item.second == id; });
                return found == m_playlistLibraryIds.end() ? id : found->first;
            };
            for (auto it = m_playlistMutations.begin(); it != m_playlistMutations.end();) {
                PlaylistMutation& mutation = it->second;
                if (mutation.epoch > snapshot.libraryMutationEpoch) {
                    ++it;
                    continue;
                }
                if (mutation.libraryId.empty()) {
                    const auto mapping = m_playlistLibraryIds.find(mutation.catalogId);
                    if (mapping != m_playlistLibraryIds.end()) mutation.libraryId = mapping->second;
                }
                const bool present = std::any_of(
                    m_savedPlaylists.begin(), m_savedPlaylists.end(), [&](const stream::Playlist& playlist) {
                        return playlist.id == mutation.libraryId ||
                               canonicalPlaylist(playlist.id) == mutation.catalogId;
                    });
                if (present == mutation.saved)
                    it = m_playlistMutations.erase(it);
                else
                    ++it;
            }
        }
        favoritesChanged =
            previousFavoritesLoaded != m_favoriteSongsLoaded || previousFavoriteIds != m_favoriteIds;
    }
    if (accountObserved) NotifyAccountObserved();
    if (invalidateLists && snapshotReady && !accountChanged) NotifyListChanged();
    if (accountChanged) NotifyAccountChanged();
    if (favoritesChanged) NotifyFavoritesChanged();
    NotifyLibraryChanged();
    if (snapshotReady) QueueSnapshotSave();
    for (auto& waiter : waiters)
        if (waiter) waiter(error);
    if (snapshotReady) {
        uint64_t current = 0;
        bool favoritesKnown = false;
        {
            std::lock_guard lock(m_mutex);
            current = m_generation;
            // The stars need the favorite ids. They are read from the service when they are
            // not known or were last confirmed long ago, not at every start and refresh.
            favoritesKnown = m_favoriteSongsLoaded ||
                             (!m_favoriteIds.empty() &&
                              cache::Store::Now() - m_favoriteIdsCheckedAt < kFavoriteIdsFreshSeconds);
        }
        if (!favoritesKnown) FetchFavorites({});
        EnrichArtistCountsAsync(current);
    }
}

void StreamProvider::RefreshLibraryPreservingDetails(Done done) {
    uint64_t generation = 0;
    bool started = false;
    {
        std::lock_guard lock(m_mutex);
        started = m_started;
        generation = m_generation;
    }
    if (!started) {
        if (done) done(L"Apple Music is not running.");
        return;
    }
    RefreshAccountAsync(generation, std::move(done), true);
}

void StreamProvider::RefreshLibraryAfterMutation(Done done) {
    uint64_t generation = 0;
    bool started = false;
    {
        std::lock_guard lock(m_mutex);
        started = m_started;
        generation = m_generation;
        if (started) m_refreshInvalidatesLists = true;
    }
    if (!started) {
        if (done) done(L"Apple Music is not running.");
        return;
    }
    // LISTWRITE_INVALIDATION: CompleteRefresh calls NotifyListChanged only for an
    // explicitly completed mutation refresh, before OnLibraryChanged.
    RefreshAccountAsync(generation, std::move(done), true);
}

void StreamProvider::RefreshLibrary(Done done) {
    uint64_t generation = 0;
    bool started = false;
    {
        std::lock_guard lock(m_mutex);
        started = m_started;
        generation = m_generation;
        // An explicit library refresh invalidates detail snapshots, while background snapshot
        // refreshes use RefreshLibraryPreservingDetails().
        if (started) ClearPlaylistDetailsLocked();
    }
    if (!started) {
        if (done) done(L"Apple Music is not running.");
        return;
    }
    RefreshAccountAsync(generation, std::move(done));
}

stream::Id StreamProvider::TrackId(const metadb_handle_ptr& item) const {
    stream::Id id;
    return item.is_valid() && ParseSongPath(item->get_path(), id) ? id : stream::Id();
}

std::wstring StreamProvider::Lyrics(const stream::Id& track, abort_callback& abort) {
    const uint64_t account = cache::AccountRevision(Utf8(Key()));
    std::wstring locale;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || !m_accountVerified || !m_account.signedIn) return {};
        locale = m_locale;
    }
    lyrics_protocol::Request request;
    request.mode = lyrics_protocol::Mode::Direct;
    request.trackId = track;
    request.trackType = TrackResourceType(track);
    request.locale = locale.empty() ? L"en-US" : std::move(locale);
    const Object response =
        Request(L"/api/v1/glasswave/lyrics", abort, L"POST", lyrics_protocol::BuildRequestBody(request));
    abort.check();
    if (cache::AccountRevision(Utf8(Key())) != account) throw exception_aborted();
    const auto results = lyrics_protocol::ParseResponse(response);
    return results.empty() ? std::wstring() : results.front().enhancedLrc;
}

metadb_handle_ptr StreamProvider::TrackItem(const stream::Id& track) const {
    return Catalog::Get().Handle(track);
}

std::optional<stream::Track> StreamProvider::FindTrack(const stream::Id& track) const {
    const auto song = Catalog::Get().FindSong(track);
    if (!song) RequestUnknownSong(track);
    return song ? std::optional<stream::Track>(song->track) : std::nullopt;
}

stream::Id StreamProvider::AlbumOfTrack(const stream::Id& track) const {
    stream::Id album = Catalog::Get().SongAlbum(track); // what album_art_applemusic.cpp names the cover by
    if (album.empty() && !Catalog::Get().FindSongRaw(track)) RequestUnknownSong(track);
    return album;
}

void StreamProvider::RequestUnknownSong(const stream::Id& track) const {
    if (track.empty()) return;
    constexpr auto kRetry = std::chrono::minutes(2);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(m_unknownMutex);
        if (const auto asked = m_unknownAsked.find(track); asked != m_unknownAsked.end() && now - asked->second < kRetry)
            return;
        m_unknownAsked[track] = now;
        m_unknownSongs.insert(track);
        if (m_unknownQueued) return;
        m_unknownQueued = true;
    }
    // Gathered until the main thread is free: a queue asks for its rows one by one in one paint.
    auto* self = const_cast<StreamProvider*>(this);
    fb2k::inMainThread([self] {
        {
            std::lock_guard unknown(self->m_unknownMutex);
            self->m_unknownQueued = false; // later requests post again; the set is read once
        }
        uint64_t generation = 0;
        {
            std::lock_guard lock(self->m_mutex);
            if (!self->m_started) return;
            generation = self->m_generation;
        }
        self->Post(generation, [self] { self->ReadUnknownSongs(); });
    });
}

void StreamProvider::ReadUnknownSongs() const {
    std::vector<stream::Id> library, catalog;
    {
        std::lock_guard lock(m_unknownMutex);
        for (const auto& id : m_unknownSongs) {
            if (Catalog::Get().FindSongRaw(id)) continue;
            (IsLibraryId(id) && id[0] == L'i' ? library : catalog).push_back(id);
        }
        m_unknownSongs.clear();
    }
    if (library.empty() && catalog.empty()) return;
    std::wstring storefront;
    try {
        storefront = Storefront();
    } catch (...) {
    }
    if (storefront.empty()) storefront = L"US";
    std::vector<Song> songs;
    const auto read = [&](const std::vector<stream::Id>& ids, bool inLibrary) {
        constexpr size_t kBatch = 100;
        for (size_t first = 0; first < ids.size(); first += kBatch) {
            std::wstring joined;
            for (size_t i = first; i < std::min(ids.size(), first + kBatch); ++i) {
                if (!joined.empty()) joined += L',';
                joined += ids[i];
            }
            const std::wstring path =
                inLibrary ? L"/v1/me/library/songs?ids=" + Encode(joined) +
                                L"&include=catalog,albums,artists&include[songs]=albums"
                          : L"/v1/catalog/" + storefront + L"/songs?ids=" + Encode(joined) + L"&include=albums,artists";
            try {
                const Object response = Apple(path, *m_abort);
                if (const auto data = json::GetArray(response, L"data")) {
                    auto parsed = ParseSongs(json::Objects(*data));
                    songs.insert(songs.end(), std::make_move_iterator(parsed.begin()),
                                 std::make_move_iterator(parsed.end()));
                }
            } catch (...) {
                // Asked again after the retry pause by whatever still needs the songs.
                std::lock_guard lock(m_unknownMutex);
                for (size_t i = first; i < std::min(ids.size(), first + kBatch); ++i) m_unknownAsked.erase(ids[i]);
            }
        }
    };
    read(library, true);
    read(catalog, false);
    if (songs.empty()) return;
    fb2k::inMainThread([songs = std::move(songs)]() mutable {
        // Persisted with the restart catalog: a queue restored next time has its names at once.
        Catalog::Get().PutAndHint(songs, true);
    });
}

metadb_handle_ptr StreamProvider::ArtItem(stream::ArtKind kind, const stream::Id& id) const {
    return Catalog::Get().CoverHandle(kind, id);
}

bool StreamProvider::SameCover(std::wstring_view a, std::wstring_view b) const {
    return SameArtworkBase(a, b);
}

bool StreamProvider::ReadFavorite(const stream::Id& track) const {
    std::lock_guard lock(m_mutex);
    return m_favoriteIds.count(track) != 0;
}

std::vector<stream::Playlist> StreamProvider::Playlists(stream::Shelf shelf) const {
    return DisplayPlaylists(RawPlaylists(shelf));
}

std::vector<stream::Playlist> StreamProvider::RawPlaylists(stream::Shelf shelf) const {
    std::vector<stream::Playlist> playlists;
    std::unordered_map<stream::Id, PlaylistMutation> mutations;
    {
        std::lock_guard lock(m_mutex);
        switch (shelf) {
        case stream::Shelf::Created:
            playlists = m_createdPlaylists;
            break;
        case stream::Shelf::Saved:
            playlists = m_savedPlaylists;
            mutations = m_playlistMutations;
            break;
        case stream::Shelf::Recommended:
            break;
        }
    }
    if (shelf == stream::Shelf::Saved) {
        const auto canonical = [this](const stream::Id& id) {
            const stream::Id mapped = CatalogIdForPlaylist(id);
            return mapped.empty() ? id : mapped;
        };
        std::erase_if(playlists, [&](const stream::Playlist& playlist) {
            const auto found = mutations.find(canonical(playlist.id));
            return found != mutations.end() && !found->second.saved;
        });
        for (const auto& [key, mutation] : mutations) {
            if (!mutation.saved || mutation.playlist.id.empty()) continue;
            const bool present =
                std::any_of(playlists.begin(), playlists.end(),
                            [&](const stream::Playlist& playlist) { return canonical(playlist.id) == key; });
            if (!present) playlists.push_back(mutation.playlist);
        }
    }
    return playlists;
}

std::optional<stream::Shelf> StreamProvider::ShelfOf(const stream::Id& playlist) const {
    std::lock_guard lock(m_mutex);
    if (std::any_of(m_createdPlaylists.begin(), m_createdPlaylists.end(),
                    [&playlist](const stream::Playlist& item) { return item.id == playlist; })) {
        return stream::Shelf::Created;
    }
    if (std::any_of(m_savedPlaylists.begin(), m_savedPlaylists.end(),
                    [&playlist](const stream::Playlist& item) { return item.id == playlist; })) {
        return stream::Shelf::Saved;
    }
    const auto mutation =
        std::find_if(m_playlistMutations.begin(), m_playlistMutations.end(), [&playlist](const auto& entry) {
            return entry.first == playlist || entry.second.catalogId == playlist ||
                   entry.second.libraryId == playlist;
        });
    if (mutation != m_playlistMutations.end() && mutation->second.saved) return stream::Shelf::Saved;
    return std::nullopt;
}

std::vector<stream::Artist> StreamProvider::FollowedArtists() const {
    std::vector<stream::Artist> artists;
    uint64_t preferenceEpoch = 0;
    {
        std::lock_guard lock(m_mutex);
        artists = m_libraryArtists;
        preferenceEpoch = m_artistPreferenceEpoch;
    }
    for (auto& artist : artists)
        ApplyArtistPreferenceOverlay(artist, preferenceEpoch);
    std::erase_if(artists,
                  [](const stream::Artist& artist) { return !artist.followKnown || !artist.followed; });
    return DisplayArtists(std::move(artists));
}

std::vector<stream::Album> StreamProvider::SavedAlbums() const {
    std::vector<stream::Album> albums;
    std::unordered_map<stream::Id, AlbumMutation> mutations;
    {
        std::lock_guard lock(m_mutex);
        albums = m_savedAlbums;
        mutations = m_albumMutations;
    }
    const auto canonical = [this](const stream::Id& id) {
        const stream::Id mapped = CatalogIdForAlbum(id);
        return mapped.empty() ? id : mapped;
    };
    std::erase_if(albums, [&](const stream::Album& album) {
        const auto found = mutations.find(canonical(album.id));
        return found != mutations.end() && !found->second.saved;
    });
    for (auto& album : albums) {
        const auto found = mutations.find(canonical(album.id));
        if (found != mutations.end() && found->second.saved && !found->second.catalogId.empty())
            album.id = found->second.catalogId;
    }
    for (const auto& [key, mutation] : mutations) {
        if (!mutation.saved) continue;
        const bool present = std::any_of(albums.begin(), albums.end(), [&](const stream::Album& album) {
            return canonical(album.id) == key;
        });
        if (!present && !mutation.album.id.empty()) {
            stream::Album album = mutation.album;
            if (!mutation.catalogId.empty()) album.id = mutation.catalogId;
            albums.push_back(std::move(album));
        }
    }
    return DisplayAlbums(std::move(albums));
}

bool StreamProvider::IsAlbumSaved(const stream::Id& album) const {
    const stream::Id canonical = CatalogIdForAlbum(album);
    const stream::Id key = canonical.empty() ? album : canonical;
    std::vector<stream::Album> saved;
    {
        std::lock_guard lock(m_mutex);
        const auto mutation = m_albumMutations.find(key);
        if (mutation != m_albumMutations.end()) return mutation->second.saved;
        saved = m_savedAlbums;
    }
    return std::any_of(saved.begin(), saved.end(), [&](const stream::Album& item) {
        const stream::Id itemCatalog = CatalogIdForAlbum(item.id);
        return item.id == album || (!itemCatalog.empty() && itemCatalog == key);
    });
}

std::optional<bool> StreamProvider::PlaylistSavedState(const stream::Id& playlist) const {
    std::lock_guard lock(m_mutex);
    return PlaylistSavedStateLocked(playlist);
}

std::optional<bool> StreamProvider::PlaylistSavedStateLocked(const stream::Id& playlist) const {
    if (!m_accountVerified || !m_account.signedIn) return std::nullopt;
    const auto mutation =
        std::find_if(m_playlistMutations.begin(), m_playlistMutations.end(), [&playlist](const auto& entry) {
            return entry.first == playlist || entry.second.catalogId == playlist ||
                   entry.second.libraryId == playlist;
        });
    if (mutation != m_playlistMutations.end()) return mutation->second.saved;
    if (!m_librarySnapshotLoaded) return std::nullopt;

    stream::Id libraryId;
    if (IsLibraryId(playlist) && !playlist.empty() && playlist[0] == L'p')
        libraryId = playlist;
    else {
        const auto mapped = m_playlistLibraryIds.find(playlist);
        if (mapped != m_playlistLibraryIds.end()) libraryId = mapped->second;
    }
    return std::any_of(m_savedPlaylists.begin(), m_savedPlaylists.end(),
                       [&playlist, &libraryId](const stream::Playlist& item) {
                           return item.id == playlist || (!libraryId.empty() && item.id == libraryId);
                       });
}

std::optional<bool> StreamProvider::ArtistPreferenceState(const stream::Id& artist) const {
    {
        std::lock_guard lock(m_mutex);
        const auto mutation = std::find_if(m_artistPreferenceMutations.begin(),
                                           m_artistPreferenceMutations.end(), [&artist](const auto& entry) {
                                               return entry.first == artist ||
                                                      entry.second.sourceId == artist ||
                                                      entry.second.catalogId == artist;
                                           });
        if (mutation != m_artistPreferenceMutations.end() && mutation->second.pending)
            return mutation->second.favorite;
        const auto found = std::find_if(m_libraryArtists.begin(), m_libraryArtists.end(),
                                        [&artist](const stream::Artist& item) { return item.id == artist; });
        if (found != m_libraryArtists.end() && found->followKnown) return found->followed;
    }
    if (const auto raw = Catalog::Get().FindArtistRaw(artist); raw && raw->followKnown) return raw->followed;
    const stream::Id catalogId = CatalogIdForArtist(artist);
    if (!catalogId.empty() && catalogId != artist) {
        if (const auto raw = Catalog::Get().FindArtistRaw(catalogId); raw && raw->followKnown)
            return raw->followed;
    }
    return std::nullopt;
}

std::optional<stream::Playlist> StreamProvider::FindPlaylist(const stream::Id& playlist) const {
    auto result = Catalog::Get().FindPlaylist(playlist);
    if (result) {
        if (const auto saved = PlaylistSavedState(playlist)) result->saved = *saved;
    }
    return result;
}

std::optional<stream::Album> StreamProvider::FindAlbum(const stream::Id& album) const {
    return Catalog::Get().FindAlbum(album);
}

std::optional<std::vector<stream::Track>> StreamProvider::CachedFavorites() const {
    std::lock_guard lock(m_mutex);
    return m_favoriteSongsLoaded ? std::optional<std::vector<stream::Track>>(DisplayTracks(m_favoriteSongs))
                                 : std::nullopt;
}

std::optional<std::vector<stream::Track>> StreamProvider::CachedPlaylist(const stream::Id& playlist) const {
    std::optional<std::vector<stream::Track>> tracks;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_playlistTracks.find(playlist);
        if (found != m_playlistTracks.end()) tracks = found->second;
    }
    return tracks ? std::optional<std::vector<stream::Track>>(DisplayTracks(std::move(*tracks)))
                  : std::nullopt;
}

std::optional<std::vector<stream::Track>> StreamProvider::CachedAlbum(const stream::Id& album) const {
    std::optional<std::vector<stream::Track>> tracks;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_albumTracks.find(album);
        if (found != m_albumTracks.end()) tracks = found->second;
    }
    return tracks ? std::optional<std::vector<stream::Track>>(DisplayTracks(std::move(*tracks)))
                  : std::nullopt;
}

std::vector<Song> StreamProvider::RecentLibrarySongs(uint64_t generation, abort_callback& abort) {
    {
        std::lock_guard lock(m_mutex);
        if (m_librarySongsLoaded && std::chrono::steady_clock::now() - m_librarySongsAt < kLibrarySnapshotTtl)
            return m_librarySongs;
    }
    std::vector<Song> songs = LoadLibrarySongs(abort);
    std::lock_guard lock(m_mutex);
    if (generation == m_generation) {
        m_librarySongs = songs;
        m_librarySongsLoaded = true;
        m_librarySongsAt = std::chrono::steady_clock::now();
    }
    return songs;
}

void StreamProvider::RefreshLibrarySongsAsync(uint64_t generation, TracksDone done) {
    Post(generation, [this, generation, done = std::move(done)]() mutable {
        std::vector<Song> songs;
        std::wstring error;
        try {
            songs = LoadLibrarySongs(*m_abort);
            if (!PutIfCurrent(generation, songs)) return;
            ResolveSongDisplayNames(songs, generation, *m_abort);
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, generation, done = std::move(done), songs = std::move(songs),
                              error = std::move(error)]() mutable {
            if (error.empty()) {
                Catalog::Get().PutAndHint(songs);
                {
                    std::lock_guard lock(m_mutex);
                    if (generation == m_generation) {
                        m_librarySongs = songs;
                        m_librarySongsLoaded = true;
                        m_librarySongsAt = std::chrono::steady_clock::now();
                    }
                }
                NotifyLibraryChanged();
            }
            if (done) done(DisplayTracks(songs), std::move(error));
        });
    });
}

void StreamProvider::FetchFavorites(TracksDone done) {
    FetchFavoritesWithInvalidation(std::move(done), false);
}

void StreamProvider::FetchFavoritesWithInvalidation(TracksDone done, bool invalidateLists) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, signInError);
        return;
    }
    uint64_t generation = 0;
    stream::Id favoritePlaylist;
    const std::wstring storefront = Storefront();
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        favoritePlaylist = m_favoritePlaylistId;
    }
    if (favoritePlaylist.empty()) {
        if (invalidateLists) NotifyListChanged();
        if (done) done({}, L"The Favorite Songs system playlist was not found in the library snapshot.");
        return;
    }

    Post(generation,
         [this, generation, favoritePlaylist, storefront, invalidateLists, done = std::move(done)]() mutable {
             PlaylistData data;
             std::wstring error;
             try {
                 data = LoadPlaylist(favoritePlaylist, storefront, true, *m_abort);
                 if (!PutIfCurrent(generation, data.songs)) return;
                 ResolveSongDisplayNames(data.songs, generation, *m_abort);
             } catch (const exception_aborted&) {
                 return;
             } catch (const std::exception& exception) {
                 error = Wide(exception.what());
             }
             PostMain(generation, [this, generation, invalidateLists, done = std::move(done),
                                   songs = std::move(data.songs), error = std::move(error)]() mutable {
                 if (invalidateLists) NotifyListChanged();
                 if (error.empty()) {
                     Catalog::Get().PutAndHint(songs);
                     {
                         std::lock_guard lock(m_mutex);
                         if (generation == m_generation) {
                             m_favoriteSongs = songs;
                             m_favoriteSongsLoaded = true;
                             m_favoriteIds.clear();
                             for (const auto& song : m_favoriteSongs)
                                 m_favoriteIds.emplace(song.track.id);
                             m_favoriteIdsCheckedAt = cache::Store::Now();
                         }
                     }
                     SaveFavoriteIdsAsync();
                     NotifyFavoritesChanged();
                     NotifyLibraryChanged();
                 }
                 if (done) done(DisplayTracks(songs), std::move(error));
             });
         });
}

void StreamProvider::FetchPlaylists(stream::Shelf shelf, PlaylistsDone done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, signInError);
        return;
    }
    uint64_t generation = 0;
    bool loaded = false;
    bool fresh = false;
    std::vector<stream::Playlist> cached;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        loaded = m_librarySnapshotLoaded;
        fresh = loaded && std::chrono::steady_clock::now() < m_librarySnapshotValidUntil;
        switch (shelf) {
        case stream::Shelf::Created:
            cached = m_createdPlaylists;
            break;
        case stream::Shelf::Saved:
            cached = m_savedPlaylists;
            break;
        case stream::Shelf::Recommended:
            break;
        }
    }
    if (loaded) {
        if (!fresh) RefreshAccountAsync(generation, {}, true);
        if (done) {
            fb2k::inMainThread(
                [done = std::move(done), cached = DisplayPlaylists(std::move(cached))]() mutable {
                    done(std::move(cached), {});
                });
        }
        return;
    }
    RefreshLibraryPreservingDetails([this, shelf, done = std::move(done)](std::wstring error) mutable {
        if (done) done(Playlists(shelf), std::move(error));
    });
}

void StreamProvider::FetchPlaylist(const stream::Id& playlist, PlaylistDone done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, {}, signInError);
        return;
    }

    const auto cachedInfo = Catalog::Get().FindPlaylist(playlist);
    const auto cachedTracks = CachedPlaylist(playlist);
    const auto region = GetAppleMusicDisplayRegion();
    const std::wstring sourceStorefront = DisplayMetadata::Get().SourceStorefront();
    if (cachedInfo && cachedTracks &&
        (cachedInfo->own || region == AppleMusicDisplayRegion::Account ||
         DisplayMetadata::Get().IsPlaylistComplete(region, sourceStorefront, playlist))) {
        if (done) {
            fb2k::inMainThread(
                [done = std::move(done), info = *cachedInfo, tracks = *cachedTracks]() mutable {
                    done(std::move(info), std::move(tracks), {});
                });
        }
        return;
    }

    // A playlist page asks for its summary and track source separately. Both calls share one
    // network detail read and both callbacks receive the same immutable result.
    const auto requestStarted = std::chrono::steady_clock::now();
    uint64_t generation = 0;
    uint64_t epoch = 0;
    std::wstring storefront;
    bool library = false;
    std::shared_ptr<PlaylistFetch> flight;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        epoch = m_playlistDetailEpoch;
        storefront = m_account.signedIn && m_account.storefront.size() == 2 ? m_account.storefront
                                                                            : std::wstring(L"US");
        library = (IsLibraryId(playlist) && playlist[0] == L'p') || m_editablePlaylists.count(playlist) != 0;
        const auto existing = m_playlistFetches.find(playlist);
        if (existing != m_playlistFetches.end() && existing->second->generation == generation &&
            existing->second->epoch == epoch) {
            if (done) existing->second->waiters.push_back(std::move(done));
            return;
        }
        flight = std::make_shared<PlaylistFetch>();
        flight->generation = generation;
        flight->epoch = epoch;
        if (done) flight->waiters.push_back(std::move(done));
        m_playlistFetches[playlist] = flight;
    }
    const bool queued = PostDetail(generation, [this, generation, epoch, playlist, storefront, library,
                                                flight, requestStarted]() mutable {
        PlaylistData data;
        std::wstring error;
        const auto loadStarted = std::chrono::steady_clock::now();
        const long long queueMs = ElapsedMs(requestStarted, loadStarted);
        try {
            data = LoadPlaylist(playlist, storefront, library, *m_abort);
            ResolveSongDisplayNames(data.songs, generation, *m_abort);
            std::vector<stream::Playlist> displayPlaylist{data.info};
            ResolvePlaylistDisplayNames(displayPlaylist, generation, *m_abort);
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, generation, epoch, playlist, flight, data = std::move(data),
                              error = std::move(error), requestStarted, queueMs]() mutable {
            const auto commitStarted = std::chrono::steady_clock::now();
            std::vector<PlaylistDone> waiters;
            bool applied = false;
            {
                std::lock_guard lock(m_mutex);
                waiters = std::move(flight->waiters);
                applied = error.empty() && !data.info.id.empty() && generation == m_generation &&
                          epoch == m_playlistDetailEpoch;
                if (applied) {
                    if (const auto saved = PlaylistSavedStateLocked(data.info.id)) data.info.saved = *saved;
                    Catalog::Get().Put(data.info);
                    m_playlistTracks[data.info.id] = Tracks(data.songs);
                    m_playlistSongs[data.info.id] = data.songs;
                    if (!data.catalogId.empty()) m_playlistLibraryIds[data.catalogId] = data.info.id;
                    m_editablePlaylists.erase(data.info.id);
                    m_renameablePlaylists.erase(data.info.id);
                    m_deletablePlaylists.erase(data.info.id);
                    if (data.info.own) m_editablePlaylists.emplace(data.info.id);
                    if (data.canRename) m_renameablePlaylists.emplace(data.info.id);
                    if (data.canDelete) m_deletablePlaylists.emplace(data.info.id);
                }
                const auto found = m_playlistFetches.find(playlist);
                if (found != m_playlistFetches.end() && found->second == flight)
                    m_playlistFetches.erase(found);
            }
            if (applied) {
                Catalog::Get().PutAndHint(data.songs);
                NotifyLibraryChanged();
            }
            const auto completed = std::chrono::steady_clock::now();
            // Keep production diagnostics privacy-safe: timings and counts only.
            std::wstring performance = L"playlist detail perf ok=";
            performance += applied ? L"1" : L"0";
            performance += L" primary_ms=" + std::to_wstring(data.metrics.primaryMs);
            performance += L" pages_ms=" + std::to_wstring(data.metrics.pagesMs);
            performance += L" hydration_ms=" + std::to_wstring(data.metrics.hydrationMs);
            performance += L" queue_ms=" + std::to_wstring(queueMs);
            performance += L" commit_ms=" + std::to_wstring(ElapsedMs(commitStarted, completed));
            performance += L" total_ms=" + std::to_wstring(ElapsedMs(requestStarted, completed));
            performance += L" rows=" + std::to_wstring(data.songs.size());
            performance += L" fallback_batches=" + std::to_wstring(data.metrics.fallbackBatches);
            netease::Log(performance);
            stream::Playlist displayInfo = data.info;
            DisplayMetadata::Get().Apply(displayInfo, DisplayMetadata::Get().SourceStorefront());
            const std::vector<stream::Track> tracks = DisplayTracks(data.songs);
            for (auto& waiter : waiters)
                if (waiter) waiter(displayInfo, tracks, error);
        });
    });
    if (!queued) {
        std::lock_guard lock(m_mutex);
        const auto found = m_playlistFetches.find(playlist);
        if (found != m_playlistFetches.end() && found->second == flight) m_playlistFetches.erase(found);
    }
}

void StreamProvider::FetchAlbum(const stream::Id& album, AlbumDone done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, {}, signInError);
        return;
    }

    const auto cachedInfo = Catalog::Get().FindAlbum(album);
    const auto cachedTracks = CachedAlbum(album);
    const auto region = GetAppleMusicDisplayRegion();
    const std::wstring sourceStorefront = DisplayMetadata::Get().SourceStorefront();
    const AlbumRequest request = ResolveAlbumRequest(album);
    if (cachedInfo && cachedTracks &&
        (region == AppleMusicDisplayRegion::Account ||
         DisplayMetadata::Get().IsAlbumComplete(region, sourceStorefront, album))) {
        if (done) {
            fb2k::inMainThread(
                [done = std::move(done), info = *cachedInfo, tracks = *cachedTracks]() mutable {
                    done(std::move(info), std::move(tracks), {});
                });
        }
        return;
    }

    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation, [this, generation, album, request, storefront, done = std::move(done)]() mutable {
        AlbumData data;
        std::wstring error;
        try {
            data = LoadAlbum(request.id, storefront, request.library, *m_abort);
            if (!PutIfCurrent(generation, data.info)) return;
            std::vector<stream::Album> displayAlbum{data.info};
            ResolveAlbumDisplayNames(displayAlbum, generation, *m_abort);
            ResolveSongDisplayNames(data.songs, generation, *m_abort);
            if (!displayAlbum.empty()) {
                DisplayMetadata::Get().Apply(displayAlbum.front(), DisplayMetadata::Get().SourceStorefront());
                data.info = std::move(displayAlbum.front());
            }
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, done = std::move(done), data = std::move(data), error = std::move(error),
                              generation, album]() mutable {
            if (error.empty() && !data.info.id.empty()) {
                Catalog::Get().PutAndHint(data.songs);
                {
                    std::lock_guard lock(m_mutex);
                    if (generation == m_generation) {
                        m_albumTracks[album] = Tracks(data.songs);
                        m_albumTracks[data.info.id] = Tracks(data.songs);
                        if (!data.catalogId.empty()) m_albumLibraryIds[data.catalogId] = data.info.id;
                    }
                }
            }
            if (done) done(std::move(data.info), DisplayTracks(data.songs), std::move(error));
        });
    });
}

void StreamProvider::FetchAlbumsTracks(std::vector<stream::Id> albums, TracksDone done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, signInError);
        return;
    }
    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation,
         [this, generation, albums = std::move(albums), storefront, done = std::move(done)]() mutable {
             std::vector<Song> songs;
             std::set<stream::Id> seen;
             std::wstring error;
             try {
                 for (const auto& id : albums) {
                     const AlbumRequest request = ResolveAlbumRequest(id);
                     auto data = LoadAlbum(request.id, storefront, request.library, *m_abort);
                     if (!PutIfCurrent(generation, data.info)) return;
                     for (auto& song : data.songs) {
                         if (!seen.emplace(song.track.id).second) continue;
                         songs.push_back(std::move(song));
                     }
                 }
                 ResolveSongDisplayNames(songs, generation, *m_abort);
             } catch (const exception_aborted&) {
                 return;
             } catch (const std::exception& exception) {
                 error = Wide(exception.what());
             }
             PostMain(generation,
                      [done = std::move(done), songs = std::move(songs), error = std::move(error)]() mutable {
                          if (error.empty()) Catalog::Get().PutAndHint(songs);
                          if (done) done(DisplayTracks(songs), std::move(error));
                      });
         });
}

void StreamProvider::FetchArtist(const stream::Id& artist, ArtistDone done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, signInError);
        return;
    }

    const auto cached = Catalog::Get().FindArtist(artist);
    const auto region = GetAppleMusicDisplayRegion();
    const std::wstring sourceStorefront = DisplayMetadata::Get().SourceStorefront();
    uint64_t preferenceEpoch = 0;
    {
        std::lock_guard lock(m_mutex);
        preferenceEpoch = m_artistPreferenceEpoch;
    }
    if (cached && cached->albumCount != stream::kUnknownCount && cached->followKnown &&
        (region == AppleMusicDisplayRegion::Account ||
         DisplayMetadata::Get().IsArtistComplete(region, sourceStorefront, artist))) {
        if (done) {
            stream::Artist info = *cached;
            ApplyArtistPreferenceOverlay(info, preferenceEpoch);
            fb2k::inMainThread(
                [done = std::move(done), info = std::move(info)]() mutable { done(std::move(info), {}); });
        }
        return;
    }

    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    bool library = IsLibraryId(artist) && artist[0] == L'r';
    stream::Id catalogId = ArtistCatalogId(artist);
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation, [this, generation, preferenceEpoch, artist, storefront, library, catalogId,
                      done = std::move(done)]() mutable {
        stream::Artist info;
        std::wstring error;
        try {
            if (auto loaded = LoadArtist(artist, storefront, library, *m_abort)) {
                info = std::move(loaded->info);
                ApplyArtistPreferenceOverlay(info, preferenceEpoch);
                if (catalogId.empty()) catalogId = loaded->catalogId;
                if (info.albumCount == stream::kUnknownCount) {
                    try {
                        info.albumCount = CountArtistAlbums(artist, catalogId, storefront, library, *m_abort);
                    } catch (const exception_aborted&) {
                        throw;
                    } catch (...) {
                    }
                }
                if (!PutIfCurrent(generation, info)) return;
                std::vector<stream::Artist> displayArtist{info};
                ResolveArtistDisplayNames(displayArtist, generation, *m_abort);
                if (!displayArtist.empty()) {
                    DisplayMetadata::Get().Apply(displayArtist.front(),
                                                 DisplayMetadata::Get().SourceStorefront());
                    info = std::move(displayArtist.front());
                }
            } else {
                error = L"Apple Music returned no artist.";
            }
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, generation, artist, catalogId, done = std::move(done),
                              info = std::move(info), error = std::move(error)]() mutable {
            bool changed = false;
            if (error.empty() && !catalogId.empty()) {
                std::lock_guard lock(m_mutex);
                m_artistCatalogIds[artist] = catalogId;
            }
            if (error.empty()) {
                std::lock_guard lock(m_mutex);
                if (generation == m_generation) {
                    for (auto& item : m_libraryArtists) {
                        if (item.id != artist || item.albumCount == info.albumCount) continue;
                        item.albumCount = info.albumCount;
                        item.trackCount = info.trackCount;
                        Catalog::Get().Put(item);
                        changed = true;
                        break;
                    }
                }
            }
            if (changed) NotifyLibraryChanged();
            if (done) done(std::move(info), std::move(error));
        });
    });
}

void StreamProvider::FetchArtistBio(const stream::Id& artist, TextDone done) {
    const auto region = GetAppleMusicDisplayRegion();
    const std::wstring sourceStorefront = DisplayMetadata::Get().SourceStorefront();
    if (const auto cached = Catalog::Get().FindArtist(artist);
        cached && (region == AppleMusicDisplayRegion::Account ||
                   DisplayMetadata::Get().IsArtistComplete(region, sourceStorefront, artist))) {
        if (done) done(cached->bio, {});
        return;
    }
    uint64_t generation = 0;
    uint64_t preferenceEpoch = 0;
    const std::wstring storefront = Storefront();
    bool library = IsLibraryId(artist) && artist[0] == L'r';
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        preferenceEpoch = m_artistPreferenceEpoch;
    }
    Post(generation,
         [this, generation, preferenceEpoch, artist, storefront, library, done = std::move(done)]() mutable {
             std::wstring text;
             std::wstring error;
             try {
                 if (auto loaded = LoadArtist(artist, storefront, library, *m_abort)) {
                     ApplyArtistPreferenceOverlay(loaded->info, preferenceEpoch);
                     if (!PutIfCurrent(generation, loaded->info)) return;
                     std::vector<stream::Artist> displayArtist{loaded->info};
                     ResolveArtistDisplayNames(displayArtist, generation, *m_abort);
                     if (!displayArtist.empty()) {
                         DisplayMetadata::Get().Apply(displayArtist.front(),
                                                      DisplayMetadata::Get().SourceStorefront());
                         text = displayArtist.front().bio;
                     }
                 } else {
                     error = L"Apple Music returned no artist.";
                 }
             } catch (const exception_aborted&) {
                 return;
             } catch (const std::exception& exception) {
                 error = Wide(exception.what());
             }
             PostMain(generation,
                      [done = std::move(done), text = std::move(text), error = std::move(error)]() mutable {
                          if (done) done(std::move(text), std::move(error));
                      });
         });
}

void StreamProvider::FetchArtistAlbums(const stream::Id& artist, size_t offset, size_t limit,
                                       AlbumPageDone done) {
    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    bool library = IsLibraryId(artist) && artist[0] == L'r';
    stream::Id catalogId = ArtistCatalogId(artist);
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation, [this, generation, artist, storefront, library, catalogId, offset, limit,
                      done = std::move(done)]() mutable {
        std::vector<stream::Album> albums;
        bool more = false;
        std::wstring error;
        try {
            if (library && catalogId.empty()) {
                catalogId = ResolveLibraryArtistCatalogId(artist, *m_abort);
            }
            const std::wstring path = ArtistAlbumsPath(artist, catalogId, storefront, library, offset, limit);
            const Object response = Apple(path, *m_abort);
            if (const auto data = json::GetArray(response, L"data"))
                albums = ParseAlbums(json::Objects(*data));
            more = !json::GetString(response, L"next").empty();
            if (!PutIfCurrent(generation, albums)) return;
            ResolveAlbumDisplayNames(albums, generation, *m_abort);
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, generation, artist, catalogId, offset, done = std::move(done),
                              albums = std::move(albums), more, error = std::move(error)]() mutable {
            bool countChanged = false;
            if (error.empty()) {
                std::lock_guard lock(m_mutex);
                if (!catalogId.empty()) m_artistCatalogIds[artist] = catalogId;
                if (offset == 0 && !more) {
                    for (auto& item : m_libraryArtists) {
                        if (item.id != artist) continue;
                        const int count =
                            static_cast<int>(std::min(albums.size(), static_cast<size_t>(INT_MAX)));
                        countChanged = item.albumCount != count;
                        item.albumCount = count;
                        Catalog::Get().Put(item);
                        break;
                    }
                }
            }
            if (error.empty() && more) EnrichArtistCountsAsync(generation);
            if (countChanged) NotifyLibraryChanged();
            if (done) done(DisplayAlbums(std::move(albums)), more, std::nullopt, std::move(error));
        });
    });
}

void StreamProvider::FetchArtistTopTracks(const stream::Id& artist, size_t offset, size_t limit,
                                          TrackPageDone done) {
    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    bool library = IsLibraryId(artist) && artist[0] == L'r';
    stream::Id catalogId = ArtistCatalogId(artist);
    const bool libraryOnly = library && catalogId.empty();
    std::wstring artistName;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        if (libraryOnly) {
            const auto info =
                std::find_if(m_libraryArtists.begin(), m_libraryArtists.end(),
                             [&artist](const stream::Artist& item) { return item.id == artist; });
            if (info != m_libraryArtists.end()) artistName = info->name;
            const auto found = m_artistLibraryTracks.find(artist);
            if (found != m_artistLibraryTracks.end()) {
                bool more = false;
                auto tracks = DisplayTracks(TrackPage(found->second, offset, limit, more));
                if (done) {
                    fb2k::inMainThread([done = std::move(done), tracks = std::move(tracks), more]() mutable {
                        done(std::move(tracks), more, {});
                    });
                }
                return;
            }
        }
    }
    if (libraryOnly && artistName.empty()) {
        if (const auto cached = Catalog::Get().FindArtistRaw(artist)) artistName = cached->name;
    }
    Post(generation, [this, generation, artist, artistName = std::move(artistName), storefront,
                      initialCatalogId = catalogId, offset, limit, libraryOnly,
                      done = std::move(done)]() mutable {
        std::vector<Song> songs;
        bool more = false;
        std::wstring error;
        stream::Id catalogId = initialCatalogId;
        bool loadLibraryOnly = libraryOnly;
        try {
            if (loadLibraryOnly) {
                catalogId = ResolveLibraryArtistCatalogId(artist, *m_abort);
                loadLibraryOnly = catalogId.empty();
            }
            if (loadLibraryOnly) {
                // Library-only artists have no popularity endpoint. Return their real library
                // tracks in album order without inventing ranking data.
                LibraryArtistData data = LoadLibraryArtistData(artist, artistName, *m_abort);
                if (!PutIfCurrent(generation, data.albums)) return;
                songs = std::move(data.songs);
                if (!PutIfCurrent(generation, songs)) return;
            } else if (catalogId.empty()) {
                error = L"No Apple Music catalog artist is available.";
            } else {
                const std::wstring path = L"/v1/catalog/" + storefront + L"/artists/" + Encode(catalogId) +
                                          L"/view/top-songs?limit=" + std::to_wstring(PageLimit(limit)) +
                                          L"&offset=" + std::to_wstring(offset) + L"&include=albums";
                const Object response = Apple(path, *m_abort);
                if (const auto data = json::GetArray(response, L"data"))
                    songs = ParseSongs(json::Objects(*data));
                HydrateSongAlbums(songs, storefront, *m_abort);
                more = !json::GetString(response, L"next").empty();
                if (!PutIfCurrent(generation, songs)) return;
            }
            ResolveSongDisplayNames(songs, generation, *m_abort);
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation,
                 [this, generation, artist, catalogId, offset, limit, loadLibraryOnly, done = std::move(done),
                  songs = std::move(songs), more, error = std::move(error)]() mutable {
                     std::vector<stream::Track> tracks;
                     bool libraryChanged = false;
                     if (error.empty() && !catalogId.empty()) {
                         std::lock_guard lock(m_mutex);
                         m_artistCatalogIds[artist] = catalogId;
                     }
                     if (error.empty()) {
                         Catalog::Get().PutAndHint(songs);
                         if (loadLibraryOnly) {
                             const auto allTracks = Tracks(songs);
                             {
                                 std::lock_guard lock(m_mutex);
                                 if (generation == m_generation) {
                                     m_artistLibraryTracks[artist] = allTracks;
                                     for (auto& item : m_libraryArtists) {
                                         if (item.id != artist) continue;
                                         item.trackCount = static_cast<int>(
                                             std::min(allTracks.size(), static_cast<size_t>(INT_MAX)));
                                         Catalog::Get().Put(item);
                                         libraryChanged = true;
                                         break;
                                     }
                                 }
                             }
                             tracks = DisplayTracks(TrackPage(allTracks, offset, limit, more));
                         } else {
                             tracks = DisplayTracks(songs);
                         }
                     }
                     if (libraryChanged) NotifyLibraryChanged();
                     if (done) done(std::move(tracks), more, std::move(error));
                 });
    });
}

void StreamProvider::FetchFollowedArtists(ArtistsDone done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, signInError);
        return;
    }
    uint64_t generation = 0;
    bool loaded = false;
    bool fresh = false;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        loaded = m_librarySnapshotLoaded;
        fresh = loaded && std::chrono::steady_clock::now() < m_librarySnapshotValidUntil;
    }
    std::vector<stream::Artist> cached = loaded ? FollowedArtists() : std::vector<stream::Artist>();
    if (loaded) {
        if (!fresh) RefreshAccountAsync(generation, {}, true);
        if (done) {
            fb2k::inMainThread([done = std::move(done), cached = std::move(cached)]() mutable {
                done(std::move(cached), {});
            });
        }
        return;
    }
    RefreshLibraryPreservingDetails([this, done = std::move(done)](std::wstring error) mutable {
        if (done) done(FollowedArtists(), std::move(error));
    });
}

void StreamProvider::FetchSavedAlbums(size_t offset, size_t limit, AlbumPageDone done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, false, std::nullopt, signInError);
        return;
    }
    uint64_t generation = 0;
    bool loaded = false;
    bool fresh = false;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        loaded = m_librarySnapshotLoaded;
        fresh = loaded && std::chrono::steady_clock::now() < m_librarySnapshotValidUntil;
    }
    std::vector<stream::Album> cached = loaded ? SavedAlbums() : std::vector<stream::Album>();
    if (loaded) {
        if (!fresh) RefreshAccountAsync(generation, {}, true);
        const size_t begin = std::min(offset, cached.size());
        const size_t end = std::min(cached.size(), begin + PageLimit(limit));
        std::vector<stream::Album> page(cached.begin() + begin, cached.begin() + end);
        if (done) {
            const bool more = end < cached.size();
            fb2k::inMainThread([done = std::move(done), page = DisplayAlbums(std::move(page)),
                                more]() mutable { done(std::move(page), more, std::nullopt, {}); });
        }
        return;
    }
    Post(generation, [this, generation, offset, limit, done = std::move(done)]() mutable {
        std::vector<stream::Album> albums;
        bool more = false;
        std::wstring error;
        try {
            const std::wstring path = L"/v1/me/library/albums?limit=" + std::to_wstring(PageLimit(limit)) +
                                      L"&offset=" + std::to_wstring(offset) + L"&include=catalog";
            const Object response = Apple(path, *m_abort);
            if (const auto data = json::GetArray(response, L"data"))
                albums = ParseAlbums(json::Objects(*data));
            more = !json::GetString(response, L"next").empty();
            if (!PutIfCurrent(generation, albums)) return;
            ResolveAlbumDisplayNames(albums, generation, *m_abort);
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [done = std::move(done), albums = std::move(albums), more,
                              error = std::move(error)]() mutable {
            if (done) done(DisplayAlbums(std::move(albums)), more, std::nullopt, std::move(error));
        });
    });
}

stream::ListCachePolicy StreamProvider::ListPolicy(const stream::ListQuery& query) const {
    if (query.cache) return *query.cache;
    if (stream::IsCatalogSearch(query.kind)) return stream::ListCachePolicy::None;
    switch (query.kind) {
    case stream::ListKind::ArtistTopTracks:
    case stream::ListKind::ArtistAlbums:
    case stream::ListKind::ArtistPlaylists:
        return stream::ListCachePolicy::Memory;
    // Genres and composers are derived from the whole library (every song page); kept on disk
    // they show at once after a restart while the derivation runs again behind them.
    case stream::ListKind::Groups:
    case stream::ListKind::PlaylistTracks:
    case stream::ListKind::Favorites:
    case stream::ListKind::AlbumTracks:
    case stream::ListKind::SectionTracks:
    case stream::ListKind::SavedAlbums:
    case stream::ListKind::FollowedArtists:
    case stream::ListKind::Playlists:
        return stream::ListCachePolicy::Persistent;
    case stream::ListKind::CatalogSearch:
    case stream::ListKind::CatalogSearchArtists:
    case stream::ListKind::CatalogSearchAlbums:
    case stream::ListKind::CatalogSearchPlaylists:
    case stream::ListKind::CatalogSearchTopResults:
        return stream::ListCachePolicy::None;
    }
    return stream::ListCachePolicy::Persistent;
}

std::string StreamProvider::ListCacheContext(const stream::ListQuery& query) const {
    (void)query;
    return AppleListCacheContext();
}

void StreamProvider::RestoreListRecords(const stream::ListPage& page) {
    // Whatever the names, a restored song's album is known again: its cover is named by the
    // album, as when the list was read (see Catalog::SongAlbum).
    if (const auto* tracks = std::get_if<std::vector<stream::Track>>(&page.rows))
        for (const auto& track : *tracks)
            Catalog::Get().PutSongAlbum(track.id, track.albumId);
    if (const auto target = DisplayTargetFor(GetAppleMusicDisplayRegion())) {
        WarmDisplayCacheFromPage(page, *target, DisplayMetadata::Get().SourceStorefront());
        // The rows carry another region's names, so they do not replace what the catalog
        // knows; songs it does not know at all are installed, or playback and the queue would
        // show them as "Apple Music <id>" until something read them again.
        if (const auto* tracks = std::get_if<std::vector<stream::Track>>(&page.rows)) {
            std::vector<Song> songs;
            songs.reserve(tracks->size());
            for (const auto& track : *tracks) {
                Song song;
                song.track = track;
                if (IsLibraryId(track.id) && track.id[0] == L'i')
                    song.libraryId = track.id;
                else
                    song.catalogId = track.id;
                songs.push_back(std::move(song));
            }
            Catalog::Get().InstallCached(std::move(songs));
        }
        return;
    }
    std::visit(
        [](const auto& rows) {
            using Rows = std::decay_t<decltype(rows)>;
            if constexpr (std::is_same_v<Rows, std::vector<stream::Track>>) {
                std::vector<Song> songs;
                songs.reserve(rows.size());
                for (const auto& track : rows) {
                    Song song;
                    song.track = track;
                    if (IsLibraryId(track.id) && track.id[0] == L'i')
                        song.libraryId = track.id;
                    else
                        song.catalogId = track.id;
                    songs.push_back(std::move(song));
                }
                auto merged = MergeSongRecordsWithCatalog(std::move(songs), false);
                // Disk list data is authoritative for the list itself; this only rebuilds
                // in-memory catalog entries and metadb hints needed by playback and artwork.
                Catalog::Get().PutAndHint(merged, false);
            } else if constexpr (std::is_same_v<Rows, std::vector<stream::Playlist>>) {
                for (const auto& row : rows) {
                    auto merged = row;
                    if (const auto existing = Catalog::Get().FindPlaylistRaw(row.id))
                        merged = MergePlaylistRecord(*existing, row, true);
                    Catalog::Get().Put(merged);
                }
            } else if constexpr (std::is_same_v<Rows, std::vector<stream::Album>>) {
                for (const auto& row : rows) {
                    auto merged = row;
                    if (const auto existing = Catalog::Get().FindAlbumRaw(row.id))
                        merged = MergeAlbumRecord(*existing, row);
                    Catalog::Get().Put(merged);
                }
            } else if constexpr (std::is_same_v<Rows, std::vector<stream::Artist>>) {
                for (const auto& row : rows) {
                    auto merged = row;
                    if (const auto existing = Catalog::Get().FindArtistRaw(row.id))
                        merged = MergeArtistRecord(*existing, row, true);
                    Catalog::Get().Put(merged);
                }
            } else if constexpr (std::is_same_v<Rows, std::vector<stream::CatalogSearchItem>>) {
                std::vector<Song> songs;
                for (const auto& item : rows) {
                    std::visit(
                        [&songs](const auto& row) {
                            using Item = std::decay_t<decltype(row)>;
                            if constexpr (std::is_same_v<Item, stream::Track>) {
                                Song song = BaselineSong(row);
                                if (const auto existing = Catalog::Get().FindSongRaw(row.id))
                                    song = MergeSongRecord(*existing, song);
                                song.track.id = row.id;
                                songs.push_back(std::move(song));
                            } else {
                                Catalog::Get().Put(row);
                            }
                        },
                        item);
                }
                Catalog::Get().PutAndHint(songs, false);
            }
        },
        page.rows);

    if (page.playlist) {
        auto merged = *page.playlist;
        if (const auto existing = Catalog::Get().FindPlaylistRaw(page.playlist->id))
            merged = MergePlaylistRecord(*existing, *page.playlist, true);
        Catalog::Get().Put(merged);
    }
    if (page.album) {
        auto merged = *page.album;
        if (const auto existing = Catalog::Get().FindAlbumRaw(page.album->id))
            merged = MergeAlbumRecord(*existing, *page.album);
        Catalog::Get().Put(merged);
    }
    if (page.artist) {
        auto merged = *page.artist;
        if (const auto existing = Catalog::Get().FindArtistRaw(page.artist->id))
            merged = MergeArtistRecord(*existing, *page.artist, true);
        Catalog::Get().Put(merged);
    }
}

void StreamProvider::FetchListPage(const stream::ListQuery& sourceQuery, const stream::ListCursor& cursor,
                                   size_t limit, stream::ListPageDone done) {
    uint64_t generation = 0;
    std::wstring storefront;
    std::wstring locale;
    uint64_t preferenceEpoch = 0;
    stream::Id favoritePlaylist;
    FavoriteBindingState favoriteBindingState = FavoriteBindingState::Unknown;
    bool signedIn = false;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        storefront = m_account.signedIn && m_account.storefront.size() == 2 ? m_account.storefront
                                                                            : std::wstring(L"US");
        locale = m_locale;
        preferenceEpoch = m_artistPreferenceEpoch;
        favoritePlaylist = m_favoritePlaylistId;
        favoriteBindingState = m_favoriteBindingState;
        signedIn = m_account.signedIn;
    }

    stream::ListQuery query = sourceQuery;
    if (query.kind == stream::ListKind::Playlists) {
        if (query.section == kCreatedSection)
            query.shelf = stream::Shelf::Created;
        else if (query.section == kSavedSection)
            query.shelf = stream::Shelf::Saved;
    }
    const bool requiresAccount =
        query.kind == stream::ListKind::PlaylistTracks || query.kind == stream::ListKind::Favorites ||
        query.kind == stream::ListKind::AlbumTracks || query.kind == stream::ListKind::SectionTracks ||
        query.kind == stream::ListKind::SavedAlbums || query.kind == stream::ListKind::FollowedArtists ||
        query.kind == stream::ListKind::Playlists;
    if (requiresAccount && !signedIn) {
        if (done) done({}, L"Please sign in to Apple Music.");
        return;
    }

    const bool favoriteQuery =
        query.kind == stream::ListKind::Favorites ||
        (query.kind == stream::ListKind::SectionTracks && query.section == kFavoritesSection);
    if (favoriteQuery && favoritePlaylist.empty()) {
        constexpr size_t kMaximumFavoriteWaiters = 64;
        bool knownEmpty = false;
        bool startBinding = false;
        bool overflow = false;
        uint64_t bindingEpoch = 0;
        {
            std::lock_guard lock(m_mutex);
            if (!m_started || generation != m_generation) return;
            favoritePlaylist = m_favoritePlaylistId;
            favoriteBindingState = m_favoriteBindingState;
            if (favoritePlaylist.empty() && favoriteBindingState != FavoriteBindingState::Empty) {
                std::erase_if(m_favoritePageWaiters, [generation](const FavoritePageWaiter& waiter) {
                    return waiter.generation != generation || ListCancelled(waiter.query.cancelled) ||
                           !waiter.done;
                });
                if (!m_favoriteBindingReadInFlight) {
                    m_favoriteBindingState = FavoriteBindingState::Pending;
                    if (m_refreshInFlight && m_refreshGeneration == generation) {
                        m_favoriteBindingReadInFlight = true;
                    } else {
                        ++m_favoriteBindingEpoch;
                        m_favoriteBindingReadInFlight = true;
                        startBinding = true;
                    }
                }
                bindingEpoch = m_favoriteBindingEpoch;
                if (m_favoritePageWaiters.size() >= kMaximumFavoriteWaiters) {
                    overflow = true;
                } else {
                    m_favoritePageWaiters.push_back({generation, query, cursor, limit, done});
                }
            } else if (favoriteBindingState == FavoriteBindingState::Empty) {
                knownEmpty = true;
            }
        }
        if (overflow) {
            if (!ListCancelled(query.cancelled) && done)
                done({}, L"Too many pending Apple Music Favorite Songs requests.");
            return;
        }
        if (knownEmpty) {
            stream::ListPage empty;
            empty.rows = std::vector<stream::Track>{};
            empty.total = 0;
            if (!ListCancelled(query.cancelled) && done) done(std::move(empty), {});
            return;
        }
        if (favoritePlaylist.empty()) {
            if (startBinding) StartFavoriteBindingRead(generation, bindingEpoch);
            return;
        }
    }
    if (favoriteQuery) query.id = favoritePlaylist;

    const uint64_t fingerprint = ListQueryFingerprint(query, storefront, locale);
    ListCursorState cursorState;
    std::wstring cursorError;
    if (!DecodeListCursor(cursor, query.kind, fingerprint, cursorState, cursorError)) {
        if (done) done({}, std::move(cursorError));
        return;
    }

    const size_t pageLimit = std::clamp<size_t>(limit, 1, 100);
    const bool derivedGroupsPage = query.kind == stream::ListKind::Groups;
    Post(generation, [this, query = std::move(query), cursorState, fingerprint, storefront, generation,
                      preferenceEpoch, pageLimit, derivedGroupsPage, done = std::move(done)]() mutable {
        if (query.cancelled && query.cancelled->load(std::memory_order_acquire)) return;
        stream::ListPage page;
        std::vector<Song> songs;
        bool trackRows = false;
        bool catalogItemRows = false;
        std::wstring error;
        try {
            abort_callback& abort = *m_abort;
            const auto nextOffset = [&cursorState](size_t sourceRows) {
                if (cursorState.offset > std::numeric_limits<size_t>::max() - sourceRows)
                    throw std::runtime_error("Apple Music list position overflowed.");
                return cursorState.offset + sourceRows;
            };
            const auto currentRequest = [&cursorState](std::wstring_view expected, auto&& add_parameters) {
                if (cursorState.path.empty()) return std::wstring();
                const std::wstring normalized = NormalizeListNext(expected, cursorState.path);
                std::wstring request = add_parameters(std::move(normalized));
                if (HistoryContains(cursorState.history, Fnv1a(Utf8(request))))
                    throw std::runtime_error("Apple Music returned a cyclic list continuation.");
                return request;
            };
            const auto setNext = [&](const Object& collection, std::wstring_view expected,
                                     auto&& add_parameters, size_t sourceRows) {
                const std::wstring raw = json::GetString(collection, L"next");
                if (raw.empty()) return;
                const std::wstring normalized = NormalizeListNext(expected, raw);
                const std::wstring nextRequest = add_parameters(normalized);
                std::wstring nextError;
                if (!BuildListCursor(cursorState, query.kind, Hex64(fingerprint),
                                     cursorState.path.empty() ? expected : cursorState.path,
                                     nextOffset(sourceRows), expected, raw, nextRequest, page.next,
                                     nextError)) {
                    throw std::runtime_error(Utf8(nextError));
                }
                page.more = true;
            };
            const auto parseRelationshipPage = [&](const Object& item, const wchar_t* relationship,
                                                   std::wstring_view expected, bool library) {
                const auto relation = Relationship(item, relationship);
                std::vector<Object> objects = EmbeddedRelationship(item, relationship);
                std::wstring next = relation ? json::GetString(*relation, L"next") : std::wstring();
                std::wstring href = relation ? json::GetString(*relation, L"href") : std::wstring();
                if (objects.empty()) {
                    const std::wstring firstPath = !href.empty() ? href : next;
                    if (!firstPath.empty()) {
                        const std::wstring path =
                            RelationshipPagePath(NormalizeListNext(expected, firstPath), library, pageLimit);
                        if (HistoryContains(cursorState.history, Fnv1a(Utf8(path))))
                            throw std::runtime_error("Apple Music returned a cyclic list continuation.");
                        const Object response = Apple(path, abort);
                        if (const auto data = json::GetArray(response, L"data"))
                            objects = json::Objects(*data);
                        if (relation)
                            if (const auto total = CollectionTotal(*relation)) page.total = total;
                        if (const auto total = CollectionTotal(response)) page.total = total;
                        setNext(
                            response, expected,
                            [library, pageLimit](std::wstring path) {
                                return RelationshipPagePath(std::move(path), library, pageLimit);
                            },
                            objects.size());
                        return objects;
                    }
                }
                if (relation)
                    if (const auto total = CollectionTotal(*relation)) page.total = total;
                setNext(
                    relation ? *relation : Object(), expected,
                    [library, pageLimit](std::wstring path) {
                        return RelationshipPagePath(std::move(path), library, pageLimit);
                    },
                    objects.size());
                return objects;
            };
            const auto loadPlaylistTracks = [&](const stream::Id& playlist, bool library) {
                trackRows = true;
                const std::wstring tracksPath = PlaylistTracksPath(playlist, storefront, library);
                std::vector<Object> objects;
                if (library) page.playlist = Catalog::Get().FindPlaylistRaw(playlist);
                if (page.playlist && page.playlist->trackCount >= 0)
                    page.total = static_cast<size_t>(page.playlist->trackCount);
                // Apple's library playlist detail endpoint rejects projected relationship
                // queries in practice, so the tracks collection itself is page one.
                // The catalog answers a resource request that carries "limit" with 400 ("Limit may
                // not be supplied on this request", observed 2026-11-13); its included tracks come
                // in the service's own page size, continued through the relationship's next link.
                if (cursorState.path.empty() && !library) {
                    const Object response = Apple(PlaylistPath(playlist, storefront, library, true), abort);
                    const auto item = FirstData(response);
                    if (!item) throw std::runtime_error("Apple Music returned no playlist.");
                    page.playlist = ParsePlaylist(*item);
                    if (!page.playlist) throw std::runtime_error("Apple Music returned an invalid playlist.");
                    objects = parseRelationshipPage(*item, L"tracks", tracksPath, library);
                    if (!page.total && page.playlist->trackCount >= 0)
                        page.total = static_cast<size_t>(page.playlist->trackCount);
                } else {
                    const std::wstring request =
                        cursorState.path.empty()
                            ? RelationshipPagePath(tracksPath, library, pageLimit)
                            : currentRequest(tracksPath, [library, pageLimit](std::wstring path) {
                                  return RelationshipPagePath(std::move(path), library, pageLimit);
                              });
                    const Object response = Apple(request, abort);
                    if (const auto data = json::GetArray(response, L"data")) objects = json::Objects(*data);
                    if (const auto total = CollectionTotal(response)) page.total = total;
                    setNext(
                        response, tracksPath,
                        [library, pageLimit](std::wstring path) {
                            return RelationshipPagePath(std::move(path), library, pageLimit);
                        },
                        objects.size());
                }
                songs = ParseSongs(objects);
            };
            const auto loadAlbumTracks = [&](const stream::Id& album, bool library) {
                trackRows = true;
                const std::wstring tracksPath = AlbumTracksPath(album, storefront, library);
                std::vector<Object> objects;
                std::optional<stream::Album> albumInfo;
                // Library album details show the same query restriction as playlists;
                // the tracks collection is the stable source for page one and continuations.
                if (cursorState.path.empty() && !library) { // no "limit": see the playlist above
                    const Object response = Apple(AlbumPath(album, storefront, library), abort);
                    const auto item = FirstData(response);
                    if (!item) throw std::runtime_error("Apple Music returned no album.");
                    albumInfo = ParseAlbum(*item);
                    if (!albumInfo) throw std::runtime_error("Apple Music returned an invalid album.");
                    page.album = albumInfo;
                    objects = parseRelationshipPage(*item, L"tracks", tracksPath, library);
                    if (!page.total && albumInfo->trackCount >= 0)
                        page.total = static_cast<size_t>(albumInfo->trackCount);
                } else {
                    const std::wstring request =
                        cursorState.path.empty()
                            ? RelationshipPagePath(tracksPath, library, pageLimit)
                            : currentRequest(tracksPath, [library, pageLimit](std::wstring path) {
                                  return RelationshipPagePath(std::move(path), library, pageLimit);
                              });
                    const Object response = Apple(request, abort);
                    if (const auto data = json::GetArray(response, L"data")) objects = json::Objects(*data);
                    if (const auto total = CollectionTotal(response)) page.total = total;
                    setNext(
                        response, tracksPath,
                        [library, pageLimit](std::wstring path) {
                            return RelationshipPagePath(std::move(path), library, pageLimit);
                        },
                        objects.size());
                }
                songs = ParseSongs(objects);
                if (!albumInfo) albumInfo = Catalog::Get().FindAlbumRaw(album);
                if (albumInfo) {
                    page.album = albumInfo;
                    if (albumInfo->trackCount >= 0) page.total = static_cast<size_t>(albumInfo->trackCount);
                    for (auto& song : songs)
                        ApplyAlbumContext(song, *albumInfo);
                }
            };

            switch (query.kind) {
            case stream::ListKind::PlaylistTracks: {
                if (query.id.empty()) throw std::runtime_error("The playlist list has no id.");
                const bool library = IsLibraryId(query.id) && query.id[0] == L'p';
                loadPlaylistTracks(query.id, library);
                break;
            }
            case stream::ListKind::Favorites: {
                loadPlaylistTracks(query.id, true);
                break;
            }
            case stream::ListKind::AlbumTracks: {
                if (query.id.empty()) throw std::runtime_error("The album list has no id.");
                const AlbumRequest request = ResolveAlbumRequest(query.id);
                loadAlbumTracks(request.id, request.library);
                break;
            }
            case stream::ListKind::SectionTracks: {
                if (query.section == kFavoritesSection) {
                    if (query.id.empty())
                        throw std::runtime_error("The Favorite Songs system playlist was not found.");
                    loadPlaylistTracks(query.id, true);
                    break;
                }
                if (query.section == kSongsSection) {
                    if (!query.id.empty())
                        throw std::runtime_error("The Songs list does not take a group id.");
                    trackRows = true;
                    const std::wstring expected = L"/v1/me/library/songs";
                    const std::wstring queryParameters =
                        L"limit=" + std::to_wstring(pageLimit) +
                        L"&include=catalog,albums,artists&include[songs]=albums"
                        L"&extend=editorialVideo&platform=web";
                    std::wstring request;
                    if (cursorState.path.empty()) {
                        request = L"/v1/me/library/songs?" + queryParameters;
                    } else {
                        request = currentRequest(expected, [&queryParameters, pageLimit](std::wstring path) {
                            path = WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                            return EnsureQuery(std::move(path), queryParameters);
                        });
                    }
                    const Object response = Apple(request, abort);
                    std::vector<Object> objects;
                    if (const auto data = json::GetArray(response, L"data")) objects = json::Objects(*data);
                    songs = ParseSongs(objects);
                    page.total = CollectionTotal(response);
                    setNext(
                        response, expected,
                        [&queryParameters, pageLimit](std::wstring path) {
                            path = WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                            return EnsureQuery(std::move(path), queryParameters);
                        },
                        objects.size());
                    break;
                }
                if (query.section != kGenresSection && query.section != kComposersSection)
                    throw std::runtime_error("This section does not provide tracks directly.");
                if (!cursorState.path.empty())
                    throw std::runtime_error("This derived list has no continuation.");
                const bool genres = query.section == kGenresSection;
                const std::wstring prefix = genres ? kGenreGroupPrefix : kComposerGroupPrefix;
                if (!StartsWith(query.id, prefix) || query.id.size() == prefix.size())
                    throw std::runtime_error("The selected group is invalid for this section.");
                songs = SongsInGroup(RecentLibrarySongs(generation, abort), query.id, genres);
                page.total = songs.size();
                trackRows = true;
                break;
            }
            case stream::ListKind::ArtistTopTracks: {
                if (query.id.empty()) throw std::runtime_error("The artist list has no id.");
                if (!query.section.empty() && query.section != L"top-songs")
                    throw std::runtime_error("The artist track view is invalid.");
                trackRows = true;
                const bool library = IsLibraryId(query.id) && query.id[0] == L'r';
                stream::Id catalogId = ArtistCatalogId(query.id);
                if (library && catalogId.empty()) catalogId = ResolveLibraryArtistCatalogId(query.id, abort);
                if (library && catalogId.empty()) {
                    if (!cursorState.path.empty())
                        throw std::runtime_error("This derived list has no continuation.");
                    std::wstring artistName;
                    if (const auto cached = Catalog::Get().FindArtistRaw(query.id)) artistName = cached->name;
                    auto data = LoadLibraryArtistData(query.id, artistName, abort);
                    songs = std::move(data.songs);
                    page.total = songs.size();
                } else {
                    const stream::Id artistId = catalogId.empty() ? query.id : catalogId;
                    if (artistId.empty())
                        throw std::runtime_error("No Apple Music catalog artist is available.");
                    const std::wstring expected =
                        L"/v1/catalog/" + storefront + L"/artists/" + Encode(artistId) + L"/view/top-songs";
                    const std::wstring parameters =
                        L"limit=" + std::to_wstring(pageLimit) + L"&include=albums";
                    std::wstring request;
                    if (cursorState.path.empty()) {
                        request = expected + L"?" + parameters;
                    } else {
                        request = currentRequest(expected, [&parameters, pageLimit](std::wstring path) {
                            path = WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                            return EnsureQuery(std::move(path), parameters);
                        });
                    }
                    const Object response = Apple(request, abort);
                    std::vector<Object> objects;
                    if (const auto data = json::GetArray(response, L"data")) objects = json::Objects(*data);
                    songs = ParseSongs(objects);
                    page.total = CollectionTotal(response);
                    setNext(
                        response, expected,
                        [&parameters, pageLimit](std::wstring path) {
                            path = WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                            return EnsureQuery(std::move(path), parameters);
                        },
                        objects.size());
                }
                if (const auto artist = Catalog::Get().FindArtist(query.id)) page.artist = artist;
                break;
            }
            case stream::ListKind::ArtistAlbums: {
                if (query.id.empty()) throw std::runtime_error("The artist list has no id.");
                const bool library = IsLibraryId(query.id) && query.id[0] == L'r';
                stream::Id catalogId = ArtistCatalogId(query.id);
                if (library && catalogId.empty()) catalogId = ResolveLibraryArtistCatalogId(query.id, abort);
                if (!query.section.empty()) {
                    if (!IsArtistAlbumView(query.section))
                        throw std::runtime_error("The artist album view is invalid.");
                    if (catalogId.empty())
                        throw std::runtime_error("No Apple Music catalog artist is available.");
                    if (library) Catalog::Get().PutArtistCatalogId(query.id, catalogId);
                    const std::wstring expected = L"/v1/catalog/" + storefront + L"/artists/" +
                                                  Encode(catalogId) + L"/view/" + query.section;
                    const auto bindView = [pageLimit](std::wstring path) {
                        return WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                    };
                    const std::wstring request =
                        cursorState.path.empty() ? bindView(expected) : currentRequest(expected, bindView);
                    Object response;
                    try {
                        response = Apple(request, abort);
                    } catch (const BridgeHttpError& exception) {
                        if (exception.status() != 404 || !cursorState.path.empty() ||
                            !VerifyCatalogArtist(generation, storefront, catalogId, abort))
                            throw;
                        page.rows = std::vector<stream::Album>{};
                        page.total = 0;
                        if (const auto artist = Catalog::Get().FindArtist(query.id)) page.artist = artist;
                        break;
                    }
                    std::vector<Object> objects;
                    if (const auto data = json::GetArray(response, L"data")) objects = json::Objects(*data);
                    page.rows = ParseAlbums(objects);
                    page.total = CollectionTotal(response);
                    setNext(response, expected, bindView, objects.size());
                    if (const auto artist = Catalog::Get().FindArtist(query.id)) page.artist = artist;
                    break;
                }
                const std::wstring expected = library && catalogId.empty()
                                                  ? L"/v1/me/library/artists/" + Encode(query.id) + L"/albums"
                                                  : L"/v1/catalog/" + storefront + L"/artists/" +
                                                        Encode(catalogId.empty() ? query.id : catalogId) +
                                                        L"/albums";
                std::wstring request;
                if (cursorState.path.empty()) {
                    request = ArtistAlbumsPath(query.id, catalogId, storefront, library, 0, pageLimit);
                } else {
                    request = currentRequest(expected, [pageLimit](std::wstring path) {
                        return WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                    });
                }
                const Object response = Apple(request, abort);
                std::vector<Object> objects;
                if (const auto data = json::GetArray(response, L"data")) objects = json::Objects(*data);
                page.rows = ParseAlbums(objects);
                if (cursorState.path.empty()) {
                    auto& albums = std::get<std::vector<stream::Album>>(page.rows);
                    std::lock_guard lock(m_mutex);
                    std::erase_if(albums, [&](const stream::Album& album) {
                        return std::any_of(
                            m_albumMutations.begin(), m_albumMutations.end(), [&](const auto& entry) {
                                return !entry.second.saved &&
                                       (entry.first == album.id || entry.second.libraryId == album.id ||
                                        entry.second.catalogId == album.id);
                            });
                    });
                    for (const auto& [key, mutation] : m_albumMutations) {
                        if (!mutation.saved || mutation.album.id.empty()) continue;
                        const bool present =
                            std::any_of(albums.begin(), albums.end(), [&](const stream::Album& album) {
                                return album.id == key || album.id == mutation.catalogId ||
                                       album.id == mutation.libraryId;
                            });
                        if (!present) albums.push_back(mutation.album);
                    }
                }
                page.total = CollectionTotal(response);
                setNext(
                    response, expected,
                    [pageLimit](std::wstring path) {
                        return WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                    },
                    objects.size());
                if (const auto artist = Catalog::Get().FindArtist(query.id)) page.artist = artist;
                break;
            }
            case stream::ListKind::ArtistPlaylists: {
                if (query.id.empty()) throw std::runtime_error("The artist list has no id.");
                if (!IsArtistPlaylistView(query.section))
                    throw std::runtime_error("The artist playlist view is invalid.");
                const bool library = IsLibraryId(query.id) && query.id[0] == L'r';
                stream::Id catalogId = ArtistCatalogId(query.id);
                if (library && catalogId.empty()) catalogId = ResolveLibraryArtistCatalogId(query.id, abort);
                if (catalogId.empty())
                    throw std::runtime_error("No Apple Music catalog artist is available.");
                if (library) Catalog::Get().PutArtistCatalogId(query.id, catalogId);
                const std::wstring expected = L"/v1/catalog/" + storefront + L"/artists/" +
                                              Encode(catalogId) + L"/view/" + query.section;
                const auto bindView = [pageLimit](std::wstring path) {
                    return WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                };
                const std::wstring request =
                    cursorState.path.empty() ? bindView(expected) : currentRequest(expected, bindView);
                Object response;
                try {
                    response = Apple(request, abort);
                } catch (const BridgeHttpError& exception) {
                    if (exception.status() != 404 || !cursorState.path.empty() ||
                        !VerifyCatalogArtist(generation, storefront, catalogId, abort))
                        throw;
                    page.rows = std::vector<stream::Playlist>{};
                    page.total = 0;
                    if (const auto artist = Catalog::Get().FindArtist(query.id)) page.artist = artist;
                    break;
                }
                std::vector<Object> objects;
                if (const auto data = json::GetArray(response, L"data")) objects = json::Objects(*data);
                std::vector<stream::Playlist> playlists;
                for (const auto& object : objects)
                    if (auto playlist = ParsePlaylist(object)) playlists.push_back(std::move(*playlist));
                page.rows = std::move(playlists);
                page.total = CollectionTotal(response);
                setNext(response, expected, bindView, objects.size());
                if (const auto artist = Catalog::Get().FindArtist(query.id)) page.artist = artist;
                break;
            }
            case stream::ListKind::SavedAlbums: {
                const std::wstring expected = L"/v1/me/library/albums";
                const std::wstring parameters =
                    L"include=catalog,artists&platform=web&fields[library-albums]=name,artwork,"
                    L"artistName,genreNames,trackCount,playParams,releaseDate";
                std::wstring request;
                if (cursorState.path.empty()) {
                    request = expected + L"?limit=" + std::to_wstring(pageLimit) + L'&' + parameters;
                } else {
                    request = currentRequest(expected, [&parameters, pageLimit](std::wstring path) {
                        path = WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                        return EnsureQuery(std::move(path), parameters);
                    });
                }
                const Object response = Apple(request, abort);
                std::vector<Object> objects;
                if (const auto data = json::GetArray(response, L"data")) objects = json::Objects(*data);
                page.rows = ParseAlbums(objects);
                page.total = CollectionTotal(response);
                setNext(
                    response, expected,
                    [&parameters, pageLimit](std::wstring path) {
                        path = WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                        return EnsureQuery(std::move(path), parameters);
                    },
                    objects.size());
                break;
            }
            case stream::ListKind::FollowedArtists: {
                const std::wstring expected = L"/v1/me/library/artists";
                const std::wstring parameters =
                    L"include=catalog&extend[library-artists]=inFavorites&extend=inFavorites&platform=web";
                std::wstring request;
                if (cursorState.path.empty()) {
                    request = expected + L"?limit=" + std::to_wstring(pageLimit) + L'&' + parameters;
                } else {
                    request = currentRequest(expected, [&parameters, pageLimit](std::wstring path) {
                        path = WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                        return EnsureQuery(std::move(path), parameters);
                    });
                }
                const Object response = Apple(request, abort);
                std::vector<Object> objects;
                if (const auto data = json::GetArray(response, L"data")) objects = json::Objects(*data);
                page.rows = ParseArtists(objects);
                page.total = CollectionTotal(response);
                setNext(
                    response, expected,
                    [&parameters, pageLimit](std::wstring path) {
                        path = WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                        return EnsureQuery(std::move(path), parameters);
                    },
                    objects.size());
                break;
            }
            case stream::ListKind::Playlists: {
                if (!query.section.empty() && query.section != kCreatedSection &&
                    query.section != kSavedSection)
                    throw std::runtime_error("This provider playlist section is unavailable.");
                if (query.shelf == stream::Shelf::Recommended)
                    throw std::runtime_error("Apple Music does not expose a recommended playlist shelf.");
                const std::wstring expected = L"/v1/me/library/playlists";
                const std::wstring parameters =
                    L"include=catalog&extend[library-playlists]=tags&platform=web"
                    L"&fields[library-playlists]=name,artwork,editorialArtwork,tags,canEdit,"
                    L"canDelete,canRename,trackCount,playParams";
                std::wstring request;
                if (cursorState.path.empty()) {
                    request = expected + L"?limit=" + std::to_wstring(pageLimit) + L'&' + parameters;
                } else {
                    request = currentRequest(expected, [&parameters, pageLimit](std::wstring path) {
                        path = WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                        return EnsureQuery(std::move(path), parameters);
                    });
                }
                const Object response = Apple(request, abort);
                std::vector<Object> objects;
                if (const auto data = json::GetArray(response, L"data")) objects = json::Objects(*data);
                std::vector<stream::Playlist> rows;
                for (const auto& object : objects) {
                    if (IsFavoriteSystemPlaylist(object)) continue;
                    auto playlist = ParsePlaylist(object);
                    if (!playlist) continue;
                    if ((query.shelf == stream::Shelf::Created && !playlist->own) ||
                        (query.shelf == stream::Shelf::Saved && playlist->own))
                        continue;
                    rows.push_back(std::move(*playlist));
                }
                page.rows = std::move(rows);
                if (cursorState.path.empty() && query.shelf == stream::Shelf::Saved) {
                    auto& playlists = std::get<std::vector<stream::Playlist>>(page.rows);
                    std::lock_guard lock(m_mutex);
                    std::erase_if(playlists, [&](const stream::Playlist& item) {
                        return std::any_of(
                            m_playlistMutations.begin(), m_playlistMutations.end(), [&](const auto& entry) {
                                return !entry.second.saved &&
                                       (entry.first == item.id || entry.second.libraryId == item.id ||
                                        entry.second.catalogId == item.id);
                            });
                    });
                    for (const auto& [key, mutation] : m_playlistMutations) {
                        if (!mutation.saved || mutation.playlist.id.empty()) continue;
                        const bool present = std::any_of(
                            playlists.begin(), playlists.end(), [&](const stream::Playlist& item) {
                                return item.id == key || item.id == mutation.catalogId ||
                                       item.id == mutation.libraryId;
                            });
                        if (!present) playlists.push_back(mutation.playlist);
                    }
                }
                setNext(
                    response, expected,
                    [&parameters, pageLimit](std::wstring path) {
                        path = WithQueryValue(std::move(path), L"limit", std::to_wstring(pageLimit));
                        return EnsureQuery(std::move(path), parameters);
                    },
                    objects.size());
                break;
            }
            case stream::ListKind::Groups: {
                if (query.section != kGenresSection && query.section != kComposersSection)
                    throw std::runtime_error("This section does not provide groups.");
                if (!cursorState.path.empty())
                    throw std::runtime_error("This derived list has no continuation.");
                songs = RecentLibrarySongs(generation, abort);
                const bool genres = query.section == kGenresSection;
                auto groups = DerivedGroups(songs, songs, genres);
                page.total = groups.size();
                page.rows = std::move(groups);
                songs.clear();
                break;
            }
            case stream::ListKind::CatalogSearch:
            case stream::ListKind::CatalogSearchArtists:
            case stream::ListKind::CatalogSearchAlbums:
            case stream::ListKind::CatalogSearchPlaylists:
            case stream::ListKind::CatalogSearchTopResults: {
                if (query.search.empty()) {
                    if (!cursorState.path.empty())
                        throw std::runtime_error("The empty search has no continuation.");
                    trackRows = query.kind == stream::ListKind::CatalogSearch;
                    page.total = 0;
                    break;
                }
                const std::wstring expected = L"/v1/catalog/" + storefront + L"/search";
                const bool topResults = query.kind == stream::ListKind::CatalogSearchTopResults;
                std::wstring types;
                switch (query.kind) {
                case stream::ListKind::CatalogSearch:
                    types = L"songs";
                    break;
                case stream::ListKind::CatalogSearchArtists:
                    types = L"artists";
                    break;
                case stream::ListKind::CatalogSearchAlbums:
                    types = L"albums";
                    break;
                case stream::ListKind::CatalogSearchPlaylists:
                    types = L"playlists";
                    break;
                case stream::ListKind::CatalogSearchTopResults:
                    types = L"songs,artists,albums,playlists";
                    break;
                default:
                    throw std::runtime_error("Unsupported Apple Music search category.");
                }
                const size_t requestLimit = std::min(pageLimit, kCatalogSearchApiLimit);
                const auto bindSearch = [&](std::wstring path) {
                    path = WithQueryValue(std::move(path), L"limit", std::to_wstring(requestLimit));
                    path = WithQueryValue(std::move(path), L"types", types);
                    path = WithQueryValue(std::move(path), L"term", Encode(query.search));
                    path =
                        WithQueryValue(std::move(path), L"with",
                                       topResults ? std::wstring_view(L"topResults") : std::wstring_view());
                    if (query.kind == stream::ListKind::CatalogSearch)
                        path = WithQueryValue(std::move(path), L"include", L"albums");
                    return path;
                };
                const std::wstring request = cursorState.path.empty()
                                                 ? bindSearch(expected)
                                                 : currentRequest(expected, [&](std::wstring path) {
                                                       return bindSearch(std::move(path));
                                                   });
                const Object response = Apple(request, abort);
                const auto results = json::GetObject(response, L"results");
                if (!results) throw std::runtime_error("Apple Music returned an invalid search.");
                const wchar_t* resultKey = query.kind == stream::ListKind::CatalogSearch          ? L"songs"
                                           : query.kind == stream::ListKind::CatalogSearchArtists ? L"artists"
                                           : query.kind == stream::ListKind::CatalogSearchAlbums  ? L"albums"
                                           : query.kind == stream::ListKind::CatalogSearchPlaylists
                                               ? L"playlists"
                                               : L"topResults";
                const auto collection = json::GetObject(*results, resultKey);
                if (!collection) {
                    if (query.kind == stream::ListKind::CatalogSearch) {
                        trackRows = true;
                    } else if (query.kind == stream::ListKind::CatalogSearchArtists) {
                        page.rows = std::vector<stream::Artist>{};
                    } else if (query.kind == stream::ListKind::CatalogSearchAlbums) {
                        page.rows = std::vector<stream::Album>{};
                    } else if (query.kind == stream::ListKind::CatalogSearchPlaylists) {
                        page.rows = std::vector<stream::Playlist>{};
                    } else {
                        page.rows = std::vector<stream::CatalogSearchItem>{};
                        catalogItemRows = true;
                    }
                    page.total = 0;
                    break;
                }
                std::vector<Object> objects;
                if (const auto data = json::GetArray(*collection, L"data")) objects = json::Objects(*data);
                if (query.kind == stream::ListKind::CatalogSearch) {
                    songs = ParseSongs(objects);
                    trackRows = true;
                } else if (query.kind == stream::ListKind::CatalogSearchArtists) {
                    page.rows = ParseArtists(objects);
                } else if (query.kind == stream::ListKind::CatalogSearchAlbums) {
                    page.rows = ParseAlbums(objects);
                } else if (query.kind == stream::ListKind::CatalogSearchPlaylists) {
                    std::vector<stream::Playlist> playlists;
                    for (const auto& object : objects)
                        if (auto playlist = ParsePlaylist(object)) playlists.push_back(std::move(*playlist));
                    page.rows = std::move(playlists);
                } else {
                    page.rows = ParseCatalogSearchItems(objects);
                    catalogItemRows = true;
                }
                page.total = CollectionTotal(*collection);
                setNext(*collection, expected, bindSearch, objects.size());
                break;
            }
            }
            if (trackRows) page.rows = DisplayTracks(songs);
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }

        if (query.cancelled && query.cancelled->load(std::memory_order_acquire)) return;
        PostMain(generation, [this, generation, preferenceEpoch, page = std::move(page),
                              songs = std::move(songs), trackRows, catalogItemRows, derivedGroupsPage,
                              error = std::move(error), done = std::move(done)]() mutable {
            bool mixedNeedsHydration = false;
            if (error.empty() && trackRows) {
                songs = MergeSongRecordsWithCatalog(std::move(songs), true);
                // The generic list cache owns persistence. Avoid rewriting the legacy
                // catalog snapshot once per streamed page.
                Catalog::Get().PutAndHint(songs, false);
                page.rows = DisplayTracks(songs);
            }
            if (error.empty() && derivedGroupsPage) {
                Catalog::Get().PutAndHint(songs);
                std::lock_guard lock(m_mutex);
                if (generation == m_generation) {
                    m_librarySongs = songs;
                    m_librarySongsLoaded = true;
                    m_librarySongsAt = std::chrono::steady_clock::now();
                }
            }
            if (error.empty() && catalogItemRows) {
                auto* items = std::get_if<std::vector<stream::CatalogSearchItem>>(&page.rows);
                if (!items) {
                    error = L"Apple Music returned a malformed top-results page.";
                } else {
                    std::vector<Song> topTracks;
                    for (auto& item : *items) {
                        if (auto* track = std::get_if<stream::Track>(&item)) {
                            Song song = BaselineSong(*track);
                            if (const auto existing = Catalog::Get().FindSongRaw(track->id))
                                song = MergeSongRecord(*existing, song);
                            song.track.id = track->id;
                            *track = song.track;
                            topTracks.push_back(std::move(song));
                        } else if (auto* playlist = std::get_if<stream::Playlist>(&item)) {
                            Catalog::Get().Put(*playlist);
                        } else if (auto* album = std::get_if<stream::Album>(&item)) {
                            Catalog::Get().Put(*album);
                        } else if (auto* artist = std::get_if<stream::Artist>(&item)) {
                            Catalog::Get().Put(*artist);
                        }
                    }
                    Catalog::Get().PutAndHint(topTracks, false);
                    mixedNeedsHydration = HasSongAlbumHydrationWork(topTracks);
                }
            }
            if (error.empty()) {
                if (page.playlist) {
                    auto merged = *page.playlist;
                    if (const auto existing = Catalog::Get().FindPlaylistRaw(page.playlist->id))
                        merged = MergePlaylistRecord(merged, *existing, false);
                    if (const auto saved = PlaylistSavedState(merged.id)) merged.saved = *saved;
                    Catalog::Get().Put(merged);
                    page.playlist = std::move(merged);
                }
                if (page.album) {
                    auto merged = *page.album;
                    if (const auto existing = Catalog::Get().FindAlbumRaw(page.album->id))
                        merged = MergeAlbumRecord(merged, *existing);
                    Catalog::Get().Put(merged);
                    page.album = std::move(merged);
                }
                if (page.artist) {
                    auto merged = *page.artist;
                    if (const auto existing = Catalog::Get().FindArtistRaw(page.artist->id))
                        merged = MergeArtistRecord(merged, *existing, false);
                    ApplyArtistPreferenceOverlay(merged, preferenceEpoch);
                    Catalog::Get().Put(merged);
                    page.artist = std::move(merged);
                }
                if (!page.total && page.playlist && page.playlist->trackCount >= 0)
                    page.total = static_cast<size_t>(page.playlist->trackCount);
                if (!page.total && page.album && page.album->trackCount >= 0)
                    page.total = static_cast<size_t>(page.album->trackCount);
                if (auto* playlistRows = std::get_if<std::vector<stream::Playlist>>(&page.rows)) {
                    for (auto& row : *playlistRows) {
                        if (const auto existing = Catalog::Get().FindPlaylistRaw(row.id))
                            row = MergePlaylistRecord(row, *existing, false);
                        if (const auto saved = PlaylistSavedState(row.id)) row.saved = *saved;
                        Catalog::Get().Put(row);
                    }
                } else if (auto* albumRows = std::get_if<std::vector<stream::Album>>(&page.rows)) {
                    for (auto& row : *albumRows) {
                        if (const auto existing = Catalog::Get().FindAlbumRaw(row.id))
                            row = MergeAlbumRecord(row, *existing);
                        Catalog::Get().Put(row);
                    }
                } else if (auto* artistRows = std::get_if<std::vector<stream::Artist>>(&page.rows)) {
                    for (auto& row : *artistRows) {
                        if (const auto existing = Catalog::Get().FindArtistRaw(row.id))
                            row = MergeArtistRecord(row, *existing, false);
                        ApplyArtistPreferenceOverlay(row, preferenceEpoch);
                        Catalog::Get().Put(row);
                    }
                }
                ApplyPageDisplayNames(page, false);
                page.enrichmentPending = (trackRows && HasSongAlbumHydrationWork(songs)) ||
                                         mixedNeedsHydration || PageNeedsDisplayNames(page) ||
                                         (derivedGroupsPage && RegionalDisplayEnabled()) ||
                                         (catalogItemRows && PageNeedsDisplayNames(page));
            }
            if (done) done(std::move(page), std::move(error));
        });
    });
}

bool StreamProvider::EnrichListPage(const stream::ListQuery& query, const stream::ListPage& page,
                                    stream::ListPageDone done) {
    if (!done || ListCancelled(query.cancelled) || !EnrichableList(query.kind)) return false;
    std::vector<Song> songs;
    std::vector<stream::Album> albums;
    std::vector<stream::Artist> artists;
    std::vector<stream::Playlist> playlists;
    std::vector<stream::CatalogSearchItem> catalogItems;
    std::vector<Song> groupSongs;
    bool derivedGroupsPage = false;
    bool derivedGenres = false;
    if (const auto* trackRowsForDisplay = std::get_if<std::vector<stream::Track>>(&page.rows)) {
        songs.reserve(trackRowsForDisplay->size());
        for (const auto& row : *trackRowsForDisplay) {
            Song baseline = BaselineSong(row);
            if (const auto existing = Catalog::Get().FindSongRaw(row.id))
                baseline = MergeSongRecord(*existing, baseline);
            baseline.track.id = row.id;
            songs.push_back(std::move(baseline));
        }
    } else if (const auto* albumRows = std::get_if<std::vector<stream::Album>>(&page.rows)) {
        albums = *albumRows;
    } else if (const auto* artistRows = std::get_if<std::vector<stream::Artist>>(&page.rows)) {
        artists = *artistRows;
    } else if (const auto* playlistRows = std::get_if<std::vector<stream::Playlist>>(&page.rows)) {
        playlists = *playlistRows;
    } else if (const auto* itemRows = std::get_if<std::vector<stream::CatalogSearchItem>>(&page.rows)) {
        catalogItems = *itemRows;
        for (const auto& item : catalogItems) {
            if (const auto* track = std::get_if<stream::Track>(&item)) {
                Song baseline = BaselineSong(*track);
                if (const auto existing = Catalog::Get().FindSongRaw(track->id))
                    baseline = MergeSongRecord(*existing, baseline);
                baseline.track.id = track->id;
                songs.push_back(std::move(baseline));
            } else if (const auto* album = std::get_if<stream::Album>(&item)) {
                albums.push_back(*album);
            } else if (const auto* artist = std::get_if<stream::Artist>(&item)) {
                artists.push_back(*artist);
            } else if (const auto* playlist = std::get_if<stream::Playlist>(&item)) {
                playlists.push_back(*playlist);
            }
        }
    } else if (std::holds_alternative<std::vector<stream::BrowseGroup>>(page.rows)) {
        derivedGroupsPage = true;
        derivedGenres = query.section == kGenresSection;
        std::lock_guard lock(m_mutex);
        groupSongs = m_librarySongs;
    }
    if (page.playlist) playlists.push_back(*page.playlist);
    if (page.album) albums.push_back(*page.album);
    if (page.artist) artists.push_back(*page.artist);

    const bool needsHydration = HasSongAlbumHydrationWork(songs);
    const bool needsNames =
        PageNeedsDisplayNames(page) || (derivedGroupsPage && RegionalDisplayEnabled() && !groupSongs.empty());
    if (!needsHydration && !needsNames) return false;

    stream::ListPage enrichedPage = page;
    enrichedPage.enrichmentPending = true;
    const auto cancelled = query.cancelled;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || !m_enrichTasks || !m_abort || ListCancelled(cancelled)) return false;
        const uint64_t generation = m_generation;
        const std::wstring storefront = m_account.signedIn && m_account.storefront.size() == 2
                                            ? m_account.storefront
                                            : std::wstring(L"US");
        // Post under the provider lock so Shutdown cannot stop and release this queue
        // between the lifetime check and enqueue.
        m_enrichTasks->Post([this, generation, storefront, cancelled, songs = std::move(songs),
                             albums = std::move(albums), artists = std::move(artists),
                             playlists = std::move(playlists), catalogItems = std::move(catalogItems),
                             groupSongs = std::move(groupSongs), derivedGroupsPage, derivedGenres,
                             enrichedPage = std::move(enrichedPage), done = std::move(done)]() mutable {
            abort_callback* providerAbort = nullptr;
            {
                std::lock_guard lock(m_mutex);
                if (!m_started || generation != m_generation || !m_abort || ListCancelled(cancelled)) return;
                providerAbort = m_abort.get();
            }

            try {
                ListHydrationAbort abort(*providerAbort, cancelled);
                if (HasSongAlbumHydrationWork(songs)) HydrateSongAlbums(songs, storefront, abort);
                ResolveSongDisplayNames(songs, generation, abort);
                ResolveAlbumDisplayNames(albums, generation, abort);
                ResolveArtistDisplayNames(artists, generation, abort);
                ResolvePlaylistDisplayNames(playlists, generation, abort);
                ResolveSongDisplayNames(groupSongs, generation, abort);
            } catch (const exception_aborted&) {
                return;
            } catch (const std::exception& exception) {
                netease::Log(L"Apple Music page enrichment failed: " +
                             Wide(std::string_view(exception.what())));
            } catch (...) {
                netease::Log(L"Apple Music page enrichment failed.");
            }
            if (ListCancelled(cancelled)) return;

            PostMain(generation, [this, generation, cancelled, songs = std::move(songs),
                                  albums = std::move(albums), artists = std::move(artists),
                                  playlists = std::move(playlists), catalogItems = std::move(catalogItems),
                                  groupSongs = std::move(groupSongs), derivedGroupsPage, derivedGenres,
                                  enrichedPage = std::move(enrichedPage), done = std::move(done)]() mutable {
                if (ListCancelled(cancelled)) return;
                {
                    std::lock_guard lock(m_mutex);
                    if (!m_started || generation != m_generation) return;
                }
                try {
                    if (auto* pageRows = std::get_if<std::vector<stream::Track>>(&enrichedPage.rows)) {
                        if (pageRows->size() != songs.size()) throw std::runtime_error("row count changed");
                        std::vector<Song> merged;
                        merged.reserve(songs.size());
                        for (size_t i = 0; i < songs.size(); ++i) {
                            const stream::Id trackId = (*pageRows)[i].id;
                            Song song = songs[i];
                            if (const auto existing = Catalog::Get().FindSongRaw(trackId))
                                song = MergeSongRecord(*existing, song);
                            song.track.id = trackId;
                            merged.push_back(std::move(song));
                        }
                        Catalog::Get().PutAndHint(merged, false);
                        *pageRows = DisplayTracks(merged);
                    }
                    if (derivedGroupsPage) {
                        auto displaySongs = DisplaySongs(groupSongs);
                        enrichedPage.rows = DerivedGroups(groupSongs, displaySongs, derivedGenres);
                    }
                    if (!catalogItems.empty()) {
                        std::unordered_map<stream::Id, stream::Track> resolvedTracks;
                        std::unordered_map<stream::Id, stream::Album> resolvedAlbums;
                        std::unordered_map<stream::Id, stream::Artist> resolvedArtists;
                        std::unordered_map<stream::Id, stream::Playlist> resolvedPlaylists;
                        std::vector<Song> mixedSongs;
                        for (auto& song : songs) {
                            const stream::Id id = song.track.id;
                            if (const auto existing = Catalog::Get().FindSongRaw(id))
                                song = MergeSongRecord(*existing, song);
                            song.track.id = id;
                            auto display = DisplayTracks({song});
                            if (!display.empty()) resolvedTracks[id] = std::move(display.front());
                            mixedSongs.push_back(std::move(song));
                        }
                        for (auto& album : albums)
                            resolvedAlbums[album.id] = std::move(album);
                        for (auto& artist : artists)
                            resolvedArtists[artist.id] = std::move(artist);
                        for (auto& playlist : playlists)
                            resolvedPlaylists[playlist.id] = std::move(playlist);
                        for (auto& item : catalogItems) {
                            std::visit(
                                [&](auto& row) {
                                    using Item = std::decay_t<decltype(row)>;
                                    if constexpr (std::is_same_v<Item, stream::Track>) {
                                        if (const auto found = resolvedTracks.find(row.id);
                                            found != resolvedTracks.end())
                                            row = found->second;
                                    } else if constexpr (std::is_same_v<Item, stream::Album>) {
                                        if (const auto found = resolvedAlbums.find(row.id);
                                            found != resolvedAlbums.end())
                                            row = found->second;
                                    } else if constexpr (std::is_same_v<Item, stream::Artist>) {
                                        if (const auto found = resolvedArtists.find(row.id);
                                            found != resolvedArtists.end())
                                            row = found->second;
                                    } else {
                                        if (const auto found = resolvedPlaylists.find(row.id);
                                            found != resolvedPlaylists.end())
                                            row = found->second;
                                    }
                                },
                                item);
                        }
                        Catalog::Get().PutAndHint(mixedSongs, false);
                        enrichedPage.rows = std::move(catalogItems);
                    }
                    ApplyPageDisplayNames(enrichedPage, false);
                } catch (...) {
                    enrichedPage.enrichmentPending = false;
                    done(std::move(enrichedPage), L"Optional Apple Music metadata could not be applied.");
                    return;
                }
                enrichedPage.enrichmentPending = false;
                done(std::move(enrichedPage), {});
            });
        });
    }
    return true;
}

void StreamProvider::WriteFavorite(const stream::Id& track, bool favorite, Done done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done(signInError);
        return;
    }

    const auto song = Catalog::Get().FindSong(track);
    stream::Id requestId;
    std::wstring resourceType;
    if (song && !song->reportingId.empty()) {
        requestId = song->reportingId;
        resourceType = L"songs";
    } else if (song && !song->catalogId.empty()) {
        requestId = song->catalogId;
        resourceType = L"songs";
    } else if (song && !song->libraryId.empty()) {
        requestId = song->libraryId;
        resourceType = L"library-songs";
    } else if (IsLibraryId(track) && track[0] == L'i') {
        requestId = track;
        resourceType = L"library-songs";
    } else if (!track.empty() && !IsLibraryId(track)) {
        requestId = track;
        resourceType = L"songs";
    }
    if (requestId.empty() || resourceType.empty()) {
        if (done) done(L"This item has no Apple Music song id that can be favorited.");
        return;
    }

    uint64_t generation = 0;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    const stream::Id libraryId = song ? song->libraryId : stream::Id();
    const stream::Id catalogId = song ? song->catalogId : stream::Id();
    // On the write queue, so a favorite never waits behind page reads.
    PostMutation(generation, [this, generation, track, requestId, resourceType, favorite, libraryId, catalogId,
                              done = std::move(done)]() mutable {
        std::wstring error;
        const auto request = [&](const stream::Id& id, const std::wstring& type) {
            const std::wstring path = L"/v1/me/favorites?ids[" + type + L"]=" + Encode(id) + L"&platform=web";
            Apple(path, *m_abort, favorite ? L"POST" : L"DELETE");
        };
        try {
            try {
                request(requestId, resourceType);
            } catch (const BridgeHttpError& exception) {
                if (exception.status() != 404 || libraryId.empty() || catalogId.empty()) throw;
                const auto mapping = ResolveLibraryPlaybackMapping(libraryId, catalogId, *m_abort);
                if (!mapping || mapping->reportingId.empty()) throw;
                request(mapping->reportingId, L"songs");
            }
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation,
                 [this, track, favorite, done = std::move(done), error = std::move(error)]() mutable {
                     if (!error.empty()) {
                         if (done) done(std::move(error));
                         return;
                     }
                     // The service accepted the change, so the known set is updated in place.
                     // Reading the whole Favorite Songs playlist back after every star (all
                     // its pages, their related records and names) only repeated the answer.
                     {
                         std::lock_guard lock(m_mutex);
                         if (favorite) {
                             m_favoriteIds.insert(track);
                         } else {
                             m_favoriteIds.erase(track);
                             std::erase_if(m_favoriteSongs,
                                           [&track](const Song& song) { return song.track.id == track; });
                         }
                         // An added song is not in the loaded rows; the Favorites list reads it.
                         if (favorite) m_favoriteSongsLoaded = false;
                     }
                     SaveFavoriteIdsAsync();
                     NotifyListChanged();
                     if (done) done({});
                 });
    });
}

void StreamProvider::SetFollowed(const stream::Id& artist, bool follow, Done done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done(signInError);
        return;
    }
    uint64_t generation = 0;
    uint64_t mutationSequence = 0;
    const std::wstring storefront = Storefront();
    const bool library = IsLibraryId(artist) && artist[0] == L'r';
    const stream::Id catalogHint = ArtistCatalogId(artist);
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        mutationSequence = ++m_artistPreferenceSequence;
    }
    PostMutation(generation, [this, generation, mutationSequence, artist, storefront, library, catalogHint,
                              follow, done = std::move(done)]() mutable {
        stream::Id catalogId;
        std::wstring error;
        try {
            const auto verified =
                ResolveVerifiedArtistCatalogId(artist, catalogHint, storefront, library, *m_abort);
            if (!verified) {
                error = L"Apple Music did not provide a verified catalog artist id.";
            } else {
                const auto request =
                    BuildArtistPreferenceRequest(*verified, follow ? ArtistPreferenceAction::Favorite
                                                                   : ArtistPreferenceAction::Unfavorite);
                if (!request) {
                    error = L"The Apple Music artist preference request is invalid.";
                } else {
                    catalogId = verified->Value();
                    Apple(request->path, *m_abort, request->method);
                }
            }
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, generation, mutationSequence, artist, storefront, catalogId, follow,
                              done = std::move(done), error = std::move(error)]() mutable {
            if (!error.empty()) {
                if (done) done(std::move(error));
                return;
            }
            bool apply = false;
            uint64_t preferenceEpoch = 0;
            {
                std::lock_guard lock(m_mutex);
                apply = mutationSequence >= m_appliedArtistPreferenceSequence;
                if (apply) {
                    m_appliedArtistPreferenceSequence = mutationSequence;
                    preferenceEpoch = ++m_artistPreferenceEpoch;
                    std::erase_if(m_artistPreferenceMutations, [&](const auto& entry) {
                        return entry.first == catalogId || entry.second.sourceId == artist ||
                               entry.second.catalogId == catalogId;
                    });
                    ArtistPreferenceMutation mutation;
                    mutation.sourceId = artist;
                    mutation.catalogId = catalogId;
                    mutation.epoch = preferenceEpoch;
                    mutation.favorite = follow;
                    mutation.pending = true;
                    m_artistPreferenceMutations[catalogId] = std::move(mutation);
                    for (auto& item : m_libraryArtists) {
                        if (item.id != artist) continue;
                        item.followKnown = true;
                        item.followed = follow;
                    }
                }
            }
            if (!apply) {
                if (done) done({});
                return;
            }
            const auto update = [follow](const stream::Id& id) {
                if (id.empty()) return;
                const auto current = Catalog::Get().FindArtistRaw(id);
                if (!current) return;
                stream::Artist updated = *current;
                updated.followKnown = true;
                updated.followed = follow;
                Catalog::Get().Put(updated);
            };
            update(artist);
            update(catalogId);
            NotifyLibraryChanged();
            if (done) done({});
            Post(generation, [this, generation, mutationSequence, preferenceEpoch, storefront, catalogId,
                              follow] {
                std::optional<ArtistPreference> observed;
                try {
                    const auto response = Apple(ArtistPath(catalogId, storefront, false), *m_abort);
                    if (const auto item = FirstData(response);
                        item && VerifiedArtistCatalogId(*item, catalogId))
                        observed = ParseArtistPreference(*item);
                } catch (const exception_aborted&) {
                    return;
                } catch (...) {
                    return;
                }
                PostMain(generation, [this, mutationSequence, preferenceEpoch, catalogId, follow, observed] {
                    if (!observed || *observed == ArtistPreference::Unknown) return;
                    const bool matches = (*observed == ArtistPreference::Favorite) == follow;
                    std::lock_guard lock(m_mutex);
                    if (mutationSequence != m_appliedArtistPreferenceSequence ||
                        preferenceEpoch != m_artistPreferenceEpoch)
                        return;
                    const auto found = m_artistPreferenceMutations.find(catalogId);
                    if (found == m_artistPreferenceMutations.end() || !found->second.pending || !matches)
                        return;
                    found->second.pending = false;
                    found->second.epoch = ++m_artistPreferenceEpoch;
                    for (auto& item : m_libraryArtists) {
                        if (item.id != found->second.sourceId) continue;
                        item.followKnown = true;
                        item.followed = follow;
                    }
                });
            });
        });
    });
}

void StreamProvider::SetAlbumSaved(const stream::Id& album, bool saved, Done done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done(signInError);
        return;
    }
    uint64_t generation = 0;
    uint64_t mutationSequence = 0;
    const std::wstring storefront = Storefront();
    const stream::Id knownLibrary = KnownLibraryId(true, album);
    stream::Id catalogId = CatalogIdForAlbum(album);
    std::optional<stream::Album> albumInfo =
        catalogId.empty() ? std::nullopt : Catalog::Get().FindAlbumRaw(catalogId);
    if (!albumInfo) albumInfo = Catalog::Get().FindAlbumRaw(album);
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        mutationSequence = ++m_libraryMutationSequence;
    }
    PostMutation(generation, [this, generation, mutationSequence, album, saved, storefront,
                              done = std::move(done), knownLibrary, catalogId,
                              albumInfo = std::move(albumInfo)]() mutable {
        stream::Id libraryId = knownLibrary;
        std::wstring error;
        try {
            if (saved) {
                if (catalogId.empty())
                    error = L"This album has no verified Apple Music catalog id.";
                else
                    Apple(L"/v1/me/library?ids[albums]=" + Encode(catalogId), *m_abort, L"POST");
            } else {
                if (libraryId.empty() && !catalogId.empty())
                    libraryId = ResolveLibraryIdFromCatalog(L"albums", L'l', catalogId, storefront, *m_abort);
                if (libraryId.empty())
                    error = L"This album has no verified Apple Music library id.";
                else
                    Apple(L"/v1/me/library/albums/" + Encode(libraryId), *m_abort, L"DELETE");
            }
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, mutationSequence, album, catalogId, libraryId, saved,
                              albumInfo = std::move(albumInfo), done = std::move(done),
                              error = std::move(error)]() mutable {
            if (!error.empty()) {
                if (done) done(std::move(error));
                return;
            }
            const stream::Id key = catalogId.empty() ? libraryId : catalogId;
            std::vector<Done> superseded;
            bool apply = false;
            {
                std::lock_guard lock(m_mutex);
                apply = mutationSequence >= m_appliedLibraryMutationSequence;
                if (!apply) {
                    // A newer operation for the same account already published. The server did
                    // acknowledge this older request, but its late completion must not undo it.
                } else {
                    m_appliedLibraryMutationSequence = mutationSequence;
                    ++m_libraryMutationEpoch;
                    if (!catalogId.empty() && !libraryId.empty()) m_albumLibraryIds[catalogId] = libraryId;
                    if (m_refreshInFlight) {
                        superseded.swap(m_refreshWaiters);
                        m_refreshInFlight = false;
                        m_refreshGeneration = 0;
                        m_refreshPreservePlaylistDetails = false;
                        m_refreshInvalidatesLists = false;
                    }
                    AlbumMutation mutation;
                    mutation.catalogId = catalogId;
                    mutation.libraryId = libraryId;
                    mutation.epoch = m_libraryMutationEpoch;
                    mutation.saved = saved;
                    mutation.pending = saved;
                    if (albumInfo) mutation.album = *albumInfo;
                    if (mutation.album.id.empty()) {
                        mutation.album.id = catalogId.empty() ? libraryId : catalogId;
                    }
                    m_albumMutations[key] = std::move(mutation);
                    if (!saved) {
                        m_savedAlbumIds.erase(album);
                        if (!catalogId.empty()) m_savedAlbumIds.erase(catalogId);
                        if (!libraryId.empty()) m_savedAlbumIds.erase(libraryId);
                        std::erase_if(m_savedAlbums, [&](const stream::Album& item) {
                            if (item.id == album || (!libraryId.empty() && item.id == libraryId)) return true;
                            if (catalogId.empty() || !IsLibraryId(item.id) || item.id.empty() ||
                                item.id[0] != L'l')
                                return false;
                            const auto mapped =
                                std::find_if(m_albumLibraryIds.begin(), m_albumLibraryIds.end(),
                                             [&item](const auto& entry) { return entry.second == item.id; });
                            return mapped != m_albumLibraryIds.end() && mapped->first == catalogId;
                        });
                    }
                }
            }
            if (!apply) {
                if (done) done({});
                return;
            }
            if (!catalogId.empty() && !libraryId.empty())
                Catalog::Get().PutAlbumLibraryId(catalogId, libraryId);
            for (auto& waiter : superseded)
                if (waiter) waiter(L"The Apple Music refresh was superseded by a library change.");
            NotifyListChanged();
            NotifyLibraryChanged();
            if (done) done({});
            RefreshLibraryAfterMutation({});
        });
    });
}

void StreamProvider::SetPlaylistSaved(const stream::Id& playlist, bool saved, Done done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done(signInError);
        return;
    }
    uint64_t generation = 0;
    uint64_t mutationSequence = 0;
    const std::wstring storefront = Storefront();
    const stream::Id knownLibrary = KnownLibraryId(false, playlist);
    stream::Id catalogId = CatalogIdForPlaylist(playlist);
    std::optional<stream::Playlist> playlistInfo =
        catalogId.empty() ? std::nullopt : Catalog::Get().FindPlaylistRaw(catalogId);
    if (!playlistInfo) playlistInfo = Catalog::Get().FindPlaylistRaw(playlist);
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        mutationSequence = ++m_libraryMutationSequence;
    }
    PostMutation(generation, [this, generation, mutationSequence, playlist, saved, storefront, knownLibrary,
                              catalogId, playlistInfo = std::move(playlistInfo),
                              done = std::move(done)]() mutable {
        stream::Id libraryId = knownLibrary;
        std::wstring error;
        try {
            if (saved) {
                if (catalogId.empty())
                    error = L"This playlist has no verified Apple Music catalog id.";
                else
                    Apple(L"/v1/me/library?ids[playlists]=" + Encode(catalogId), *m_abort, L"POST");
            } else {
                if (libraryId.empty() && !catalogId.empty())
                    libraryId =
                        ResolveLibraryIdFromCatalog(L"playlists", L'p', catalogId, storefront, *m_abort);
                if (libraryId.empty()) {
                    error = L"This playlist has no verified Apple Music library id.";
                } else {
                    const PlaylistData data = LoadPlaylist(libraryId, storefront, true, *m_abort, false);
                    if (data.favorite || IsFavoritePlaylist(libraryId))
                        throw std::runtime_error("The Favorite Songs playlist cannot be changed.");
                    if (data.info.own)
                        throw std::runtime_error("Use Delete playlist for an owned Apple Music playlist.");
                    if (!data.canDelete)
                        throw std::runtime_error("Apple Music does not allow this playlist to be removed.");
                    Apple(L"/v1/me/library/playlists/" + Encode(libraryId), *m_abort, L"DELETE");
                }
            }
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, mutationSequence, playlist, catalogId, libraryId, saved,
                              playlistInfo = std::move(playlistInfo), done = std::move(done),
                              error = std::move(error)]() mutable {
            if (!error.empty()) {
                if (done) done(std::move(error));
                return;
            }
            const stream::Id key = catalogId.empty() ? libraryId : catalogId;
            std::vector<Done> superseded;
            bool apply = false;
            {
                std::lock_guard lock(m_mutex);
                apply = mutationSequence >= m_appliedLibraryMutationSequence;
                if (apply) {
                    m_appliedLibraryMutationSequence = mutationSequence;
                    ++m_libraryMutationEpoch;
                    if (!catalogId.empty() && !libraryId.empty()) m_playlistLibraryIds[catalogId] = libraryId;
                    if (m_refreshInFlight) {
                        superseded.swap(m_refreshWaiters);
                        m_refreshInFlight = false;
                        m_refreshGeneration = 0;
                        m_refreshPreservePlaylistDetails = false;
                        m_refreshInvalidatesLists = false;
                    }
                    PlaylistMutation mutation;
                    mutation.catalogId = catalogId;
                    mutation.libraryId = libraryId;
                    mutation.epoch = m_libraryMutationEpoch;
                    mutation.saved = saved;
                    mutation.pending = true;
                    if (playlistInfo) mutation.playlist = *playlistInfo;
                    mutation.playlist.id = libraryId.empty() ? catalogId : libraryId;
                    m_playlistMutations[key] = std::move(mutation);
                    if (!saved) {
                        std::erase_if(m_savedPlaylists, [&](const stream::Playlist& item) {
                            if (item.id == playlist || (!libraryId.empty() && item.id == libraryId))
                                return true;
                            const auto mapped = m_playlistLibraryIds.find(catalogId);
                            return !catalogId.empty() && mapped != m_playlistLibraryIds.end() &&
                                   mapped->second == item.id;
                        });
                    }
                }
            }
            if (!apply) {
                if (done) done({});
                return;
            }
            stream::Playlist catalogPlaylist = playlistInfo ? *playlistInfo : stream::Playlist{};
            catalogPlaylist.id = !catalogId.empty() ? catalogId : key;
            catalogPlaylist.saved = saved && !catalogPlaylist.own;
            Catalog::Get().Put(catalogPlaylist);
            for (auto& waiter : superseded)
                if (waiter) waiter(L"The Apple Music refresh was superseded by a library change.");
            NotifyListChanged();
            NotifyLibraryChanged();
            if (done) done({});
            RefreshLibraryAfterMutation({});
        });
    });
}

bool StreamProvider::IsEditablePlaylist(const stream::Id& id) const {
    std::lock_guard lock(m_mutex);
    return m_editablePlaylists.count(id) != 0;
}

bool StreamProvider::IsFavoritePlaylist(const stream::Id& id) const {
    std::lock_guard lock(m_mutex);
    return !id.empty() && id == m_favoritePlaylistId;
}

stream::Id StreamProvider::KnownLibraryId(bool album, const stream::Id& id) const {
    const wchar_t prefix = album ? L'l' : L'p';
    if (IsLibraryId(id) && id[0] == prefix) return id;
    std::lock_guard lock(m_mutex);
    const auto& known = album ? m_albumLibraryIds : m_playlistLibraryIds;
    const auto found = known.find(id);
    return found == known.end() ? stream::Id() : found->second;
}

stream::Id StreamProvider::ArtistCatalogId(const stream::Id& artist) const {
    if (!IsLibraryId(artist) || artist[0] != L'r') return artist;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_artistCatalogIds.find(artist);
        if (found != m_artistCatalogIds.end()) return found->second;
    }
    return Catalog::Get().FindArtistCatalogId(artist);
}

namespace {

// Album counts of the library's artists, kept across runs. Counting costs one or two
// requests per artist, so the counts are read back instead of being asked for again at
// every start; they are recounted after a week, and an artist's page corrects its own.
constexpr int64_t kArtistCountsFreshSeconds = 7 * 24 * 60 * 60;
constexpr char kArtistCountsSchema[] = "artist-album-counts/1";
constexpr int kNoAlbumCount = -1; // asked, and the service has no albums of the artist

cache::Key ArtistCountsKey() {
    return {cache::Kind::Data, "apple", cache::AccountScope("apple"), "artist-album-counts", "1"};
}

std::unordered_map<stream::Id, int> LoadArtistCounts() {
    std::unordered_map<stream::Id, int> counts;
    try {
        const auto record = cache::GetStore().Lookup(ArtistCountsKey());
        if (!record || record->Info().contentType != kArtistCountsSchema ||
            cache::Store::Now() - record->Info().validatedAt >= kArtistCountsFreshSeconds)
            return counts;
        std::ifstream input(record->Path(), std::ios::binary);
        std::string id;
        int count = 0;
        while (input >> id >> count)
            if (count >= kNoAlbumCount) counts[Wide(id)] = count;
    } catch (...) {
        counts.clear();
    }
    return counts;
}

void SaveArtistCounts(const std::unordered_map<stream::Id, int>& counts, int64_t validatedAt) {
    try {
        auto writer = cache::GetStore().Begin(ArtistCountsKey());
        if (!writer) return;
        {
            std::ofstream output(writer->Path(), std::ios::binary | std::ios::trunc);
            for (const auto& [id, count] : counts)
                if (!id.empty() && id.find_first_of(L" \t\r\n") == std::wstring::npos)
                    output << Utf8(id) << ' ' << count << '\n';
            output.close();
            if (!output) return;
        }
        cache::Metadata metadata;
        metadata.contentType = kArtistCountsSchema;
        metadata.validatedAt = validatedAt;
        writer->Commit(std::move(metadata));
    } catch (...) {
        // The counts are only saved requests.
    }
}

} // namespace

void StreamProvider::EnrichArtistCountsAsync(uint64_t generation) {
    std::vector<std::pair<stream::Id, stream::Id>> artists;
    {
        std::lock_guard lock(m_mutex);
        if (!m_started || generation != m_generation || !m_enrichTasks || m_artistCountsInFlight) return;
        // Only artists whose count is not known yet. A count costs one or two requests per
        // artist; the ones already counted (also restored from the snapshot) keep theirs
        // until the artist's page reads the albums again.
        for (const auto& artist : m_libraryArtists) {
            if (artist.albumCount != stream::kUnknownCount) continue;
            const auto found = m_artistCatalogIds.find(artist.id);
            artists.emplace_back(artist.id, found == m_artistCatalogIds.end() ? stream::Id() : found->second);
        }
        if (artists.empty()) return;
        m_artistCountsInFlight = true;
    }

    const std::wstring storefront = Storefront();
    m_enrichTasks->Post([this, generation, storefront, artists = std::move(artists)]() mutable {
        std::unordered_map<stream::Id, int> counts;
        std::unordered_map<stream::Id, int> remembered = LoadArtistCounts();
        // The first count of a recount period dates the record, so the whole set ages together.
        int64_t rememberedAt = cache::Store::Now();
        try {
            if (const auto record = cache::GetStore().Lookup(ArtistCountsKey()); record && !remembered.empty())
                rememberedAt = record->Info().validatedAt;
        } catch (...) {
        }
        bool counted = false;
        try {
            abort_callback& abort = *m_abort;
            for (const auto& [libraryId, catalogId] : artists) {
                if (const auto known = remembered.find(libraryId); known != remembered.end()) {
                    if (known->second != kNoAlbumCount) counts[libraryId] = known->second;
                    continue;
                }
                counted = true;
                // A count the service definitely cannot give (it does not know the artist) is
                // remembered as such, so it is not asked for again at every start.
                const auto count = [&](const stream::Id& catalog) -> std::optional<int> {
                    try {
                        return CountArtistAlbums(libraryId, catalog, storefront, true, abort);
                    } catch (const exception_aborted&) {
                        throw;
                    } catch (const std::exception& exception) {
                        netease::Log(L"apple: album count of " + libraryId + L" failed: " +
                                     Wide(exception.what()));
                        if (DefiniteNotFound(exception)) return kNoAlbumCount;
                    } catch (...) {
                    }
                    return std::nullopt;
                };
                auto result = count(catalogId);
                if ((!result || *result == kNoAlbumCount) && catalogId.empty()) {
                    try {
                        const stream::Id resolved = ResolveLibraryArtistCatalogId(libraryId, abort);
                        if (!resolved.empty()) result = count(resolved);
                    } catch (const exception_aborted&) {
                        throw;
                    } catch (...) {
                    }
                }
                if (result) remembered[libraryId] = *result;
                if (result && *result != kNoAlbumCount) counts[libraryId] = *result;
            }
        } catch (const exception_aborted&) {
        } catch (...) {
        }
        if (counted) {
            SaveArtistCounts(remembered, rememberedAt);
        }

        fb2k::inMainThread([this, generation, counts = std::move(counts)]() mutable {
            bool rerun = false;
            uint64_t current = 0;
            {
                std::lock_guard lock(m_mutex);
                if (!m_started) {
                    m_artistCountsInFlight = false;
                    return;
                }
                m_artistCountsInFlight = false;
                if (generation != m_generation) {
                    rerun = true;
                    current = m_generation;
                } else if (!counts.empty()) {
                    for (auto& artist : m_libraryArtists) {
                        const auto found = counts.find(artist.id);
                        if (found == counts.end()) continue;
                        artist.albumCount = found->second;
                        Catalog::Get().Put(artist);
                    }
                }
            }
            if (rerun)
                EnrichArtistCountsAsync(current);
            else if (!counts.empty())
                NotifyLibraryChanged();
        });
    });
}

void StreamProvider::AddToPlaylist(const stream::Id& playlist, std::vector<stream::Id> tracks, Done done) {
    if (tracks.empty()) {
        if (done) done(L"No songs selected.");
        return;
    }
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done(signInError);
        return;
    }
    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    const stream::Id knownLibrary = KnownLibraryId(false, playlist);
    const std::set<stream::Id> requested(tracks.begin(), tracks.end());
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation, [this, generation, playlist, tracks = std::move(tracks), requested, storefront,
                      knownLibrary, done = std::move(done)]() mutable {
        std::wstring error;
        stream::Id libraryId = knownLibrary;
        try {
            if (libraryId.empty())
                libraryId = ResolveLibraryIdFromCatalog(L"playlists", L'p', playlist, storefront, *m_abort);
            const PlaylistData data = LoadPlaylist(libraryId, storefront, true, *m_abort, false);
            if (data.favorite || IsFavoritePlaylist(libraryId))
                throw std::runtime_error("The Favorite Songs playlist cannot be changed.");
            if (!data.info.own)
                throw std::runtime_error("Apple Music can only append to an editable library playlist.");
            const Object body = AppendTracksBody(tracks);
            Apple(L"/v1/me/library/playlists/" + Encode(libraryId) + L"/tracks", *m_abort, L"POST", &body);
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, libraryId, requested, done = std::move(done),
                              error = std::move(error)]() mutable {
            if (error.empty()) {
                RefreshKnownPlaylist(
                    libraryId,
                    [this, libraryId, requested, done = std::move(done)](std::wstring refreshError) mutable {
                        if (refreshError.empty()) {
                            const auto tracks = CachedPlaylist(libraryId);
                            if (!tracks) {
                                refreshError = L"Apple Music did not return the refreshed playlist.";
                            } else {
                                for (const auto& wanted : requested) {
                                    if (std::none_of(tracks->begin(), tracks->end(),
                                                     [&wanted](const stream::Track& track) {
                                                         return track.id == wanted;
                                                     })) {
                                        refreshError = L"Apple Music did not confirm the appended song.";
                                        break;
                                    }
                                }
                            }
                        }
                        if (done) done(std::move(refreshError));
                    },
                    true);
                return;
            }
            if (done) done(std::move(error));
        });
    });
}

void StreamProvider::RemoveFromPlaylist(const stream::Id& playlist, std::vector<stream::Id> tracks,
                                        Done done) {
    if (tracks.empty()) {
        if (done) done(L"No songs selected.");
        return;
    }
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done(signInError);
        return;
    }
    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    const stream::Id knownLibrary = KnownLibraryId(false, playlist);
    const std::set<stream::Id> requestedTracks(tracks.begin(), tracks.end());
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation, [this, generation, playlist, tracks = std::move(tracks), requestedTracks, storefront,
                      knownLibrary, done = std::move(done)]() mutable {
        stream::Id libraryId = knownLibrary;
        std::wstring error;
        bool mutationAttempted = false;
        try {
            if (libraryId.empty())
                libraryId = ResolveLibraryIdFromCatalog(L"playlists", L'p', playlist, storefront, *m_abort);
            PlaylistData data = LoadPlaylist(libraryId, storefront, true, *m_abort);
            if (data.favorite || IsFavoritePlaylist(libraryId))
                throw std::runtime_error("The Favorite Songs playlist cannot be changed.");
            if (!data.info.own)
                throw std::runtime_error(
                    "Apple Music only permits removing tracks from an editable library playlist.");

            const std::set<stream::Id> requested(tracks.begin(), tracks.end());
            std::set<stream::Id> resolved;
            std::set<stream::Id> librarySongIds;
            for (const auto& song : data.songs) {
                if (requested.count(song.track.id) == 0) continue;
                if (song.libraryId.empty())
                    throw std::runtime_error(
                        "An Apple Music playlist entry has no library song id and cannot be removed.");
                resolved.emplace(song.track.id);
                librarySongIds.emplace(song.libraryId);
            }
            if (resolved.size() != requested.size())
                throw std::runtime_error("One or more selected songs are not in this playlist.");

            for (const auto& librarySongId : librarySongIds) {
                mutationAttempted = true;
                Apple(L"/v1/me/library/playlists/" + Encode(libraryId) + L"/tracks?ids[library-songs]=" +
                          Encode(librarySongId) + L"&mode=all",
                      *m_abort, L"DELETE");
            }
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, libraryId, requestedTracks, mutationAttempted, done = std::move(done),
                              error = std::move(error)]() mutable {
            if (!mutationAttempted) {
                if (done) done(std::move(error));
                return;
            }
            RefreshKnownPlaylist(
                libraryId,
                [this, libraryId, requestedTracks, done = std::move(done),
                 mutationError = std::move(error)](std::wstring refreshError) mutable {
                    if (refreshError.empty() && mutationError.empty()) {
                        const auto remaining = CachedPlaylist(libraryId);
                        if (!remaining) {
                            refreshError = L"Apple Music did not return the refreshed playlist.";
                        } else {
                            for (const auto& wanted : requestedTracks) {
                                if (std::any_of(remaining->begin(), remaining->end(),
                                                [&wanted](const stream::Track& track) {
                                                    return track.id == wanted;
                                                })) {
                                    refreshError = L"Apple Music did not confirm the removed song.";
                                    break;
                                }
                            }
                        }
                    }
                    if (done) done(refreshError.empty() ? std::move(mutationError) : std::move(refreshError));
                },
                true);
        });
    });
}

void StreamProvider::CreatePlaylist(std::wstring name, CreatedDone done) {
    if (name.empty()) {
        if (done) done({}, L"The playlist name is empty.");
        return;
    }
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, signInError);
        return;
    }
    uint64_t generation = 0;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation, [this, generation, name = std::move(name), done = std::move(done)]() mutable {
        stream::Id id;
        std::wstring error;
        try {
            const Object body = CreatePlaylistBody(name);
            const Object response = Apple(L"/v1/me/library/playlists", *m_abort, L"POST", &body);
            if (const auto item = FirstData(response)) id = json::GetString(*item, L"id");
            if (id.empty()) error = L"Apple Music did not return the new playlist id.";
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, id, done = std::move(done), error = std::move(error)]() mutable {
            if (!error.empty()) {
                if (done) done(id, std::move(error));
                return;
            }
            RefreshLibraryAfterMutation([this, id,
                                         done = std::move(done)](std::wstring refreshError) mutable {
                if (refreshError.empty()) {
                    const auto playlists = Playlists(stream::Shelf::Created);
                    if (std::none_of(playlists.begin(), playlists.end(),
                                     [&id](const stream::Playlist& playlist) { return playlist.id == id; }))
                        refreshError = L"Apple Music did not expose the new playlist after refresh.";
                }
                if (done) done(id, std::move(refreshError));
            });
        });
    });
}

void StreamProvider::RenamePlaylist(const stream::Id& playlist, std::wstring name, Done done) {
    if (name.empty()) {
        if (done) done(L"The playlist name is empty.");
        return;
    }
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done(signInError);
        return;
    }
    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    const stream::Id knownLibrary = KnownLibraryId(false, playlist);
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation, [this, generation, playlist, name = std::move(name), storefront, knownLibrary,
                      done = std::move(done)]() mutable {
        stream::Id libraryId = knownLibrary;
        std::wstring error;
        try {
            if (libraryId.empty())
                libraryId = ResolveLibraryIdFromCatalog(L"playlists", L'p', playlist, storefront, *m_abort);
            const PlaylistData data = LoadPlaylist(libraryId, storefront, true, *m_abort, false);
            if (data.favorite || IsFavoritePlaylist(libraryId))
                throw std::runtime_error("The Favorite Songs playlist cannot be changed.");
            if (!data.info.own || !data.canRename)
                throw std::runtime_error("Apple Music does not allow this playlist to be renamed.");
            const Object body = RenamePlaylistBody(name);
            Apple(L"/v1/me/library/playlists/" + Encode(libraryId) + L"?platform=web", *m_abort, L"PATCH",
                  &body);
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation,
                 [this, libraryId, name, done = std::move(done), error = std::move(error)]() mutable {
                     if (!error.empty()) {
                         if (done) done(std::move(error));
                         return;
                     }
                     RefreshKnownPlaylist(
                         libraryId,
                         [this, libraryId, name, done = std::move(done)](std::wstring refreshError) mutable {
                             if (refreshError.empty()) {
                                 const auto playlist = FindPlaylist(libraryId);
                                 if (!playlist || playlist->name != name)
                                     refreshError = L"Apple Music did not confirm the playlist rename.";
                             }
                             if (done) done(std::move(refreshError));
                         },
                         true);
                 });
    });
}

void StreamProvider::DeletePlaylist(const stream::Id& playlist, Done done) {
    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done(signInError);
        return;
    }
    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    const stream::Id knownLibrary = KnownLibraryId(false, playlist);
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation, [this, generation, playlist, storefront, knownLibrary,
                      done = std::move(done)]() mutable {
        stream::Id libraryId = knownLibrary;
        std::wstring error;
        try {
            if (libraryId.empty())
                libraryId = ResolveLibraryIdFromCatalog(L"playlists", L'p', playlist, storefront, *m_abort);
            const PlaylistData data = LoadPlaylist(libraryId, storefront, true, *m_abort, false);
            if (data.favorite || IsFavoritePlaylist(libraryId))
                throw std::runtime_error("The Favorite Songs playlist cannot be changed.");
            if (!data.canDelete)
                throw std::runtime_error("Apple Music does not allow this playlist to be deleted.");
            Apple(L"/v1/me/library/playlists/" + Encode(libraryId), *m_abort, L"DELETE");
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation, [this, libraryId, done = std::move(done), error = std::move(error)]() mutable {
            if (!error.empty()) {
                if (done) done(std::move(error));
                return;
            }
            {
                std::lock_guard lock(m_mutex);
                ++m_playlistDetailEpoch;
                CancelPlaylistFetchesLocked();
                m_playlistTracks.erase(libraryId);
                m_playlistSongs.erase(libraryId);
            }
            RefreshLibraryAfterMutation([this, libraryId,
                                         done = std::move(done)](std::wstring refreshError) mutable {
                if (refreshError.empty()) {
                    const auto exists = [&libraryId](const std::vector<stream::Playlist>& playlists) {
                        return std::any_of(playlists.begin(), playlists.end(),
                                           [&libraryId](const stream::Playlist& playlist) {
                                               return playlist.id == libraryId;
                                           });
                    };
                    if (exists(Playlists(stream::Shelf::Created)) || exists(Playlists(stream::Shelf::Saved)))
                        refreshError = L"Apple Music did not confirm the playlist deletion.";
                    else
                        Catalog::Get().RemovePlaylist(libraryId);
                }
                if (done) done(std::move(refreshError));
            });
        });
    });
}

void StreamProvider::ReorderPlaylists(std::vector<stream::Id> playlists, Done done) {
    (void)playlists;
    if (done) done(L"Reordering Apple Music playlists is not supported by the documented API.");
}

void StreamProvider::SearchCatalog(std::wstring query, TracksDone done) {
    if (query.empty()) {
        if (done) done({}, L"The search text is empty.");
        return;
    }
    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation, [this, generation, storefront, query = std::move(query),
                      done = std::move(done)]() mutable {
        std::vector<Song> songs;
        std::wstring error;
        try {
            const std::wstring path = L"/v1/catalog/" + storefront + L"/search?types=songs&limit=25&term=" +
                                      Encode(query) + L"&include=albums";
            const Object response = Apple(path, *m_abort);
            if (const auto results = json::GetObject(response, L"results")) {
                if (const auto songsObject = json::GetObject(*results, L"songs")) {
                    if (const auto data = json::GetArray(*songsObject, L"data"))
                        songs = ParseSongs(json::Objects(*data));
                }
            }
            HydrateSongAlbums(songs, storefront, *m_abort);
            if (!PutIfCurrent(generation, songs)) return;
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        PostMain(generation,
                 [done = std::move(done), songs = std::move(songs), error = std::move(error)]() mutable {
                     if (error.empty()) Catalog::Get().PutAndHint(songs);
                     if (done) done(DisplayTracks(songs), std::move(error));
                 });
    });
}

void StreamProvider::FetchCatalogSearchSuggestions(CatalogSearchSuggestionQuery query,
                                                   CatalogSearchSuggestionsDone done) {
    if (ListCancelled(query.cancelled)) return;
    if (query.term.empty()) {
        if (done) done({}, {});
        return;
    }
    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    const AppleMusicDisplayRegion displayRegion = GetAppleMusicDisplayRegion();
    const uint64_t displayEpoch = DisplayMetadata::Get().Epoch();
    std::wstring locale;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        locale = m_locale;
    }
    Post(generation, [this, generation, storefront, displayRegion, displayEpoch, locale,
                      query = std::move(query), done = std::move(done)]() mutable {
        if (ListCancelled(query.cancelled)) return;
        CatalogSearchSuggestions suggestions;
        std::wstring error;
        try {
            const std::wstring path =
                L"/v1/catalog/" + storefront + L"/search/suggestions?term=" + Encode(query.term) +
                L"&kinds=terms,topResults&types=songs,artists,albums,playlists&l=" + locale;
            const Object response = Apple(path, *m_abort);
            const auto results = json::GetObject(response, L"results");
            if (!results) throw std::runtime_error("Apple Music returned invalid search suggestions.");
            if (const auto rows = json::GetArray(*results, L"suggestions")) {
                for (const auto& value : *rows) {
                    if (!value || value.ValueType() != JsonValueType::Object) continue;
                    const Object suggestion = value.GetObject();
                    const std::wstring kind = json::GetString(suggestion, L"kind");
                    if (kind == L"terms") {
                        CatalogSearchSuggestionTerm term;
                        term.searchTerm = json::GetString(suggestion, L"searchTerm");
                        term.displayTerm = json::GetString(suggestion, L"displayTerm");
                        if (term.searchTerm.empty()) term.searchTerm = term.displayTerm;
                        if (term.displayTerm.empty()) term.displayTerm = term.searchTerm;
                        if (!term.searchTerm.empty()) suggestions.ordered.emplace_back(std::move(term));
                        continue;
                    }
                    if (kind != L"topResults") continue;
                    const auto content = json::GetObject(suggestion, L"content");
                    if (!content) continue;
                    const Object& item = *content;
                    const std::wstring type = json::GetString(item, L"type");
                    if (type == L"songs") {
                        auto songs = ParseSongs({item});
                        if (!songs.empty()) suggestions.ordered.emplace_back(std::move(songs.front().track));
                    } else if (type == L"artists") {
                        auto artists = ParseArtists({item});
                        if (!artists.empty()) suggestions.ordered.emplace_back(std::move(artists.front()));
                    } else if (type == L"albums") {
                        auto albums = ParseAlbums({item});
                        if (!albums.empty()) suggestions.ordered.emplace_back(std::move(albums.front()));
                    } else if (type == L"playlists") {
                        if (auto playlist = ParsePlaylist(item))
                            suggestions.ordered.emplace_back(std::move(*playlist));
                    }
                }
            }
        } catch (const exception_aborted&) {
            return;
        } catch (const std::exception& exception) {
            error = Wide(exception.what());
        }
        if (ListCancelled(query.cancelled) || displayEpoch != DisplayMetadata::Get().Epoch() ||
            displayRegion != GetAppleMusicDisplayRegion())
            return;
        PostMain(generation, [this, generation, cancelled = query.cancelled, storefront, displayRegion,
                              displayEpoch, locale, done = std::move(done),
                              suggestions = std::move(suggestions), error = std::move(error)]() mutable {
            bool currentGeneration = false;
            bool currentLocale = false;
            {
                std::lock_guard lock(m_mutex);
                currentGeneration = m_started && generation == m_generation;
                currentLocale = locale == m_locale;
            }
            if (!currentGeneration || !currentLocale || ListCancelled(cancelled) ||
                displayEpoch != DisplayMetadata::Get().Epoch() ||
                displayRegion != GetAppleMusicDisplayRegion() || Storefront() != storefront)
                return;
            if (done) done(std::move(suggestions), std::move(error));
        });
    });
}

void StreamProvider::FetchGroups(const stream::Id& section, GroupsDone done) {
    const bool genres = section == kGenresSection;
    const bool composers = section == kComposersSection;
    if (!genres && !composers) {
        if (done) done({}, L"This section does not provide groups.");
        return;
    }

    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, signInError);
        return;
    }

    uint64_t generation = 0;
    bool loaded = false;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        loaded = m_librarySongsLoaded;
    }
    if (loaded) {
        std::vector<Song> songs;
        {
            std::lock_guard lock(m_mutex);
            songs = m_librarySongs;
        }
        auto displaySongs = DisplaySongs(songs);
        if (done) done(DerivedGroups(songs, displaySongs, genres), {});
        return;
    }

    RefreshLibrarySongsAsync(generation, [this, genres, done = std::move(done)](std::vector<stream::Track>,
                                                                                std::wstring error) mutable {
        if (!error.empty()) {
            if (done) done({}, std::move(error));
            return;
        }
        std::vector<Song> songs;
        {
            std::lock_guard lock(m_mutex);
            songs = m_librarySongs;
        }
        auto displaySongs = DisplaySongs(songs);
        if (done) done(DerivedGroups(songs, displaySongs, genres), {});
    });
}

void StreamProvider::FetchSectionTracks(const stream::Id& section, const stream::Id& group, TracksDone done) {
    if (section == kFavoritesSection) {
        FetchFavorites(std::move(done));
        return;
    }

    const bool genres = section == kGenresSection;
    const bool composers = section == kComposersSection;
    const bool songsSection = section == kSongsSection;
    if (!songsSection && !genres && !composers) {
        if (done) done({}, L"This section does not provide tracks directly.");
        return;
    }
    if (songsSection && !group.empty()) {
        if (done) done({}, L"The Songs section does not take a group id.");
        return;
    }
    if ((genres && !StartsWith(group, kGenreGroupPrefix)) ||
        (composers && !StartsWith(group, kComposerGroupPrefix))) {
        if (done) done({}, L"The selected group is invalid for this section.");
        return;
    }

    std::wstring signInError;
    if (!RequireSignedIn(signInError)) {
        if (done) done({}, signInError);
        return;
    }

    uint64_t generation = 0;
    bool loaded = false;
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        loaded = m_librarySongsLoaded;
    }
    auto deliver = [this, group, genres, composers, songsSection,
                    done = std::move(done)](std::wstring error) mutable {
        if (!error.empty()) {
            if (done) done({}, std::move(error));
            return;
        }
        std::vector<Song> songs;
        {
            std::lock_guard lock(m_mutex);
            songs = m_librarySongs;
        }
        songs = DisplaySongs(std::move(songs));
        if (songsSection) {
            if (done) done(DisplayTracks(songs), {});
        } else if (genres) {
            if (done) done(DisplayTracks(TracksInGroup(songs, group, true)), {});
        } else if (composers) {
            if (done) done(DisplayTracks(TracksInGroup(songs, group, false)), {});
        } else if (done) {
            done({}, L"Unsupported Apple Music section.");
        }
    };

    if (loaded) {
        deliver({});
        return;
    }
    RefreshLibrarySongsAsync(
        generation, [deliver = std::move(deliver)](std::vector<stream::Track>, std::wstring error) mutable {
            deliver(std::move(error));
        });
}

void StreamProvider::RefreshKnownPlaylist(const stream::Id& id, Done done, bool invalidateLists) {
    uint64_t generation = 0;
    uint64_t epoch = 0;
    const std::wstring storefront = Storefront();
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
        if (invalidateLists) {
            // The mutation made the old full-list snapshot unsafe even if the confirming
            // detail refresh below fails.
            ++m_playlistDetailEpoch;
            CancelPlaylistFetchesLocked();
            m_playlistTracks.erase(id);
            m_playlistSongs.erase(id);
        }
        epoch = m_playlistDetailEpoch;
    }
    PostDetail(generation,
               [this, generation, epoch, id, storefront, invalidateLists, done = std::move(done)]() mutable {
                   PlaylistData data;
                   std::wstring error;
                   try {
                       data = LoadPlaylist(id, storefront, true, *m_abort);
                       if (!PutIfCurrent(generation, data.info)) return;
                   } catch (const exception_aborted&) {
                       return;
                   } catch (const std::exception& exception) {
                       error = Wide(exception.what());
                   }
                   PostMain(generation, [this, generation, epoch, id, invalidateLists, data = std::move(data),
                                         error = std::move(error), done = std::move(done)]() mutable {
                       bool applied = false;
                       if (error.empty()) {
                           {
                               std::lock_guard lock(m_mutex);
                               applied = generation == m_generation && epoch == m_playlistDetailEpoch;
                           }
                           if (!applied) {
                               if (done) done(std::move(error));
                               return;
                           }
                       }
                       if (invalidateLists) NotifyListChanged();
                       if (applied) {
                           Catalog::Get().PutAndHint(data.songs);
                           {
                               std::lock_guard lock(m_mutex);
                               m_playlistTracks[id] = Tracks(data.songs);
                               m_playlistSongs[id] = data.songs;
                               if (!data.catalogId.empty()) m_playlistLibraryIds[data.catalogId] = id;
                               m_editablePlaylists.erase(id);
                               m_renameablePlaylists.erase(id);
                               m_deletablePlaylists.erase(id);
                               if (data.info.own) m_editablePlaylists.emplace(id);
                               if (data.canRename) m_renameablePlaylists.emplace(id);
                               if (data.canDelete) m_deletablePlaylists.emplace(id);
                               const auto update = [&id, &data](std::vector<stream::Playlist>& playlists) {
                                   for (auto& playlist : playlists) {
                                       if (playlist.id != id) continue;
                                       if (!data.info.name.empty()) playlist.name = data.info.name;
                                       playlist.trackCount = data.info.trackCount;
                                       if (!data.info.coverUrl.empty())
                                           playlist.coverUrl = data.info.coverUrl;
                                       break;
                                   }
                               };
                               update(m_createdPlaylists);
                               update(m_savedPlaylists);
                           }
                           NotifyLibraryChanged();
                       }
                       if (done) done(std::move(error));
                   });
               });
}

void StreamProvider::RefreshKnownAlbum(const stream::Id& id) {
    uint64_t generation = 0;
    const std::wstring storefront = Storefront();
    const AlbumRequest request = ResolveAlbumRequest(id);
    {
        std::lock_guard lock(m_mutex);
        generation = m_generation;
    }
    Post(generation, [this, generation, id, request, storefront] {
        AlbumData data;
        try {
            data = LoadAlbum(request.id, storefront, request.library, *m_abort);
            if (!PutIfCurrent(generation, data.info)) return;
        } catch (const exception_aborted&) {
            return;
        } catch (...) {
            return;
        }
        PostMain(generation, [this, id, data = std::move(data)]() mutable {
            Catalog::Get().PutAndHint(data.songs);
            {
                std::lock_guard lock(m_mutex);
                m_albumTracks[id] = Tracks(data.songs);
                m_albumTracks[data.info.id] = Tracks(data.songs);
                if (!data.catalogId.empty()) m_albumLibraryIds[data.catalogId] = id;
            }
            NotifyLibraryChanged();
        });
    });
}

} // namespace glasswave::applemusic
