#pragma once

// Snapshot of the foobar2000 Media Library, pre-formatted for WMP 12 style browsing:
// all tracks sorted by album artist / year / album / disc / track, plus artist, album and
// genre groupings. Rebuilt (debounced by the caller) whenever the library changes.

namespace glasswave::media {

struct Track {
    metadb_handle_ptr handle;
    std::wstring albumArtist, artist, album, genre, title, number, length, year;
    std::wstring composer, rating, folderPath;  // rating "1".."5" or empty; folder as on disk
    std::wstring search;  // lower-cased "artist album title genre" for filtering
    std::wstring folder;  // lower-cased directory, groups compilations without ALBUM ARTIST
    bool hasAlbumArtist = false;
    double seconds = 0;
    size_t albumIndex = SIZE_MAX;  // into Groups(GroupKind::Album)
};

// Ways to browse Music, as WMP 12's navigation pane offers them (Artist = album artist).
enum class GroupKind { Artist, Album, Genre, Year, Rating, ContributingArtist, Composer, Folder };
inline constexpr size_t kGroupKinds = 8;

struct Group {
    std::wstring key;        // unique within its kind
    std::wstring name;       // display name
    std::wstring secondary;  // album: artist; others: empty
    std::wstring year;       // album only
    std::vector<size_t> tracks;  // indices into LibraryModel::Tracks()
    std::wstring search;
    double seconds = 0;  // total length
};

class LibraryModel {
public:
    // Reads the whole library (main thread). Cheap enough for tens of thousands of tracks.
    void Reload();

    bool Enabled() const { return m_enabled; }
    const std::vector<Track>& Tracks() const { return m_tracks; }
    const std::vector<Group>& Groups(GroupKind kind) const;
    const Group* FindGroup(GroupKind kind, std::wstring_view key) const;
    const Track* FindTrack(const metadb_handle_ptr& handle) const;  // linear; for clicks
    // Position of an album in the "All music" order (album artist, year, title).
    size_t AlbumRank(size_t album) const { return album < m_albumRank.size() ? m_albumRank[album] : SIZE_MAX; }

private:
    bool m_enabled = false;
    std::vector<Track> m_tracks;
    std::array<std::vector<Group>, kGroupKinds> m_groups;
    std::vector<size_t> m_albumRank;
};

std::wstring Lower(std::wstring_view s);
bool MatchesFilter(std::wstring_view haystackLower, std::wstring_view filterLower);

}  // namespace glasswave::media
