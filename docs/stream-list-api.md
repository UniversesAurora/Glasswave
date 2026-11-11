# Stream list API contract

This document defines the provider-neutral list API used by catalog search and artist views.
Provider implementations own endpoint mapping, authentication, cursors and country/language
context. Consumers own presentation and scheduling.

## Artist views

`Provider::ArtistSections(artist)` returns read-only `ArtistSection` descriptors. It must not
perform network access. Each descriptor has:

- `id`: a stable semantic key used in `ListQuery::section`.
- `label`: an untranslated UI key or provider-owned literal. Display code applies `Tr`.
- `rowKind`: `Tracks`, `Albums` or `Playlists`.

```cpp
struct ArtistSection {
    Id id;
    std::wstring label;
    ArtistSectionRowKind rowKind;
};

virtual std::vector<ArtistSection> Provider::ArtistSections(const Id& artist) const;
ListQuery ArtistSectionQuery(const Id& artist, const ArtistSection& section);
```

The base provider returns independent Top songs and Albums descriptors. Apple may expose all
eight documented views:

| key | rows |
| --- | --- |
| `top-songs` | tracks |
| `featured-albums` | albums |
| `full-albums` | albums |
| `singles` | albums |
| `live-albums` | albums |
| `appears-on-albums` | albums |
| `compilation-albums` | albums |
| `featured-playlists` | playlists |

Consumers call `ArtistSectionQuery(artist, section)` and open the resulting `ListSession`.
They must keep datasets, cursors and cache identities independent. Presentation may compose
multiple sections into one page. `ListKind`, artist ID and section key are separate
parts of session and persistent-cache identity. A library-only artist may advertise a
distinct library fallback; it must not label a local ordering as an official ranking.

`ArtistTopTracks`, `ArtistAlbums` and the appended `ArtistPlaylists` kind carry the rows.
`CatalogSearch` remains the existing Songs category, so its enum value is unchanged.

## Catalog search

Search categories are independent list queries and preserve independent cursors and optional
cache entries:

- `CatalogSearch`: Songs.
- `CatalogSearchArtists`: Artists.
- `CatalogSearchAlbums`: Albums.
- `CatalogSearchPlaylists`: Playlists.
- `CatalogSearchTopResults`: mixed Best/Top results.

All five categories default to `ListCachePolicy::None`. A caller may opt into Memory or
Persistent storage per query. Local filtering is a presentation operation and does not alter
the source query.

`Provider::CatalogSearchKinds()` advertises the typed categories without network access. The
base implementation returns Songs only when the legacy `Features::catalogSearch` flag is set.
Apple overrides it with all five kinds. `SupportsCatalogSearchSuggestions()` defaults to false
and is independent of paged search support; Apple returns true for its documented suggestions
endpoint.

```cpp
virtual std::vector<ListKind> Provider::CatalogSearchKinds() const;
virtual bool Provider::SupportsCatalogSearchSuggestions() const;
```

Top results use `std::vector<CatalogSearchItem>`, where `CatalogSearchItem` is
`std::variant<Track, Artist, Album, Playlist>`. Vector position is the server rank. The
variant arm is the resource type, and `CatalogSearchItemKey()` supplies a type-qualified
identity. Consumers must not synthesize Best by concatenating the typed categories.
The Artists, Albums, Songs and Playlists categories reuse their existing homogeneous row
vectors; only Best adds an appended `ListRows` arm for mixed rows.

The cache codec persists mixed rows in their original order. A persistent opt-in therefore
retains server rank and resource type without introducing provider-specific fields.

## Search suggestions

Suggestions are a separate provider call and are not `ListSession` pages:

```cpp
CatalogSearchSuggestionQuery query;
query.term = text;
query.cancelled = callerOwnedToken;

provider.FetchCatalogSearchSuggestions(
    std::move(query),
    [](Provider::CatalogSearchSuggestions suggestions, std::wstring error) {
        // suggestions.ordered: CatalogSearchSuggestionTerm or CatalogSearchItem,
        // retained in the original server interleaving.
    });
```

Suggestions are never persisted. The caller owns debounce scheduling and the cancellation
token. A new consumer must not cancel unrelated consumers globally. Providers compare their
country, language and account/context generations before delivery and reject stale results.
The `ordered` vector is authoritative: consumers may group the entries for display, but must
not reconstruct a different service order from separate derived arrays.
Full Best search (`with=topResults`) and the suggestions service
(`/search/suggestions`) remain distinct requests. The first returns the ranked heterogeneous
Best dataset; the second returns completion terms plus typed top suggestions.

## Loading and cache lifecycle

`ListSession` publishes a persisted prefix before HTTP refresh. While
`SessionRestoring() && !AccountVerified()`, it may read and publish cached rows but does not
start list HTTP, continuation or enrichment requests. Account verification is performed by
the provider's account flow. `NotifyAccountObserved()` resumes active reads with their pending
page demand intact.

`ListCacheInvalidation::RetireInMemory` retires publishers and generation ownership while
leaving the account scope, context prefix and provider-list guard readable.
`PurgePersistent` is reserved for explicit data mutations and clear operations and rotates the
provider-local guard. Manifest publication checks ownership both when obtaining the Store
write ticket and when committing, so a retired writer cannot supersede or invalidate a newer
publication.
