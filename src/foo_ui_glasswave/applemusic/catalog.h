#pragma once

#include "applemusic/ids.h"
#include "stream/catalog.h"
#include "stream/model.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace glasswave::applemusic {

// What Apple keeps of a song beyond its stream::Track and cover: the ids of its library and
// catalog entries, the library's flags and the names the track has no field for.
struct SongExtra {
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

struct Song : SongExtra {
    stream::Track track;
    std::wstring coverUrl;
};

// Apple metadata is shared by the provider, decoder threads and album-art workers. The records
// are stream:: records in a stream::Catalog (Records()); this class keeps what they do not carry
// (a Song's library ids, composers and genres, the delivered lengths, the library/catalog id
// maps) and joins it with them. All reads are synchronized; only PutAndHint touches foobar2000's
// main-thread metadata service. The disk cache contains library metadata and cover URLs, never
// credentials.
class Catalog {
  public:
    static Catalog& Get();

    // The shared records: handles, paths, restore, cache.
    stream::Catalog& Records() { return m_records; }

    void Put(const Song& song);
    void Put(const std::vector<Song>& songs);
    void Put(const stream::Playlist& playlist);
    void Put(const stream::Station& station); // for its cover
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
    // The album a song belongs to, also for songs known only from a list restored from disk
    // (whose names may be another region's and are not put into the catalog). A song's cover
    // is named by its album, so the UI and the cover store must agree on this in every run.
    void PutSongAlbum(const stream::Id& songId, const stream::Id& albumId);
    stream::Id SongAlbum(const stream::Id& songId) const;

    void RemovePlaylist(const stream::Id& id);
    void RemoveAlbum(const stream::Id& id);

    metadb_handle_ptr Handle(const stream::Id& songId) const { return m_records.Handle(songId); }
    metadb_handle_ptr CoverHandle(stream::ArtKind kind, const stream::Id& id) const {
        return m_records.CoverHandle(kind, id);
    }
    std::wstring CoverUrl(stream::ArtKind kind, const stream::Id& id) const;

    // Main thread: update metadb with tags and durations in one batch. Streamed list pages
    // pass false because the generic list cache owns their persistence.
    void PutAndHint(const std::vector<Song>& songs, bool persist = true);

    // An uploaded song's length as delivered for playback. Apple computes a library length
    // from the upload, and it can be wrong (an MP3 measured at twice its length); the
    // delivered length replaces it in every record put here, across library reloads and
    // restarts. 0 when none was measured.
    int MeasuredLength(const stream::Id& songId) const;
    // Main thread: records a delivered length that disagrees with the library's, and
    // publishes it to the song's record and metadb.
    void PutMeasuredLength(const stream::Id& songId, int durationMs);

    // Restart cache. ReadCacheFile is worker-safe and has no side effects; InstallCached
    // merges only records not already present, then publishes their metadb hints.
    std::vector<Song> ReadCacheFile() const;
    void InstallCached(std::vector<Song> songs);
    // Reapply the current display overlay to cached metadb hints without changing raw records.
    void RefreshHints() const { m_records.HintAll(); }
    // Main thread, at quit: writes the restart cache now when a deferred write is still pending.
    void FlushPendingSave() { m_records.FlushPendingSave(); }
    // A UI-language change invalidates localized names without deleting the restart cache.
    void ClearMemory();
    // Sign-out/account switch: remove private library metadata from memory and disk.
    void Clear();

  private:
    Catalog();
    static stream::CatalogTraits Traits(Catalog& self);
        std::wstring EncodeExtra(const stream::Id& id) const;
    void WriteTags(const stream::Track& stored, file_info& info) const;
    // Under m_mutex: reads the measured lengths file once, then corrects a song's length.
    void LoadMeasuredLocked() const;
    void ApplyMeasuredLocked(Song& song) const;
    void SaveMeasuredLocked() const;

    stream::Catalog m_records;
    mutable std::mutex m_mutex;
    mutable bool m_measuredLoaded = false;
    mutable std::unordered_map<stream::Id, int> m_measured;
    std::unordered_map<stream::Id, SongExtra> m_extras;
    std::unordered_map<stream::Id, stream::Id> m_albumLibraryIds;
    std::unordered_map<stream::Id, stream::Id> m_artistCatalogIds;
    std::unordered_map<stream::Id, stream::Id> m_songAlbums;
};

} // namespace glasswave::applemusic
