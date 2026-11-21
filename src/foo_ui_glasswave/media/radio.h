#pragma once

#include "stream/provider.h"

#include <functional>
#include <string>

// Radio mode (owner, 2026-11-15): a station - Apple Music's stations, NetEase's personal FM and
// heartbeat mode - plays in the queue (Now Playing) as a list without an end. Only the songs the
// service has sent so far are queued, two ahead of the one playing; more are asked for as those
// start, so what comes later is never shown. Songs already played stay above the playing one, so
// Previous goes back through them, never before the first. Shuffle and repeating the list do not
// apply (the window greys them); repeating one song does. A station that takes "recommend less"
// (NetEase's 不感兴趣) has it in the shuffle button's place: the song playing is reported and the
// next one plays. Playing anything else into the queue ends the station.
namespace glasswave::media::radio {

// Main thread. Replaces the queue with the station's first songs.
void Play(stream::Provider& provider, const stream::Id& station, std::wstring name);
bool Active();
std::wstring Name();
bool CanDislike(); // the station playing takes "recommend less"
// Previous does something: always off a station; on one, unless its first song plays and the
// station does not go back from there (Provider::StationBacksAtStart).
bool CanGoBack();
void DislikeCurrent();
// Called (main thread) when a station starts or ends, and as its songs move on.
int Subscribe(std::function<void()> callback);
void Unsubscribe(int token);

} // namespace glasswave::media::radio
