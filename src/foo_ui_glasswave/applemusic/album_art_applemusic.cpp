#include "pch.h"
#include "applemusic/artwork_url.h"
#include "core/text.h"
#include "applemusic/catalog.h"
#include "applemusic/client.h"
#include "applemusic/display_metadata.h"
#include "applemusic/playback_identity.h"
#include "cache/images.h"
#include "stream/art_extractor.h"

#include <SDK/album_art_helpers.h>

#include <algorithm>
#include <set>

namespace glasswave::applemusic {

namespace {

namespace json = core::json;

constexpr unsigned kMaxRelationshipDepth = 4;
constexpr size_t kMaxRelationshipItems = 32;
constexpr size_t kMaxTraversalObjects = 96;
constexpr size_t kMaxArtworkBytes = 20u * 1024u * 1024u, kMaxOriginalBytes = 64u * 1024u * 1024u;

// Apple's artwork template takes any size and caps it at the uploaded original (9999 gave
// 1800 and 2400 px on 2026-11-14; 100000 is refused). 1200 px JPEG covers every view up to
// that size; a larger cover gets the original's size, and a saved one the original's pixels
// as lossless PNG (the JPEG rendition is re-compressed even at full size).
struct Artwork {
    const wchar_t* size;
    const wchar_t* format;
    const char* version;
    size_t maxBodyBytes;
};
Artwork ArtworkFor(int sizePx) {
    if (sizePx == cache::kOriginalSize) return {L"9999", L"png", "original-png-v1", kMaxOriginalBytes};
    if (sizePx > 1200) return {L"9999", L"jpg", "original-jpg-bb-v1", kMaxOriginalBytes};
    return {L"1200", L"jpg", "1200-jpg-bb-v1", kMaxArtworkBytes};
}

using core::Utf8;
using core::Wide;

std::optional<Object> FirstObject(const Object& response, const wchar_t* key) {
    const auto array = json::GetArray(response, key);
    if (!array) return std::nullopt;
    const auto objects = json::Objects(*array);
    if (objects.empty()) return std::nullopt;
    return objects.front();
}

class ArtworkSearch {
  public:
    explicit ArtworkSearch(const Object& response) : m_response(response) {}

    std::wstring Find(const Object& item) { return Find(item, 0); }

  private:
    std::optional<Object> FindResource(const Object& reference) const {
        const std::wstring type = json::GetString(reference, L"type");
        const std::wstring id = json::GetString(reference, L"id");
        if (type.empty() || id.empty()) return std::nullopt;
        const auto resources = json::GetObject(m_response, L"resources");
        if (!resources) return std::nullopt;
        const auto bucket = json::GetObject(*resources, type.c_str());
        return bucket ? json::GetObject(*bucket, id.c_str()) : std::nullopt;
    }

    std::wstring Find(const Object& reference, unsigned depth) {
        if (depth > kMaxRelationshipDepth || ++m_objects > kMaxTraversalObjects) return {};

        const auto resource = FindResource(reference);
        const Object& item = resource ? *resource : reference;
        const std::wstring identity = json::GetString(item, L"type") + L":" + json::GetString(item, L"id");
        if (identity != L":" && !m_seen.insert(identity).second) return {};

        if (const auto attributes = json::GetObject(item, L"attributes")) {
            for (const wchar_t* name : {L"artwork", L"editorialArtwork"}) {
                const auto artwork = json::GetObject(*attributes, name);
                if (!artwork) continue;
                const std::wstring url = json::GetString(*artwork, L"url");
                if (!url.empty()) return url;
            }
        }

        const auto relationships = json::GetObject(item, L"relationships");
        if (!relationships) return {};
        for (const wchar_t* name : {L"catalog", L"albums", L"artists"}) {
            const auto relation = json::GetObject(*relationships, name);
            if (!relation) continue;
            const auto data = json::GetArray(*relation, L"data");
            if (!data) continue;
            const auto related = json::Objects(*data);
            const size_t count = std::min(related.size(), kMaxRelationshipItems);
            for (size_t i = 0; i < count; ++i) {
                const std::wstring url = Find(related[i], depth + 1);
                if (!url.empty()) return url;
            }
        }
        return {};
    }

