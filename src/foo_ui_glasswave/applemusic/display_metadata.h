#pragma once

#include "applemusic/catalog.h"
#include "applemusic/config.h"

#include <chrono>
#include <mutex>
#include <cstdint>
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

// Catalog records remain authoritative and unmodified. This cache only overlays display
// names on copies, keyed by the display mode, source storefront, locale and stable ID.
class DisplayMetadata {
  public:
    static DisplayMetadata& Get();

    void Clear();
    // Starts a new region projection without discarding other regions' warm display entries.
    // Pending writes from the old projection fail their epoch check.
    void BeginTransition();
    // Keeps stale names available for first paint while forcing the next legitimate page read
    // to perform a bounded background refresh.
    void InvalidateFreshness();
    uint64_t Epoch() const;
    void SetSourceStorefront(std::wstring storefront);
    std::wstring SourceStorefront() const;

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

    void PutSong(std::wstring_view sourceStorefront, const stream::Id& id, SongDisplayNames names);
    void PutAlbum(std::wstring_view sourceStorefront, const stream::Id& id, AlbumDisplayNames names);
    void PutArtist(std::wstring_view sourceStorefront, const stream::Id& id, ArtistDisplayNames names);
    void PutPlaylist(std::wstring_view sourceStorefront, const stream::Id& id, PlaylistDisplayNames names);
    void PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                    SongDisplayNames names);
    void PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                    SongDisplayNames names, bool complete);
    void PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                    SongDisplayNames names, bool complete, uint64_t expectedEpoch);
    void PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                     AlbumDisplayNames names);
    void PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                     AlbumDisplayNames names, bool complete);
    void PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront, const stream::Id& id,
                     AlbumDisplayNames names, bool complete, uint64_t expectedEpoch);
    void PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                      const stream::Id& id, ArtistDisplayNames names);
    void PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                      const stream::Id& id, ArtistDisplayNames names, bool complete);
    void PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                      const stream::Id& id, ArtistDisplayNames names, bool complete, uint64_t expectedEpoch);
    void PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                        const stream::Id& id, PlaylistDisplayNames names);
    void PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                        const stream::Id& id, PlaylistDisplayNames names, bool complete);
    void PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                        const stream::Id& id, PlaylistDisplayNames names, bool complete,
                        uint64_t expectedEpoch);

    void Apply(Song& song, std::wstring_view sourceStorefront) const;
    void Apply(stream::Track& track, std::wstring_view sourceStorefront) const;
    void Apply(stream::Album& album, std::wstring_view sourceStorefront) const;
    void Apply(stream::Artist& artist, std::wstring_view sourceStorefront) const;
    void Apply(stream::Playlist& playlist, std::wstring_view sourceStorefront) const;

  private:
    struct SongEntry {
        SongDisplayNames names;
        std::chrono::steady_clock::time_point validatedAt{};
        uint64_t used = 0;
        bool complete = false;
    };
    struct AlbumEntry {
        AlbumDisplayNames names;
        std::chrono::steady_clock::time_point validatedAt{};
        uint64_t used = 0;
        bool complete = false;
    };
    struct ArtistEntry {
        ArtistDisplayNames names;
        std::chrono::steady_clock::time_point validatedAt{};
        uint64_t used = 0;
        bool complete = false;
    };
    struct PlaylistEntry {
        PlaylistDisplayNames names;
        std::chrono::steady_clock::time_point validatedAt{};
        uint64_t used = 0;
        bool complete = false;
    };

    DisplayMetadata() = default;
    std::string Key(const DisplayTarget& target, std::wstring_view kind, std::wstring_view sourceStorefront,
                    const stream::Id& id) const;

    mutable std::mutex m_mutex;
    std::wstring m_sourceStorefront;
    mutable uint64_t m_clock = 0;
    mutable uint64_t m_epoch = 0;
    mutable std::unordered_map<std::string, SongEntry> m_songs;
    mutable std::unordered_map<std::string, AlbumEntry> m_albums;
    mutable std::unordered_map<std::string, ArtistEntry> m_artists;
    mutable std::unordered_map<std::string, PlaylistEntry> m_playlists;
};

} // namespace glasswave::applemusic
