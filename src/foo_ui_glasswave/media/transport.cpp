#include "pch.h"
#include "media/transport.h"

#include "media/playback_scheduler.h"
#include "media/special_playlists.h"

namespace glasswave::media::transport {

void PlayPause(const std::function<bool()>& startHere) {
    if (PendingPlay()) return; // the list asked to play comes next, not the old queue
    auto pc = playback_control::get();
    if (pc->is_playing()) {
        pc->play_or_pause();
        return;
    }
    if (startHere && startHere()) return;
    pc->start();
}

void Stop() {
    CancelPendingPlay();
    if (auto* scheduler = PlaybackScheduler::Existing()) scheduler->StopRequested();
    playback_control::get()->stop();
}

void Previous() {
    playback_control::get()->previous();
}

void Next() {
    playback_control::get()->next();
}

} // namespace glasswave::media::transport
