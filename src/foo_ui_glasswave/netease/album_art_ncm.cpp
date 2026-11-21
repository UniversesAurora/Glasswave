#include "pch.h"
#include "core/text.h"
#include "netease/catalog.h"
#include "netease/session.h"
#include "stream/art_extractor.h"

namespace glasswave::netease {

namespace {

// Three renditions: 500 px for lists, grids and tiles, 1200 px where a cover is drawn larger
// than that (the mini player and the full screen), the original beyond 1200 px and for a
// saved cover; each its own cache entry.
constexpr int kSmallRendition = 500, kLargeRendition = 1200;
constexpr size_t kMaxCoverBytes = 20u * 1024u * 1024u, kMaxOriginalBytes = 64u * 1024u * 1024u;

struct Rendition {
    const wchar_t* param;
    const char* version;
    size_t maxBodyBytes;
};
Rendition RenditionFor(int sizePx) {
    // The original is the uploaded file itself: the URL without a param. A cover drawn larger
    // than the 1200 px rendition (full screen at a high DPI) and a saved one use it.
    if (sizePx > kLargeRendition) return Rendition{nullptr, "original-v1", kMaxOriginalBytes};
    return sizePx > kSmallRendition ? Rendition{L"1200y1200", "1200y1200-v1", kMaxCoverBytes}
                                    : Rendition{L"500y500", "500y500-v1", kMaxCoverBytes};
}

std::wstring CoverUrl(std::wstring url, const Rendition& rendition) {
    if (url.empty()) return {};
    if (url.rfind(L"http://", 0) == 0) url = L"https://" + url.substr(7); // NCM avatars come as http
    if (!rendition.param) return url;
    url += (url.find(L'?') == std::wstring::npos ? L"?param=" : L"&param=") + std::wstring(rendition.param);
    return url;
}

[[noreturn]] void Fail(const std::wstring& error, const char* otherwise) {
    throw std::runtime_error(error.empty() ? otherwise : core::Utf8(error));
}

// NetEase's cover traits for stream::ArtExtractor.
struct NcmArt {
    static constexpr const char* kProvider = "ncm";
    using Rendition = netease::Rendition;

    static GUID ExtractorGuid() {
        return {0x2d61b0e4, 0x7a3c, 0x4d95, {0x8f, 0x12, 0x6b, 0x5e, 0x0c, 0x74, 0x9a, 0x21}};
    }
    static const stream::Catalog& Records() { return Catalog::Get().Records(); }
    static stream::Id AlbumOf(const stream::Id& song) {
        const auto found = Catalog::Get().FindSong(song);
        return found && Num(found->track.albumId) ? found->track.albumId : stream::Id();
    }
    static Rendition RenditionFor(int sizePx) { return netease::RenditionFor(sizePx); }

    static std::wstring KnownUrl(const stream::ArtRef& ref, const stream::Id& album, const Rendition& rendition) {
        const stream::Catalog& records = Catalog::Get().Records();
        std::wstring url = records.CoverUrl(ref.kind, ref.id);
        if (ref.kind == stream::ArtKind::Track && url.empty() && !album.empty())
            url = records.CoverUrl(stream::ArtKind::Album, album);
        return CoverUrl(std::move(url), rendition);
    }
    static bool Usable(const std::wstring& url) { return !url.empty(); }

    static stream::ArtUrl ResolveUrl(const stream::ArtRef& ref, const stream::Id&, const std::wstring& known,
                                     const Rendition& rendition, unsigned attempt, abort_callback& abort) {
        auto& provider = Session::Get();
        std::wstring error;
        if (ref.kind == stream::ArtKind::Track && (known.empty() || attempt > 0)) {
            if (!provider.Service().EnsureStarted(abort, &error)) Fail(error, "NetEase service is unavailable");
            std::vector<Song> songs;
            if (!provider.Api().SongDetails({Num(ref.id)}, abort, songs, error))
                Fail(error, "Cannot read the song's details");
            Catalog::Get().Put(ToSongs(songs));
            if (songs.empty()) return {known, false};
            return {CoverUrl(songs.front().coverUrl, rendition), songs.front().coverUrl.empty()};
        }
        // An album known only from its songs' rows: its own record has the cover's address, as a
        // song's details do for a song (a never-opened album's cover stayed away until a restart,
        // owner 2026-11-14).
        if (ref.kind == stream::ArtKind::Album && known.empty()) {
            if (!provider.Service().EnsureStarted(abort, &error)) Fail(error, "NetEase service is unavailable");
            Album info;
            std::vector<Song> songs;
            if (!provider.Api().GetAlbum(Num(ref.id), abort, info, songs, error))
                Fail(error, "Cannot read the album");
            Catalog::Get().Records().Put(ToAlbum(info));
            return {CoverUrl(info.coverUrl, rendition), info.coverUrl.empty()};
        }
        return {known, false};
    }

    static std::vector<cache::HttpHeader> Headers() {
        return {{L"User-Agent",
                 L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                 L"Glasswave/0.1"},
                {L"Referer", L"https://music.163.com/"}};
    }
};

static service_factory_single_t<stream::ArtExtractor<NcmArt>> g_ncm_art;
static service_factory_single_t<stream::ArtFallback<NcmArt>> g_ncm_art_fallback;

} // namespace

} // namespace glasswave::netease
