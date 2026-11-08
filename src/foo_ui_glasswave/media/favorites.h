#pragma once

// "Favorites" is an ordinary foobar2000 playlist that the Favorite column, the taskbar
// button and menus add songs to and remove them from.
namespace glasswave::media {

inline constexpr const char* kFavoritesName = "Favorites";

// Optional provider dispatcher for tracks that are not local files (NetEase ncm://
// handles). The hook runs on the main thread.
class FavoriteHook {
public:
    virtual ~FavoriteHook() = default;
    virtual bool Handles(const metadb_handle_ptr& track) const = 0;
    virtual bool IsFavorite(const metadb_handle_ptr& track) const = 0;
    virtual void Toggle(const metadb_handle_ptr& track) = 0;
    virtual void Set(const metadb_handle_ptr& track, bool favorite) = 0;
};
void SetFavoriteHook(FavoriteHook* hook);

t_size FavoritesPlaylist(bool create);  // SIZE_MAX when missing and !create
bool IsFavorite(const metadb_handle_ptr& track);
void ToggleFavorite(const metadb_handle_ptr& track);
bool AllFavorite(metadb_handle_list_cref tracks);
void SetFavorite(metadb_handle_list_cref tracks, bool favorite);  // add or remove all

}  // namespace glasswave::media
