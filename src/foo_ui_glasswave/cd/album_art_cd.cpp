#include "pch.h"
#include "cache/images.h"
#include "cd/musicbrainz.h"

#include <SDK/album_art_helpers.h>

// An audio CD's cover: the front picture the Cover Art Archive keeps for the disc's
// MusicBrainz release, cached under the disc like any provider's artwork.
namespace glasswave::cd {

namespace {

struct Rendition {
    const wchar_t* suffix;
    const char* version;
};
Rendition RenditionFor(int sizePx) {
    if (sizePx > 1200) return {L"front", "original-v1"};
    return sizePx > 500 ? Rendition{L"front-1200", "1200-v1"} : Rendition{L"front-500", "500-v1"};
}

class EmptyArtPaths : public album_art_path_list {
  public:
    const char* get_path(t_size) const override { return ""; }
    t_size get_count() const override { return 0; }
};

class CdArtInstance : public album_art_extractor_instance_v2 {
  public:
    explicit CdArtInstance(metadb_handle_ptr track) : m_track(std::move(track)) {}

    album_art_path_list::ptr query_paths(const GUID&, abort_callback&) override {
        album_art_path_list::ptr paths;
        paths ^= fb2k::service_new<EmptyArtPaths>();
        return paths;
    }

    album_art_data_ptr query(const GUID& what, abort_callback& abort) override {
        if (what != album_art_ids::cover_front) throw exception_album_art_not_found();
        // foobar2000's disc path ("cdda://<id>") names the disc, so every track shares one entry.
        const std::string disc = DiscKey(m_track->get_path());
        const Rendition rendition = RenditionFor(cache::SizeHint());
        cache::ImageRequest request;
        request.identity = {"musicbrainz", "disc", disc.substr(7), rendition.version};
        if (std::wstring_view(rendition.suffix) == L"front")
            request.limits.maxBodyBytes = 64u * 1024u * 1024u;
        request.resolve = [track = m_track, rendition](abort_callback& workerAbort, unsigned) {
            const auto release = LookupDisc(track, workerAbort);
            if (!release || !release->frontCover) return std::optional<cache::HttpSource>();
            cache::HttpSource source;
            source.url = L"https://coverartarchive.org/release/" + release->id + L"/" + rendition.suffix;
            source.headers = {{L"User-Agent", L"Glasswave/0.1 (foobar2000 user interface component)"}};
            return std::optional<cache::HttpSource>(std::move(source));
        };
        if (auto result = cache::Images::Get().Open(std::move(request), abort))
            return album_art_data_impl::g_create(result->bytes.data(), result->bytes.size());
        throw exception_album_art_not_found();
    }

  private:
    metadb_handle_ptr m_track;
};

class CdArtFallback : public album_art_fallback {
  public:
    album_art_extractor_instance_v2::ptr open(metadb_handle_list_cref items,
                                              pfc::list_base_const_t<GUID> const&, abort_callback&) override {
        for (t_size i = 0; i < items.get_count(); ++i) {
            if (!items[i].is_valid() || !IsCdPath(items[i]->get_path())) continue;
            album_art_extractor_instance_v2::ptr instance;
            instance ^= fb2k::service_new<CdArtInstance>(items[i]);
            return instance;
        }
        throw exception_album_art_not_found();
    }
};

service_factory_single_t<CdArtFallback> g_cdArtFallback;

} // namespace

} // namespace glasswave::cd
