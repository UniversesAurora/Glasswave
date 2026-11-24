#pragma once

#include "stream/model.h"

#include <SDK/foobar2000.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// What a provider has seen of its service, kept as stream:: records: the records the lists
// showed or playback asked about, the metadb handles that name them, the tags fed to metadb
// for them, and the restart cache of songs. One class for every provider; a provider differs
// only in its CatalogTraits and in the side tables it keeps for what the stream:: records do
// not carry (Apple's library ids, NetEase's codec and bit rate).
namespace glasswave::stream {

struct CatalogTraits {
    // <profile>\glasswave\<dir>\cache\songs.tsv is the restart cache.
    std::wstring dir;
    // The metadb paths are <scheme>://song/<id> and <scheme>://cover/<kind>/<id>, <kind> being
    // song, album, artist, playlist or station. Playlists and play history hold these paths, so
    // they never change; only the id's spelling differs between services.
    std::string scheme;
    // An id as a path segment ("" when the id cannot be named: no handle) and back.
    std::string (*encodeId)(const Id& id) = nullptr;
    bool (*decodeId)(std::string_view segment, Id& id) = nullptr;
    // First line of the restart cache. A file with another first line is not read, so a change
    // of the record or `encodeExtra` format only needs a new header.
    std::string cacheHeader;
    // Tags fed to metadb for a song (default: WriteTrackTags). A provider adds what it keeps
    // beside the track (genre, codec...) or rewrites the names it shows differently.
    std::function<void(const Track& stored, file_info& info)> tags;
    // What the provider keeps of a song beside the track, as one tab- and newline-free text for
    // the restart cache; empty when it keeps nothing. Called on the thread that saves.
    std::function<std::wstring(const Id& song)> encodeExtra;
};

// A song as a provider hands it over. `cover` is the song's own picture; `retag` says that what
// the provider keeps beside the track changed in a way metadb shows (the track alone is compared
// by the catalog).
struct TrackRecord {
    Track track;
    std::wstring cover;
    bool retag = false;
};

// A song of the restart cache: the track, its cover and the provider's own text (`extra`).
struct CachedTrack {
    Track track;
    std::wstring cover;
    std::wstring extra;
};

// Title, artist, album, album artist (the first credit) and length: the tags every provider writes.
void WriteTrackTags(const Track& track, file_info& info);

// A song stored without what its service links it to: its album, or an artist's id. A list
// row or a station's song can arrive so; the catalog reads it again when it is asked for.
bool LacksLinks(const Track& track);

// Merge-on-restore: `primary` stays, `fallback` only fills what `primary` lacks. A sparse record
// (a row of a list cache, a search summary) never overwrites a richer one.
std::vector<ArtistRef> MergeArtistRefs(std::vector<ArtistRef> primary,
                                       const std::vector<ArtistRef>& fallback);
Track MergeTrack(const Track& primary, const Track& fallback);
Playlist MergePlaylist(const Playlist& primary, const Playlist& fallback, bool mergeFlags);
Album MergeAlbum(const Album& primary, const Album& fallback);
Artist MergeArtist(const Artist& primary, const Artist& fallback, bool mergeFollowState);

// All reads and writes are synchronized: the records are shared by the provider, decoder threads
// and the album-art workers. Only the metadb calls (Flush..., Hint...) are for the main thread.
// The disk cache holds songs only, never credentials.
class Catalog {
  public:
    explicit Catalog(CatalogTraits traits);

    // ---- records ---------------------------------------------------------------------------
    // Put stores the record as given. A song that is new or shown differently is fed to metadb
    // in one batch with the others, on the main thread.
    void Put(const TrackRecord& song);
    void Put(const std::vector<TrackRecord>& songs);
    void Put(const Album& album);
    void Put(const Artist& artist);
    void Put(const Playlist& playlist);
    void Put(const Station& station);
    void Put(const std::vector<Album>& albums);
    void Put(const std::vector<Artist>& artists);
    void Put(const std::vector<Playlist>& playlists);

    // Restore merges a sparse record (from a list cache or a summary) into what is known, or
    // stores it when nothing is. RestoreAndHint (main thread) also feeds the songs to metadb now.
    void Restore(const TrackRecord& song);
    void Restore(const Album& album);
    void Restore(const Artist& artist);
    void Restore(const Playlist& playlist);
    void RestoreAndHint(const std::vector<TrackRecord>& songs);

    std::optional<Track> FindTrack(const Id& id) const;
    std::optional<Album> FindAlbum(const Id& id) const;
    std::optional<Artist> FindArtist(const Id& id) const;
    std::optional<Playlist> FindPlaylist(const Id& id) const;
    std::optional<Station> FindStation(const Id& id) const;

    void ErasePlaylist(const Id& id);
    void EraseAlbum(const Id& id);

    // The picture URL of a record ("" when none is known). A preferred cover (PreferCover) wins.
    std::wstring CoverUrl(ArtKind kind, const Id& id) const;
    // Some entries carry a different picture per endpoint: pin the one to show everywhere.
    void PreferCover(ArtKind kind, const Id& id, std::wstring coverUrl);
    void ClearPreferredCovers();

