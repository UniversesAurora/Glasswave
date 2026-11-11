#pragma once

#include "stream/list.h"

#include <memory>

namespace glasswave::stream {
class Provider;

enum class ListLoadMode {
    OnDemand,
    All
};
struct ListRowRange {
    size_t offset = 0;
    size_t count = 0;
};
enum class ListCacheInvalidation {
    RetireInMemory,
    PurgePersistent,
};
struct ListState {
    ListPage page; // Accumulated contiguous prefix, never a page delta.
    bool loading = false;
    bool complete = false;
    bool refreshing = false;
    bool fromCache = false;
    std::wstring error;
    size_t pages = 0;
    uint64_t version = 0;
    uint64_t dataVersion = 0;
    // For this dataVersion, [0, appendOffset) is unchanged. Zero denotes a new
    // generation/replacement; status-only emissions retain the last publication marker.
    size_t appendOffset = 0;
    // Contiguous prefix whose available related metadata has finished loading. Ordinary
    // rows need not wait; layouts requiring exact relationships use this frontier.
    size_t metadataReadyRows = 0;
    // Only these rows changed metadata; their occurrence identities and order are stable.
    std::optional<ListRowRange> updatedRows;
};

// One lease on a shared logical read. Methods and notifications run on the main thread.
// Destroying/canceling a lease drops its listener and auto-drain demand. Other consumers
// of the same list continue independently; changed accounts/languages revoke old results.
class ListSession final {
  public:
    using Changed = std::function<void(const ListState&)>;
    static std::shared_ptr<ListSession> Open(Provider& provider, ListQuery query, Changed changed,
                                             ListLoadMode mode = ListLoadMode::OnDemand);
    ~ListSession();
    void RequestNext();
    void SetMode(ListLoadMode mode);
    void Refresh();
    void Cancel();
    ListState State() const;
    // Borrowed until the next session mutation; main-thread consumers can read a large
    // prefix without copying it merely to inspect status or process an appended suffix.
    const ListState& View() const;

  private:
    struct Impl;
    explicit ListSession(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;
};

// RetireInMemory cancels active publishers and resets generation ownership while preserving
// readable persisted snapshots. PurgePersistent also rotates the provider-local disk guard
// for explicit data mutations and clears.
void InvalidateListCache(std::wstring_view provider, bool restart = true,
                         ListCacheInvalidation mode = ListCacheInvalidation::PurgePersistent);
void ShutdownListSessions();

} // namespace glasswave::stream
