#pragma once

#include "stream/provider.h"
#include "ui/page.h"

// The pages of a streaming provider's library and the combined All Music > Favorites.
namespace glasswave::ui {

// The page for a Remote view (nullptr for a view that has no page of its own).
std::unique_ptr<Page> MakeRemotePage(PageHost& host, stream::Provider& provider, const View& view);
// All Music > Favorites: the local Favorites playlist and every provider's favorites.
std::unique_ptr<Page> MakeAllFavoritesPage(PageHost& host, const View& view);

// Views of a provider's pages, for the navigation pane, breadcrumb and links.
View RemoteView(const stream::Provider& provider, View::Kind kind);
View SectionView(const stream::Provider& provider, const stream::NavigationSection& section);
View GroupTracksView(const stream::Provider& provider, const stream::NavigationSection& section,
                     const stream::BrowseGroup& group);
View ShelfView(const stream::Provider& provider, stream::Shelf shelf, stream::Id sectionId = {},
               std::wstring sectionLabel = {});
View PlaylistView(const stream::Provider& provider, const stream::Id& id, std::wstring name,
                  std::optional<stream::Shelf> shelf, stream::Id sectionId = {});
View ArtistView(const stream::Provider& provider, const stream::Id& id, std::wstring name,
                stream::Id sectionId = {});
View AlbumView(const stream::Provider& provider, const stream::Id& id, std::wstring name, const stream::Id& artistId = {},
               std::wstring artistName = {}, stream::Id sectionId = {});
View TopTracksView(const stream::Provider& provider, const stream::Id& artistId, std::wstring artistName,
                   stream::Id sectionId = {});
View CatalogSearchView(const stream::Provider& provider, std::wstring query);
const wchar_t* ShelfLabel(stream::Shelf shelf);

// Drops provider-owned page state kept for Back/Forward after account or language changes.
void InvalidateRemotePageCache(std::wstring_view provider);

// Asks for a catalog query and opens its results page. The title and action label use the
// provider's display name rather than an Apple-specific string.
void PromptCatalogSearch(PageHost& host, stream::Provider& provider);

// Playlist commands shared by pages, tiles and the navigation pane.
// The context menu of one playlist (tile or navigation row); onUnsaved runs after an
// uncollect succeeded.
void ShowRemotePlaylistMenu(PageHost& host, stream::Provider& provider, const stream::Id& id, const std::wstring& name,
                            std::optional<stream::Shelf> shelf, const stream::Id& sectionId, POINT screen,
                            std::function<void(const stream::Id&)> onUnsaved = {});
void CreateRemotePlaylist(PageHost& host, stream::Provider& provider);
void RenameRemotePlaylist(PageHost& host, stream::Provider& provider, const stream::Id& id, std::wstring name);
void DeleteRemotePlaylist(PageHost& host, stream::Provider& provider, const stream::Id& id, std::wstring name,
                          const stream::Id& sectionId = {});

}  // namespace glasswave::ui