    // ---- handles and paths -----------------------------------------------------------------
    std::string SongPath(const Id& song) const;
    bool ParseSongPath(const char* path, Id& song) const;
    std::string CoverPath(ArtKind kind, const Id& id) const;
    bool ParseCoverPath(const char* path, ArtKind& kind, Id& id) const;
    // Null when the id has no path.
    metadb_handle_ptr Handle(const Id& song) const;
    metadb_handle_ptr CoverHandle(ArtKind kind, const Id& id) const;

    // ---- metadb ----------------------------------------------------------------------------
    // The tags a song gets in metadb and in an input's file_info (traits.tags, else the
    // default).
    void WriteTags(const Track& track, file_info& info) const;
    // Main thread: Put, then feed the queued songs to metadb at once and queue the restart cache
    // for a write. List pages pass persist = false: the generic list cache owns their persistence.
    void PutAndHint(const std::vector<TrackRecord>& songs, bool persist);
    // Main thread: feeds the queued songs to metadb now.
    void FlushHints();
    // Main thread: feeds these songs (or every song) to metadb, as they are.
    void Hint(const std::vector<Id>& ids) const;
    void HintAll() const;

    // ---- reading from the service ------------------------------------------------------------
    // A song something asked for that the records lack, or hold without its links (LacksLinks),
    // is read from the service. Want gathers the asks until the main thread is free and hands
    // them to the provider's reader in one batch. A song is asked for again only after a pause,
    // and not at all once a read answered for it (Settle), even when the service had no more to
    // give. Asks made while no reader is set (before the provider starts) wait for one.
    // Want and Settle are for any thread, StartReading and StopReading for the main thread.
    using Reader = std::function<void(std::vector<Id> songs)>;
    void Want(const Id& song);
    // Clears any prior settled/cooldown state and queues the song for reading again.
    void Rewant(const Id& song);
    // Clears all settled/cooldown markers (e.g. when the display region changes).
    void ClearSettled();
    // The provider read these songs and put what the service returned.
    void Settle(const std::vector<Id>& songs);
    // `read` runs on the main thread with each batch. The provider puts what it read and settles
    // the songs it got an answer for; a failed read settles nothing.
    void StartReading(Reader read);
    void StopReading();

    // ---- restart cache -----------------------------------------------------------------------
    // ReadCache is worker-safe and has no side effects. InstallMissing stores the songs not
    // already known and returns their ids; the caller hints them once its side tables hold them.
    std::vector<CachedTrack> ReadCache() const;
    std::vector<Id> InstallMissing(std::vector<CachedTrack> songs);
    // Main thread: one deferred write, on a worker, for a burst of puts.
    void QueueSave();
    // Main thread, at quit: waits for a deferred write in flight, writes now when one is still
    // pending, and starts no deferred write after it.
    void FlushPendingSave();

    // A language change: drops the records and what was read, keeps the restart cache. Stations
    // stay.
    void ClearMemory();
    // Sign-out or account switch: drops every record and the restart cache.
    void Clear();

  private:
    struct TrackEntry {
        Track track;
        std::wstring cover;
    };

    // Under m_mutex: stores a record; true when a flush of queued hints must be posted.
    bool StoreLocked(const TrackRecord& song);
    // Under m_saveMutex: writes the restart cache. Writes nothing while no song is known, so a
    // catalog cleared for a language change does not empty the cache on disk.
    void SaveLocked() const;
    void PostHints();
    void Hint(const std::vector<Track>& tracks) const;
    // Main thread: hands the songs asked for to the reader, but those found meanwhile.
    void FlushWanted();
    void ForgetReads();
    std::filesystem::path CacheFile() const;
    static std::wstring PreferredKey(ArtKind kind, const Id& id);

    CatalogTraits m_traits;
    mutable std::mutex m_mutex;
    mutable std::mutex m_saveMutex; // one writer of songs.tsv at a time
    std::atomic_bool m_saveDirty = false;
    std::atomic_bool m_savePosted = false;
    std::atomic_bool m_saveClosed = false; // FlushPendingSave ran: quitting
    std::vector<Id> m_hintQueue;           // songs new or shown differently, for metadb
    bool m_hintPosted = false;
    std::unordered_map<Id, TrackEntry> m_tracks;
    std::unordered_map<Id, Album> m_albums;
    std::unordered_map<Id, Artist> m_artists;
    std::unordered_map<Id, Playlist> m_playlists;
    std::unordered_map<Id, Station> m_stations;
    std::unordered_map<std::wstring, std::wstring> m_preferred;

    mutable std::mutex m_readMutex; // the reading state below
    Reader m_reader;
    std::vector<Id> m_wanted;
    bool m_wantedPosted = false;
    std::unordered_map<Id, std::chrono::steady_clock::time_point> m_asked;
    std::unordered_set<Id> m_settled;
};

} // namespace glasswave::stream
