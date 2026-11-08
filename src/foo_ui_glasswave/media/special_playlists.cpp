#include "pch.h"
#include "media/special_playlists.h"
#include "media/favorites.h"

namespace glasswave::media {

namespace {

// {2C4F8E61-7B3A-4D52-9E17-5A0C3B6D8F24}: smart playlists created once
cfg_var_modern::cfg_int cfg_smartCreated({0x2c4f8e61, 0x7b3a, 0x4d52, {0x9e, 0x17, 0x5a, 0x0c, 0x3b, 0x6d, 0x8f, 0x24}}, 0);

// Keeps a playlist from being removed or renamed; its content stays editable.
class ProtectLock : public playlist_lock {
public:
    bool query_items_add(t_size, const pfc::list_base_const_t<metadb_handle_ptr>&, const bit_array&) override {
        return true;
    }
    bool query_items_reorder(const t_size*, t_size) override { return true; }
    bool query_items_remove(const bit_array&, bool) override { return true; }
    bool query_item_replace(t_size, const metadb_handle_ptr&, const metadb_handle_ptr&) override { return true; }
    bool query_playlist_rename(const char*, t_size) override { return false; }
    bool query_playlist_remove() override { return false; }
    bool execute_default_action(t_size) override { return false; }
    void on_playlist_index_change(t_size) override {}
    void on_playlist_remove() override {}
    void get_lock_name(pfc::string_base& out) override { out = "Glasswave (built-in list)"; }
    void show_ui() override {}
    t_uint32 get_filter_mask() override { return filter_rename | filter_remove_playlist; }
};

void Protect(t_size playlist) {
    auto pm = playlist_manager::get();
    if (playlist == SIZE_MAX || pm->playlist_lock_is_present(playlist)) return;
    pm->playlist_lock_install(playlist, fb2k::service_new<ProtectLock>());
}

}  // namespace

const std::vector<SmartPreset>& SmartPresets() {
    // Fields from foobar2000's Playback Statistics; queries use its SORT syntax.
    static const std::vector<SmartPreset> presets = {
        {"Library (Full)", "ALL", false, 0},
        {"Never Played", "NOT %play_count% GREATER 0", true, 0},
        {"Recently Played", "%last_played% DURING LAST 1 WEEK SORT DESCENDING BY %last_played%", true, 0},
        {"Most Played", "%play_count% GREATER 1 SORT DESCENDING BY %play_count%", true, 0},
        {"Recently Added", "%added% DURING LAST 4 WEEKS SORT DESCENDING BY %added%", true, 0},
        {"Favorites", nullptr, true, 1},
        {"Rated 5 Stars", "%rating% IS 5", false, 2},
        {"Rated 4 Stars", "%rating% IS 4", false, 2},
        {"Rated 3 Stars", "%rating% IS 3", false, 2},
        {"Rated 2 Stars", "%rating% IS 2", false, 2},
        {"Rated 1 Star", "%rating% IS 1", false, 2},
        {"Unrated", "NOT %rating% PRESENT", false, 2},
    };
    return presets;
}

t_size QueuePlaylist(bool create) {
    auto pm = playlist_manager::get();
    return create ? pm->find_or_create_playlist(kQueueName) : pm->find_playlist(kQueueName);
}

bool IsQueue(t_size playlist) { return playlist != SIZE_MAX && playlist == QueuePlaylist(false); }

bool IsSmart(t_size playlist) {
    if (playlist == SIZE_MAX) return false;
    try {
        return autoplaylist_manager::get()->is_client_present(playlist);
    } catch (...) {
        return false;
    }
}

bool IsAutoPlaylist(t_size playlist) {
    if (playlist == SIZE_MAX) return false;
    if (IsSmart(playlist)) return true;
    // While foobar2000 restores auto playlists, the lock is installed before the client is
    // registered. The lock blocks item edits; our own queue / Favorites lock does not.
    auto pm = playlist_manager::get();
    if (!pm->playlist_lock_is_present(playlist)) return false;
    const t_uint32 mask = pm->playlist_lock_get_filter_mask(playlist);
    return (mask & (playlist_lock::filter_add | playlist_lock::filter_remove | playlist_lock::filter_reorder |
                    playlist_lock::filter_replace)) != 0;
}

bool IsSpecial(t_size playlist) {
    if (playlist == SIZE_MAX) return false;
    return IsQueue(playlist) || playlist == FavoritesPlaylist(false) || IsAutoPlaylist(playlist);
}

t_size AddSmartPreset(const SmartPreset& preset) {
    if (!preset.query) return FavoritesPlaylist(true);
    auto pm = playlist_manager::get();
    if (const t_size existing = pm->find_playlist(preset.name); existing != SIZE_MAX) return existing;
    const t_size p = pm->create_playlist(preset.name, SIZE_MAX, SIZE_MAX);
    if (p == SIZE_MAX) return SIZE_MAX;
    try {
        autoplaylist_manager::get()->add_client_simple(preset.query, "", p, 0);
    } catch (const std::exception& e) {
        console::print("Glasswave: smart playlist \"", preset.name, "\" failed: ", e.what());
    }
    return p;
}

t_size NewSmartPlaylist() {
    auto pm = playlist_manager::get();
    const t_size p = pm->create_playlist("New Smart Playlist", SIZE_MAX, SIZE_MAX);
    if (p == SIZE_MAX) return p;
    try {
        auto apm = autoplaylist_manager::get();
        apm->add_client_simple("ALL", "", p, 0);
        if (auto client = apm->query_client(p); client.is_valid()) client->show_ui(p);
    } catch (const std::exception& e) {
        popup_message::g_show(e.what(), "Smart playlist");
    }
    return p;
}

void EnsureSpecialPlaylists(bool restore) {
    Protect(QueuePlaylist(true));
    Protect(FavoritesPlaylist(true));
    if (cfg_smartCreated.get() && !restore) return;
    cfg_smartCreated = 1;
    for (const auto& preset : SmartPresets())
        if (preset.byDefault) AddSmartPreset(preset);
}
void PlayQueue(const metadb_handle_list& items, size_t start) {
    if (items.get_count() == 0) return;
    auto pm = playlist_manager::get();
    const t_size p = QueuePlaylist(true);
    Protect(p);
    pm->playlist_undo_backup(p);
    pm->playlist_clear(p);
    pm->playlist_add_items(p, items, pfc::bit_array_false());
    pm->set_playing_playlist(p);
    const t_size at = std::min<t_size>(start, items.get_count() - 1);
    pm->playlist_set_focus_item(p, at);
    pm->playlist_execute_default_action(p, at);
}

void PlayNext(const metadb_handle_list& items) {
    if (items.get_count() == 0) return;
    auto pm = playlist_manager::get();
    const t_size p = QueuePlaylist(true);
    t_size playlist = SIZE_MAX, index = SIZE_MAX;
    t_size at = 0;
    if (pm->get_playing_item_location(&playlist, &index) && playlist == p) at = index + 1;
    pm->playlist_undo_backup(p);
    pm->playlist_insert_items(p, at, items, pfc::bit_array_false());
    if (!playback_control::get()->is_playing()) {
        pm->set_playing_playlist(p);
        pm->playlist_execute_default_action(p, at);
    }
}

void AddToQueue(const metadb_handle_list& items) {
    if (items.get_count() == 0) return;
    auto pm = playlist_manager::get();
    const t_size p = QueuePlaylist(true);
    pm->playlist_undo_backup(p);
    pm->playlist_add_items(p, items, pfc::bit_array_false());
}

}  // namespace glasswave::media
