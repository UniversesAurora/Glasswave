#pragma once

// Playlists with a role, like a current player's (Apple Music / NetEase Cloud Music):
//  - the play queue ("Now Playing"): double-clicking a song anywhere replaces it with that
//    list and plays from the song; "Play next" inserts after the playing song, "Add to
//    queue" appends; shuffle and repeat act on it. It cannot be removed or renamed.
//  - Favorites (media/favorites.h): cannot be removed or renamed either.
//  - smart playlists: foobar2000 auto playlists. Presets (library, never played, history,
//    most played, recently added, by rating) can be added any time; a default set is
//    created on first run and can be restored. Any auto playlist counts as smart.
// Playlists are found by their (English) names; the UI shows translated names.
namespace glasswave::media {

inline constexpr const char* kQueueName = "Now Playing";

struct SmartPreset {
    const char* name;   // playlist name in foobar2000 (English key)
    const char* query;  // foobar2000 query, sort included; nullptr = the Favorites playlist
    bool byDefault;     // part of the set created on first run / restored
    int group;          // menu group (separators between groups)
};
const std::vector<SmartPreset>& SmartPresets();

t_size QueuePlaylist(bool create);
bool IsQueue(t_size playlist);
bool IsSmart(t_size playlist);    // an auto playlist
bool IsSpecial(t_size playlist);  // queue, Favorites or a smart playlist

// Creates the queue and Favorites, protects both, and creates the default smart playlists on
// first run (or the missing ones when `restore`).
void EnsureSpecialPlaylists(bool restore);
// Adds the preset as a playlist (or returns the existing one); SIZE_MAX on failure.
t_size AddSmartPreset(const SmartPreset& preset);
// A new empty auto playlist with foobar2000's query dialog open.
t_size NewSmartPlaylist();

void PlayQueue(const metadb_handle_list& items, size_t start);  // replace the queue and play
void PlayNext(const metadb_handle_list& items);                 // insert after the playing song
void AddToQueue(const metadb_handle_list& items);               // append

}  // namespace glasswave::media
