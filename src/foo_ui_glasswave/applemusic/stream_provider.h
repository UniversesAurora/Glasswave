#pragma once

#include "applemusic/catalog.h"
#include "applemusic/client.h"
#include "stream/account_gate.h"
#include "stream/executor.h"
#include "stream/provider.h"
#include "stream/snapshot_saver.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace glasswave::applemusic {

class StreamProvider final : public stream::Provider {
  public:
    static StreamProvider& Get();

    void Start() override;
    void Shutdown();

    std::wstring_view Key() const override { return L"apple"; }
    std::wstring Name() const override { return L"Apple Music"; }
    std::vector<std::wstring> AppNames() const override { return {L"Apple Music"}; }
    std::wstring Wording(std::wstring_view english) const override;
    std::wstring_view TextContext() const override { return L"Apple Music"; }
    std::wstring ArtistTracksLabel(const stream::Id& artist) const override;
    std::vector<stream::ArtistSection> ArtistSections(const stream::Id& artist) const override;
    std::vector<stream::ListKind> CatalogSearchKinds() const override;
    stream::Capabilities Offers() const override;
    bool ReadAlbumSaved(const stream::Id& album) const override;
    std::optional<bool> ReadSongsSavedState(const std::vector<stream::Id>& tracks) const override;
    std::optional<bool> ReadPlaylistSavedState(const stream::Id& playlist) const override;
    std::optional<bool> ReadArtistPreferenceState(const stream::Id& artist) const override;
    std::vector<settings::Field> SettingFields() override;

    bool SignedIn() const override;
    std::wstring AccountName() const override;
    bool SessionRestoring() const override;
    bool AccountVerified() const override;
    std::wstring SessionRestoreError() const override;
    bool RetrySessionRestore(Done done = {}) override;
    void SignIn(HWND owner) override;
    void SignOut() override;
    void RefreshLibrary(Done done = {}) override;

    stream::Id TrackId(const metadb_handle_ptr& item) const override;
    std::wstring Lyrics(const stream::Id& track, abort_callback& abort) override;
    stream::Catalog* Records() const override;
    std::optional<stream::Track> FindTrack(const stream::Id& track) const override;
    stream::Id AlbumOfTrack(const stream::Id& track) const override;
    // A song the catalog does not know (a queue from an earlier run, a list not opened in this
    // one) is read in a batch with the others asked for meanwhile and published to foobar2000's
    // metadata, so every view gets its name and album. A failed read is asked again later.
    // Any thread.
    void RequestUnknownSong(const stream::Id& track) const;
    bool SameCover(std::wstring_view a, std::wstring_view b) const override;

    bool ReadFavorite(const stream::Id& track) const override;
    std::vector<stream::Playlist> Playlists(stream::Shelf shelf) const override;
    std::optional<stream::Shelf> ShelfOf(const stream::Id& playlist) const override;
    std::vector<stream::Artist> FollowedArtists() const override;
    std::vector<stream::Album> SavedAlbums() const override;
    std::optional<stream::Playlist> FindPlaylist(const stream::Id& playlist) const override;
    std::optional<stream::Album> FindAlbum(const stream::Id& album) const override; // display overlay

    void FetchArtist(const stream::Id& artist, ArtistDone done) override;
    void FetchArtistBio(const stream::Id& artist, TextDone done) override;
    void FetchSongDetails(const stream::Id& track, SongDetailsDone done) override;
    void FetchHome(HomeDone done) override;
    void FetchStationTracks(const stream::Id& station, TracksDone done) override;
    void FetchListPage(const stream::ListQuery& query, const stream::ListCursor& cursor, size_t limit,
                       stream::ListPageDone done) override;
    bool EnrichListPage(const stream::ListQuery& query, const stream::ListPage& page,
                        stream::ListPageDone done) override;
    bool NeedsAccount(const stream::ListQuery& query) const override;
    std::string ListCacheContext(const stream::ListQuery& query) const override;
    void RestoreListRecords(const stream::ListPage& page) override;
    void PutStations(const std::vector<stream::Station>& stations) override;

