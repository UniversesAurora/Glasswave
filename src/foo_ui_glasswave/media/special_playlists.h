#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "media/playback_scheduler.h"

// Playlists with a role, like a current player's (Apple Music / NetEase Cloud Music):
//  - the play queue ("Now Playing"): double-clicking a song anywhere replaces it with that
//    list and plays from the song; "Play next" creates the newest priority block, while
//    "Add to queue" inserts into the ordinary future; shuffle and repeat act on it. It
//    cannot be removed or renamed.
//  - Favorites (media/favorites.h): cannot be removed or renamed either.
//  - smart playlists: foobar2000 auto playlists. Presets (library, never played, history,
//    most played, recently added, by rating) can be added any time; a default set is
//    created on first run and can be restored. Any auto playlist counts as smart.
// Playlists are found by their (English) names; the UI shows translated names.
namespace glasswave::media {

inline constexpr const char* kQueueName = "Now Playing";

struct SmartPreset {
    const char* name;  // playlist name in foobar2000 (English key)
    const char* query; // foobar2000 query, sort included; nullptr = the Favorites playlist
    bool byDefault;    // part of the set created on first run / restored
    int group;         // menu group (separators between groups)
};
const std::vector<SmartPreset>& SmartPresets();

t_size QueuePlaylist(bool create);
bool IsQueue(t_size playlist);
bool IsSmart(t_size playlist); // an auto playlist client is registered
// An auto playlist for grouping purposes: the client is present, or the playlist is locked
// against item edits (the lock autoplaylist_manager installs, which is already visible when
// on_playlist_locked fires, before is_client_present() reports the client).
bool IsAutoPlaylist(t_size playlist);
bool IsSpecial(t_size playlist); // queue, Favorites or a smart playlist

// Creates the queue and Favorites, protects both, and creates the default smart playlists on
// first run (or the missing ones when `restore`).
void EnsureSpecialPlaylists(bool restore);
// Adds the preset as a playlist (or returns the existing one); SIZE_MAX on failure.
t_size AddSmartPreset(const SmartPreset& preset);
// A new empty auto playlist with foobar2000's query dialog open.
t_size NewSmartPlaylist();

// Replace and play a fresh source. False when nothing was played (no items, or the scheduler
// refused the request): the queue is left as it was.
bool PlayQueue(const metadb_handle_list& items, size_t start);
// Playing a list that must be read first (a service's playlist or album from its card or page).
// Asking stops what plays at once; until the list is read, Play does not bring the old queue
// back (owner, 2026-11-20: the old song went on for 15 s after a card's play was pressed). A
// newer request, Stop (media::transport::Stop), or any playback that starts meanwhile supersedes
// it. foobar2000's own Stop pressed while nothing plays reports no event and cannot give it up.
uint64_t BeginPendingPlay();
bool TakePendingPlay(uint64_t token); // true when this request is still the one to play; clears it
void DropPendingPlay(uint64_t token); // its read failed
bool PendingPlay();                   // the transport's Play waits for the list instead
void CancelPendingPlay();
// Called after PlayQueue started a fresh source (the window notes a playlist played from its page).
void SetPlayQueueListener(std::function<void()> listener);
void PlayNext(const metadb_handle_list& items);                // newest priority block goes next
void AddToQueue(const metadb_handle_list& items);              // ordinary future insertion
// Feed/paging token: changed only by Replace/Clear, not ordinary edits or track advance.
bool AppendSourceBatch(const metadb_handle_list& items, uint64_t expectedGeneration);

// Glasswave's predictable queue modes. repeat is 0=off, 1=all, 2=one. These are separate
// from foobar2000's active playback order; while our queue is used the core order is Default
// (Repeat (track) for repeat one, so the core replays the song and Next walks the list).
bool SetQueuePlaybackMode(bool shuffle, int repeat);
bool QueueShuffleEnabled();
int QueueRepeatMode();
std::vector<size_t> ScheduledUpcomingIndices();
std::vector<ScheduledUpcomingEntry> ScheduledUpcomingSnapshot();
bool ReorderUpcomingSnapshot(uint64_t expectedRevision, const std::vector<ScheduledUpcomingEntry>& ordered);
std::optional<size_t> ScheduledNextIndex();
uint64_t QueueScheduleRevision();
uint64_t QueueContentGeneration();

} // namespace glasswave::media
