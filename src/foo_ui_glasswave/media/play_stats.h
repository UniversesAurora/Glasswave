#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace glasswave::media {

// Cross-source play statistics behind All Music's Recently Played and Most Played.
// foobar2000 auto playlists only see the Media Library, so streaming tracks never enter
// them; this small store keeps every source together, with a metadata snapshot taken
// when a track starts (enough to list a song even before its catalog is fetched).
struct PlayRecord {
    std::string key;  // stable identity: local path + subsong, or "<provider>:<track id>"
    std::string path;
    std::string title, artist, album;
    std::wstring provider;  // streaming provider key; empty for local files
    std::wstring trackId, albumId, artistId;  // the provider's ids (album and first artist link)
    int durationMs = 0;
    uint32_t subsong = 0;
    uint32_t count = 0;
    int64_t lastPlayed = 0;  // Unix seconds
};

class PlayStats {
public:
    static PlayStats& Get();

    void Record(const metadb_handle_ptr& track);  // a track started playing
    // Takes the current tags of tracks already recorded into their snapshots (a CD tagged
    // after it started playing).
    void Refresh(metadb_handle_list_cref items);
    // The stored count of this track (0 when it has never been recorded).
    uint32_t CountFor(const metadb_handle_ptr& track) const;
    // Drops the last-played time (the track leaves Recently Played) but keeps the count.
    void ForgetRecent(metadb_handle_list_cref items);
    // Resets the count (the track leaves Most Played) but keeps the last-played time.
    void ClearCounts(metadb_handle_list_cref items);
    void ForgetAllRecent();  // clears the whole Recently Played list
    void ClearAllCounts();   // clears the whole Most Played list
    // Lists leave out the tracks of a CD that is not in a drive (cd::DiscPresent); their counts stay.
    std::vector<PlayRecord> Recent(size_t limit);
    std::vector<PlayRecord> MostPlayed(size_t limit);

private:
    void Load();
    void Save() const;
    void Prune();
    void RebuildIndex();

    std::vector<PlayRecord> m_records;
    std::unordered_map<std::string, size_t> m_index;
    bool m_loaded = false;
};

}  // namespace glasswave::media
