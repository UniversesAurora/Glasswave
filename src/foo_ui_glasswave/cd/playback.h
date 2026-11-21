#pragma once

#include <functional>

// Audio CDs in playback. A CD foobar2000 plays from a playlist of its own (File > Open audio CD) is
// taken over by the queue, that playlist goes, and the disc is tagged from MusicBrainz. When a disc
// leaves its drive, the queue drops its tracks. CD tracks are a media::PathBehaviour: they play
// only while their disc is in a drive and are looked up (lyrics) only once their disc is tagged.
namespace glasswave::cd {

// Main thread: a disc may have left a drive (WM_DEVICECHANGE). `checked` runs on the main
// thread once the drives are read (the play history may show fewer CD tracks).
void DropEjectedDiscs(std::function<void()> checked = {});

} // namespace glasswave::cd
