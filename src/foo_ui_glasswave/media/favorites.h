#pragma once

// "Favorites" is an ordinary foobar2000 playlist that the Favorite column, the taskbar
// button and menus add songs to and remove them from.
namespace glasswave::media {

inline constexpr const char* kFavoritesName = "Favorites";

t_size FavoritesPlaylist(bool create);  // SIZE_MAX when missing and !create
bool IsFavorite(const metadb_handle_ptr& track);
void ToggleFavorite(const metadb_handle_ptr& track);

}  // namespace glasswave::media
