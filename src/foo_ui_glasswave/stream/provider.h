#pragma once

#include "app/setting_fields.h"
#include "stream/model.h"
#include "stream/list.h"

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// A streaming music provider behind a library section of the navigation pane (ADR 0009).
// The UI works only with this interface and the records in stream/model.h; each provider
// maps its own service to it. Playback stays in foobar2000: a provider's tracks are
// metadb items handled by its own input, so playlists, the queue and the play history
// mix them freely with local files.
//
// Threading: every method is called on foobar2000's main thread, and every callback is
// delivered there. A callback can arrive after the page that asked for it is gone; the
// caller decides whether the result still matters (see ui::PageToken). Providers may call
// their own thread-safe helpers (a Catalog) from worker threads.
//
// Completion: a `done` handed to any method runs exactly once, on the main thread, with an
// empty error on success. A provider that is shut down, signed out or switched to another
// account while a call is in flight, or that was never started, completes it with an error
// instead of dropping it; GuardDone below is the tool for that. The one exception is a call
// that carries a cancellation token (ListQuery::cancelled,
// CatalogSearchSuggestionQuery::cancelled): once the token is set, `done` may be dropped
// without running. What is not covered: a callback queued to the main thread when
// foobar2000 quits never runs.
namespace glasswave::stream {

// One thing a source can do. A source declares exactly the abilities its service has and
// the UI offers nothing else, so sources differ without the UI knowing which one it talks
// to. Reads and writes are separate (a service may list favorites without a way to change
// them, or append to a playlist without a way to remove from it). A new ability is one more
// enumerator; nothing is implied by another.
enum class Capability {
    // The signed-in user's library.
    ReadFavorites,
    AddFavorite,
    RemoveFavorite,
    LibraryArtists,  // the library lists artists
    FollowArtists,   // the artist relation is "follow"
    FavoriteArtists, // the artist relation is "favorite"
    LibraryAlbums,
    SaveAlbums, // add albums to and remove them from the library
    SaveSongs,  // add single songs to and remove them from the library
    LibraryPlaylists,
    SavePlaylists, // add other users' playlists to and remove them from the library
    // The user's own playlists.
    CreatePlaylists,
    AppendPlaylistTracks,
    RemovePlaylistTracks,
    RenamePlaylists,
    DeletePlaylists,
    ReorderPlaylists, // the library's playlist order can be changed
    // The catalog.
    RecommendedPlaylists, // a set of playlists recommended to the user
    CatalogSearch,
    SearchSuggestions, // completions and top results while typing
    NativeLyrics,      // the service has its own lyrics for its tracks
    SongDetails,       // FetchSongDetails: a song's details, credits or similar songs
    TrackPopularity,   // Track::popularity is a 0-100 score worth a column
    SimilarArtists,    // ListKind::ArtistSimilarArtists (the default artist sections offer it)
    Download,          // ResolveDownload: a song's file can be saved (download::Service)
    Home,              // FetchHome: the service's home page (featured cards, recommended rows)
    Stations,          // FetchStationTracks: stations, endless streams of songs (radio mode)
    Count
};

class Capabilities {
  public:
    constexpr Capabilities() = default;
    constexpr Capabilities(std::initializer_list<Capability> list) {
        for (const Capability capability : list)
            m_bits |= Bit(capability);
    }
    constexpr bool Has(Capability capability) const { return (m_bits & Bit(capability)) != 0; }

