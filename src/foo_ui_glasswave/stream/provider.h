#pragma once

#include "app/setting_fields.h"
#include "stream/model.h"
#include "stream/list.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A streaming music provider behind a library section of the navigation pane (ADR 0009).
// The UI works only with this interface and the records in stream/model.h; each provider
// maps its own service to it. Playback stays in foobar2000: a provider's tracks are
// metadb items handled by its own input, so playlists, the queue and the play history
// mix them freely with local files.
//
// Threading: every method is called on foobar2000's main thread, and every callback is
// delivered there. A callback can arrive after the page that asked for it is gone; the
// caller decides whether the result still matters (see ui::PageToken).
namespace glasswave::stream {

enum class LibraryAction {
    Add,
    Remove
};
enum class ArtistPreferenceAction {
    Favorite,
    Unfavorite
};

class Provider {
  public:
    // Optional features; the UI hides what a provider does not offer.
    struct Features {
        bool favorites = false; // legacy full add/remove favorite capability
        bool followArtists = false;
        bool favoriteArtists = false;
        bool saveAlbums = false;       // collect albums into the library
        bool savePlaylists = false;    // collect other users' playlists
        bool editPlaylists = false;    // create, rename, delete, add and remove tracks
        bool reorderPlaylists = false; // the library's playlist order can be changed
        bool recommended = false;      // a daily set of recommended playlists
        bool artistTopTracks = false;  // artist pages expose a track-list tile/page
        // Read access and individual writes can differ (Apple permits playlist append,
        // for example, without a documented removal endpoint).
        bool libraryFavorites = false; // favorites can be read even when toggling is unsupported
        bool addFavorites = false;
        bool removeFavorites = false;
        bool libraryArtists = false;
        bool libraryAlbums = false;
        bool libraryPlaylists = false;
        bool createPlaylists = false;
        bool appendPlaylistTracks = false;
        bool removePlaylistTracks = false;
        bool renamePlaylists = false;
        bool deletePlaylists = false;
        bool catalogSearch = false;

        bool CanCreatePlaylist() const { return editPlaylists || createPlaylists; }
        bool CanSetFavorite(bool favorite) const {
            return favorites || (favorite ? addFavorites : removeFavorites);
        }
        bool CanAppendPlaylist() const { return editPlaylists || appendPlaylistTracks; }
        bool CanRemovePlaylistTracks() const { return editPlaylists || removePlaylistTracks; }
        bool CanRenamePlaylist() const { return editPlaylists || renamePlaylists; }
        bool CanDeletePlaylist() const { return editPlaylists || deletePlaylists; }
    };

    // Changes the provider reports by itself (sign-in, a write finished, a background
    // refresh). None of them requires the open page to be re-read.
    class Listener {
      public:
        virtual ~Listener() = default;
        // The same account was observed after a restore or bootstrap. Visible data may be
        // reused while verification proceeds.
        virtual void OnAccountObserved(Provider&) {}
        // The account identity changed, was restored to a different identity, or signed out.
        virtual void OnAccountChanged(Provider&) {}
        // Names, covers, counts or the library lists changed.
        virtual void OnLibraryChanged(Provider&) {}
        // Only the favorite flags changed.
        virtual void OnFavoritesChanged(Provider&) {}
        // Cached provider labels/snapshots were refreshed for a new UI language.
        virtual void OnLanguageChanged(Provider&) {}
        // Presentation metadata changed without changing provider/account/list identity.
        // Consumers should re-project the visible page while retaining canonical records.
        virtual void OnDisplayChanged(Provider&) {}
    };

