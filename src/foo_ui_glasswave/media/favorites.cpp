#include "pch.h"
#include "media/favorites.h"
#include "stream/provider.h"

#include <unordered_set>

namespace glasswave::media {

// A streaming provider's tracks are favorites in that provider's own library (its liked
// list); local files use the Favorites playlist.

t_size FavoritesPlaylist(bool create) {
    auto pm = playlist_manager::get();
    t_size p = pm->find_playlist(kFavoritesName);
    if (p == SIZE_MAX && create) p = pm->create_playlist(kFavoritesName, SIZE_MAX, SIZE_MAX);
    return p;
}

namespace {

// The local songs in the Favorites playlist. List views ask once per painted row and a sort by
// the Favorite column once per song, so one read serves until the current main-thread task
// ends; SetFavorite drops it before it changes the playlist, whose callbacks may paint at once.
class LocalFavorites {
  public:
    static bool Contains(const metadb_handle_ptr& track) {
        if (!s_read) {
            if (const t_size p = FavoritesPlaylist(false); p != SIZE_MAX)
                playlist_manager::get()->playlist_get_all_items(p, s_items);
            for (t_size i = 0; i < s_items.get_count(); ++i) s_set.insert(s_items[i].get_ptr());
            s_read = true;
            fb2k::inMainThread([] { Drop(); });
        }
        return s_set.count(track.get_ptr()) != 0;
    }
    static void Drop() {
        s_read = false;
        s_set.clear();
        s_items.remove_all();
    }

  private:
    static inline bool s_read = false;
    static inline metadb_handle_list s_items; // keeps the set's handles alive while it is read
    static inline std::unordered_set<const metadb_handle*> s_set;
};

} // namespace

bool IsFavorite(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return false;
    if (auto* provider = stream::ProviderOf(track)) return provider->IsFavorite(provider->TrackId(track));
    return LocalFavorites::Contains(track);
}

void ToggleFavorite(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return;
    SetFavorite(pfc::list_single_ref_t<metadb_handle_ptr>(track), !IsFavorite(track));
}

bool AllFavorite(metadb_handle_list_cref tracks) {
    if (tracks.get_count() == 0) return false;
    for (t_size i = 0; i < tracks.get_count(); ++i)
        if (!IsFavorite(tracks[i])) return false;
    return true;
}

void SetFavorite(metadb_handle_list_cref tracks, bool favorite) {
    auto pm = playlist_manager::get();
    metadb_handle_list local;
    for (t_size i = 0; i < tracks.get_count(); ++i) {
        const auto& track = tracks[i];
        if (!track.is_valid()) continue;
        if (auto* provider = stream::ProviderOf(track)) {
            const stream::Id id = provider->TrackId(track);
            if (provider->CanSetFavorite(favorite) && provider->IsFavorite(id) != favorite)
                provider->SetFavorite(id, favorite);
        } else {
            local.add_item(track);
        }
    }
    if (local.get_count() == 0) return;
    LocalFavorites::Drop();
    const t_size p = FavoritesPlaylist(favorite);
    if (p == SIZE_MAX) return;
    metadb_handle_list items;
    pm->playlist_get_all_items(p, items);
    if (favorite) {
        metadb_handle_list add;
        for (t_size i = 0; i < local.get_count(); ++i)
            if (items.find_item(local[i]) == SIZE_MAX && add.find_item(local[i]) == SIZE_MAX) add.add_item(local[i]);
        if (add.get_count()) pm->playlist_add_items(p, add, pfc::bit_array_false());
        return;
    }
    pfc::bit_array_bittable remove(items.get_count());
    bool any = false;
    for (t_size i = 0; i < items.get_count(); ++i)
        if (local.find_item(items[i]) != SIZE_MAX) {
            remove.set(i, true);
            any = true;
        }
    if (any) pm->playlist_remove_items(p, remove);
}

}  // namespace glasswave::media