  private:
    static constexpr uint64_t Bit(Capability capability) {
        return uint64_t{1} << static_cast<unsigned>(capability);
    }
    uint64_t m_bits = 0;
};

// What a downloaded file is tagged with: the song as the service describes it. Empty fields
// and zero numbers are left out of the file.
struct DownloadTags {
    std::wstring title, album, albumArtist;
    std::vector<std::wstring> artists; // in credit order, one ARTIST value each
    int disc = 0, number = 0;
    std::wstring date;  // "YYYY-MM-DD" or "YYYY"
    std::vector<std::wstring> genres;
    std::wstring coverUrl; // the front cover at the largest size the service offers
};

// Where a song's file is: an HTTP(S) address and the request headers it needs. The server must
// answer range requests: the download engine fetches parts of the file over several
// connections and resumes it later.
struct DownloadLink {
    std::wstring url;
    std::vector<std::pair<std::wstring, std::wstring>> headers; // name, value
};

// One song's file as the service delivers it (Capability::Download). The download service
// names, fetches, writes and tags the file; the source only says where the bytes are.
struct DownloadSource {
    std::string extension; // the file's, without the dot ("flac", "mp3", "m4a")
    std::wstring format;   // shown in the downloads list ("FLAC", "MP3 320 kbps"), untranslated
    DownloadTags tags;
    // The file's address; any worker thread, blocking. renew: the last address stopped working
    // (links expire), so the source asks its service again, and throws when the service now
    // gives a different file, which would not continue the same download.
    std::function<DownloadLink(bool renew, abort_callback& abort)> link;
};

class Provider {
  public:

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
    using ArtistDone = std::function<void(Artist artist, std::wstring error)>;
    using TextDone = std::function<void(std::wstring text, std::wstring error)>;
    using SongDetailsDone = std::function<void(SongDetails details, std::wstring error)>;
    using CreatedDone = std::function<void(Id playlist, std::wstring error)>;
    using HomeDone = std::function<void(Home home, std::wstring error)>;
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

    // Stable short key used in saved views, cache keys and play history.
    virtual std::wstring_view Key() const = 0;
    // Display name, untranslated (the UI passes it through Tr).
    virtual std::wstring Name() const = 0;
    // ---- wording -----------------------------------------------------------------------
    // Services name the same concepts differently (a "playlist" is 歌单 on one and 播放列表 on
    // another; "collect" is "add to library" elsewhere). The UI writes every source-facing
    // string once, in the common English wording, and shows Text(): the source's own English
    // term for it (Wording) translated in the source's context (TextContext), so a language
    // file can carry "<context> | <english>" entries per source and falls back to the common
    // translation. A new term needs no interface change, and a new source only overrides the
    // words that differ.
    std::wstring Text(std::wstring_view english) const;
    // The source's own English term for a common one. Default: the common term.
    virtual std::wstring Wording(std::wstring_view english) const { return std::wstring(english); }
    // Translation context of this source's terms. Empty: only the common translations.
    virtual std::wstring_view TextContext() const { return {}; }
    // The service's own desktop app as the Start menu names it (its display names in any
    // language): its icon marks this source's covers in results mixed from several sources.
    virtual std::vector<std::wstring> AppNames() const { return {}; }
    // Untranslated label for an artist's track-list tile and page.
    virtual std::wstring ArtistTracksLabel(const Id& artist) const;
    // Read-only, network-free descriptors for independently loadable artist views. Labels
    // follow the normal untranslated-key contract and are passed through Tr by display code.
    virtual std::vector<ArtistSection> ArtistSections(const Id& artist) const;
    // Typed catalog-search categories available from this provider. The default is the song
    // search of a source with Capability::CatalogSearch; a source may advertise more kinds.
    virtual std::vector<ListKind> CatalogSearchKinds() const;
    // Everything this source can do (see Capability). Constant for a source's lifetime.
    virtual Capabilities Offers() const = 0;
    bool Can(Capability capability) const { return Offers().Has(capability); }
    bool CanSetFavorite(bool favorite) const {
        return Can(favorite ? Capability::AddFavorite : Capability::RemoveFavorite);
    }
    // Whether reading this list needs a signed-in account (the lists of the user's library, not
    // the public catalog). ListSession fails such a read with SignInRequiredMessage() while the
    // source is signed out, so a source does not check it in FetchListPage. Default: no list does.
    virtual bool NeedsAccount(const ListQuery& query) const {
        (void)query;
        return false;
    }
    // "Please sign in to <Name()>." in the UI language.
    std::wstring SignInRequiredMessage() const;
    // ---- library state ---------------------------------------------------------------
    // A change of the library (SetFollowed, SetAlbumSaved, SetPlaylistSaved, SetSongsSaved)
    // reads as its new state from the click on, until the service has answered and the
    // provider's next library read has confirmed it; a refused change reads as the source's
    // state again. These ask the pending changes first and the source (the Read* hooks) next.
    //
    // Whether the album is in the user's library. Asked per row.
    bool IsAlbumSaved(const Id& album) const;
    // Capability::SaveSongs: true when every one of `tracks` is in the library, false when one
    // is known not to be, nothing while that is not known yet.
    std::optional<bool> SongsSavedState(const std::vector<Id>& tracks) const;
    // The library state of a playlist / an artist while it differs from the last snapshot (a
    // change in flight or just acknowledged); nullopt: the snapshot is right.
    std::optional<bool> PlaylistSavedState(const Id& playlist) const;
    std::optional<bool> ArtistPreferenceState(const Id& artist) const;
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
    // The cover one of this provider's tracks shows: its album's when the album is known, so
    // every track of an album shares one picture that is downloaded and decoded once, else
    // the track's own. The provider's art extractor stores the picture under the same
    // identity. Empty when the item is not this provider's.
    std::wstring ArtKeyOf(const metadb_handle_ptr& item) const;
    // The album of a track for ArtKeyOf. The art extractor must answer the same in every run,
    // so a provider overrides this with whatever its extractor consults.
    virtual Id AlbumOfTrack(const Id& track) const;
    // Prefix of every ArtKey of one kind, to drop all of them after a refresh.
    std::wstring ArtKeyPrefix(ArtKind kind) const;
    // True when two cover URLs show the same image (e.g. only a size parameter differs).
    virtual bool SameCover(std::wstring_view a, std::wstring_view b) const { return a == b; }