    using Done = std::function<void(std::wstring error)>; // empty error = success
    using TracksDone = std::function<void(std::vector<Track> tracks, std::wstring error)>;
    using GroupsDone = std::function<void(std::vector<BrowseGroup> groups, std::wstring error)>;
    using TrackPageDone = std::function<void(std::vector<Track> tracks, bool more, std::wstring error)>;
    using PlaylistsDone = std::function<void(std::vector<Playlist> playlists, std::wstring error)>;
    using PlaylistDone =
        std::function<void(Playlist playlist, std::vector<Track> tracks, std::wstring error)>;
    using AlbumDone = std::function<void(Album album, std::vector<Track> tracks, std::wstring error)>;
    // Some providers only learn whether the user follows the artist from its album list.
    using AlbumPageDone = std::function<void(std::vector<Album> albums, bool more,
                                             std::optional<bool> artistFollowed, std::wstring error)>;
    using ArtistDone = std::function<void(Artist artist, std::wstring error)>;
    using ArtistsDone = std::function<void(std::vector<Artist> artists, std::wstring error)>;
    using TextDone = std::function<void(std::wstring text, std::wstring error)>;
    using CreatedDone = std::function<void(Id playlist, std::wstring error)>;
    struct CatalogSearchSuggestionQuery {
        std::wstring term;
        std::shared_ptr<std::atomic_bool> cancelled;
    };
    struct CatalogSearchSuggestionTerm {
        std::wstring searchTerm;
        std::wstring displayTerm;
    };
    using CatalogSearchSuggestionItem = std::variant<CatalogSearchSuggestionTerm, CatalogSearchItem>;
    struct CatalogSearchSuggestions {
        // The original interleaving of completion terms and typed top results.
        std::vector<CatalogSearchSuggestionItem> ordered;
    };
    using CatalogSearchSuggestionsDone =
        std::function<void(CatalogSearchSuggestions suggestions, std::wstring error)>;

    virtual ~Provider() = default;

    // Starts the provider's session (main thread, idempotent). Called before the UI reads
    // anything from it and at foobar2000 start-up, whichever comes first.
    virtual void Start() = 0;

    // Stable short key used in saved views, cache keys and play history ("ncm").
    virtual std::wstring_view Key() const = 0;
    // Display name, untranslated (the UI passes it through Tr).
    virtual std::wstring Name() const = 0;
    // Provider-scoped wording for playlist controls and empty states. The default is the
    // common translation key; a provider may map it to wording that fits its service.
    virtual std::wstring PlaylistText(std::wstring_view english) const;
    // Untranslated label for an artist's track-list tile and page.
    virtual std::wstring ArtistTracksLabel(const Id& artist) const;
    // Read-only, network-free descriptors for independently loadable artist views. Labels
    // follow the normal untranslated-key contract and are passed through Tr by display code.
    virtual std::vector<ArtistSection> ArtistSections(const Id& artist) const;
    // Typed catalog-search categories available from this provider. The default preserves
    // the legacy song-only catalogSearch capability; Apple may advertise all five kinds.
    virtual std::vector<ListKind> CatalogSearchKinds() const;
    virtual bool SupportsCatalogSearchSuggestions() const { return false; }
    virtual Features Offers() const = 0;
    virtual std::wstring LibraryActionText(LibraryAction action) const;
    virtual std::wstring LibraryStateText(bool saved) const;
    virtual std::wstring ArtistPreferenceActionText(ArtistPreferenceAction action) const;
    virtual bool IsAlbumSaved(const Id& album) const;
    virtual std::optional<bool> PlaylistSavedState(const Id& playlist) const;
    virtual std::optional<bool> ArtistPreferenceState(const Id& artist) const;
    // The provider's own rows on the preferences page (quality, caches, account, ...).
    virtual std::vector<settings::Field> SettingFields() { return {}; }

    // ---- account ---------------------------------------------------------------------
    virtual bool SignedIn() const = 0;
    virtual std::wstring AccountName() const = 0;
    // A persisted session snapshot may make cached navigation visible before network
    // verification. These capabilities do not grant write or authenticated-read access.
    virtual bool SessionRestoring() const { return false; }
    virtual bool AccountVerified() const { return SignedIn(); }
    // An explicit foreground read may observe a failed session restore without discarding
    // cached rows. Providers without an asynchronous restore path keep both defaults inert.
    virtual std::wstring SessionRestoreError() const { return {}; }
    // Joins the current restore attempt or starts one when none is pending. The callback runs on
    // the main thread. Repeated calls while an attempt is active must share that attempt.
    virtual bool RetrySessionRestore(Done done = {}) {
        (void)done;
        return false;
    }
    virtual void SignIn(HWND owner) = 0; // the provider's own sign-in dialog
    virtual void SignOut() = 0;
    // Re-reads the account's library lists (followed artists, saved albums, playlists).
    virtual void RefreshLibrary(Done done = {}) = 0;
    // Called by the UI only after an effective language transition. Providers may override
    // this to update cached labels without network access; the default re-reads the snapshot.
    virtual void OnLanguageChanged();

