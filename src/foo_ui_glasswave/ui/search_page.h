#pragma once

#include "ui/page.h"
#include "ui/search_scope.h"

// The catalog search's results page (owner, 2026-11-15). Tabs above the results, as NetEase
// Cloud Music's search page has them, drawn as Media Center's large text tabs: All results (an
// overview after Apple Music's search page: the top results, then a row each of artists, albums,
// songs and playlists, each with a link to its own tab), Songs, Artists, Albums, Playlists and
// Lyrics. A tab shows only where a source in the scope has that kind of search.
//
// Every source at once mixes the results by rank (each source's first, then each one's second
// ...): songs get a Source column, covers a mark of their source on the corner.
namespace glasswave::ui {

View SearchView(const SearchScope& scope, std::wstring term, std::wstring tab = {});
SearchScope SearchViewScope(const View& view);
std::unique_ptr<Page> MakeSearchPage(PageHost& host, const View& view);

} // namespace glasswave::ui
