#include "pch.h"
#include "cache/images.h"
#include "cache/art_absence.h"
#include "netease/catalog.h"
#include "netease/handle.h"
#include "core/log.h"
#include "netease/session.h"

#include <SDK/album_art_helpers.h>

namespace glasswave::netease {

namespace {

// Three renditions: 500 px for lists, grids and tiles, 1200 px where a cover is drawn larger
// than that (the mini player and the full screen), the original beyond 1200 px and for a
// saved cover; each its own cache entry.
constexpr int kSmallRendition = 500, kLargeRendition = 1200;
struct Rendition {
    const wchar_t* param;
    const char* version;
};
Rendition RenditionFor(int sizePx) {
    // The original is the uploaded file itself: the URL without a param. A cover drawn larger
    // than the 1200 px rendition (full screen at a high DPI) and a saved one use it.
    if (sizePx > kLargeRendition) return Rendition{nullptr, "original-v1"};
    return sizePx > kSmallRendition ? Rendition{L"1200y1200", "1200y1200-v1"} : Rendition{L"500y500", "500y500-v1"};
}

std::wstring CoverUrl(std::wstring url, const Rendition& rendition) {
    if (url.empty()) return {};
    if (url.rfind(L"http://", 0) == 0) url = L"https://" + url.substr(7); // NCM avatars come as http
    if (!rendition.param) return url;
    url += (url.find(L'?') == std::wstring::npos ? L"?param=" : L"&param=") + std::wstring(rendition.param);
    return url;
}

const char* ArtKindName(CoverKind kind) {
    switch (kind) {
    case CoverKind::Song:
        return "song";
    case CoverKind::Album:
        return "album";
    case CoverKind::Artist:
        return "artist";
    case CoverKind::Playlist:
        return "playlist";
    }
    return "";
}

} // namespace

class EmptyArtPaths : public album_art_path_list {
  public:
    const char* get_path(t_size) const override { return ""; }
    t_size get_count() const override { return 0; }
};

class NcmArtInstance : public album_art_extractor_instance_v2 {
  public:
    explicit NcmArtInstance(std::string path) : m_path(std::move(path)) {}

    album_art_path_list::ptr query_paths(const GUID&, abort_callback&) override {
        album_art_path_list::ptr paths;
        paths ^= fb2k::service_new<EmptyArtPaths>();
        return paths;
    }

