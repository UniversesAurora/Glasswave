#include "pch.h"
#include "media/special_playlists.h"
#include "media/favorites.h"
#include "media/playback_scheduler.h"

namespace glasswave::media {

namespace {

// {2C4F8E61-7B3A-4D52-9E17-5A0C3B6D8F24}: smart playlists created once
cfg_var_modern::cfg_int cfg_smartCreated(
    {0x2c4f8e61, 0x7b3a, 0x4d52, {0x9e, 0x17, 0x5a, 0x0c, 0x3b, 0x6d, 0x8f, 0x24}}, 0);

// Keeps a playlist from being removed or renamed; its content stays editable.
class ProtectLock : public playlist_lock {
  public:
    bool query_items_add(t_size, const pfc::list_base_const_t<metadb_handle_ptr>&,
                         const bit_array&) override {
        return true;
    }
    bool query_items_reorder(const t_size*, t_size) override { return true; }
    bool query_items_remove(const bit_array&, bool) override { return true; }
    bool query_item_replace(t_size, const metadb_handle_ptr&, const metadb_handle_ptr&) override {
        return true;
    }
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

} // namespace

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

bool IsQueue(t_size playlist) {
    return playlist != SIZE_MAX && playlist == QueuePlaylist(false);
}

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
namespace {
std::function<void()> g_playQueueListener;
} // namespace

void SetPlayQueueListener(std::function<void()> listener) {
    g_playQueueListener = std::move(listener);
}

namespace {
uint64_t g_pendingPlay = 0; // the request being read; 0: none
uint64_t g_pendingSerial = 0;
} // namespace

uint64_t BeginPendingPlay() {
    g_pendingPlay = ++g_pendingSerial;
    auto pc = playback_control::get();
    if (pc->is_playing()) pc->stop();
    return g_pendingPlay;
}

bool TakePendingPlay(uint64_t token) {
    if (token == 0 || token != g_pendingPlay) return false;
    g_pendingPlay = 0;
    return true;
}

void DropPendingPlay(uint64_t token) {
    if (token == g_pendingPlay) g_pendingPlay = 0;
}

bool PendingPlay() { return g_pendingPlay != 0; }

void CancelPendingPlay() { g_pendingPlay = 0; }

namespace {
// Playback that starts while a list is still being read to play was asked for after it (a media
// key, foobar2000's own Play or Next, a double click in another view): it supersedes the list.
// PlayQueue, which plays the list itself, has given up the request before it starts.
class PendingPlaySupersede : public play_callback_static {
  public:
    unsigned get_flags() override { return flag_on_playback_starting; }
    void on_playback_starting(play_control::t_track_command, bool) override { CancelPendingPlay(); }
    void on_playback_new_track(metadb_handle_ptr) override {}
    void on_playback_stop(play_control::t_stop_reason) override {}
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info&) override {}
    void on_playback_dynamic_info_track(const file_info&) override {}
    void on_playback_time(double) override {}
    void on_volume_change(float) override {}
};
play_callback_static_factory_t<PendingPlaySupersede> g_pendingPlaySupersede;
} // namespace

bool PlayQueue(const metadb_handle_list& items, size_t start) {
    if (items.get_count() == 0) return false;
    g_pendingPlay = 0; // whatever was being read to play, this is what plays now
    const t_size p = QueuePlaylist(true);
    Protect(p);
    if (PlaybackScheduler::Get().PlayQueue(items, start)) {
        if (g_playQueueListener) g_playQueueListener();
        return true;
    }
    console::print("Glasswave: PlayQueue request rejected; queue left unchanged.");
    return false;
}

void PlayNext(const metadb_handle_list& items) {
    if (items.get_count() == 0) return;
    const t_size p = QueuePlaylist(true);
    Protect(p);
    if (PlaybackScheduler::Get().PlayNext(items)) return;
    console::print("Glasswave: PlayNext request rejected; queue left unchanged.");
}

void AddToQueue(const metadb_handle_list& items) {
    if (items.get_count() == 0) return;
    const t_size p = QueuePlaylist(true);
    Protect(p);
    if (PlaybackScheduler::Get().AddToQueue(items)) return;
    console::print("Glasswave: AddToQueue request rejected; queue left unchanged.");
}

bool AppendSourceBatch(const metadb_handle_list& items, uint64_t expectedGeneration) {
    if (items.get_count() == 0) return false;
    auto pm = playlist_manager::get();
    Protect(QueuePlaylist(true));
    return PlaybackScheduler::Get().AppendSourceBatch(items, expectedGeneration);
}

bool SetQueuePlaybackMode(bool shuffle, int repeat) {
    return PlaybackScheduler::Get().SetPlaybackMode(shuffle, repeat);
}

bool QueueShuffleEnabled() {
    return PlaybackScheduler::Get().QueueShuffleEnabled();
}

int QueueRepeatMode() {
    return PlaybackScheduler::Get().QueueRepeatMode();
}

std::vector<size_t> ScheduledUpcomingIndices() {
    return PlaybackScheduler::Get().ScheduledUpcomingIndices();
}

std::vector<ScheduledUpcomingEntry> ScheduledUpcomingSnapshot() {
    return PlaybackScheduler::Get().ScheduledUpcomingSnapshot();
}

bool ReorderUpcomingSnapshot(uint64_t expectedRevision, const std::vector<ScheduledUpcomingEntry>& ordered) {
    return PlaybackScheduler::Get().ReorderUpcomingSnapshot(expectedRevision, ordered);
}

std::optional<size_t> ScheduledNextIndex() {
    return PlaybackScheduler::Get().ScheduledNextIndex();
}

uint64_t QueueScheduleRevision() {
    return PlaybackScheduler::Get().ScheduleRevision();
}

uint64_t QueueContentGeneration() {
    return PlaybackScheduler::Get().ContentGeneration();
}

} // namespace glasswave::media