    // ---- lyrics ------------------------------------------------------------------------
    // The provider's own lyrics for one of its tracks as LRC: enhanced LRC (<mm:ss.xx>) for
    // word timing, translations as reference lines with the line's timestamp. Blocking, on a
    // worker thread (lyrics::Service). Empty when the provider has none.
    // Asked only of a source with Capability::NativeLyrics.
    virtual std::wstring Lyrics(const Id& /*track*/, abort_callback& /*abort*/) { return {}; }

    // ---- downloads (Capability::Download) ---------------------------------------------
    // Where one of its songs' file comes from, at the source's download quality. Blocking, on
    // a download worker thread; throws (std::exception, a message for the user) when the song
    // cannot be downloaded: not signed in, no rights, only a preview.
    virtual DownloadSource ResolveDownload(const Id& track, abort_callback& abort);

    // ---- library snapshot (last fetched; cheap) ----------------------------------------
    // A song being favorited or unfavorited reads as its new state from the click on, until
    // the service has answered; a refused change reads as the service's state again.
    bool IsFavorite(const Id& track) const;
    virtual std::vector<Playlist> Playlists(Shelf shelf) const = 0;
    virtual std::optional<Shelf> ShelfOf(const Id& playlist) const = 0;
    virtual std::vector<Artist> FollowedArtists() const = 0;
    virtual std::vector<Album> SavedAlbums() const = 0;
    virtual std::optional<Playlist> FindPlaylist(const Id& playlist) const = 0;
    virtual std::optional<Album> FindAlbum(const Id& album) const = 0;

    // ---- reads ----------------------------------------------------------------------------
    // Every list (tracks, playlists, albums, artists, groups, search results) is read through
    // FetchListPage; the records below are single objects.
    virtual void FetchArtist(const Id& artist, ArtistDone done) = 0;
    virtual void FetchArtistBio(const Id& artist, TextDone done) = 0;
    // Everything the service knows of one song (Capability::SongDetails); main thread.
    virtual void FetchSongDetails(const Id& /*track*/, SongDetailsDone done) {
        if (done) done({}, L"This source has no song details.");
    }
    // Cached provider-defined navigation. This method must not trigger network access.
    virtual std::vector<NavigationSection> Sections() const;
    virtual std::vector<Playlist> SectionPlaylists(const Id& section) const;
    virtual std::optional<NavigationSection> SectionOfPlaylist(const Id& playlist) const;
    // The service's home page for the signed-in user (Capability::Home); main thread. Every
    // record it names (songs, albums, playlists, artists) is known to the source afterwards, so
    // covers and pages work as for search results.
    virtual void FetchHome(HomeDone done) {
        if (done) done({}, L"This source has no home page.");
    }

