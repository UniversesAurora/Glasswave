#pragma once

#include <functional>

// Playback that foobar2000 starts on its own, outside Glasswave's lists, joins the queue:
//  - a file opened from Explorer ("Open with"): foobar2000 replaces the active playlist (the
//    queue, see MainWindow::KeepQueueActive) with it and plays it; the queue then becomes the
//    whole folder around it, the song playing on.
//  - an audio CD (File > Open audio CD): foobar2000 plays it from a playlist of its own; the
//    queue takes the disc over and that playlist goes. The disc is tagged from MusicBrainz.
// When a disc leaves its drive, the queue drops its tracks.
namespace glasswave::media {

// Main thread: a disc may have left a drive (WM_DEVICECHANGE). `checked` runs on the main
// thread once the drives are read (the play history may show fewer CD tracks).
void DropEjectedDiscs(std::function<void()> checked = {});

} // namespace glasswave::media
