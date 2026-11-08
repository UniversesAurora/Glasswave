#pragma once

#include "app/setting_fields.h"
#include "stream/model.h"

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

class Provider {
public:
    // Optional features; the UI hides what a provider does not offer.
    struct Features {
        bool favorites = false;         // like / unlike tracks
        bool followArtists = false;
        bool saveAlbums = false;        // collect albums into the library
        bool savePlaylists = false;     // collect other users' playlists
        bool editPlaylists = false;     // create, rename, delete, add and remove tracks
        bool reorderPlaylists = false;  // the library's playlist order can be changed
        bool recommended = false;       // a daily set of recommended playlists
        bool artistTopTracks = false;   // "Hot songs" on artist pages
    };

    // Changes the provider reports by itself (sign-in, a write finished, a background
    // refresh). None of them requires the open page to be re-read.
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void OnAccountChanged(Provider&) {}
        // Names, covers, counts or the library lists changed.
        virtual void OnLibraryChanged(Provider&) {}
        // Only the favorite flags changed.
        virtual void OnFavoritesChanged(Provider&) {}
    };

    using Done = std::function<void(std::wstring error)>;  // empty error = success
    using TracksDone = std::function<void(std::vector<Track> tracks, std::wstring error)>;
    using TrackPageDone = std::function<void(std::vector<Track> tracks, bool more, std::wstring error)>;
    using PlaylistsDone = std::function<void(std::vector<Playlist> playlists, std::wstring error)>;
    using PlaylistDone = std::function<void(Playlist playlist, std::vector<Track> tracks, std::wstring error)>;
    using AlbumDone = std::function<void(Album album, std::vector<Track> tracks, std::wstring error)>;
    // Some providers only learn whether the user follows the artist from its album list.
    using AlbumPageDone = std::function<void(std::vector<Album> albums, bool more,
                                             std::optional<bool> artistFollowed, std::wstring error)>;
    using ArtistDone = std::function<void(Artist artist, std::wstring error)>;
    using ArtistsDone = std::function<void(std::vector<Artist> artists, std::wstring error)>;
    using TextDone = std::function<void(std::wstring text, std::wstring error)>;
    using CreatedDone = std::function<void(Id playlist, std::wstring error)>;

    virtual ~Provider() = default;

    // Starts the provider's session (main thread, idempotent). Called before the UI reads
    // anything from it and at foobar2000 start-up, whichever comes first.
    virtual void Start() = 0;

    // Stable short key used in saved views, cache keys and play history ("ncm").
    virtual std::wstring_view Key() const = 0;
    // Display name, untranslated (the UI passes it through Tr).
    virtual std::wstring Name() const = 0;
    virtual Features Offers() const = 0;
    // The provider's own rows on the preferences page (quality, caches, account, ...).
    virtual std::vector<settings::Field> SettingFields() { return {}; }

    // ---- account ---------------------------------------------------------------------
    virtual bool SignedIn() const = 0;
    virtual std::wstring AccountName() const = 0;
    virtual void SignIn(HWND owner) = 0;  // the provider's own sign-in dialog
    virtual void SignOut() = 0;
    // Re-reads the account's library lists (followed artists, saved albums, playlists).
    virtual void RefreshLibrary(Done done = {}) = 0;

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
    void NotifyAccountChanged();
    void NotifyLibraryChanged();
    void NotifyFavoritesChanged();

private:
    std::vector<Listener*> m_listeners;
};

// The providers compiled into the component, in navigation order. Providers register
// themselves while the component loads; Providers() is safe from then on (main thread).
const std::vector<Provider*>& Providers();
void StartProviders();  // Start() on every provider
void Register(Provider& provider);
void Unregister(Provider& provider);
Provider* FindProvider(std::wstring_view key);
// The provider an item belongs to, or nullptr for local files.
Provider* ProviderOf(const metadb_handle_ptr& item);

}  // namespace glasswave::stream
