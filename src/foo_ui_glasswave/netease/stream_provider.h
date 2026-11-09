#pragma once

#include "stream/provider.h"

namespace glasswave::netease {

// NetEase Cloud Music as a stream::Provider: maps the session (netease::Provider) and its
// catalog to the provider-neutral records. Ids are the decimal NetEase ids.
class StreamProvider final : public stream::Provider {
public:
    static StreamProvider& Get();

    void Start() override;
    void Shutdown();  // stops the session and its helper

    std::wstring_view Key() const override { return L"ncm"; }
    std::wstring Name() const override { return L"NetEase Cloud Music"; }
    Features Offers() const override;
    std::vector<settings::Field> SettingFields() override;

    bool SignedIn() const override;
    std::wstring AccountName() const override;
    void SignIn(HWND owner) override;
    void SignOut() override;
    void RefreshLibrary(Done done) override;

    stream::Id TrackId(const metadb_handle_ptr& item) const override;
    std::wstring Lyrics(const stream::Id& track, abort_callback& abort) override;
    metadb_handle_ptr TrackItem(const stream::Id& track) const override;
    std::optional<stream::Track> FindTrack(const stream::Id& track) const override;
    metadb_handle_ptr ArtItem(stream::ArtKind kind, const stream::Id& id) const override;
    bool SameCover(std::wstring_view a, std::wstring_view b) const override;

    bool IsFavorite(const stream::Id& track) const override;
    std::vector<stream::Playlist> Playlists(stream::Shelf shelf) const override;
    std::optional<stream::Shelf> ShelfOf(const stream::Id& playlist) const override;
    std::vector<stream::Artist> FollowedArtists() const override;
    std::vector<stream::Album> SavedAlbums() const override;
    std::optional<stream::Playlist> FindPlaylist(const stream::Id& playlist) const override;
    std::optional<stream::Album> FindAlbum(const stream::Id& album) const override;
    std::optional<std::vector<stream::Track>> CachedFavorites() const override;
    std::optional<std::vector<stream::Track>> CachedPlaylist(const stream::Id& playlist) const override;
    std::optional<std::vector<stream::Track>> CachedAlbum(const stream::Id& album) const override;

    void FetchFavorites(TracksDone done) override;
    void FetchPlaylists(stream::Shelf shelf, PlaylistsDone done) override;
    void FetchPlaylist(const stream::Id& playlist, PlaylistDone done) override;
    void FetchAlbum(const stream::Id& album, AlbumDone done) override;
    void FetchAlbumsTracks(std::vector<stream::Id> albums, TracksDone done) override;
    void FetchArtist(const stream::Id& artist, ArtistDone done) override;
    void FetchArtistBio(const stream::Id& artist, TextDone done) override;
    void FetchArtistAlbums(const stream::Id& artist, size_t offset, size_t limit, AlbumPageDone done) override;
    void FetchArtistTopTracks(const stream::Id& artist, size_t offset, size_t limit, TrackPageDone done) override;
    void FetchFollowedArtists(ArtistsDone done) override;
    void FetchSavedAlbums(size_t offset, size_t limit, AlbumPageDone done) override;

    void SetFavorite(const stream::Id& track, bool favorite, Done done) override;
    void SetFollowed(const stream::Id& artist, bool follow, Done done) override;
    void SetAlbumSaved(const stream::Id& album, bool saved, Done done) override;
    void SetPlaylistSaved(const stream::Id& playlist, bool saved, Done done) override;
    void AddToPlaylist(const stream::Id& playlist, std::vector<stream::Id> tracks, Done done) override;
    void RemoveFromPlaylist(const stream::Id& playlist, std::vector<stream::Id> tracks, Done done) override;
    void CreatePlaylist(std::wstring name, CreatedDone done) override;
    void RenamePlaylist(const stream::Id& playlist, std::wstring name, Done done) override;
    void DeletePlaylist(const stream::Id& playlist, Done done) override;
    void ReorderPlaylists(std::vector<stream::Id> playlists, Done done) override;

private:
    StreamProvider() = default;
    void OnSessionChanged();

    bool m_started = false;
    bool m_signedIn = false;
};

}  // namespace glasswave::netease
