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
    // Persistent metadata is throttled for this long after a successful validation. The
    // first demand in each process still validates once even when that metadata is recent.
    int64_t freshForSeconds = 5 * 60;
    int64_t retryBaseSeconds = 10;
    int64_t retryMaxSeconds = 5 * 60;
};

struct ImageRequest {
    ImageIdentity identity;
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

// Memory-only demand schedule. False while a refresh is running or its failure backoff has
// not elapsed. The UI uses this when an old bitmap is still being painted.
bool ImageRefreshDue(std::string_view artKey);

// Provider-format identity used by UI caches: "<provider>:<art-kind>:<id>".
std::string ImageArtKey(const ImageIdentity& identity);

// {Kind::Image, provider, AccountScope(provider), artKind/id, renditionVersion}.
// AccountScope performs the Store lookup, so call this only from a worker.
Key MakeImageKey(const ImageIdentity& identity);

// Persistent SWR cache for provider artwork. A cached generation is leased from Store,
// copied, and returned before conditional revalidation starts. A cold miss may wait on the
// coalesced fetch; painting never calls this class. Results with changed bytes are published
// through a writer ticket and announced on the background thread to Subscribers.
class Images {
public:
    static Images& Get();
    static Images* Existing();
    static void Shutdown();

    ~Images();
    Images(const Images&) = delete;
    Images& operator=(const Images&) = delete;

    std::optional<ImageResult> Open(ImageRequest request, foobar2000_io::abort_callback& abort);
    bool RefreshDue(std::string_view artKey) const;

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
