#pragma once

#include "cache/art_absence.h"
#include "cache/images.h"
#include "core/text.h"
#include "stream/catalog.h"

#include <SDK/album_art_helpers.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// The foobar2000 album art extractor of a streaming source, written once. A source supplies its
// Traits (what is its cover's URL, how a rendition is asked for, which headers the CDN expects);
// the template does the rest: which paths are its own, one cache identity per picture ("a song
// shows its album's cover: stored under the album, alternates under the song"), the cache's
// resolver with the account check and the confirmed-missing memory, and the failure log.
//
//   struct Traits {
//       static constexpr const char* kProvider = "apple";   // ImageIdentity::provider
//       static GUID ExtractorGuid();
//       static const stream::Catalog& Records();             // parses the metadb paths
//       static stream::Id AlbumOf(const stream::Id& song);    // empty when unknown
//       struct Rendition { const char* version; size_t maxBodyBytes; ... };
//       static Rendition RenditionFor(int sizePx);
//       // The picture's address as the source already knows it, in the rendition; "" when it
//       // does not (the song's own, else its album's).
//       static std::wstring KnownUrl(const ArtRef&, const stream::Id& album, const Rendition&);
//       // Whether an address can be fetched at all.
//       static bool Usable(const std::wstring& url);
//       // Asks the service. Runs when KnownUrl is not usable and again (attempt > 0) after an
//       // expired address. `known` is what KnownUrl returned. May throw; abort passes through.
//       static ArtUrl ResolveUrl(const ArtRef&, const stream::Id& album, const std::wstring& known,
//                                const Rendition&, unsigned attempt, abort_callback&);
//       static std::vector<cache::HttpHeader> Headers();
//   };
namespace glasswave::stream {

struct ArtRef {
    ArtKind kind = ArtKind::Track;
    Id id;
};

// The answer of a source's service: the address (final, in the asked rendition) and whether the
// service confirmed that the picture does not exist (remembered, so it is not asked for again).
struct ArtUrl {
    std::wstring url;
    bool missing = false;
};

// "song", "album", "artist", "playlist" or "station": the ImageIdentity::artKind.
const char* ArtKindName(ArtKind kind);

// Whether `path` is a cover or a song path of `records` (a song shows its cover).
bool ParseArtPath(const Catalog& records, const char* path, ArtRef& ref);

// Writes one line to the log for a picture that could not be had, at most once per picture in a
// few minutes (the UI asks again after a pause, a list of covers that do not exist would repeat).
void LogArtFailure(const std::string& artKey, std::wstring_view what);

// Why a request failed, filled by its resolver (a worker) and read by the caller after the
// transfer ended.
class ArtFailure {
  public:
    void Set(std::wstring text) {
        std::lock_guard lock(m_mutex);
        m_text = std::move(text);
    }
    std::wstring Get() const {
        std::lock_guard lock(m_mutex);
        return m_text;
    }

  private:
    mutable std::mutex m_mutex;
    std::wstring m_text;
};

class EmptyArtPaths : public album_art_path_list {
  public:
    const char* get_path(t_size) const override { return ""; }
    t_size get_count() const override { return 0; }
};

template <class Traits>
class ArtInstance : public album_art_extractor_instance_v2 {
  public:
    explicit ArtInstance(std::string path) : m_path(std::move(path)) {}

    album_art_path_list::ptr query_paths(const GUID&, abort_callback&) override {
        album_art_path_list::ptr paths;
        paths ^= fb2k::service_new<EmptyArtPaths>();
        return paths;
    }

