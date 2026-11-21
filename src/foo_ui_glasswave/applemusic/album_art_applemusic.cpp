#include "pch.h"
#include "applemusic/artwork_url.h"
#include "applemusic/catalog.h"
#include "applemusic/client.h"
#include "applemusic/handle.h"
#include "cache/images.h"
#include "cache/art_absence.h"

#include <SDK/album_art_helpers.h>

#include <algorithm>
#include <set>

namespace glasswave::applemusic {

namespace {

namespace json = netease::json;

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
    bool original;
};
Artwork ArtworkFor(int sizePx) {
    if (sizePx == cache::kOriginalSize) return {L"9999", L"png", "original-png-v1", true};
    if (sizePx > 1200) return {L"9999", L"jpg", "original-jpg-bb-v1", true};
    return {L"1200", L"jpg", "1200-jpg-bb-v1", false};
}

std::wstring Wide(const char* text) {
    return text ? pfc::stringcvt::string_wide_from_utf8(text).get_ptr() : std::wstring();
}

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_utf8_from_wide(text.data(), text.size()).get_ptr();
}

const char* ArtKindName(stream::ArtKind kind) {
    switch (kind) {
    case stream::ArtKind::Track:
        return "song";
    case stream::ArtKind::Album:
        return "album";
    case stream::ArtKind::Artist:
        return "artist";
    case stream::ArtKind::Playlist:
        return "playlist";
    case stream::ArtKind::Station:
        return "station";
    }
    return "";
}

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
    switch (kind) {
    case stream::ArtKind::Track:
        if (IsLibraryId(id) && id[0] == L'i') {
            path = L"/v1/me/library/songs/" + Encode(id) + L"?include=catalog";
        } else {
            path = L"/v1/catalog/" + Storefront(abort) + L"/songs/" + Encode(id) + L"?include=albums";
        }
        break;
    case stream::ArtKind::Album:
        path = IsLibraryId(id) && id[0] == L'l'
                   ? L"/v1/me/library/albums/" + Encode(id) + L"?include=catalog"
                   : L"/v1/catalog/" + Storefront(abort) + L"/albums/" + Encode(id);
        break;
    case stream::ArtKind::Artist:
        path = IsLibraryId(id) && id[0] == L'r'
                   ? L"/v1/me/library/artists/" + Encode(id) + L"?include=catalog"
                   : L"/v1/catalog/" + Storefront(abort) + L"/artists/" + Encode(id);
        break;
    case stream::ArtKind::Playlist:
        path = IsLibraryId(id) && id[0] == L'p'
                   ? L"/v1/me/library/playlists/" + Encode(id)
                   : L"/v1/catalog/" + Storefront(abort) + L"/playlists/" + Encode(id);
        break;
    case stream::ArtKind::Station:
        path = L"/v1/catalog/" + Storefront(abort) + L"/stations/" + Encode(id);
        break;
    }
    const Object response = Apple(path, abort);
    const auto item = FirstObject(response, L"data");
    return item ? ArtworkSearch(response).Find(*item) : std::wstring();
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

class EmptyArtPaths : public album_art_path_list {
  public:
    const char* get_path(t_size) const override { return ""; }
    t_size get_count() const override { return 0; }
};

class AppleArtInstance : public album_art_extractor_instance_v2 {
  public:
    explicit AppleArtInstance(std::string path) : m_path(std::move(path)) {}

    album_art_path_list::ptr query_paths(const GUID&, abort_callback&) override {
        album_art_path_list::ptr paths;
        paths ^= fb2k::service_new<EmptyArtPaths>();
        return paths;
    }

