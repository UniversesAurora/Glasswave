#include "pch.h"
#include "cache/images.h"
#include "cache/art_absence.h"
#include "netease/catalog.h"
#include "netease/handle.h"
#include "netease/log.h"
#include "netease/session.h"

#include <SDK/album_art_helpers.h>

namespace glasswave::netease {

namespace {

std::wstring CoverUrl(std::wstring url) {
    if (url.empty()) return {};
    if (url.rfind(L"http://", 0) == 0) url = L"https://" + url.substr(7); // NCM avatars come as http
    url += url.find(L'?') == std::wstring::npos ? L"?param=500y500" : L"&param=500y500";
    return url;
}

std::wstring Wide(const char* text) {
    return text ? pfc::stringcvt::string_wide_from_utf8(text).get_ptr() : std::wstring();
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

        Log(L"art: query " + Wide(m_path.c_str()));

        cache::ImageRequest request;
        request.identity = {"ncm", ArtKindName(kind), std::to_string(id), "500y500-v1"};
        request.resolve = [kind, id, identity = request.identity, epoch = cache::art_absence::Epoch(),
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

            url = CoverUrl(std::move(url));
            if (url.empty()) return std::optional<cache::HttpSource>();
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

        try {
            if (auto result = cache::Images::Get().Open(std::move(request), abort))
                return album_art_data_impl::g_create(result->bytes.data(), result->bytes.size());
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
        }
        if (!abort.is_aborting()) {
            Log(L"art: cover download failed");
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
        Log(L"art: open " + Wide(path ? path : ""));
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
                Log(L"art: fallback for " + Wide(path ? path : ""));
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