    album_art_data_ptr query(const GUID& what, abort_callback& abort) override {
        if (what != album_art_ids::cover_front) throw exception_album_art_not_found();
        ArtRef ref;
        if (!ParseArtPath(Traits::Records(), m_path.c_str(), ref)) throw exception_album_art_not_found();

        // A song shows its album's cover. Storing it under the album makes every song of the
        // album, the album page and the grids share one download (stream::Provider::ArtKeyOf
        // names the same identity for the UI).
        Id album;
        if (ref.kind == ArtKind::Track) album = Traits::AlbumOf(ref.id);

        const auto rendition = Traits::RenditionFor(cache::SizeHint());
        const std::string kind = ArtKindName(ref.kind);
        cache::ImageRequest request;
        request.identity = album.empty() ? cache::ImageIdentity{Traits::kProvider, kind, core::Utf8(ref.id),
                                                               rendition.version}
                                         : cache::ImageIdentity{Traits::kProvider, ArtKindName(ArtKind::Album),
                                                               core::Utf8(album), rendition.version};
        if (!album.empty())
            request.alternates.push_back({Traits::kProvider, kind, core::Utf8(ref.id), rendition.version});
        request.limits.maxBodyBytes = rendition.maxBodyBytes;
        const auto failure = std::make_shared<ArtFailure>();
        const uint64_t account = cache::AccountRevision(Traits::kProvider);
        request.resolve = [ref, album, rendition, failure, account, identity = request.identity,
                           epoch = cache::art_absence::Epoch()](abort_callback& workerAbort, unsigned attempt) {
            // The account this was asked for is gone: the picture is no longer wanted, which is
            // not an abort of the caller.
            if (cache::AccountRevision(Traits::kProvider) != account) throw cache::AccountChanged();
            std::wstring url = Traits::KnownUrl(ref, album, rendition);
            if (attempt == 0 && url.empty() && cache::art_absence::KnownMissing(identity)) {
                failure->Set(L"confirmed missing");
                return std::optional<cache::HttpSource>();
            }
            if (attempt != 0 || !Traits::Usable(url)) {
                try {
                    ArtUrl found = Traits::ResolveUrl(ref, album, url, rendition, attempt, workerAbort);
                    url = std::move(found.url);
                    if (found.missing) cache::art_absence::StoreMissing(identity, account, epoch);
                } catch (const exception_aborted&) {
                    throw;
                } catch (const std::exception& error) {
                    failure->Set(core::Wide(error.what()));
                    return std::optional<cache::HttpSource>();
                } catch (...) {
                    failure->Set(L"unknown error");
                    return std::optional<cache::HttpSource>();
                }
            }
            if (!Traits::Usable(url)) {
                failure->Set(L"no cover address known");
                return std::optional<cache::HttpSource>();
            }
            cache::art_absence::Clear(identity, account, epoch);
            cache::HttpSource source;
            source.url = std::move(url);
            source.headers = Traits::Headers();
            return std::optional<cache::HttpSource>(std::move(source));
        };

        std::wstring reason;
        try {
            if (auto result = cache::Images::Get().Open(request, abort))
                return album_art_data_impl::g_create(result->bytes.data(), result->bytes.size());
            reason = failure->Get();
            if (reason.empty()) reason = L"no picture";
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& error) {
            reason = core::Wide(error.what());
        } catch (...) {
            reason = L"unknown error";
        }
        // A request dropped for an account change or an abort is not a failed cover.
        if (!abort.is_aborting() && cache::AccountRevision(Traits::kProvider) == account)
            LogArtFailure(cache::ImageArtKey(request.identity) + "/" + rendition.version,
                          core::Wide(Traits::kProvider) + L" " + core::Wide(kind) + L" " + ref.id + L" (" +
                              core::Wide(rendition.version) + L"): " + reason);
        throw exception_album_art_not_found();
    }

  private:
    std::string m_path;
};

template <class Traits>
class ArtExtractor : public album_art_extractor_v2 {
  public:
    bool is_our_path(const char* path, const char*) override {
        ArtRef ref;
        return ParseArtPath(Traits::Records(), path, ref);
    }

    album_art_extractor_instance_ptr open(file_ptr, const char* path, abort_callback&) override {
        return fb2k::service_new<ArtInstance<Traits>>(std::string(path ? path : ""));
    }

    GUID get_guid() override { return Traits::ExtractorGuid(); }
};

// Some art-manager paths never reach a registered extractor; this fallback handles the online
// lookup when the local/default sources have nothing.
template <class Traits>
class ArtFallback : public album_art_fallback {
  public:
    album_art_extractor_instance_v2::ptr open(metadb_handle_list_cref items, pfc::list_base_const_t<GUID> const&,
                                              abort_callback&) override {
        for (t_size i = 0; i < items.get_count(); ++i) {
            const char* path = items[i].is_valid() ? items[i]->get_path() : nullptr;
            ArtRef ref;
            if (ParseArtPath(Traits::Records(), path, ref)) {
                album_art_extractor_instance_v2::ptr instance;
                instance ^= fb2k::service_new<ArtInstance<Traits>>(std::string(path));
                return instance;
            }
        }
        throw exception_album_art_not_found();
    }
};

} // namespace glasswave::stream