    // These write routes mirror the private Apple Music current-web contract. They are not
    // a documented stable API and have not been exercised against a real account here.
    void WriteFavorite(const stream::Id& track, bool favorite, Done done) override;
    void WriteFollowed(const stream::Id& artist, bool follow, Done done) override;
    void WriteAlbumSaved(const stream::Id& album, bool saved, Done done) override;
    void WriteSongsSaved(std::vector<stream::Id> tracks, bool saved, Done done) override;
    void WritePlaylistSaved(const stream::Id& playlist, bool saved, Done done) override;
    void AddToPlaylist(const stream::Id& playlist, std::vector<stream::Id> tracks, Done done = {}) override;
    void RemoveFromPlaylist(const stream::Id& playlist, std::vector<stream::Id> tracks,
                            Done done = {}) override;
    void CreatePlaylist(std::wstring name, CreatedDone done) override;
    void RenamePlaylist(const stream::Id& playlist, std::wstring name, Done done = {}) override;
    void DeletePlaylist(const stream::Id& playlist, Done done = {}) override;
    void ReorderPlaylists(std::vector<stream::Id> playlists, Done done = {}) override;

    void FetchCatalogSearchSuggestions(CatalogSearchSuggestionQuery query,
                                       CatalogSearchSuggestionsDone done) override;
    std::vector<stream::NavigationSection> Sections() const override;

    void OnLanguageChanged() override;

  private:
    StreamProvider();
    ~StreamProvider() override;
    StreamProvider(const StreamProvider&) = delete;
    StreamProvider& operator=(const StreamProvider&) = delete;
    // Albums removed here leave a listed page; with `addSaved`, albums added here join it.
    void ApplyAlbumMutations(std::vector<stream::Album>& albums, bool addSaved) const;

    struct Snapshot {
        uint64_t editMark = 0; // stream::Provider::LibraryEditMark when the refresh began
        bool accountVerified = false;
        uint64_t libraryMutationEpoch = 0;
        uint64_t artistPreferenceEpoch = 0;
        uint64_t refreshToken = 0;
        AccountState account;
        DrmState drm;
        std::vector<stream::Playlist> createdPlaylists;
        std::vector<stream::Playlist> savedPlaylists;
        std::vector<stream::Album> savedAlbums;
        std::vector<stream::Artist> libraryArtists;
        std::unordered_map<stream::Id, std::vector<stream::Track>> playlistTracks;
        std::unordered_map<stream::Id, std::vector<Song>> playlistSongs;
        std::unordered_map<stream::Id, stream::Id> playlistLibraryIds;
        std::unordered_map<stream::Id, stream::Id> albumLibraryIds;
        std::unordered_map<stream::Id, stream::Id> artistCatalogIds;
        std::set<stream::Id> savedAlbumIds;
        std::set<stream::Id> editablePlaylists;
        std::set<stream::Id> renameablePlaylists;
        std::set<stream::Id> deletablePlaylists;
        stream::Id favoritePlaylistId;
        std::vector<Song> librarySongs;
        std::vector<Song> favoriteSongs;
        bool librarySongsLoaded = false;
        bool favoriteSongsLoaded = false;
    };