    // ---- tracks as foobar2000 items ---------------------------------------------------
    // Empty when the item does not belong to this provider.
    virtual Id TrackId(const metadb_handle_ptr& item) const = 0;
    bool Owns(const metadb_handle_ptr& item) const { return !TrackId(item).empty(); }
    virtual metadb_handle_ptr TrackItem(const Id& track) const = 0;
    virtual std::optional<Track> FindTrack(const Id& track) const = 0;
    // An item whose album art is the cover of the given object (fed to ArtCache).
    virtual metadb_handle_ptr ArtItem(ArtKind kind, const Id& id) const = 0;
    // Cache key shared by every view of the same cover: "<key>:<kind>:<id>".
    std::wstring ArtKey(ArtKind kind, const Id& id) const;
    // Prefix of every ArtKey of one kind, to drop all of them after a refresh.
    std::wstring ArtKeyPrefix(ArtKind kind) const;
    // True when two cover URLs show the same image (e.g. only a size parameter differs).
    virtual bool SameCover(std::wstring_view a, std::wstring_view b) const { return a == b; }

    // ---- lyrics ------------------------------------------------------------------------
    // The provider's own lyrics for one of its tracks as LRC: enhanced LRC (<mm:ss.xx>) for
    // word timing, translations as reference lines with the line's timestamp. Blocking, on a
    // worker thread (lyrics::Service). Empty when the provider has none.
    virtual std::wstring Lyrics(const Id& /*track*/, abort_callback& /*abort*/) { return {}; }
    virtual bool HasNativeLyrics() const { return false; }

    // ---- library snapshot (last fetched; cheap) ----------------------------------------
    virtual bool IsFavorite(const Id& track) const = 0;
    virtual std::vector<Playlist> Playlists(Shelf shelf) const = 0;
    virtual std::optional<Shelf> ShelfOf(const Id& playlist) const = 0;
    virtual std::vector<Artist> FollowedArtists() const = 0;
    virtual std::vector<Album> SavedAlbums() const = 0;
    virtual std::optional<Playlist> FindPlaylist(const Id& playlist) const = 0;
    virtual std::optional<Album> FindAlbum(const Id& album) const = 0;
    // The last track lists a page showed, so it can appear at once while it revalidates.
    // nullopt: never loaded.
    virtual std::optional<std::vector<Track>> CachedFavorites() const = 0;
    virtual std::optional<std::vector<Track>> CachedPlaylist(const Id& playlist) const = 0;
    virtual std::optional<std::vector<Track>> CachedAlbum(const Id& album) const = 0;

    // ---- reads ----------------------------------------------------------------------------
    virtual void FetchFavorites(TracksDone done) = 0;
    virtual void FetchPlaylists(Shelf shelf, PlaylistsDone done) = 0;
    virtual void FetchPlaylist(const Id& playlist, PlaylistDone done) = 0;
    virtual void FetchAlbum(const Id& album, AlbumDone done) = 0;
    virtual void FetchAlbumsTracks(std::vector<Id> albums, TracksDone done) = 0;
    virtual void FetchArtist(const Id& artist, ArtistDone done) = 0;
    virtual void FetchArtistBio(const Id& artist, TextDone done) = 0;
    virtual void FetchArtistAlbums(const Id& artist, size_t offset, size_t limit, AlbumPageDone done) = 0;
    virtual void FetchArtistTopTracks(const Id& artist, size_t offset, size_t limit, TrackPageDone done) = 0;
    virtual void FetchFollowedArtists(ArtistsDone done) = 0;
    virtual void FetchSavedAlbums(size_t offset, size_t limit, AlbumPageDone done) = 0;
    // Cached provider-defined navigation. This method must not trigger network access.
    virtual std::vector<NavigationSection> Sections() const;
    virtual std::vector<Playlist> SectionPlaylists(const Id& section) const;
    virtual void FetchSectionPlaylists(const Id& section, PlaylistsDone done);
    virtual std::optional<NavigationSection> SectionOfPlaylist(const Id& playlist) const;
    virtual void FetchGroups(const Id&, GroupsDone done) {
        if (done) done({}, L"Grouped browsing is unavailable for this provider.");
    }
    virtual void FetchSectionTracks(const Id&, const Id&, TracksDone done) {
        if (done) done({}, L"Section tracks are unavailable for this provider.");
    }
    virtual void SearchCatalog(std::wstring, TracksDone done) {
        if (done) done({}, L"Catalog search is unavailable for this provider.");
    }
    // Separate from paged search and ListSession caching. The caller owns debounce scheduling
    // and the query's cancellation token; providers must reject stale context generations.
    virtual void FetchCatalogSearchSuggestions(CatalogSearchSuggestionQuery query,
                                               CatalogSearchSuggestionsDone done) {
        (void)query;
        if (done) done({}, L"Catalog search suggestions are unavailable for this provider.");
    }

