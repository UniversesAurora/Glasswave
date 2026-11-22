#pragma once

#include "stream/account_gate.h"
#include "stream/executor.h"
#include "netease/convert.h"
#include "stream/provider.h"
#include "stream/snapshot_saver.h"

#include <memory>
#include <mutex>

namespace glasswave::netease {

// NetEase Cloud Music as a stream::Provider: maps the session (netease::Provider) and its
// catalog to the provider-neutral records. Ids are the decimal NetEase ids.
class StreamProvider final : public stream::Provider {
  public:
    static StreamProvider& Get();

    void Start() override;
    void Shutdown() override; // stops the session and its helper

    std::wstring_view Key() const override { return L"ncm"; }
    std::wstring Name() const override { return L"NetEase Cloud Music"; }
    std::vector<std::wstring> AppNames() const override { return {L"网易云音乐", L"NetEase Cloud Music"}; }
    stream::Capabilities Offers() const override;
    std::vector<stream::ListKind> CatalogSearchKinds() const override;
    std::vector<settings::Field> SettingFields() override;
    std::wstring_view TextContext() const override { return L"NetEase Cloud Music"; }
    std::wstring HomeListBadge(const stream::Id& list) const override;
    // The service answers a quick run of album reads with "操作频繁" (owner, 2026-11-14).
    std::chrono::milliseconds ListRequestGap(stream::ListKind kind) const override {
        return kind == stream::ListKind::AlbumTracks ? std::chrono::milliseconds(600) : std::chrono::milliseconds();
    }
    std::string PlaybackSettings() const override;

    bool SignedIn() const override;
    std::wstring AccountName() const override;
    bool SessionRestoring() const override;
    bool AccountVerified() const override;
    void SignIn(HWND owner) override;
    void SignOut() override;
    void RefreshLibrary(Done done) override;

    stream::Id TrackId(const metadb_handle_ptr& item) const override;
    std::wstring Lyrics(const stream::Id& track, abort_callback& abort) override;
    stream::DownloadSource ResolveDownload(const stream::Id& track, abort_callback& abort) override;
    stream::Catalog* Records() const override;
    metadb_handle_ptr ArtItem(stream::ArtKind kind, const stream::Id& id) const override;
    bool SameCover(std::wstring_view a, std::wstring_view b) const override;

    bool ReadFavorite(const stream::Id& track) const override;
    bool ReadAlbumSaved(const stream::Id& album) const override;
    std::vector<stream::Playlist> Playlists(stream::Shelf shelf) const override;
    std::optional<stream::Shelf> ShelfOf(const stream::Id& playlist) const override;
    std::vector<stream::Artist> FollowedArtists() const override;
    std::vector<stream::Album> SavedAlbums() const override;
    void FetchArtist(const stream::Id& artist, ArtistDone done) override;
    void FetchArtistBio(const stream::Id& artist, TextDone done) override;
    void FetchSongDetails(const stream::Id& track, SongDetailsDone done) override;
    bool NeedsAccount(const stream::ListQuery& query) const override;
    void FetchListPage(const stream::ListQuery& query, const stream::ListCursor& cursor, size_t limit,
                       stream::ListPageDone done) override;
    void FetchCatalogSearchSuggestions(CatalogSearchSuggestionQuery query,
                                       CatalogSearchSuggestionsDone done) override;
    void FetchHome(HomeDone done) override;
    void FetchStationTracks(const stream::Id& station, TracksDone done) override;
    bool CanDislike(const stream::Id& station) const override;
    void DislikeStationTrack(const stream::Id& station, const stream::Id& track, Done done) override;
    void RestoreListRecords(const stream::ListPage& page) override;

    void WriteFavorite(const stream::Id& track, bool favorite, Done done) override;
    void WriteFollowed(const stream::Id& artist, bool follow, Done done) override;
    void WriteAlbumSaved(const stream::Id& album, bool saved, Done done) override;
    void WritePlaylistSaved(const stream::Id& playlist, bool saved, Done done) override;
    void AddToPlaylist(const stream::Id& playlist, std::vector<stream::Id> tracks, Done done) override;
    void RemoveFromPlaylist(const stream::Id& playlist, std::vector<stream::Id> tracks, Done done) override;
    void CreatePlaylist(std::wstring name, CreatedDone done) override;
    void RenamePlaylist(const stream::Id& playlist, std::wstring name, Done done) override;
    void DeletePlaylist(const stream::Id& playlist, Done done) override;
    void ReorderPlaylists(std::vector<stream::Id> playlists, Done done) override;

  private:
    struct IdSnapshotStore;

    StreamProvider();
    void LoadBootstrapSnapshot();
    // Back to "nothing known" for a new session; `authPending`: a check of the account is
    // expected next. Called with m_cacheMutex held at Start, Shutdown and on an account change.
    void ResetSessionLocked(bool authPending);
    // What the snapshot saver asks of this provider (m_snapshots).
    bool SnapshotEligible() const;
    bool SnapshotEligibleLocked() const;
    bool FillSnapshot(cache::provider_snapshot::Snapshot& payload) const;
    void OnSessionChanged();
    void PostListPage(stream::ListPageDone done, stream::ListPage page, std::vector<SongRecord> songs,
                      std::wstring error, std::shared_ptr<std::atomic_bool> cancelled);
    void FinishListWrite(bool ok, std::wstring error, Done done,
                         std::vector<stream::ListTarget> targets = {});
    void ClearIdSnapshots();
    // The catalog's reader (stream::Catalog::StartReading): songs asked for that it lacks or
    // holds without their album or artist ids, read in one batch and published to foobar2000's
    // metadata. Main thread.
    void ReadSongs(std::vector<stream::Id> ids);

    bool m_signedIn = false;
    stream::AccountGate m_gate;
    mutable std::mutex m_cacheMutex;
    std::wstring m_accountId;
    std::vector<stream::Playlist> m_cachedCreated;
    std::vector<stream::Playlist> m_cachedSaved;
    stream::Executor m_exec; // one queue: every blocking read and the snapshot save
    stream::SnapshotSaver m_snapshots;
    std::shared_ptr<IdSnapshotStore> m_idSnapshots;
    std::atomic<int64_t> m_heartbeatSeed{0}; // heartbeat mode's next seed: the last song it sent
};

} // namespace glasswave::netease
