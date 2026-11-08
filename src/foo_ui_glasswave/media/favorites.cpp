#include "pch.h"
#include "media/favorites.h"

namespace glasswave::media {

t_size FavoritesPlaylist(bool create) {
    auto pm = playlist_manager::get();
    t_size p = pm->find_playlist(kFavoritesName);
    if (p == SIZE_MAX && create) p = pm->create_playlist(kFavoritesName, SIZE_MAX, SIZE_MAX);
    return p;
}

bool IsFavorite(const metadb_handle_ptr& track) {
    const t_size p = FavoritesPlaylist(false);
    if (p == SIZE_MAX || !track.is_valid()) return false;
    metadb_handle_list items;
    playlist_manager::get()->playlist_get_all_items(p, items);
    for (t_size i = 0; i < items.get_count(); ++i)
        if (items[i] == track) return true;
    return false;
}

void ToggleFavorite(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return;
    auto pm = playlist_manager::get();
    const t_size p = FavoritesPlaylist(true);
    if (p == SIZE_MAX) return;
    metadb_handle_list items;
    pm->playlist_get_all_items(p, items);
    for (t_size i = 0; i < items.get_count(); ++i)
        if (items[i] == track) {
            pm->playlist_remove_items(p, pfc::bit_array_one(i));
            return;
        }
    metadb_handle_list add;
    add.add_item(track);
    pm->playlist_add_items(p, add, pfc::bit_array_false());
}

}  // namespace glasswave::media