    // The worker queues of m_exec: reads, library writes (one at a time, in order), playlist
    // details and the second stage of page enrichment.
    enum Queue : size_t { kTasks, kMutations, kDetails, kEnrich };
    // Adds an album or a playlist to the library, or removes it (the two differ only in the
    // resource, the checks before a removal and the lists a change touches).
    void WriteLibraryItem(bool album, const stream::Id& id, bool saved, Done done);
    void RefreshAccountAsync(uint64_t generation, Done done, bool preservePlaylistDetails = false);
    void RefreshLibraryPreservingDetails(Done done = {});
    void RefreshLibraryAfterMutation(Done done = {});
    uint64_t ApplyVerifiedAccount(uint64_t generation, uint64_t refreshToken, const AccountState& account);
    void CompleteRefresh(uint64_t generation, Snapshot snapshot, std::wstring error);
    void LoadBootstrapSnapshot(uint64_t generation);
    // What the snapshot saver asks of this provider (m_snapshots).
    bool SnapshotEligible() const;
    bool SnapshotEligibleLocked() const;
    bool FillSnapshot(cache::provider_snapshot::Snapshot& payload) const;
    void StartFavoriteBindingRead(uint64_t generation, uint64_t bindingEpoch);
    void CompleteFavoriteBinding(uint64_t generation, uint64_t bindingEpoch, stream::Id playlist,
                                 bool complete, std::wstring error);
    std::vector<stream::Playlist> RawPlaylists(stream::Shelf shelf) const;
    bool VerifyCatalogArtist(uint64_t generation, std::wstring_view storefront, std::wstring_view catalogId,
                             abort_callback& abort);
    // Back to "nothing known" for a new session: the account, the library, everything in flight.
    // `authPending`: a check of the account is expected next (start-up). Called with m_mutex held
    // at Start, Shutdown and SignOut.
    void ResetSessionLocked(bool authPending);
    void ClearLibraryLocked();
    void ClearPlaylistDetailsLocked();
    bool PutIfCurrent(uint64_t generation, const Song& song);
    bool PutIfCurrent(uint64_t generation, const std::vector<Song>& songs);
    bool PutIfCurrent(uint64_t generation, const stream::Playlist& playlist);
    bool PutIfCurrent(uint64_t generation, const stream::Album& album);
    bool PutIfCurrent(uint64_t generation, const std::vector<stream::Album>& albums);
    bool PutIfCurrent(uint64_t generation, const stream::Artist& artist);
    bool PutIfCurrent(uint64_t generation, const std::vector<stream::Artist>& artists);
    bool RequireSignedIn(std::wstring& error) const;
    std::wstring Storefront() const;
    void OnDisplayMetadataChanged();
    void ApplyDisplayMetadataChange();
    stream::Id CatalogIdForAlbum(const stream::Id& id) const;
    struct AlbumRequest {
        stream::Id id;
        bool library = false;
    };
    AlbumRequest ResolveAlbumRequest(const stream::Id& id) const;
    stream::Id CatalogIdForArtist(const stream::Id& id) const;
    stream::Id CatalogIdForPlaylist(const stream::Id& id) const;
    void ResolveSongDisplayNames(std::vector<Song>& songs, uint64_t generation, abort_callback& abort);
    void ResolveAlbumDisplayNames(std::vector<stream::Album>& albums, uint64_t generation,
                                  abort_callback& abort);
    void ResolveArtistDisplayNames(std::vector<stream::Artist>& artists, uint64_t generation,
                                   abort_callback& abort);
    void ResolvePlaylistDisplayNames(std::vector<stream::Playlist>& playlists, uint64_t generation,
                                     abort_callback& abort);
    void ApplyArtistPreferenceOverlay(stream::Artist& artist, uint64_t capturedEpoch) const;
    void ApplyArtistPreferenceOverlayLocked(stream::Artist& artist, uint64_t capturedEpoch) const;
    std::optional<bool> PlaylistSavedStateLocked(const stream::Id& playlist) const;
    bool IsEditablePlaylist(const stream::Id& id) const;
    bool IsFavoritePlaylist(const stream::Id& id) const;
    stream::Id KnownLibraryId(bool album, const stream::Id& id) const;
    stream::Id ArtistCatalogId(const stream::Id& artist) const;
    void EnrichArtistCountsAsync(uint64_t generation);
    void RefreshKnownPlaylist(const stream::Id& id, Done done = {}, bool invalidateLists = false);
    void RefreshKnownAlbum(const stream::Id& id);
    void RefreshLibrarySongsAsync(uint64_t generation, TracksDone done);
    // The tracks of a playlist the last detail read kept; nullopt when it was never read.
    std::optional<std::vector<stream::Track>> CachedPlaylist(const stream::Id& playlist) const;
    // Reads the Favorite Songs playlist so the stars know which songs are favorites.
    void RefreshFavoriteSongs();

