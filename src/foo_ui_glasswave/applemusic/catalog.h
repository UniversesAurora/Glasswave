#pragma once

#include "applemusic/handle.h"
#include "stream/model.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace glasswave::applemusic {

struct Song {
    stream::Track track;
    std::wstring coverUrl;
    std::wstring catalogId;
    std::wstring libraryId;
    std::wstring libraryAlbumId;
    std::wstring reportingId;
    std::wstring isrc;
    std::wstring contentRating;
    bool customTitle = false;
    bool customAlbum = false;
    std::vector<std::wstring> genreNames;
    std::vector<std::wstring> composerNames;
    std::wstring composerName;
};

// Apple metadata is shared by the provider, decoder threads and album-art workers.
// All reads are synchronized; only PutAndHint touches foobar2000's main-thread metadata
// service. The disk cache contains library metadata and cover URLs, never credentials.
class Catalog {
  public:
    static Catalog& Get();

    void Put(const Song& song);
    void Put(const std::vector<Song>& songs);
    void Put(const stream::Playlist& playlist);
    void Put(const std::vector<stream::Playlist>& playlists);
    void Put(const stream::Album& album);
    void Put(const std::vector<stream::Album>& albums);
    void Put(const stream::Artist& artist);
    void Put(const std::vector<stream::Artist>& artists);

    std::optional<Song> FindSong(const stream::Id& id) const;
    std::optional<Song> FindSongRaw(const stream::Id& id) const;
    std::optional<stream::Playlist> FindPlaylist(const stream::Id& id) const;
    std::optional<stream::Playlist> FindPlaylistRaw(const stream::Id& id) const;
    std::optional<stream::Album> FindAlbum(const stream::Id& id) const;
    std::optional<stream::Album> FindAlbumRaw(const stream::Id& id) const;
    std::optional<stream::Artist> FindArtist(const stream::Id& id) const;
    std::optional<stream::Artist> FindArtistRaw(const stream::Id& id) const;
    void PutAlbumLibraryId(const stream::Id& catalogId, const stream::Id& libraryId);
    stream::Id FindAlbumCatalogId(const stream::Id& libraryId) const;
    void PutArtistCatalogId(const stream::Id& libraryId, const stream::Id& catalogId);
    stream::Id FindArtistCatalogId(const stream::Id& libraryId) const;

    void RemovePlaylist(const stream::Id& id);
    void RemoveAlbum(const stream::Id& id);

    metadb_handle_ptr Handle(const stream::Id& songId) const;
    metadb_handle_ptr CoverHandle(stream::ArtKind kind, const stream::Id& id) const;
    std::wstring CoverUrl(stream::ArtKind kind, const stream::Id& id) const;

    // Main thread: update metadb with tags and durations in one batch. Streamed list pages
    // pass false because the generic list cache owns their persistence.
    void PutAndHint(const std::vector<Song>& songs, bool persist = true);

    // Restart cache. ReadCacheFile is worker-safe and has no side effects; InstallCached
    // merges only records not already present, then publishes their metadb hints.
    std::vector<Song> ReadCacheFile() const;
    void InstallCached(std::vector<Song> songs);
    void Save() const;
    // Reapply the current display overlay to cached metadb hints without changing raw records.
    void RefreshHints() const;
    // A UI-language change invalidates localized names without deleting the restart cache.
    void ClearMemory();
    // Sign-out/account switch: remove private library metadata from memory and disk.
    void Clear();

  private:
    Catalog() = default;
    void Hint(const std::vector<Song>& songs) const;

    mutable std::mutex m_mutex;
    std::unordered_map<stream::Id, Song> m_songs;
    std::unordered_map<stream::Id, stream::Playlist> m_playlists;
    std::unordered_map<stream::Id, stream::Album> m_albums;
    std::unordered_map<stream::Id, stream::Artist> m_artists;
    std::unordered_map<stream::Id, stream::Id> m_albumLibraryIds;
    std::unordered_map<stream::Id, stream::Id> m_artistCatalogIds;
};

} // namespace glasswave::applemusic
