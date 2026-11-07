#pragma once

// Snapshot of the foobar2000 Media Library, pre-formatted for WMP 12 style browsing:
// all tracks sorted by album artist / year / album / disc / track, plus artist, album and
// genre groupings. Rebuilt (debounced by the caller) whenever the library changes.

namespace glasswave::media {

struct Track {
    metadb_handle_ptr handle;
    std::wstring albumArtist, artist, album, genre, title, number, length, year;
    std::wstring search;  // lower-cased "artist album title genre" for filtering
    std::wstring folder;  // lower-cased directory, groups compilations without ALBUM ARTIST
    bool hasAlbumArtist = false;
};

enum class GroupKind { Artist, Album, Genre };

struct Group {
    std::wstring key;        // unique within its kind
    std::wstring name;       // display name
    std::wstring secondary;  // album: artist; others: empty
    std::wstring year;       // album only
    std::vector<size_t> tracks;  // indices into LibraryModel::Tracks()
    std::wstring search;
};

class LibraryModel {
public:
    // Reads the whole library (main thread). Cheap enough for tens of thousands of tracks.
    void Reload();

    bool Enabled() const { return m_enabled; }
    const std::vector<Track>& Tracks() const { return m_tracks; }
    const std::vector<Group>& Groups(GroupKind kind) const;
    const Group* FindGroup(GroupKind kind, std::wstring_view key) const;

private:
    bool m_enabled = false;
    std::vector<Track> m_tracks;
    std::vector<Group> m_artists, m_albums, m_genres;
};

std::wstring Lower(std::wstring_view s);
bool MatchesFilter(std::wstring_view haystackLower, std::wstring_view filterLower);

}  // namespace glasswave::media