    // FetchListPage: a read is prepared on the main thread (FetchListPage, which also parks the
    // Favorite Songs reads until their playlist is bound), loaded on a worker by the loader of
    // its kind (ReadListPage and the Read*Page methods fill a ListLoad) and committed on the
    // main thread (FinishListPage).
    struct ListLoad;
    struct ArtistSource {
        bool library = false;
        stream::Id catalogId;
    };
    bool HoldForFavoritePlaylist(const stream::ListQuery& query, const stream::ListCursor& cursor,
                                 size_t limit, uint64_t generation, const stream::ListPageDone& done,
                                 stream::Id& favoritePlaylist);
    void ReadListPage(ListLoad& load);
    void ReadPlaylistTracksPage(ListLoad& load);
    void ReadAlbumTracksPage(ListLoad& load);
    void ReadSectionTracksPage(ListLoad& load);
    void ReadLibrarySongsPage(ListLoad& load);
    void ReadGroupSongsPage(ListLoad& load);
    void ReadArtistTopTracksPage(ListLoad& load);
    void ReadArtistAlbumsPage(ListLoad& load);
    void ReadArtistPlaylistsPage(ListLoad& load);
    void ReadSimilarArtistsPage(ListLoad& load);
    void ReadSavedAlbumsPage(ListLoad& load);
    void ReadFollowedArtistsPage(ListLoad& load);
    void ReadPlaylistsPage(ListLoad& load);
    void ReadGroupsPage(ListLoad& load);
    void ReadCatalogSearchPage(ListLoad& load);
    ArtistSource ResolveArtistSource(const stream::Id& artist, abort_callback& abort) const;
    std::optional<Object> ReadArtistView(ListLoad& load, const stream::Id& catalogId,
                                         const std::wstring& request);
    void FinishListPage(uint64_t generation, uint64_t preferenceEpoch, stream::ListPage page,
                        std::vector<Song> songs, bool trackRows, bool catalogItemRows, bool derivedGroupsPage,
                        std::wstring error, stream::ListPageDone done);