    // ---- stations (Capability::Stations) ------------------------------------------------
    // A station's next songs, a few at a time; the service decides as the songs play, so each
    // call can answer differently and the end is never known. Main thread. The UI plays them in
    // radio mode (stream/radio.h): asked again as the last ones start, no shuffle, no list repeat.
    virtual void FetchStationTracks(const Id& station, TracksDone done) {
        (void)station;
        if (done) done({}, L"This source has no stations.");
    }
    // Whether a station takes "recommend less" for its songs (a "not interested" feedback).
    virtual bool CanDislike(const Id& station) const {
        (void)station;
        return false;
    }
    // Recommend less like this song on this station (the service learns from it).
    virtual void DislikeStationTrack(const Id& station, const Id& track, Done done) {
        (void)station, (void)track;
        if (done) done(L"This station does not take feedback.");
    }
    // Separate from paged search and ListSession caching. The caller owns debounce scheduling
    // and the query's cancellation token; providers must reject stale context generations.
    virtual void FetchCatalogSearchSuggestions(CatalogSearchSuggestionQuery query,
                                               CatalogSearchSuggestionsDone done) {
        (void)query;
        if (done) done({}, L"Catalog search suggestions are unavailable for this provider.");
    }

    // The one read entry for every list (see ListKind): returns exactly one page of the
    // query, starting at `cursor` (the provider owns its encoding) and at most about `limit`
    // rows. A service with no paging answers the whole list as one final page. A kind the
    // source does not have completes with an error.
    virtual void FetchListPage(const ListQuery& query, const ListCursor& cursor, size_t limit,
                               ListPageDone done) = 0;
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
    // Stations a page shows (a home row), so their covers resolve (ArtKind::Station).
    virtual void PutStations(const std::vector<Station>& stations) { (void)stations; }

    // ---- writes (the snapshot is updated before `done` runs) ------------------------
    // Shows the new state at once (listeners hear OnFavoritesChanged) and completes in the
    // background whatever the UI does meanwhile; on failure the old state comes back.
    void SetFavorite(const Id& track, bool favorite, Done done = {});
    // The same for the other library changes: the new state shows from the click on (listeners
    // hear OnLibraryChanged, and again when a refused change is taken back), the source's
    // Write* hook does the work.
    void SetFollowed(const Id& artist, bool follow, Done done = {});
    void SetAlbumSaved(const Id& album, bool saved, Done done = {});
    // Capability::SaveSongs: the songs themselves, not their albums.
    void SetSongsSaved(std::vector<Id> tracks, bool saved, Done done = {});
    void SetPlaylistSaved(const Id& playlist, bool saved, Done done = {});
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

    // The service's own favorite state and its write; `done` may run on any thread.
    virtual bool ReadFavorite(const Id& track) const = 0;
    virtual void WriteFavorite(const Id& track, bool favorite, Done done) = 0;

    // The source's own answers for the library state queries, asked when no change of the
    // user's is pending. Read* answer from the snapshot, never from the network.
    // Default: a scan of SavedAlbums(), which suits a small library (a source with a large one
    // answers with a lookup).
    virtual bool ReadAlbumSaved(const Id& album) const;
    virtual std::optional<bool> ReadSongsSavedState(const std::vector<Id>& tracks) const;
    virtual std::optional<bool> ReadPlaylistSavedState(const Id& playlist) const;
    virtual std::optional<bool> ReadArtistPreferenceState(const Id& artist) const;
    // The service's writes behind SetFollowed, SetAlbumSaved, SetPlaylistSaved and SetSongsSaved;
    // `done` may run on any thread, once. A completion that is dropped unrun (the source shut
    // down, the account changed) must still run with an error (GuardDone), so a change it was
    // carrying is taken back. A source that has updated its own snapshot by the time it
    // answers calls ConfirmLibraryEdits after that; one that refreshes later does it then.
    virtual void WriteFollowed(const Id& artist, bool follow, Done done) = 0;
    virtual void WriteAlbumSaved(const Id& album, bool saved, Done done) = 0;
    // Capability::SaveSongs. Default: an error.
    virtual void WriteSongsSaved(std::vector<Id> tracks, bool saved, Done done);
    virtual void WritePlaylistSaved(const Id& playlist, bool saved, Done done) = 0;

    // A change the service has acknowledged keeps showing as the user made it until the source's
    // own state shows it too. A source takes the mark before it reads its library and calls
    // Confirm after a successful read: the changes acknowledged before the mark are in what it
    // read, so they stop overriding it. (A change that is never confirmed stops overriding after
    // a while, as a favorite does.)
    uint64_t LibraryEditMark() const;
    void ConfirmLibraryEdits(uint64_t mark);

