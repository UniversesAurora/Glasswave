#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace glasswave::media {

// Cross-source play statistics behind All Music's Recently Played and Most Played.
// foobar2000 auto playlists only see the Media Library, so NetEase tracks never enter
// them; this small store keeps every source together, with a metadata snapshot taken
// when a track starts (enough to list a song even before its catalog is fetched).
struct PlayRecord {
    std::string key;  // stable identity: local path + subsong, or "ncm:<id>"
    std::string path;
    std::string title, artist, album;
    int64_t songId = 0;  // NetEase song id (0 for local)
    int64_t albumId = 0;
    int64_t artistId = 0;
    int durationMs = 0;
    uint32_t subsong = 0;
    uint32_t count = 0;
    int64_t lastPlayed = 0;  // Unix seconds
    bool remote = false;
};

class PlayStats {
public:
    static PlayStats& Get();

    void Record(const metadb_handle_ptr& track);  // a track started playing
    std::vector<PlayRecord> Recent(size_t limit);
    std::vector<PlayRecord> MostPlayed(size_t limit);

private:
    void Load();
    void Save() const;
    void Prune();

    std::vector<PlayRecord> m_records;
    bool m_loaded = false;
};

}  // namespace glasswave::media