    // Generic paged reads. The default adapts legacy full-list methods as one final page;
    // providers with a paged service override this to return exactly one network page.
    virtual void FetchListPage(const ListQuery& query, const ListCursor& cursor, size_t limit,
                               ListPageDone done);
    // Optional second stage, called after usable rows have been published. Return true
    // when accepted and deliver once unless canceled. Row type/count/order/IDs must stay
    // identical; only metadata may change. False means no further metadata is needed.
    virtual bool EnrichListPage(const ListQuery&, const ListPage&, ListPageDone) { return false; }
    // Opaque context for data whose meaning depends on more than ListQuery, such as an account
    // or display locale. ListSession captures it when a read is opened and compares the current
    // value before Fetch and callback publication.
    virtual std::string ListCacheContext(const ListQuery& query) const {
        (void)query;
        return {};
    }
    virtual ListCachePolicy ListPolicy(const ListQuery& query) const;
    // Rehydrate metadata required by TrackItem/FindTrack and artwork on a disk cache hit.
    virtual void RestoreListRecords(const ListPage& page) { (void)page; }

    // ---- writes (the snapshot is updated before `done` runs) ------------------------
    virtual void SetFavorite(const Id& track, bool favorite, Done done = {}) = 0;
    virtual void SetFollowed(const Id& artist, bool follow, Done done = {}) = 0;
    virtual void SetAlbumSaved(const Id& album, bool saved, Done done = {}) = 0;
    virtual void SetPlaylistSaved(const Id& playlist, bool saved, Done done = {}) = 0;
    virtual void AddToPlaylist(const Id& playlist, std::vector<Id> tracks, Done done = {}) = 0;
    virtual void RemoveFromPlaylist(const Id& playlist, std::vector<Id> tracks, Done done = {}) = 0;
    virtual void CreatePlaylist(std::wstring name, CreatedDone done) = 0;
    virtual void RenamePlaylist(const Id& playlist, std::wstring name, Done done = {}) = 0;
    virtual void DeletePlaylist(const Id& playlist, Done done = {}) = 0;
    // The user's own and saved playlists in their new order (own first).
    virtual void ReorderPlaylists(std::vector<Id> playlists, Done done = {}) = 0;

    void AddListener(Listener* listener);
    void RemoveListener(Listener* listener);

  protected:
    void NotifyListChanged();
    void NotifyAccountObserved();
    void NotifyAccountChanged();
    void NotifyLibraryChanged();
    void NotifyFavoritesChanged();
    void NotifyLanguageChanged();
    void NotifyDisplayChanged();

  private:
    std::vector<Listener*> m_listeners;
};

// The providers compiled into the component, in navigation order. Providers register
// themselves while the component loads; Providers() is safe from then on (main thread).
const std::vector<Provider*>& Providers();
void StartProviders(); // Start() on every provider
void Register(Provider& provider);
void Unregister(Provider& provider);
Provider* FindProvider(std::wstring_view key);
// The provider an item belongs to, or nullptr for local files.
Provider* ProviderOf(const metadb_handle_ptr& item);

} // namespace glasswave::stream
