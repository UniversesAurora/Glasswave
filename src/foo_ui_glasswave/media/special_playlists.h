#pragma once

// Playlists with a role, like a current player's (Apple Music / NetEase Cloud Music):
//  - the play queue ("Now Playing"): double-clicking a song anywhere replaces it with that
//    list and plays from the song; "Play next" inserts after the playing song, "Add to
//    queue" appends; shuffle and repeat act on it. It cannot be removed or renamed.
//  - Favorites (media/favorites.h): cannot be removed or renamed either.
//  - smart playlists: foobar2000 auto playlists for recently played, most played, never
//    played, recently added and rated songs; created on first run, restorable any time.
// Playlists are found by their (English) names; the UI shows translated names.
namespace glasswave::media {

inline constexpr const char* kQueueName = "Now Playing";

struct SmartPreset {
    const char* name;   // playlist name in foobar2000
    const char* query;  // foobar2000 query, sort included
};
const std::vector<SmartPreset>& SmartPresets();

t_size QueuePlaylist(bool create);
bool IsQueue(t_size playlist);
bool IsSmartName(const char* name);
bool IsSpecial(t_size playlist);  // queue, Favorites or a smart playlist

// Creates the queue and Favorites, protects both, and creates the smart playlists on first
// run (or all missing ones when `restoreSmart`).
void EnsureSpecialPlaylists(bool restoreSmart);

void PlayQueue(const metadb_handle_list& items, size_t start);  // replace the queue and play
void PlayNext(const metadb_handle_list& items);                 // insert after the playing song
void AddToQueue(const metadb_handle_list& items);               // append

}  // namespace glasswave::media
