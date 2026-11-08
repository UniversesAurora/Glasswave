#pragma once

#include "netease/types.h"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace glasswave::netease {

// Songs and playlists seen by the UI or fetched for playback. Metadb handles are derived
// from the stable ncm:// paths; metadata is pushed into the core cache so persisted
// playlists show titles after a restart.
class Catalog {
public:
    static Catalog& Get();

    void Put(const Song& song);
    void Put(const std::vector<Song>& songs);
    void Put(const Playlist& playlist);
    void Put(const Artist& artist);
    void Put(const std::vector<Album>& albums);

    std::optional<Song> FindSong(int64_t id) const;
    std::optional<Playlist> FindPlaylist(int64_t id) const;
    std::optional<Artist> FindArtist(int64_t id) const;
    std::optional<Album> FindAlbum(int64_t id) const;

    metadb_handle_ptr Handle(int64_t songId) const;
    metadb_handle_ptr PlaylistHandle(int64_t playlistId) const;
    metadb_handle_ptr ArtistHandle(int64_t artistId) const;
    metadb_handle_ptr AlbumHandle(int64_t albumId) const;
    std::wstring SongCover(int64_t songId) const;
    std::wstring PlaylistCover(int64_t playlistId) const;
    std::wstring ArtistCover(int64_t artistId) const;
    std::wstring AlbumCover(int64_t albumId) const;

    // Main thread: store and feed file_info to metadb in one batch.
    void PutAndHint(const std::vector<Song>& songs);

    // Restart cache: <profile>\glasswave\netease\cache\songs.tsv. Load also pushes the
    // cached tags into metadb so persisted ncm:// playlist entries format correctly.
    void Load();
    void Save() const;

private:
    Catalog() = default;
    void Hint(const std::vector<Song>& songs);

    mutable std::mutex m_mutex;
    std::unordered_map<int64_t, Song> m_songs;
    std::unordered_map<int64_t, Playlist> m_playlists;
    std::unordered_map<int64_t, Artist> m_artists;
    std::unordered_map<int64_t, Album> m_albums;
};

}  // namespace glasswave::netease
