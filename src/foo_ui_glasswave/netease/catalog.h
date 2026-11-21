#pragma once

#include "netease/types.h"

#include <atomic>
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
    void Put(const Album& album);
    void Put(const std::vector<Album>& albums);

    // Metadata restored from a provider list cache fills missing fields only. In particular,
    // it never replaces an existing cover URL with the sparse record used by the disk cache.
    void Restore(const Song& song);
    void Restore(const Playlist& playlist);
    void Restore(const Artist& artist);
    void Restore(const Album& album);

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

    // Some curated entries carry a different image per endpoint. Pin the cover the user
    // should always see (the personal radar uses its Recommended image everywhere).
    void PreferCover(int64_t playlistId, std::wstring coverUrl);
    void ClearPreferredCovers();

    // Main thread: store and feed file_info to metadb in one batch. `persist` queues the restart
    // cache for a deferred write on a worker; list pages pass false because the generic list
    // cache already owns their persistence.
    void PutAndHint(const std::vector<Song>& songs, bool persist = true);
    // Main thread: merge cached list metadata into richer records, then feed the result to
    // metadb without rewriting the legacy NetEase TSV cache.
    void RestoreAndHint(const std::vector<Song>& songs);

    // Restart cache: <profile>\glasswave\netease\cache\songs.tsv. Load also pushes the
    // cached tags into metadb so persisted ncm:// playlist entries format correctly.
    void Load();
    void Save() const;
    // Main thread, at quit: writes the restart cache now when a deferred write is still pending.
    void FlushPendingSave();

private:
    Catalog() = default;
    void Hint(const std::vector<Song>& songs);
    // Under m_mutex: stores a record; true when a flush of queued metadb hints must be posted.
    bool StoreLocked(const Song& song);
    void FlushHints(); // main thread
    void QueueSave();  // main thread: one deferred write for a burst of puts

    mutable std::mutex m_mutex;
    mutable std::mutex m_saveMutex; // one writer of songs.tsv at a time
    std::atomic_bool m_saveDirty = false;
    bool m_savePosted = false; // main thread
    std::vector<int64_t> m_hintQueue; // records new or shown differently, for metadb
    bool m_hintPosted = false;
    std::unordered_map<int64_t, Song> m_songs;
    std::unordered_map<int64_t, Playlist> m_playlists;
    std::unordered_map<int64_t, std::wstring> m_preferredCovers;
    std::unordered_map<int64_t, Artist> m_artists;
    std::unordered_map<int64_t, Album> m_albums;
};

}  // namespace glasswave::netease