  private:
    struct PendingFavorite {
        bool favorite = false;
        uint64_t serial = 0;
        std::chrono::steady_clock::time_point since;
    };
    void FinishFavorite(const Id& track, uint64_t serial, std::wstring error, Done done);

    // A library change not confirmed by the source yet: what the user asked for, and where it
    // stands. Per kind and id; the latest click on an id decides what it shows.
    enum class EditKind { Follow, Album, Playlist, Song };
    struct PendingEdit {
        bool value = false;
        uint64_t serial = 0;
        uint64_t ackSerial = 0; // 0: the service has not answered yet
        std::chrono::steady_clock::time_point since;
    };
    using EditKey = std::pair<EditKind, Id>;
    std::optional<bool> PendingEditState(EditKind kind, const Id& id) const;
    void SetEdit(EditKind kind, std::vector<Id> ids, bool value, Done done,
                 std::function<void(Done)> write);
    void FinishEdit(EditKind kind, const std::vector<Id>& ids, uint64_t serial, std::wstring error,
                    Done done);

    std::vector<Listener*> m_listeners;
    mutable std::mutex m_favoritesMutex;
    mutable std::map<Id, PendingFavorite> m_pendingFavorites;
    uint64_t m_favoriteSerial = 0;
    mutable std::mutex m_editsMutex;
    mutable std::map<EditKey, PendingEdit> m_pendingEdits;
    uint64_t m_editSerial = 0;
};

namespace detail {

template <class Fn>
struct DoneArgs;

// Calls a completion with default-constructed results and `error` in the last place.
template <class... Args>
struct DoneArgs<std::function<void(Args...)>> {
    using Fn = std::function<void(Args...)>;
    static void Fail(Fn& fn, std::wstring error) {
        Fail(fn, std::move(error), std::make_index_sequence<sizeof...(Args) - 1>{});
    }

  private:
    template <size_t... I>
    static void Fail(Fn& fn, std::wstring error, std::index_sequence<I...>) {
        using Tuple = std::tuple<Args...>;
        fn(std::tuple_element_t<I, Tuple>{}..., std::move(error));
    }
};

template <class Fn>
struct GuardState {
    Fn fn;
    std::wstring error;
    std::shared_ptr<std::atomic_bool> cancelled;

    ~GuardState() {
        if (!fn || (cancelled && cancelled->load(std::memory_order_acquire))) return;
        try {
            fb2k::inMainThread([fn = std::move(fn), error = std::move(error)]() mutable {
                DoneArgs<Fn>::Fail(fn, std::move(error));
            });
        } catch (...) {
        }
    }
};

} // namespace detail

// Wraps a completion (any Provider::*Done type: the last parameter is the error text) so it
// runs exactly once. The wrapper is copyable and shares one state: the first call runs the
// original, and when the last copy is destroyed without a call (a queued task was dropped at
// shutdown or on an account change) the original receives `error` on the main thread, with
// default-constructed results. A set `cancelled` token waives that last step.
template <class Fn>
Fn GuardDone(Fn done, std::wstring error, std::shared_ptr<std::atomic_bool> cancelled = {}) {
    if (!done) return done;
    auto state = std::make_shared<detail::GuardState<Fn>>();
    state->fn = std::move(done);
    state->error = std::move(error);
    state->cancelled = std::move(cancelled);
    return [state](auto&&... args) {
        Fn fn = std::move(state->fn);
        state->fn = nullptr;
        if (fn) fn(std::forward<decltype(args)>(args)...);
    };
}

// The providers compiled into the component, in navigation order. Providers register
// themselves while the component loads; Providers() is safe from then on (main thread).
const std::vector<Provider*>& Providers();
void StartProviders(); // Start() on every provider
void Register(Provider& provider);
void Unregister(Provider& provider);
Provider* FindProvider(std::wstring_view key);
// The provider an item belongs to, or nullptr for local files.
Provider* ProviderOf(const metadb_handle_ptr& item);
// The tags a source gives its song (Track::badges), for any item; none for a local file.
std::vector<std::wstring> TrackBadges(const metadb_handle_ptr& item);

} // namespace glasswave::stream
