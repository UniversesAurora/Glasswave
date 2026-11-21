#pragma once

#include <memory>

namespace glasswave::lyrics {
class Searcher;
}

namespace glasswave::applemusic::lyrics_source {

// The Apple Music catalog searcher, for any song (StreamProvider::LyricsSearchers).
std::shared_ptr<lyrics::Searcher> MakeSearcher();

} // namespace glasswave::applemusic::lyrics_source
