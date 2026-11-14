#pragma once

#include "cache/store.h"

#include <SDK/abort_callback.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glasswave::cache {

using HttpHeader = std::pair<std::wstring, std::wstring>;

// Network identity supplied by a provider. The URL is deliberately absent: it can be
// signed, rotated, or private and is never part of a cache key or persisted metadata.
struct ImageIdentity {
    std::string provider;
    std::string artKind;
    std::string id;
    std::string renditionVersion;
};

struct HttpSource {
    std::wstring url;
    std::vector<HttpHeader> headers;
};

struct ImageLimits {
    size_t maxBodyBytes = 20u * 1024u * 1024u;
    // A cached picture is used without any request for this long after it was last confirmed.
    // An identity names one rendition of one object, so its bytes rarely change; a consumer
    // that learns of a change (a new cover URL, a manual refresh) calls Invalidate instead of
    // every view polling the server.
    int64_t freshForSeconds = 7 * 24 * 60 * 60;
    int64_t retryBaseSeconds = 10;
    int64_t retryMaxSeconds = 5 * 60;
};

struct ImageRequest {
    ImageIdentity identity;
    // Other names the same picture may have been stored under (a song's cover kept under the
    // song while its album was not yet known). A miss on `identity` reads them from disk
    // before anything goes to the network.
    std::vector<ImageIdentity> alternates;
    // Resolve a current URL only when network work starts. attempt is 0 first and 1 after
    // an expired URL/auth response, so Apple's resolver can force fresh metadata once.
    std::function<std::optional<HttpSource>(foobar2000_io::abort_callback&, unsigned attempt)> resolve;
    ImageLimits limits;
};

struct ImageResult {
    std::vector<unsigned char> bytes;
    bool fromCache = false;
    bool revalidating = false;
};

struct ImageEvent {
    std::string artKey;
    uint64_t accountRevision = 0;
};

// Provider-format identity used by UI caches: "<provider>:<art-kind>:<id>".
std::string ImageArtKey(const ImageIdentity& identity);

// {Kind::Image, provider, AccountScope(provider), artKind/id, renditionVersion}.
// AccountScope performs the Store lookup, so call this only from a worker.
Key MakeImageKey(const ImageIdentity& identity);

// While one is alive on a thread, Images::Open on that thread answers from the cache alone:
// a picture that is not cached returns nothing at once instead of waiting on the network, and
// TakeCacheMiss() then reports it, so the caller can hand the work to a lane that may wait.
class CacheOnlyScope {
  public:
    CacheOnlyScope();
    ~CacheOnlyScope();
    CacheOnlyScope(const CacheOnlyScope&) = delete;
    CacheOnlyScope& operator=(const CacheOnlyScope&) = delete;

  private:
    bool m_previous;
};
bool TakeCacheMiss();

// The pixel size the picture is decoded for, while one is alive on a thread (0: unknown). A
// provider with several renditions picks the one that size needs: lists a small one, the
// player's large cover a sharp one.
class SizeHintScope {
  public:
    explicit SizeHintScope(int px);
    ~SizeHintScope();
    SizeHintScope(const SizeHintScope&) = delete;
    SizeHintScope& operator=(const SizeHintScope&) = delete;

  private:
    int m_previous;
};
int SizeHint();

// Persistent cache for provider artwork: one download per identity, shared by every view
// and every later run. A cached generation is returned at once; it is revalidated in the
// background (a conditional request) only once it is older than its freshness window or was
// invalidated, and listeners hear about it only when the bytes really changed. A cold miss
// waits on the single coalesced fetch; painting never calls this class.
class Images {
public:
    static Images& Get();
    static Images* Existing();
    static void Shutdown();

    ~Images();
    Images(const Images&) = delete;
    Images& operator=(const Images&) = delete;

    std::optional<ImageResult> Open(ImageRequest request, foobar2000_io::abort_callback& abort);

    int Subscribe(std::function<void(ImageEvent event)> callback);
    void Unsubscribe(int token);

    // Forces the next demand under this stable art-key prefix to revalidate even when the
    // persisted entry is still fresh. No timer is created.
    void Invalidate(std::string_view artKeyPrefix);

private:
    Images();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace glasswave::cache
