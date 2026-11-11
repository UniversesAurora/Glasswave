#pragma once

#include "stream/list.h"

#include <functional>
#include <memory>

namespace glasswave::cache::lists {

struct Identity {
    std::string provider;
    std::string logical; // Versioned semantic query including effective UI language.
    uint64_t accountRevision = 0;
};
struct Record {
    stream::ListPage page; // SavePage/UpdatePage: one source page. Load: the accumulated prefix.
    size_t pages = 0;      // SavePage/UpdatePage: 1-based page ordinal. Load: number of pages.
    int64_t validatedAt = 0;
    // Load only: contiguous prefix whose persisted pages contain completed enrichment.
    size_t metadataReadyRows = 0;
};
using Loaded = std::function<void(std::optional<Record>)>;
// Reports whether the exact background page/manifest publication completed. Called on the
// main thread when accepted by the cache worker; false includes rejection or write failure.
using Persisted = std::function<void(bool success)>;

// Background disk work, callbacks on the main thread. Values are untrusted until their
// schema, bounded sizes, row kind and page chain have been validated.
void Load(Identity identity, Loaded done);
// The caller applies ListCachePolicy before calling. A trailing page with more=false
// completes the snapshot; older generations and noncontiguous page ordinals are rejected.
void SavePage(Identity identity, Record record, uint64_t generation, Persisted persisted = {});
// Replaces only the cached rows and enrichment marker of an existing page. The source page's
// identity, row kind/count, ordering, cursor, total and revision are immutable.
void UpdatePage(Identity identity, Record record, uint64_t generation, Persisted persisted = {});
// Cancels in-memory publishers and resets generation ownership while keeping the current
// account scope, context key and persisted guard readable.
void RetireProvider(std::string provider);
// Cancels in-memory publishers and rotates the provider-local persisted guard.
void InvalidateProvider(std::string provider);
void Shutdown();

} // namespace glasswave::cache::lists
