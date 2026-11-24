#pragma once

#include <memory>
#include <string>

namespace glasswave::lyrics {
class Searcher;
}

namespace glasswave::applemusic::lyrics_source {

// Ordered BCP-47 locale priority list for Apple Music lyrics requests (e.g. "zh-Hans-CN,en-US").
std::wstring EffectiveLyricsLocale();

// The Apple Music catalog searcher, for any song (StreamProvider::LyricsSearchers).
std::shared_ptr<lyrics::Searcher> MakeSearcher();

} // namespace glasswave::applemusic::lyrics_source