    const Object& m_response;
    std::set<std::wstring> m_seen;
    size_t m_objects = 0;
};

std::wstring Storefront(abort_callback& abort) {
    const std::wstring known = DisplayMetadata::Get().SourceStorefront();
    if (known.size() == 2) return known;
    try {
        const AccountState account = GetAccount(abort);
        if (account.signedIn && account.storefront.size() == 2) return account.storefront;
    } catch (const exception_aborted&) {
        throw;
    } catch (...) {
    }
    return L"US";
}

std::wstring FetchCoverUrl(stream::ArtKind kind, const std::wstring& id, abort_callback& abort) {
    std::wstring path;
    std::wstring storefront;
    const auto ensureStorefront = [&]() -> const std::wstring& {
        if (storefront.empty()) storefront = Storefront(abort);
        return storefront;
    };
    const auto fetchUrl = [&](const std::wstring& requestPath) -> std::wstring {
        const Object response = Apple(requestPath, abort);
        const auto item = FirstObject(response, L"data");
        return item ? ArtworkSearch(response).Find(*item) : std::wstring();
    };
    switch (kind) {
    case stream::ArtKind::Track: {
        const auto song = Catalog::Get().FindSongRaw(id);
        const stream::Id reportingId =
            song && !song->reportingId.empty() ? song->reportingId : Catalog::Get().FindReportingId(id);
        if (IsCatalogSongId(reportingId)) {
            path = L"/v1/catalog/" + ensureStorefront() + L"/songs/" + Encode(reportingId) + L"?include=albums";
        } else if (IsLibraryId(id) && id[0] == L'i') {
            path = L"/v1/me/library/songs/" + Encode(id) + L"?include=catalog";
        } else if (song && !song->libraryId.empty()) {
            path = L"/v1/me/library/songs/" + Encode(song->libraryId) + L"?include=catalog";
        } else {
            path = L"/v1/catalog/" + ensureStorefront() + L"/songs/" + Encode(id) + L"?include=albums";
        }
        try {
            return fetchUrl(path);
        } catch (const BridgeHttpError& error) {
            if (error.status() != 404 || !IsCatalogSongId(id)) throw;
            MatchHints hints;
            if (song) {
                hints.title = song->track.title;
                hints.album = song->track.album;
                hints.contentRating = song->contentRating;
                hints.durationMs = song->track.durationMs;
            }
            const auto equivalents = ResolveCatalogEquivalents(ensureStorefront(), id, hints, {id}, abort);
            if (equivalents.empty()) throw;
            return fetchUrl(L"/v1/catalog/" + ensureStorefront() + L"/songs/" + Encode(equivalents.front()) +
                            L"?include=albums");
        }
    }
    case stream::ArtKind::Album:
        path = IsLibraryId(id) && id[0] == L'l'
                   ? L"/v1/me/library/albums/" + Encode(id) + L"?include=catalog"
                   : L"/v1/catalog/" + ensureStorefront() + L"/albums/" + Encode(id);
        break;
    case stream::ArtKind::Artist:
        path = IsLibraryId(id) && id[0] == L'r'
                   ? L"/v1/me/library/artists/" + Encode(id) + L"?include=catalog"
                   : L"/v1/catalog/" + ensureStorefront() + L"/artists/" + Encode(id);
        break;
    case stream::ArtKind::Playlist:
        path = IsLibraryId(id) && id[0] == L'p'
                   ? L"/v1/me/library/playlists/" + Encode(id)
                   : L"/v1/catalog/" + ensureStorefront() + L"/playlists/" + Encode(id);
        break;
    case stream::ArtKind::Station:
        path = L"/v1/catalog/" + ensureStorefront() + L"/stations/" + Encode(id);
        break;
    }
    return fetchUrl(path);
}

std::wstring ResizeArtwork(std::wstring url, const Artwork& artwork) {
    if (url.empty()) return {};
    const auto replace = [&url](std::wstring_view token, std::wstring_view value) {
        for (size_t at = 0; (at = url.find(token, at)) != std::wstring::npos; at += value.size())
            url.replace(at, token.size(), value);
    };
    replace(L"{w}", artwork.size);
    replace(L"{h}", artwork.size);
    replace(L"{f}", artwork.format);
    replace(L"{c}", L"bb");
    return url;
}

// Apple Music's cover traits for stream::ArtExtractor.
struct AppleArt {
    static constexpr const char* kProvider = "apple";
    using Rendition = Artwork;

    static GUID ExtractorGuid() {
        return {0xc4a7dba2, 0x5f52, 0x4ac7, {0x9f, 0x72, 0x6d, 0xea, 0x21, 0xf4, 0x88, 0x63}};
    }
    static const stream::Catalog& Records() { return Catalog::Get().Records(); }
    static stream::Id AlbumOf(const stream::Id& song) { return Catalog::Get().SongAlbum(song); }
    static Rendition RenditionFor(int sizePx) { return ArtworkFor(sizePx); }

    static std::wstring KnownUrl(const stream::ArtRef& ref, const stream::Id& album,
                                 const Rendition& artwork) {
        std::wstring url = Catalog::Get().CoverUrl(ref.kind, ref.id);
        if (url.empty() && !album.empty()) url = Catalog::Get().CoverUrl(stream::ArtKind::Album, album);
        return ResizeArtwork(std::move(url), artwork);
    }
    static bool Usable(const std::wstring& url) { return IsAllowedAppleArtworkUrl(url); }
    static stream::ArtUrl ResolveUrl(const stream::ArtRef& ref, const stream::Id&, const std::wstring&,
                                     const Rendition& artwork, unsigned, abort_callback& abort) {
        std::wstring url = ResizeArtwork(FetchCoverUrl(ref.kind, ref.id, abort), artwork);
        return {url, url.empty()};
    }
    static std::vector<cache::HttpHeader> Headers() {
        return {{L"User-Agent", L"Glasswave/0.1 Apple Music artwork"}};
    }
};

static service_factory_single_t<stream::ArtExtractor<AppleArt>> g_apple_art;
static service_factory_single_t<stream::ArtFallback<AppleArt>> g_apple_art_fallback;

} // namespace

} // namespace glasswave::applemusic
