#pragma once

#include "stream/model.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace glasswave::stream {

// Shared cancellation token carrying both a lock-free atomic flag and a lazily created
// Win32 manual-reset event so transport layers (WinHTTP, etc.) can cancel immediately
// without polling threads.
class CancelFlag {
  public:
    CancelFlag() = default;
    CancelFlag(const CancelFlag&) = delete;
    CancelFlag& operator=(const CancelFlag&) = delete;
    ~CancelFlag() {
        if (HANDLE h = m_event.exchange(nullptr, std::memory_order_acq_rel)) CloseHandle(h);
    }

    bool load(std::memory_order order = std::memory_order_seq_cst) const noexcept {
        return m_cancelled.load(order);
    }
    void store(bool value, std::memory_order order = std::memory_order_seq_cst) noexcept {
        m_cancelled.store(value, order);
        if (value) {
            if (HANDLE h = m_event.load(std::memory_order_acquire)) SetEvent(h);
        }
    }
    bool operator=(bool value) noexcept {
        store(value);
        return value;
    }
    operator bool() const noexcept { return load(); }
    HANDLE Event() const noexcept {
        if (HANDLE h = m_event.load(std::memory_order_acquire)) return h;
        HANDLE created = CreateEventW(nullptr, TRUE,
                                      m_cancelled.load(std::memory_order_acquire) ? TRUE : FALSE, nullptr);
        if (!created) return nullptr;
        HANDLE expected = nullptr;
        if (!m_event.compare_exchange_strong(expected, created, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            CloseHandle(created);
            return expected;
        }
        if (m_cancelled.load(std::memory_order_acquire)) SetEvent(created);
        return created;
    }

  private:
    std::atomic_bool m_cancelled{false};
    mutable std::atomic<HANDLE> m_event{nullptr};
};

using CancelToken = std::shared_ptr<CancelFlag>;

inline CancelToken MakeCancelToken() {
    return std::make_shared<CancelFlag>();
}

inline bool IsCancelled(const CancelToken& token) noexcept {
    return token && token->load(std::memory_order_acquire);
}

enum class ListKind {
    PlaylistTracks,
    Favorites,
    AlbumTracks,
    SectionTracks,
    ArtistTopTracks,
    ArtistAlbums,
    SavedAlbums,
    FollowedArtists,
    Playlists,
    Groups,
    CatalogSearch,
    // Appended because persisted cursors and diagnostics may use stable enum values.
    ArtistPlaylists,
    CatalogSearchArtists,
    CatalogSearchAlbums,
    CatalogSearchPlaylists,
    CatalogSearchTopResults,
    ArtistSimilarArtists, // rows: Artist
    CatalogSearchLyrics,  // rows: Track, songs whose lyrics match (Track::lyricMatch)
    HomeList,             // rows: Track, a home card's daily song list (HomeCard::list; page.playlist
                          // names it)
};
enum class ListCachePolicy {
    None,
    Memory,
    Persistent
};

struct ListQuery {
    ListKind kind = ListKind::PlaylistTracks;
    Id id;
    Id section;
    Shelf shelf = Shelf::Created;
    std::wstring search;
    // nullopt lets the provider choose; search defaults to None.
    std::optional<ListCachePolicy> cache;
    // Transport lifetime is excluded from semantic identity and persistence.
    CancelToken cancelled;
    bool operator==(const ListQuery& other) const {
        return kind == other.kind && id == other.id && section == other.section && shelf == other.shelf &&
               search == other.search && cache == other.cache;
    }
};

// Identifies which cached lists a provider write affected. Empty `id` or `section`
// matches any list of `kind`.
struct ListTarget {
    ListKind kind = ListKind::PlaylistTracks;
    Id id;
    Id section;
};

// The provider owns the cursor encoding. For offset services, `offset` counts source records
// consumed, not rows emitted after availability filtering. `value` carries any opaque server
// cursor or stable snapshot version needed to make the continuation safe.
struct ListCursor {
    std::wstring value;
    size_t offset = 0;
    bool operator==(const ListCursor&) const = default;
};

using ListRows = std::variant<std::vector<Track>, std::vector<Playlist>, std::vector<Album>,
                              std::vector<Artist>, std::vector<BrowseGroup>, std::vector<CatalogSearchItem>>;
struct ListPage {
    ListRows rows = std::vector<Track>{};
    std::optional<Playlist> playlist;
    std::optional<Album> album;
    std::optional<Artist> artist;
    ListCursor next;
    bool more = false;
    std::optional<size_t> total;
    // Stable identity of the source ordering for this page and its next cursor.
    std::wstring revision;
    // Rows are usable now; optional related records are still being fetched. Persisted
    // snapshots retain this marker so views requiring those relations can wait safely.
    bool enrichmentPending = false;
};
using ListPageDone = std::function<void(ListPage page, std::wstring error)>;

// CatalogSearch is the Songs category. The other kinds are independent requests, cursors and
// optional cache entries; CatalogSearchTopResults preserves the server's mixed ranking (its
// best matches) and must not be synthesized from the other categories.
inline bool IsCatalogSearch(ListKind kind) {
    return kind == ListKind::CatalogSearch || kind == ListKind::CatalogSearchArtists ||
           kind == ListKind::CatalogSearchAlbums || kind == ListKind::CatalogSearchPlaylists ||
           kind == ListKind::CatalogSearchTopResults || kind == ListKind::CatalogSearchLyrics;
}

// The cursor bookkeeping of a service that pages by offset: `received` source rows were read
// starting at `cursor`, and the service says whether more follow. An empty answer ends the list
// whatever `more` says, so a service that keeps claiming more cannot loop. Sources whose cursors
// are opaque server links do not use it.
inline void SetOffsetNext(ListPage& page, const ListCursor& cursor, size_t received, bool more) {
    page.next = {{}, cursor.offset + received};
    page.more = more && received != 0;
}

inline ListQuery ArtistSectionQuery(const Id& artist, const ArtistSection& section) {
    ListQuery query;
    query.id = artist;
    query.section = section.id;
    switch (section.rowKind) {
    case ArtistSectionRowKind::Tracks:
        query.kind = ListKind::ArtistTopTracks;
        break;
    case ArtistSectionRowKind::Albums:
        query.kind = ListKind::ArtistAlbums;
        break;
    case ArtistSectionRowKind::Playlists:
        query.kind = ListKind::ArtistPlaylists;
        break;
    case ArtistSectionRowKind::Artists:
        query.kind = ListKind::ArtistSimilarArtists;
        break;
    }
    return query;
}

} // namespace glasswave::stream
