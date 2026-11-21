#pragma once

// Playback that foobar2000 starts on its own, outside Glasswave's lists, joins the queue: a file
// opened from Explorer ("Open with") replaces the active playlist (the queue, see
// MainWindow::KeepQueueActive) and plays; the queue then becomes the whole folder around it, the
// song playing on. An audio CD foobar2000 plays from its own playlist is cd/playback.h's.
// Nothing to call: it listens from the component's start.