    mutable std::mutex m_mutex;
    std::condition_variable m_artistVerificationCondition;
    bool m_started = false;
    // Verified once the helper returns a successful account snapshot; the initial signed-out
    // placeholder is not an identity and must not invalidate startup work.
    stream::AccountGate m_gate;
    std::wstring m_lastAuthError;
    bool m_refreshInFlight = false;
    uint64_t m_refreshGeneration = 0;
    uint64_t m_refreshSequence = 0;
    bool m_refreshPreservePlaylistDetails = false;
    bool m_refreshInvalidatesLists = false;
    bool m_displayChangePosted = false;
    std::vector<Done> m_refreshWaiters;
    bool m_artistCountsInFlight = false;
    bool m_librarySnapshotLoaded = false;
    std::chrono::steady_clock::time_point m_librarySnapshotValidUntil{};
    stream::Executor m_exec;
    stream::SnapshotSaver m_snapshots;
    AccountState m_account;
    std::wstring m_cachedAccountName;
    std::wstring m_cachedStorefront;
    DrmState m_drm;
    std::vector<stream::Playlist> m_createdPlaylists;
    std::vector<stream::Playlist> m_savedPlaylists;
    std::vector<stream::Album> m_savedAlbums;
    std::vector<stream::Artist> m_libraryArtists;
    std::unordered_map<stream::Id, std::vector<stream::Track>> m_playlistTracks;
    std::unordered_map<stream::Id, std::vector<Song>> m_playlistSongs;
    enum class FavoriteBindingState {
        Unknown,
        Pending,
        Ready,
        Empty
    };
    struct FavoritePageWaiter {
        uint64_t generation = 0;
        stream::ListQuery query;
        stream::ListCursor cursor;
        size_t limit = 0;
        stream::ListPageDone done;
    };
    struct ArtistExistenceEntry {
        uint64_t generation = 0;
        bool known = false;
        bool exists = false;
        std::chrono::steady_clock::time_point validUntil{};
    };
    struct AlbumMutation {
        stream::Id catalogId;
        stream::Id libraryId;
        stream::Album album;
        uint64_t epoch = 0;
        bool saved = false;
        bool pending = false;
    };
    struct PlaylistMutation {
        stream::Id catalogId;
        stream::Id libraryId;
        stream::Playlist playlist;
        uint64_t epoch = 0;
        bool saved = false;
        bool pending = false;
    };
    struct ArtistPreferenceMutation {
        stream::Id sourceId;
        stream::Id catalogId;
        uint64_t epoch = 0;
        bool favorite = false;
        bool pending = true;
    };
    uint64_t m_playlistDetailEpoch = 0;
    std::unordered_map<stream::Id, std::vector<stream::Track>> m_artistLibraryTracks;
    std::unordered_map<stream::Id, stream::Id> m_playlistLibraryIds;
    std::unordered_map<stream::Id, stream::Id> m_albumLibraryIds;
    std::unordered_map<stream::Id, stream::Id> m_artistCatalogIds;
    std::unordered_map<std::wstring, ArtistExistenceEntry> m_artistExistence;
    std::unordered_map<stream::Id, AlbumMutation> m_albumMutations;
    std::unordered_map<stream::Id, PlaylistMutation> m_playlistMutations;
    mutable std::unordered_map<stream::Id, ArtistPreferenceMutation> m_artistPreferenceMutations;
    uint64_t m_libraryMutationEpoch = 0;
    uint64_t m_libraryMutationSequence = 0;
    uint64_t m_appliedLibraryMutationSequence = 0;
    mutable uint64_t m_artistPreferenceEpoch = 0;
    uint64_t m_artistPreferenceSequence = 0;
    uint64_t m_appliedArtistPreferenceSequence = 0;
    std::set<stream::Id> m_savedAlbumIds;
    std::set<stream::Id> m_editablePlaylists;
    std::set<stream::Id> m_renameablePlaylists;
    std::set<stream::Id> m_deletablePlaylists;
    std::wstring m_locale;
    stream::Id m_favoritePlaylistId;
    FavoriteBindingState m_favoriteBindingState = FavoriteBindingState::Unknown;
    uint64_t m_favoriteBindingEpoch = 0;
    bool m_favoriteBindingReadInFlight = false;
    std::vector<FavoritePageWaiter> m_favoritePageWaiters;
    std::vector<Song> m_librarySongs;
    std::chrono::steady_clock::time_point m_librarySongsAt{}; // when m_librarySongs was read
    // Songs added to or removed from the library here, by every id they are known by (catalog,
    // library, the track's own): Apple lists a change only after a while, and these win until
    // the account changes.
    std::unordered_map<stream::Id, bool> m_songLibraryEdits;
    bool SongRemovedHereLocked(const Song& song) const;
    // RequestUnknownSong: ids waiting for the next batch, and when each was last asked for.
    mutable std::mutex m_unknownMutex;
    mutable std::set<stream::Id> m_unknownSongs;
    mutable std::unordered_map<stream::Id, std::chrono::steady_clock::time_point> m_unknownAsked;
    mutable bool m_unknownQueued = false;
    void ReadUnknownSongs() const;
    // The library's songs for the lists derived from them (genres, composers and their songs):
    // the copy read within the library window, else one read shared by every such list. Worker.
    std::vector<Song> RecentLibrarySongs(uint64_t generation, abort_callback& abort);
    std::vector<Song> m_favoriteSongs;
    std::set<stream::Id> m_favoriteIds;
    int64_t m_favoriteIdsCheckedAt = 0; // when the ids were last read from the service
    void SaveFavoriteIdsAsync();
    bool m_librarySongsLoaded = false;
    bool m_favoriteSongsLoaded = false;
};

} // namespace glasswave::applemusic
