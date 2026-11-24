#pragma once

#include "applemusic/config.h"
#include "applemusic/song.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace glasswave::applemusic {

struct DisplayTarget {
    AppleMusicDisplayRegion region = AppleMusicDisplayRegion::Account;
    std::wstring storefront;
    std::wstring locale;
};

std::optional<DisplayTarget> DisplayTargetFor(AppleMusicDisplayRegion region);

struct SongDisplayNames {
    std::wstring title;
    std::wstring album;
    std::vector<stream::ArtistRef> artists;
    std::wstring composerName;
    std::vector<std::wstring> composerNames;
    std::vector<std::wstring> genreNames;
    stream::Id albumId;
    std::wstring coverUrl;
    bool artistsAuthoritative = false;
};

struct AlbumDisplayNames {
    std::wstring name;
    std::wstring description;
    std::vector<stream::ArtistRef> artists;
    std::wstring coverUrl;
    bool artistsAuthoritative = false;
};

struct ArtistDisplayNames {
    std::wstring name;
    std::wstring bio;
    std::wstring avatarUrl;
};

struct PlaylistDisplayNames {
    std::wstring name;
    std::wstring description;
    std::wstring coverUrl;
};

// Persisted regional display overlay for Apple Music. Canonical Catalog records stay raw
// (account storefront); this cache overlays localized names on copies, keyed solely by
// (display target storefront, locale, kind, stable ID) so account-storefront startup
// transitions never split or orphan cached regional entries.
class DisplayMetadata {
  public:
    static DisplayMetadata& Get();

    void Clear();
    // Starts a new region projection without discarding other regions' warm display entries.
    // Pending writes from the old projection fail their epoch check.
    void BeginTransition();
    // Keeps cached names available for first paint while forcing the next read to revalidate.
    void InvalidateFreshness();
    uint64_t Epoch() const;
    void SetSourceStorefront(std::wstring storefront);
    std::wstring SourceStorefront() const;

    // Restart cache (<profile>/glasswave/applemusic/cache/display.tsv). LoadCache runs
    // synchronously on startup; writes are debounced and flushed on shutdown.
    void LoadCache();
    void FlushPendingSave();

    // Pure serialization helpers (also used by unit tests).
    std::string Serialize() const;
    size_t Deserialize(std::string_view bytes);

    bool HasSong(std::wstring_view sourceStorefront, const stream::Id& id) const;
    bool HasAlbum(std::wstring_view sourceStorefront, const stream::Id& id) const;
    bool HasArtist(std::wstring_view sourceStorefront, const stream::Id& id) const;
    bool HasPlaylist(std::wstring_view sourceStorefront, const stream::Id& id) const;
    bool HasSong(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                 const stream::Id& id) const;
    bool HasAlbum(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                  const stream::Id& id) const;
    bool HasArtist(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                   const stream::Id& id) const;
    bool HasPlaylist(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                     const stream::Id& id) const;
    bool IsSongComplete(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                        const stream::Id& id) const;
    bool IsAlbumComplete(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                         const stream::Id& id) const;
    bool IsArtistComplete(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                          const stream::Id& id) const;
    bool IsPlaylistComplete(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                            const stream::Id& id) const;

    // Return true when visible display fields changed.
    bool PutSong(std::wstring_view sourceStorefront, const stream::Id& id, SongDisplayNames names);
    bool PutAlbum(std::wstring_view sourceStorefront, const stream::Id& id, AlbumDisplayNames names);
    bool PutArtist(std::wstring_view sourceStorefront, const stream::Id& id, ArtistDisplayNames names);
    bool PutPlaylist(std::wstring_view sourceStorefront, const stream::Id& id, PlaylistDisplayNames names);
    bool PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                    SongDisplayNames names);
    bool PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                    SongDisplayNames names, bool complete);
    bool PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                    SongDisplayNames names, bool complete, uint64_t expectedEpoch);
    bool PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                     AlbumDisplayNames names);
    bool PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                     AlbumDisplayNames names, bool complete);
    bool PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                     AlbumDisplayNames names, bool complete, uint64_t expectedEpoch);
    bool PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                      const stream::Id& id, ArtistDisplayNames names);
    bool PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                      const stream::Id& id, ArtistDisplayNames names, bool complete);
    bool PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                      const stream::Id& id, ArtistDisplayNames names, bool complete, uint64_t expectedEpoch);
    bool PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                        const stream::Id& id, PlaylistDisplayNames names);
    bool PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                        const stream::Id& id, PlaylistDisplayNames names, bool complete);
    bool PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                        const stream::Id& id, PlaylistDisplayNames names, bool complete,
                        uint64_t expectedEpoch);

    void Apply(Song& song, std::wstring_view sourceStorefront) const;
    void Apply(stream::Track& track, std::wstring_view sourceStorefront) const;
    void Apply(stream::Album& album, std::wstring_view sourceStorefront) const;
    void Apply(stream::Artist& artist, std::wstring_view sourceStorefront) const;
    void Apply(stream::Playlist& playlist, std::wstring_view sourceStorefront) const;

  private:
    struct SongEntry {
        AppleMusicDisplayRegion region = AppleMusicDisplayRegion::Account;
        stream::Id id;
        SongDisplayNames names;
        int64_t validatedAt = 0;
        uint64_t used = 0;
        bool complete = false;
    };
    struct AlbumEntry {
        AppleMusicDisplayRegion region = AppleMusicDisplayRegion::Account;
        stream::Id id;
        AlbumDisplayNames names;
        int64_t validatedAt = 0;
        uint64_t used = 0;
        bool complete = false;
    };
    struct ArtistEntry {
        AppleMusicDisplayRegion region = AppleMusicDisplayRegion::Account;
        stream::Id id;
        ArtistDisplayNames names;
        int64_t validatedAt = 0;
        uint64_t used = 0;
        bool complete = false;
    };
    struct PlaylistEntry {
        AppleMusicDisplayRegion region = AppleMusicDisplayRegion::Account;
        stream::Id id;
        PlaylistDisplayNames names;
        int64_t validatedAt = 0;
        uint64_t used = 0;
        bool complete = false;
    };

    DisplayMetadata() = default;
    static std::string Key(const DisplayTarget& target, std::wstring_view kind, const stream::Id& id);
    void QueueSave();
    void SaveLocked() const;
    void ApplyArtistFallback(std::vector<stream::ArtistRef>& artists,
                             const std::vector<stream::ArtistRef>& explicitNames,
                             std::wstring_view sourceStorefront) const;

    mutable std::mutex m_mutex;
    mutable std::mutex m_saveMutex;
    std::wstring m_sourceStorefront;
    mutable uint64_t m_clock = 0;
    mutable uint64_t m_epoch = 0;
    mutable std::unordered_map<std::string, SongEntry> m_songs;
    mutable std::unordered_map<std::string, AlbumEntry> m_albums;
    mutable std::unordered_map<std::string, ArtistEntry> m_artists;
    mutable std::unordered_map<std::string, PlaylistEntry> m_playlists;
    std::atomic_bool m_saveDirty = false;
    std::atomic_bool m_savePosted = false;
    std::atomic_bool m_saveClosed = false;
};

} // namespace glasswave::applemusic
