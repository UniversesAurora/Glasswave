#pragma once

#include <SDK/foobar2000.h>

namespace glasswave::media {

// Unified event-driven non-audio lookahead coordinator for regular playlists, Now Playing queue
// (including Shuffle / Repeat / PlayNext) and Radio mode. Runs zero polling timers (idle = idle):
// wakes only on playback, schedule, playlist, queue, radio, provider and metadb-hint events.
class PlaybackLookahead final {
  public:
    static void Start();
    static void Shutdown();
    static PlaybackLookahead* Existing();

    ~PlaybackLookahead();
    PlaybackLookahead(const PlaybackLookahead&) = delete;
    PlaybackLookahead& operator=(const PlaybackLookahead&) = delete;

    // Coalesces an evaluation onto the main thread. Safe from any thread.
    void Schedule();
    void OnPlaybackStarting(play_control::t_track_command command);
    void OnPlaybackNewTrack();
    void OnPlaybackStopped();
    void OnMetadbChanged(metadb_handle_list_cref items);

  private:
    PlaybackLookahead();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace glasswave::media