    album_art_data_ptr query(const GUID& what, abort_callback& abort) override {
        if (what != album_art_ids::cover_front) throw exception_album_art_not_found();
        CoverKind kind = CoverKind::Song;
        int64_t id = 0;
        if (!ParseCoverPath(m_path.c_str(), kind, id)) {
            kind = CoverKind::Song;
            if (!ParseSongPath(m_path.c_str(), id)) throw exception_album_art_not_found();
        }

        // A song shows its album's cover. Storing it under the album makes every song of the
        // album, the album page and the grids share one download (stream::Provider::ArtKeyOf
        // names the same identity for the UI).
        int64_t albumOfSong = 0;
        if (kind == CoverKind::Song)
            if (const auto song = Catalog::Get().FindSong(id)) albumOfSong = song->albumId;

        const Rendition rendition = RenditionFor(cache::SizeHint());
        cache::ImageRequest request;
        if (!rendition.param) request.limits.maxBodyBytes = 64u * 1024u * 1024u; // an uploaded original
        request.identity = albumOfSong ? cache::ImageIdentity{"ncm", ArtKindName(CoverKind::Album),
                                                              std::to_string(albumOfSong), rendition.version}
                                       : cache::ImageIdentity{"ncm", ArtKindName(kind), std::to_string(id),
                                                              rendition.version};
        if (albumOfSong)
            request.alternates.push_back({"ncm", ArtKindName(kind), std::to_string(id), rendition.version});
        auto noAddress = std::make_shared<std::atomic_bool>(false); // why a download failed, for the log
        request.resolve = [kind, id, albumOfSong, rendition, noAddress, identity = request.identity,
                           epoch = cache::art_absence::Epoch(),
                           account = cache::AccountRevision("ncm")](abort_callback& workerAbort,
                                                                    unsigned attempt) {
            if (cache::AccountRevision("ncm") != account) throw exception_aborted();
            std::wstring url;
            switch (kind) {
            case CoverKind::Playlist:
                url = Catalog::Get().PlaylistCover(id);
                break;
            case CoverKind::Artist:
                url = Catalog::Get().ArtistCover(id);
                break;
            case CoverKind::Album:
                url = Catalog::Get().AlbumCover(id);
                break;
            case CoverKind::Song:
                url = Catalog::Get().SongCover(id);
                if (url.empty() && albumOfSong) url = Catalog::Get().AlbumCover(albumOfSong);
                break;
            }

            if (kind == CoverKind::Song && (url.empty() || attempt > 0)) {
                if (attempt == 0 && url.empty() && cache::art_absence::KnownMissing(identity))
                    return std::optional<cache::HttpSource>();
                auto& provider = Session::Get();
                std::wstring error;
                if (!provider.Service().EnsureStarted(workerAbort, &error))
                    return std::optional<cache::HttpSource>();
                std::vector<Song> songs;
                if (!provider.Api().SongDetails({id}, workerAbort, songs, error))
                    return std::optional<cache::HttpSource>();
                Catalog::Get().Put(songs);
                if (!songs.empty()) url = songs.front().coverUrl;
                if (!songs.empty() && url.empty()) cache::art_absence::StoreMissing(identity, account, epoch);
            }

            // An album known only from its songs' rows: its own record has the cover's address,
            // as a song's details do for a song (a never-opened album's cover stayed away until
            // a restart, owner 2026-11-14).
            if (kind == CoverKind::Album && url.empty()) {
                if (attempt == 0 && cache::art_absence::KnownMissing(identity))
                    return std::optional<cache::HttpSource>();
                auto& provider = Session::Get();
                std::wstring error;
                if (!provider.Service().EnsureStarted(workerAbort, &error))
                    return std::optional<cache::HttpSource>();
                Album info;
                std::vector<Song> songs;
                if (!provider.Api().GetAlbum(id, workerAbort, info, songs, error))
                    return std::optional<cache::HttpSource>();
                Catalog::Get().Put(info);
                url = info.coverUrl;
                if (url.empty()) cache::art_absence::StoreMissing(identity, account, epoch);
            }

            url = CoverUrl(std::move(url), rendition);
            if (url.empty()) {
                noAddress->store(true);
                return std::optional<cache::HttpSource>();
            }
            cache::art_absence::Clear(identity, account, epoch);
            cache::HttpSource source;
            source.url = std::move(url);
            source.headers = {
                {L"User-Agent",
                 L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                 L"Glasswave/0.1"},
                {L"Referer", L"https://music.163.com/"}};
            return std::optional<cache::HttpSource>(std::move(source));
        };

        std::wstring reason;
        try {
            if (auto result = cache::Images::Get().Open(std::move(request), abort))
                return album_art_data_impl::g_create(result->bytes.data(), result->bytes.size());
            reason = noAddress->load() ? L"no cover address known" : L"no picture";
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& e) {
            reason = pfc::stringcvt::string_wide_from_utf8(e.what()).get_ptr();
        } catch (...) {
            reason = L"unknown error";
        }
        if (!abort.is_aborting()) {
            const auto wide = [](const char* text) {
                return std::wstring(pfc::stringcvt::string_wide_from_utf8(text ? text : "").get_ptr());
            };
            core::Log(L"art: cover download failed: " + wide(ArtKindName(kind)) + L" " + std::to_wstring(id) + L" (" +
                wide(rendition.version) + L"): " + reason);
        }
        throw exception_album_art_not_found();
    }

  private:
    std::string m_path;
};

class NcmArtExtractor : public album_art_extractor_v2 {
  public:
    bool is_our_path(const char* path, const char* extension) override {
        (void)extension;
        CoverKind kind = CoverKind::Song;
        int64_t id = 0;
        if (ParseCoverPath(path, kind, id)) return true;
        return ParseSongPath(path, id);
    }

    album_art_extractor_instance_ptr open(file_ptr filehint, const char* path,
                                          abort_callback& abort) override {
        (void)filehint;
        (void)abort;
        return fb2k::service_new<NcmArtInstance>(std::string(path ? path : ""));
    }

    GUID get_guid() override {
        static const GUID guid = {
            0x2d61b0e4, 0x7a3c, 0x4d95, {0x8f, 0x12, 0x6b, 0x5e, 0x0c, 0x74, 0x9a, 0x21}};
        return guid;
    }
};

static service_factory_single_t<NcmArtExtractor> g_ncm_art;

// Some art-manager paths never reach a registered extractor; this fallback handles the
// online lookup when the local/default sources have nothing.
class NcmArtFallback : public album_art_fallback {
  public:
    album_art_extractor_instance_v2::ptr open(metadb_handle_list_cref items,
                                              pfc::list_base_const_t<GUID> const& ids,
                                              abort_callback& abort) override {
        (void)ids;
        (void)abort;
        for (t_size i = 0; i < items.get_count(); ++i) {
            const char* path = items[i].is_valid() ? items[i]->get_path() : nullptr;
            CoverKind kind = CoverKind::Song;
            int64_t id = 0;
            if (ParseCoverPath(path, kind, id) || ParseSongPath(path, id)) {
                album_art_extractor_instance_v2::ptr instance;
                instance ^= fb2k::service_new<NcmArtInstance>(std::string(path ? path : ""));
                return instance;
            }
        }
        throw exception_album_art_not_found();
    }
};

static service_factory_single_t<NcmArtFallback> g_ncm_art_fallback;

} // namespace glasswave::netease