    album_art_data_ptr query(const GUID& what, abort_callback& abort) override {
        if (what != album_art_ids::cover_front) throw exception_album_art_not_found();
        CoverKind kind = CoverKind::Track;
        std::wstring id;
        if (!ParseCoverPath(m_path.c_str(), kind, id) && !ParseSongPath(m_path.c_str(), id)) {
            throw exception_album_art_not_found();
        }

        const stream::ArtKind artKind = kind == CoverKind::Album      ? stream::ArtKind::Album
                                        : kind == CoverKind::Artist   ? stream::ArtKind::Artist
                                        : kind == CoverKind::Playlist ? stream::ArtKind::Playlist
                                        : kind == CoverKind::Station  ? stream::ArtKind::Station
                                                                      : stream::ArtKind::Track;
        // A song shows its album's cover. Storing it under the album makes every song of the
        // album, the album page and the grids share one download (stream::Provider::ArtKeyOf
        // names the same identity for the UI). The URL is still resolved through the song.
        stream::Id albumOfSong;
        if (artKind == stream::ArtKind::Track) albumOfSong = Catalog::Get().SongAlbum(id);
        std::wstring catalogUrl = Catalog::Get().CoverUrl(artKind, id);
        if (catalogUrl.empty() && !albumOfSong.empty())
            catalogUrl = Catalog::Get().CoverUrl(stream::ArtKind::Album, albumOfSong);

        const Artwork artwork = ArtworkFor(cache::SizeHint());
        cache::ImageRequest request;
        request.identity = albumOfSong.empty()
                               ? cache::ImageIdentity{"apple", ArtKindName(artKind), Utf8(id), artwork.version}
                               : cache::ImageIdentity{"apple", ArtKindName(stream::ArtKind::Album),
                                                      Utf8(albumOfSong), artwork.version};
        if (!albumOfSong.empty())
            request.alternates.push_back({"apple", ArtKindName(artKind), Utf8(id), artwork.version});
        request.limits.maxBodyBytes = artwork.original ? kMaxOriginalBytes : kMaxArtworkBytes;
        request.resolve = [artKind, id, artwork, identity = request.identity, epoch = cache::art_absence::Epoch(),
                           account = cache::AccountRevision("apple"),
                           catalogUrl = ResizeArtwork(std::move(catalogUrl), artwork)](
                              abort_callback& workerAbort, unsigned attempt) mutable {
            std::wstring url = attempt == 0 ? catalogUrl : std::wstring();
            if (cache::AccountRevision("apple") != account) throw exception_aborted();
            if (attempt == 0 && url.empty() && cache::art_absence::KnownMissing(identity))
                return std::optional<cache::HttpSource>();
            if (attempt != 0 || !IsAllowedAppleArtworkUrl(url)) {
                try {
                    url = ResizeArtwork(FetchCoverUrl(artKind, id, workerAbort), artwork);
                    if (url.empty()) cache::art_absence::StoreMissing(identity, account, epoch);
                } catch (const exception_aborted&) {
                    throw;
                } catch (...) {
                    return std::optional<cache::HttpSource>();
                }
            }
            if (!IsAllowedAppleArtworkUrl(url)) return std::optional<cache::HttpSource>();
            cache::art_absence::Clear(identity, account, epoch);
            cache::HttpSource source;
            source.url = std::move(url);
            source.headers.emplace_back(L"User-Agent", L"Glasswave/0.1 Apple Music artwork");
            return std::optional<cache::HttpSource>(std::move(source));
        };

        try {
            if (auto result = cache::Images::Get().Open(std::move(request), abort))
                return album_art_data_impl::g_create(result->bytes.data(), result->bytes.size());
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
        }
        throw exception_album_art_not_found();
    }

  private:
    std::string m_path;
};

class AppleArtExtractor : public album_art_extractor_v2 {
  public:
    bool is_our_path(const char* path, const char* extension) override {
        (void)extension;
        CoverKind kind = CoverKind::Track;
        stream::Id id;
        if (ParseCoverPath(path, kind, id)) return true;
        return ParseSongPath(path, id);
    }

    album_art_extractor_instance_ptr open(file_ptr filehint, const char* path,
                                          abort_callback& abort) override {
        (void)filehint;
        (void)abort;
        return fb2k::service_new<AppleArtInstance>(std::string(path ? path : ""));
    }

    GUID get_guid() override {
        static const GUID guid = {
            0xc4a7dba2, 0x5f52, 0x4ac7, {0x9f, 0x72, 0x6d, 0xea, 0x21, 0xf4, 0x88, 0x63}};
        return guid;
    }
};

static service_factory_single_t<AppleArtExtractor> g_apple_art;

class AppleArtFallback : public album_art_fallback {
  public:
    album_art_extractor_instance_v2::ptr open(metadb_handle_list_cref items,
                                              pfc::list_base_const_t<GUID> const& ids,
                                              abort_callback& abort) override {
        (void)ids;
        (void)abort;
        for (t_size i = 0; i < items.get_count(); ++i) {
            const char* path = items[i].is_valid() ? items[i]->get_path() : nullptr;
            CoverKind kind = CoverKind::Track;
            stream::Id id;
            if (ParseCoverPath(path, kind, id) || ParseSongPath(path, id)) {
                album_art_extractor_instance_v2::ptr instance;
                instance ^= fb2k::service_new<AppleArtInstance>(std::string(path ? path : ""));
                return instance;
            }
        }
        throw exception_album_art_not_found();
    }
};

static service_factory_single_t<AppleArtFallback> g_apple_art_fallback;

} // namespace

} // namespace glasswave::applemusic
