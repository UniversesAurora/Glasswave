#pragma once

#include <functional>

// The transport commands of every Glasswave control: the library window's transport band, the
// mini and full screen players, the taskbar's thumbnail buttons, the Playback menu and the
// windows' keys. Their rules live here once: a list asked to play and still being read
// (media::PendingPlay) is what Play waits for, and Stop gives it up. A play started anywhere else
// (foobar2000's own commands, a media key) supersedes such a list too (special_playlists.cpp);
// only foobar2000's own Stop pressed while nothing plays cannot be seen, as the core reports no
// event for it. Main thread.
namespace glasswave::media::transport {

// Play / pause. Nothing while a list asked to play is still being read. When nothing plays,
// `startHere` may start something itself (the library window plays the focused row) and returns
// whether it did; otherwise the queue starts.
void PlayPause(const std::function<bool()>& startHere = {});
// Stops playback and gives up a list still being read to play.
void Stop();
void Previous();
void Next();

// The play modes the shuffle and repeat buttons set: repeat 0 off, 1 all (the list), 2 one.
struct Modes {
    bool shuffle = false;
    int repeat = 0;
};
// The shuffle button. On a station it is "recommend less like this" (radio::DislikeCurrent) and
// the modes stay; otherwise shuffle turns over.
Modes ShufflePressed(Modes current);
// The repeat button: off, all, one in turn. A station has no list to repeat and no shuffle: one
// song or none.
Modes RepeatPressed(Modes current);

} // namespace glasswave::media::transport
