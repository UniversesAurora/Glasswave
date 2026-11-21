#pragma once

#include "media/playback_schedule.h"

#include <SDK/foobar2000.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace glasswave::media {

enum class ScheduledUpcomingSource {
    CoreQueue,
    Playlist,
};

struct ScheduledUpcomingEntry {
    ScheduledUpcomingSource source = ScheduledUpcomingSource::Playlist;
    std::size_t queueIndex = SIZE_MAX;    // Exact SDK queue position for CoreQueue rows.
    std::size_t playlistIndex = SIZE_MAX; // Physical token for Playlist or own CoreQueue rows;
                                          // foreign CoreQueue rows deliberately use SIZE_MAX.
    metadb_handle_ptr handle;             // Preserves duplicate row identity for UI rendering.
};

class PlaybackSchedulerProbeAccess final {
  public:
    struct HostState {
        std::shared_ptr<const void> data;
    };

    static bool Install();
    static void Uninstall();
    static HostState Capture();
    static bool Restore(const HostState& state);
    static bool Same(const HostState& left, const HostState& right);
    static std::optional<PlaybackOccurrenceId> CurrentOccurrence();
    static std::vector<PlaybackOccurrenceId> PhysicalOccurrences();
    static PlaybackScheduleState ScheduleState();
    static std::uint64_t ContentGeneration();
    static bool CheckpointUsesCurrentSchema();
    static bool AdvanceOccurrence(PlaybackOccurrenceId occurrence, bool from_history);
    static bool AdvanceExplicit(PlaybackOccurrenceId occurrence);
    static bool TickModel(double remainingSeconds);
};

// Main-thread bridge between the deterministic schedule and foobar2000's protected Now
// Playing playlist. Core playback order is Default (Repeat (track) while repeating one song).
// The host projects future order physically into the owned playlist and prepends PlayNext to
// the explicit core queue while preserving all existing queue rows.
class PlaybackScheduler final {
  public:
    static void Start();
    static void Shutdown();
    static PlaybackScheduler& Get();
    static PlaybackScheduler* Existing();

    ~PlaybackScheduler();
    PlaybackScheduler(const PlaybackScheduler&) = delete;
    PlaybackScheduler& operator=(const PlaybackScheduler&) = delete;

    bool SetPlaybackMode(bool shuffle, int repeat);
    bool QueueShuffleEnabled() const;
    int QueueRepeatMode() const;

    bool PlayQueue(const metadb_handle_list& items, std::size_t start);
    // Makes `items` the queue around the song playing from it (items[start]) without
    // restarting it; false when that song is not what plays.
    bool AdoptPlaying(const metadb_handle_list& items, std::size_t start);
    // The scheduler is editing the queue playlist itself (its callbacks are its own echo).
    bool Editing() const;
    bool PlayNext(const metadb_handle_list& items);
    bool AddToQueue(const metadb_handle_list& items);
    // Paged-source append. The token changes only on Replace/Clear, never on an ordinary
    // queue edit or track transition. On success this follows AddToQueue's ordinary-future
    // placement and keeps the same content generation.
    bool AppendSourceBatch(const metadb_handle_list& items, std::uint64_t expectedGeneration);

    // Playlist indices for Glasswave's projected future. The explicit SDK queue still runs
    // ahead of these rows; use ScheduledUpcomingSnapshot() for the combined actual order.
    std::vector<std::size_t> ScheduledUpcomingIndices();
    std::vector<ScheduledUpcomingEntry> ScheduledUpcomingSnapshot();
    // What Up Next shows (PlaybackSchedule::DisplayUpcoming after the explicit queue), and how
    // many songs repeat-all replays after it (0: no repeat-all footer).
    std::vector<ScheduledUpcomingEntry> DisplayUpcomingSnapshot();
    std::size_t RepeatCycleSize();
    bool ReorderUpcomingSnapshot(std::uint64_t expectedRevision,
                                 const std::vector<ScheduledUpcomingEntry>& ordered);
    // Glasswave's own next row after the explicit SDK queue. Prefetch already consults that
    // queue first; ScheduleRevision changes when either side changes.
    std::optional<std::size_t> ScheduledNextIndex();
    std::uint64_t ScheduleRevision() const;
    // The song playback is starting towards (on_playback_starting): the next or previous one for
    // Next / Previous, the one asked for when a list starts; null when unknown.
    metadb_handle_ptr StartingTrack(play_control::t_track_command command);
    // Stable feed token for a list currently projected into Now Playing.
    std::uint64_t ContentGeneration() const;

    // Main-thread lifetime-bound notification. Calls are deferred and coalesced; each listener
    // should only request its own UI/prefetch refresh. Returns 0 when registration was rejected.
    std::uint64_t AddListener(std::function<void()> listener);
    void RemoveListener(std::uint64_t listener);

    // Callback entry points. They only post work; mutations and playback_control calls never
    // run inside a foobar2000 global callback.
    void OnPlaybackStarting(play_control::t_track_command command, bool paused);
    void OnTrackStarted(const metadb_handle_ptr& track);
    void OnPlaybackStopped(play_control::t_stop_reason reason);
    void OnPlaybackTime();
    // The user is about to stop playback through Glasswave (media::transport::Stop). A song that
    // is still opening is given up: its stop is not a failed start, which would play the next one.
    // Main thread, before playback_control::stop.
    void StopRequested();

    void QueuePlaylistAdded(std::size_t base, const metadb_handle_list& items);
    void QueuePlaylistReordered(const t_size* order, std::size_t count);
    void QueuePlaylistRemoved(const bit_array& mask, std::size_t old_count);
    void QueuePlaylistReplaced();
    void QueuePlaylistAvailabilityChanged();
    void OnCoreQueueChanged();
    void CorePlaybackOrderChanged();
    void OnNativeOrderMode();

  private:
    friend class PlaybackSchedulerProbeAccess;

    struct Impl;
    explicit PlaybackScheduler(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace glasswave::media
