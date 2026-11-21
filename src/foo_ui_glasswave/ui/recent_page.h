#pragma once

#include "ui/page.h"

// All Music > Recently Played (owner, 2026-11-15): tabs over the list, as on the search page:
// Songs (the play history), Albums (the albums of those songs, the latest first) and Playlists
// (the playlists played from their pages or play buttons, media/recent_lists.h).
namespace glasswave::ui {

std::unique_ptr<Page> MakeRecentPage(PageHost& host, const View& view);

} // namespace glasswave::ui
