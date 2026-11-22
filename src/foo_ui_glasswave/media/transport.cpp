#include "pch.h"
#include "media/transport.h"

#include "media/playback_scheduler.h"
#include "media/radio.h"
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
    if (radio::Active()) {
        radio::GoPrevious();
        return;
    }
    playback_control::get()->previous();
}

void Next() {
    if (radio::Active()) {
        radio::GoNext();
        return;
    }
    playback_control::get()->next();
}

Modes ShufflePressed(Modes current) {
    if (radio::Active()) {
        radio::DislikeCurrent();
        return current;
    }
    current.shuffle = !current.shuffle;
    return current;
}

Modes RepeatPressed(Modes current) {
    if (radio::Active()) return {false, current.repeat == 0 ? 2 : 0};
    return {current.shuffle, (current.repeat + 1) % 3};
}

} // namespace glasswave::media::transport
