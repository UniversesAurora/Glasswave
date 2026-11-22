#include "pch.h"
#include "media/playback_scheduler_impl.h"
#include "media/radio.h"

namespace glasswave::media {

using namespace scheduler;

namespace scheduler {

cfg_var_modern::cfg_int cfg_shuffle(
    {0x6b42d913, 0x0f86, 0x4aa7, {0x9c, 0x35, 0x8a, 0x2f, 0x3d, 0x1e, 0x70, 0x54}}, 0);

cfg_var_modern::cfg_int cfg_repeat(
    {0x6b42d914, 0x0f86, 0x4aa7, {0x9c, 0x35, 0x8a, 0x2f, 0x3d, 0x1e, 0x70, 0x54}}, 0);

cfg_var_modern::cfg_string cfg_checkpoint(
    {0x6b42d915, 0x0f86, 0x4aa7, {0x9c, 0x35, 0x8a, 0x2f, 0x3d, 0x1e, 0x70, 0x54}}, "");

} // namespace scheduler

namespace {

std::unique_ptr<class PlaybackScheduler> g_scheduler;
bool g_schedulerStopped = false;
bool g_schedulerStarting = false;
bool g_probeEnabled = false;
std::optional<std::size_t> g_probeCurrentIndex;

bool NameContains(const char* name, const char* text) {
    if (!name || !text) return false;
    const auto lower = [](char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
    };
    const std::string_view source(name);
    const std::string_view wanted(text);
    if (wanted.empty() || wanted.size() > source.size()) return false;
    for (std::size_t i = 0; i + wanted.size() <= source.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < wanted.size(); ++j) {
            if (lower(source[i + j]) != lower(wanted[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

std::size_t CurrentQueueIndex(std::size_t count, bool prefer_focus) {
    if (!count) return SIZE_MAX;
    if (g_probeEnabled && g_probeCurrentIndex && *g_probeCurrentIndex < count) return *g_probeCurrentIndex;
    auto manager = playlist_manager::get();
    const t_size queue = QueuePlaylist(false);
    t_size playlist = SIZE_MAX, index = SIZE_MAX;
    if (queue != SIZE_MAX && manager->get_playing_item_location(&playlist, &index) && playlist == queue &&
        index < count) {
        return index;
    }
    if (prefer_focus && queue != SIZE_MAX) {
        index = manager->playlist_get_focus_item(queue);
        if (index < count) return index;
    }
    return SIZE_MAX;
}

std::size_t OrderIndex(const char* name) {
    auto manager = playlist_manager::get();
    for (t_size i = 0, count = manager->playback_order_get_count(); i < count; ++i)
        if (pfc::stricmp_ascii(manager->playback_order_get_name(i), name) == 0) return i;
    return SIZE_MAX;
}

std::string UpcomingTokenKey(ScheduledUpcomingSource source, std::size_t index) {
    std::string key = source == ScheduledUpcomingSource::CoreQueue ? "C" : "P";
    AppendLe64(key, index);
    return key;
}

} // namespace


namespace {

struct ProbeHostSnapshot {
    PlaybackScheduleState scheduleState;
    std::uint64_t scheduleSeed = 0;
    std::vector<PlaybackOccurrenceId> physical;
    std::map<PlaybackOccurrenceId, metadb_handle_ptr> handles;
    std::vector<t_playback_queue_item> ownedCoreQueue;
    std::uint64_t nextOccurrence = 1;
    std::uint64_t contentGeneration = 0;
    std::string savedFingerprint;
    PlaybackScheduleState savedState;
    std::string savedCheckpoint;
    std::string cfgCheckpoint;
    int cfgShuffle = 0;
    int cfgRepeat = 0;
    bool checkpointDirty = false;
    bool checkpointValid = false;
    bool modelInitialized = false;
    std::optional<std::size_t> probeCurrentIndex;
};

} // namespace

PlaybackScheduler::Impl::Impl() : schedule(NextSeed()) {}

PlaybackScheduler::Impl::~Impl() = default;

void PlaybackScheduler::Impl::Start() {
    PFC_ASSERT(core_api::is_main_thread());
    if (started) return;
    started = true;
    lifetime = std::make_shared<int>(0); // Shutdown ended the previous one's posts
    const int repeat = std::clamp<int>(static_cast<int>(cfg_repeat.get()), 0, 2);
    schedule.SetMode(cfg_shuffle.get() != 0, static_cast<PlaybackRepeatMode>(repeat));
    playlistEvents = std::make_unique<PlaylistEvents>(*this);
    Synchronize();
    ApplyCoreOrder();
}

void PlaybackScheduler::Impl::Shutdown() {
    PFC_ASSERT(core_api::is_main_thread());
    if (!started) return;
    SaveCheckpoint(true);
    InvalidatePosted();
    started = false;
    lifetime.reset();
    modelInitialized = false;
    playlistEvents.reset();
    physical.clear();
    physicalIndex.clear();
    handles.clear();
    ownedCoreQueue.clear();
    listeners.Clear();
    listenerNotifyQueued = false;
}

bool PlaybackScheduler::Impl::SetPlaybackMode(bool shuffle, int repeat) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!started || !IsValidRepeat(repeat)) return false;
    const auto mode = static_cast<PlaybackRepeatMode>(repeat);
    const bool changed = schedule.shuffle_enabled() != shuffle || schedule.repeat_mode() != mode;
    if (!schedule.SetMode(shuffle, mode)) return false;
    cfg_shuffle = shuffle ? 1 : 0;
    cfg_repeat = repeat;
    if (changed) {
        checkpointDirty = true;
        if (modelInitialized) ApplyProjection();
        SaveCheckpoint();
    }
    ApplyCoreOrder();
    if (changed) RequestListenerNotification();
    return true;
}

void PlaybackScheduler::Impl::EnsureModel() {
    if (modelInitialized) return;
    const std::size_t current = EnsureSidecar();
    const t_size queue = QueuePlaylist(false);
    metadb_handle_list items;
    if (queue != SIZE_MAX) playlist_manager::get()->playlist_get_all_items(queue, items);
    const bool loaded = LoadCheckpoint(items);
    if (!loaded) {
        const auto canonical = physical;
        const auto ordered = physical;
        const std::size_t at = current == SIZE_MAX ? 0 : current;
        schedule.Restore(std::span<const PlaybackOccurrenceId>(canonical),
                         std::span<const PlaybackOccurrenceId>(ordered), at, NextSeed());
    } else if (current != SIZE_MAX && current < physical.size() && !schedule.shuffle_enabled() &&
               !schedule.HasPriority() && schedule.Current() != physical[current]) {
        // Keep history and future aligned around foobar2000's actual resumed row when a
        // linear/radio checkpoint was behind the last played track.
        if (schedule.StartExplicit(std::span<const PlaybackOccurrenceId>(physical), current))
            checkpointDirty = true;
    }
    modelInitialized = true;
    if (loaded) ApplyProjection();
}

// The occurrences' physical order, and the index from occurrence to row that follows it.
void PlaybackScheduler::Impl::SetPhysical(std::vector<PlaybackOccurrenceId> order) {
    physical = std::move(order);
    ReindexPhysical();
}

void PlaybackScheduler::Impl::ReindexPhysical() {
    physicalIndex.clear();
    for (std::size_t i = 0; i < physical.size(); ++i)
        physicalIndex[physical[i]] = i;
}

// A fresh source of `items` played from `start`: the sidecar, the schedule and the physical
// order begin anew (PlayQueue, AdoptPlaying). Returns the items' occurrences in their order.
std::vector<PlaybackOccurrenceId> PlaybackScheduler::Impl::ResetModelFresh(const metadb_handle_list& items,
                                                                           std::size_t start) {
    physical.clear();
    physicalIndex.clear();
    handles.clear();
    modelInitialized = false;
    auto occurrences = AllocateOccurrences(items);
    schedule.ResetFresh(std::span<const PlaybackOccurrenceId>(occurrences), start, NextSeed());
    SetPhysical(schedule.ProjectionOrder());
    modelInitialized = true;
    ++contentGeneration;
    checkpointDirty = true;
    return occurrences;
}

bool PlaybackScheduler::Impl::PlayQueue(const metadb_handle_list& items, std::size_t start) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!started || items.get_count() == 0) return false;

    if (!RemoveOwnedCoreQueueEntries())
        console::print("Glasswave: stale explicit queue rows could not be removed.");
    InvalidatePosted();
    ResetModelFresh(items, std::min(start, items.get_count() - 1));

    const auto ordered = HandlesFor(physical);
    auto manager = playlist_manager::get();
    const t_size queue = QueuePlaylist(true);
    if (queue == SIZE_MAX) return false;
    mapGuard = true;
    manager->playlist_undo_backup(queue);
    manager->playlist_clear(queue);
    const bool inserted = manager->playlist_add_items(queue, ordered, pfc::bit_array_false());
    mapGuard = false;
    if (!inserted) {
        ScheduleSynchronize();
        return false;
    }

    const auto current = schedule.Current();
    const auto currentIt = current ? physicalIndex.find(*current) : physicalIndex.end();
    if (currentIt == physicalIndex.end()) return false;
    if (!StartOccurrence(*current)) return false;
    ApplyCoreOrder();
    SaveCheckpoint();
    RequestListenerNotification();
    return true;
}

// PlayQueue without starting anything: the song playing from the queue stays where it is (its
// row is kept, so foobar2000's playing location stays valid) and becomes `items[start]` of a
// fresh source, the other songs placed around it.
bool PlaybackScheduler::Impl::AdoptPlaying(const metadb_handle_list& items, std::size_t start) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!started || start >= items.get_count()) return false;
    auto manager = playlist_manager::get();
    const t_size queue = QueuePlaylist(false);
    t_size playingList = SIZE_MAX, playingRow = SIZE_MAX;
    if (queue == SIZE_MAX || !manager->get_playing_item_location(&playingList, &playingRow) ||
        playingList != queue || manager->playlist_get_item_handle(queue, playingRow) != items[start])
        return false;

    if (!RemoveOwnedCoreQueueEntries())
        console::print("Glasswave: stale explicit queue rows could not be removed.");
    InvalidatePosted();
    CancelSynchronize();
    wholeReplaceDirty = false; // the outside replacement this adopts is settled here
    const auto occurrences = ResetModelFresh(items, start);

    const auto ordered = HandlesFor(physical);
    const std::size_t at = physicalIndex[occurrences[start]];
    metadb_handle_list before, after;
    for (std::size_t i = 0; i < ordered.get_count(); ++i)
        if (i < at)
            before.add_item(ordered[i]);
        else if (i > at)
            after.add_item(ordered[i]);
    const t_size count = manager->playlist_get_item_count(queue);
    mapGuard = true;
    manager->playlist_undo_backup(queue);
    pfc::bit_array_bittable others(count);
    for (t_size i = 0; i < count; ++i)
        others.set(i, i != playingRow);
    manager->playlist_remove_items(queue, others);
    manager->playlist_insert_items(queue, 0, before, pfc::bit_array_false());
    manager->playlist_add_items(queue, after, pfc::bit_array_false());
    mapGuard = false;
    manager->playlist_set_focus_item(queue, at);

    ApplyCoreOrder();
    ApplyProjection();
    SaveCheckpoint();
    RequestListenerNotification();
    return true;
}

bool PlaybackScheduler::Impl::PlayNext(const metadb_handle_list& items) {
    PFC_ASSERT(core_api::is_main_thread());
    return InsertNewOccurrences(items, true);
}

bool PlaybackScheduler::Impl::AddToQueue(const metadb_handle_list& items) {
    PFC_ASSERT(core_api::is_main_thread());
    return InsertNewOccurrences(items, false);
}

bool PlaybackScheduler::Impl::AppendSourceBatch(const metadb_handle_list& items,
                                                std::uint64_t expectedGeneration) {
    PFC_ASSERT(core_api::is_main_thread());
    return InsertNewOccurrences(items, false, expectedGeneration);
}

bool PlaybackScheduler::Impl::InsertNewOccurrences(const metadb_handle_list& items, bool playNext,
                                                   std::optional<std::uint64_t> expectedGeneration) {
    if (!started || items.get_count() == 0 ||
        (expectedGeneration && *expectedGeneration != contentGeneration)) {
        return false;
    }
    CancelSynchronize();
    EnsureSidecar();
    EnsureModel();
    const t_size queue = QueuePlaylist(false);
    if (queue == SIZE_MAX) return false;

    const auto scheduleBefore = schedule.Snapshot();
    const auto seedBefore = schedule.seed();
    const auto physicalBefore = physical;
    const auto ownedBefore = ownedCoreQueue;
    std::vector<t_playback_queue_item> queueBefore = CoreQueueSnapshot();
    const auto checkpointDirtyBefore = checkpointDirty;
    const auto nextOccurrenceBefore = nextOccurrence;
    const auto probeIndexBefore = g_probeCurrentIndex;
    metadb_handle_list playlistBefore;
    playlist_manager::get()->playlist_get_all_items(queue, playlistBefore);
    const t_size focusBefore = playlist_manager::get()->playlist_get_focus_item(queue);
    pfc::bit_array_bittable selectionBefore(playlistBefore.get_count());
    playlist_manager::get()->playlist_get_selection_mask(queue, selectionBefore);
    std::vector<bool> selectedBefore;
    selectedBefore.reserve(playlistBefore.get_count());
    for (std::size_t i = 0; i < playlistBefore.get_count(); ++i)
        selectedBefore.push_back(selectionBefore[i]);

    const auto occurrences = AllocateOccurrences(items);
    const auto rollback = [&] {
        mapGuard = true;
        try {
            auto manager = playlist_manager::get();
            manager->playlist_undo_backup(queue);
            manager->playlist_clear(queue);
            manager->playlist_add_items(queue, playlistBefore, pfc::bit_array_false());
            pfc::bit_array_bittable selected(playlistBefore.get_count());
            for (std::size_t i = 0; i < selectedBefore.size(); ++i)
                selected.set(i, selectedBefore[i]);
            manager->playlist_set_selection(queue, pfc::bit_array_true(), selected);
            manager->playlist_set_focus_item(queue, focusBefore);
        } catch (...) {
        }
        mapGuard = false;
        schedule.RestoreSnapshot(scheduleBefore, seedBefore);
        SetPhysical(physicalBefore);
        for (const auto occurrence : occurrences)
            handles.erase(occurrence);
        nextOccurrence = nextOccurrenceBefore;
        ownedCoreQueue = ownedBefore;
        RebuildCoreQueue(queueBefore);
        checkpointDirty = checkpointDirtyBefore;
        g_probeCurrentIndex = probeIndexBefore;
    };

    mapGuard = true;
    playlist_manager::get()->playlist_undo_backup(queue);
    const bool inserted = playlist_manager::get()->playlist_add_items(queue, items, pfc::bit_array_false());
    mapGuard = false;
    if (!inserted) {
        rollback();
        return false;
    }

    physical.insert(physical.end(), occurrences.begin(), occurrences.end());
    for (std::size_t i = physicalBefore.size(); i < physical.size(); ++i)
        physicalIndex[physical[i]] = i;
    const bool committed = playNext
                               ? schedule.AddPlayNext(std::span<const PlaybackOccurrenceId>(occurrences))
                               : schedule.AddOrdinary(std::span<const PlaybackOccurrenceId>(occurrences));
    if (!committed || !ApplyProjection()) {
        rollback();
        return false;
    }
    if (playNext && !PrependCoreQueue(std::span<const PlaybackOccurrenceId>(occurrences))) {
        console::print("Glasswave: PlayNext could not prepend the explicit queue safely.");
        rollback();
        return false;
    }
    if (playNext && !g_probeEnabled && !playback_control::get()->is_playing()) {
        StartOccurrence(occurrences.front());
        const auto index = physicalIndex.find(occurrences.front());
        const auto handle = handles.find(occurrences.front());
        if (index != physicalIndex.end() && handle != handles.end())
            RemoveCoreQueueItem({handle->second, queue, index->second});
    }
    checkpointDirty = true;
    SaveCheckpoint();
    RequestListenerNotification();
    return true;
}

std::vector<std::size_t> PlaybackScheduler::Impl::ScheduledUpcomingIndices() {
    PFC_ASSERT(core_api::is_main_thread());
    if (!modelInitialized) return {};
    std::vector<std::size_t> out;
    const auto upcoming = schedule.Upcoming();
    out.reserve(upcoming.size());
    for (const auto occurrence : upcoming) {
        const auto found = physicalIndex.find(occurrence);
        if (found != physicalIndex.end()) out.push_back(found->second);
    }
    return out;
}

std::vector<ScheduledUpcomingEntry> PlaybackScheduler::Impl::ScheduledUpcomingSnapshot(bool display) {
    PFC_ASSERT(core_api::is_main_thread());
    std::vector<ScheduledUpcomingEntry> out;
    const auto queue = CoreQueueSnapshot();
    const auto upcoming = modelInitialized ? (display ? schedule.DisplayUpcoming() : schedule.Upcoming())
                                           : std::vector<PlaybackOccurrenceId>{};
    out.reserve(queue.size() + upcoming.size());
    std::unordered_set<std::size_t> coveredPhysical;
    const t_size queuePlaylist = QueuePlaylist(false);
    auto owned = BucketQueueItems(ownedCoreQueue);
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const auto& item = queue[i];
        const bool isOwned = TakeMatchingQueueItem(owned, item);
        out.push_back(
            {ScheduledUpcomingSource::CoreQueue, i, isOwned ? item.m_item : SIZE_MAX, item.m_handle});
        if (item.m_playlist == queuePlaylist && item.m_item < physical.size())
            coveredPhysical.insert(item.m_item);
    }
    if (!modelInitialized) return out;
    for (const auto occurrence : upcoming) {
        const auto index = physicalIndex.find(occurrence);
        const auto handle = handles.find(occurrence);
        if (index != physicalIndex.end() && !coveredPhysical.contains(index->second))
            out.push_back({ScheduledUpcomingSource::Playlist, SIZE_MAX, index->second,
                           handle == handles.end() ? metadb_handle_ptr() : handle->second});
    }
    return out;
}

bool PlaybackScheduler::Impl::ReorderUpcomingSnapshot(std::uint64_t expectedRevision,
                                                      const std::vector<ScheduledUpcomingEntry>& ordered) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!started || !modelInitialized || mapGuard || expectedRevision != schedule.revision()) return false;

    const auto current = ScheduledUpcomingSnapshot();
    if (current.size() != ordered.size()) return false;
    std::unordered_map<std::string, ScheduledUpcomingEntry> byToken;
    byToken.reserve(current.size());
    for (const auto& row : current) {
        const auto index =
            row.source == ScheduledUpcomingSource::CoreQueue ? row.queueIndex : row.playlistIndex;
        if (!byToken.emplace(UpcomingTokenKey(row.source, index), row).second) return false;
    }
    for (const auto& row : ordered) {
        const auto index =
            row.source == ScheduledUpcomingSource::CoreQueue ? row.queueIndex : row.playlistIndex;
        const auto found = byToken.find(UpcomingTokenKey(row.source, index));
        if (found == byToken.end() || found->second.playlistIndex != row.playlistIndex ||
            found->second.handle != row.handle) {
            return false;
        }
        byToken.erase(found);
    }
    if (!byToken.empty()) return false;

    std::vector<t_playback_queue_item> queueBefore = CoreQueueSnapshot();

    const auto scheduleBefore = schedule.Snapshot();
    const auto seedBefore = schedule.seed();
    const auto physicalBefore = physical;
    const auto ownedBefore = ownedCoreQueue;
    const auto probeIndexBefore = g_probeCurrentIndex;
    const auto currentOccurrence = schedule.Current();

    std::vector<PlaybackOccurrenceId> desiredFuture;
    std::vector<std::optional<PlaybackOccurrenceId>> orderedOccurrences(ordered.size());
    std::unordered_set<PlaybackOccurrenceId> desiredSet;
    for (std::size_t rowIndex = 0; rowIndex < ordered.size(); ++rowIndex) {
        const auto& row = ordered[rowIndex];
        if (row.source == ScheduledUpcomingSource::CoreQueue && row.playlistIndex == SIZE_MAX) continue;
        if (row.playlistIndex >= physical.size()) return false;
        const auto occurrence = physical[row.playlistIndex];
        const auto handle = handles.find(occurrence);
        if (handle == handles.end() || handle->second != row.handle) return false;
        orderedOccurrences[rowIndex] = occurrence;
        if (currentOccurrence && occurrence == *currentOccurrence) continue;
        if (!desiredSet.insert(occurrence).second) return false;
        desiredFuture.push_back(occurrence);
    }

    std::optional<std::size_t> lastForeign;
    for (std::size_t i = 0; i < ordered.size(); ++i)
        if (ordered[i].source == ScheduledUpcomingSource::CoreQueue && ordered[i].playlistIndex == SIZE_MAX) {
            lastForeign = i;
        }

    auto physicalAfter = physical;
    std::vector<std::size_t> slots;
    for (std::size_t i = 0; i < physicalAfter.size(); ++i)
        if (desiredSet.contains(physicalAfter[i])) slots.push_back(i);
    if (slots.size() != desiredFuture.size()) return false;
    for (std::size_t i = 0; i < slots.size(); ++i)
        physicalAfter[slots[i]] = desiredFuture[i];

    std::size_t currentAfter = SIZE_MAX;
    if (currentOccurrence) {
        const auto found = std::find(physicalAfter.begin(), physicalAfter.end(), *currentOccurrence);
        if (found == physicalAfter.end()) return false;
        currentAfter = static_cast<std::size_t>(found - physicalAfter.begin());
    }

    const auto rollback = [&] {
        schedule.RestoreSnapshot(scheduleBefore, seedBefore);
        ApplyPhysicalOrder(physicalBefore);
        RebuildCoreQueue(queueBefore);
        ownedCoreQueue = ownedBefore;
        g_probeCurrentIndex = probeIndexBefore;
    };

    // Under shuffle the future is in shuffle order and holds songs physically before the
    // current one. Writing the dragged order into the playlist and adopting it as physical
    // order would turn those songs into history: the list lost most of its rows after a drop
    // (owner, 2026-11-13). The playlist stays as it is; only the future takes the new order.
    if (schedule.shuffle_enabled()) {
        currentAfter = SIZE_MAX;
        if (currentOccurrence) {
            const auto found = physicalIndex.find(*currentOccurrence);
            if (found != physicalIndex.end()) currentAfter = found->second;
        }
        if (!schedule.ReorderFuture(std::span<const PlaybackOccurrenceId>(desiredFuture))) {
            rollback();
            return false;
        }
    } else if (!ApplyPhysicalOrder(physicalAfter) ||
               !schedule.AdoptPhysicalOrder(std::span<const PlaybackOccurrenceId>(physicalAfter), currentAfter)) {
        rollback();
        return false;
    }

    std::vector<t_playback_queue_item> projectedQueue;
    std::vector<t_playback_queue_item> projectedOwned;
    const t_size ownQueue = QueuePlaylist(false);
    if (lastForeign) {
        projectedQueue.reserve(*lastForeign + 1);
        for (std::size_t i = 0; i <= *lastForeign; ++i) {
            const auto& row = ordered[i];
            if (row.source == ScheduledUpcomingSource::CoreQueue && row.playlistIndex == SIZE_MAX) {
                if (row.queueIndex >= queueBefore.size()) {
                    rollback();
                    return false;
                }
                projectedQueue.push_back(queueBefore[row.queueIndex]);
                continue;
            }
            if (!orderedOccurrences[i]) {
                rollback();
                return false;
            }
            const auto occurrence = *orderedOccurrences[i];
            const auto physicalRow = physicalIndex.find(occurrence);
            const auto handle = handles.find(occurrence);
            if (physicalRow == physicalIndex.end() || handle == handles.end() ||
                handle->second != row.handle) {
                rollback();
                return false;
            }
            const auto item = t_playback_queue_item{handle->second, ownQueue, physicalRow->second};
            projectedQueue.push_back(item);
            projectedOwned.push_back(item);
        }
    }

    if (!RebuildCoreQueue(projectedQueue)) {
        rollback();
        return false;
    }
    ownedCoreQueue = std::move(projectedOwned);
    if (g_probeEnabled) {
        if (currentAfter == SIZE_MAX)
            g_probeCurrentIndex.reset();
        else
            g_probeCurrentIndex = currentAfter;
    }
    checkpointDirty = true;
    SaveCheckpoint();
    RequestListenerNotification();
    return true;
}

std::optional<std::size_t> PlaybackScheduler::Impl::ScheduledNextIndex() {
    PFC_ASSERT(core_api::is_main_thread());
    if (!modelInitialized) return std::nullopt;
    const auto next = schedule.Next();
    if (!next) return std::nullopt;
    const auto found = physicalIndex.find(*next);
    return found == physicalIndex.end() ? std::nullopt : std::optional<std::size_t>(found->second);
}

std::uint64_t PlaybackScheduler::Impl::AddListener(std::function<void()> listener) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!started || !listener) return 0;
    // A listener that throws must not stop the others.
    return listeners.Add([listener = std::move(listener)] {
        try {
            listener();
        } catch (...) {
        }
    });
}

void PlaybackScheduler::Impl::RemoveListener(std::uint64_t listener) {
    PFC_ASSERT(core_api::is_main_thread());
    listeners.Remove(listener);
}

PlaybackOccurrenceId PlaybackScheduler::Impl::AllocateOccurrence() {
    while (nextOccurrence == 0 || handles.contains(nextOccurrence))
        ++nextOccurrence;
    return nextOccurrence++;
}

std::vector<PlaybackOccurrenceId> PlaybackScheduler::Impl::AllocateOccurrences(
    const metadb_handle_list& items) {
    std::vector<PlaybackOccurrenceId> out;
    out.reserve(items.get_count());
    for (const auto& item : items) {
        const auto occurrence = AllocateOccurrence();
        handles[occurrence] = item;
        out.push_back(occurrence);
    }
    return out;
}

std::size_t PlaybackScheduler::Impl::EnsureSidecar() {
    auto manager = playlist_manager::get();
    const t_size queue = QueuePlaylist(false);
    if (queue == SIZE_MAX) {
        physical.clear();
        physicalIndex.clear();
        handles.clear();
        return SIZE_MAX;
    }
    metadb_handle_list items;
    manager->playlist_get_all_items(queue, items);
    bool exact = physical.size() == items.get_count();
    for (std::size_t i = 0; exact && i < physical.size(); ++i) {
        const auto found = handles.find(physical[i]);
        exact = found != handles.end() && found->second == items[i];
    }
    if (!exact) RebuildSidecar(items);
    ReindexPhysical();
    return CurrentQueueIndex(physical.size(), true);
}

void PlaybackScheduler::Impl::RebuildSidecar(const metadb_handle_list& items) {
    const auto oldPhysical = physical;
    const auto oldHandles = handles;
    std::unordered_map<std::string, std::deque<PlaybackOccurrenceId>> oldByHandle;
    oldByHandle.reserve(oldPhysical.size());
    for (const auto occurrence : oldPhysical) {
        const auto found = oldHandles.find(occurrence);
        if (found != oldHandles.end()) oldByHandle[HandleIdentityKey(found->second)].push_back(occurrence);
    }
    physical.clear();
    handles.clear();
    physical.reserve(items.get_count());
    for (const auto& item : items) {
        const auto key = HandleIdentityKey(item);
        std::optional<PlaybackOccurrenceId> matched;
        if (const auto found = oldByHandle.find(key); found != oldByHandle.end() && !found->second.empty()) {
            matched = found->second.front();
            found->second.pop_front();
        }
        const auto occurrence = matched.value_or(AllocateOccurrence());
        physical.push_back(occurrence);
        handles[occurrence] = item;
    }
}

metadb_handle_list PlaybackScheduler::Impl::HandlesFor(
    std::span<const PlaybackOccurrenceId> occurrences) const {
    metadb_handle_list out;
    for (const auto occurrence : occurrences) {
        const auto found = handles.find(occurrence);
        if (found != handles.end()) out.add_item(found->second);
    }
    return out;
}

bool PlaybackScheduler::Impl::ApplyProjection() {
    PFC_ASSERT(core_api::is_main_thread());
    if (!modelInitialized || mapGuard) return false;
    const auto desired = schedule.ProjectionOrder();
    if (desired.size() != physical.size()) return false;
    if (ApplyPhysicalOrder(desired)) return true;
    ScheduleSynchronize(); // the playlist and the model disagree: read the playlist again
    return false;
}

// Reorders the queue playlist's rows into `desired`, a permutation of the physical order: the
// playlist (one undo step), the physical order and the core queue entries that point into it
// move together. False when `desired` is no permutation of it or foobar2000 refused.
bool PlaybackScheduler::Impl::ApplyPhysicalOrder(const std::vector<PlaybackOccurrenceId>& desired) {
    if (desired == physical) return true;
    const auto oldPhysical = physical;
    std::unordered_map<PlaybackOccurrenceId, std::size_t> oldIndex;
    oldIndex.reserve(physical.size());
    for (std::size_t i = 0; i < physical.size(); ++i)
        oldIndex[physical[i]] = i;
    std::vector<t_size> permutation;
    permutation.reserve(desired.size());
    for (const auto occurrence : desired) {
        const auto found = oldIndex.find(occurrence);
        if (found == oldIndex.end()) return false;
        permutation.push_back(found->second);
    }
    const t_size queue = QueuePlaylist(false);
    if (queue == SIZE_MAX) return false;
    mapGuard = true;
    bool changed = false;
    try {
        auto manager = playlist_manager::get();
        manager->playlist_undo_backup(queue);
        changed = manager->playlist_reorder_items(queue, permutation.data(), permutation.size());
    } catch (...) {
        mapGuard = false;
        return false;
    }
    mapGuard = false;
    if (!changed) return false;
    SetPhysical(desired);
    RemapOwnedCoreQueue(oldPhysical);
    return true;
}

bool PlaybackScheduler::Impl::StartOccurrence(PlaybackOccurrenceId occurrence, bool paused) {
    const auto found = physicalIndex.find(occurrence);
    if (found == physicalIndex.end()) return false;
    if (g_probeEnabled) {
        const t_size queue = QueuePlaylist(false);
        if (queue == SIZE_MAX) return false;
        auto manager = playlist_manager::get();
        manager->set_active_playlist(queue);
        manager->set_playing_playlist(queue);
        manager->playlist_set_focus_item(queue, found->second);
        g_probeCurrentIndex = found->second;
        schedule.Started(occurrence, false);
        ApplyProjection();
        return true;
    }
    const t_size queue = QueuePlaylist(false);
    if (queue == SIZE_MAX) return false;
    auto manager = playlist_manager::get();
    manager->set_active_playlist(queue);
    manager->set_playing_playlist(queue);
    manager->playlist_set_focus_item(queue, found->second);
    exactStart = occurrence;
    pendingClickStart.reset();
    manager->playlist_execute_default_action(queue, found->second);
    if (paused) playback_control::get()->pause(true);
    return true;
}

bool PlaybackScheduler::Impl::StartExplicitAt(PlaybackOccurrenceId occurrence) {
    const auto found = physicalIndex.find(occurrence);
    const auto handle = handles.find(occurrence);
    if (found == physicalIndex.end() || handle == handles.end()) return false;

    const auto before = schedule.Snapshot();
    bool selectedPriority = false;
    for (const auto& block : before.priorityBlocks)
        selectedPriority |= std::find(block.begin(), block.end(), occurrence) != block.end();

    if (selectedPriority) {
        const t_size queue = QueuePlaylist(false);
        const auto selectedQueueItem =
            std::find_if(ownedCoreQueue.begin(), ownedCoreQueue.end(), [&](const auto& item) {
                return item.m_playlist == queue && item.m_item == found->second &&
                       item.m_handle == handle->second;
            });
        if (selectedQueueItem != ownedCoreQueue.end()) {
            const auto queuedItem = *selectedQueueItem;
            std::vector<t_playback_queue_item> snapshot = CoreQueueSnapshot();
            const bool stillQueued =
                std::find(snapshot.begin(), snapshot.end(), queuedItem) != snapshot.end();
            if (stillQueued) {
                if (!RemoveCoreQueueItem(queuedItem)) return false;
            } else {
                ReconcileOwnedCoreQueue(snapshot);
            }
        }
    }

    if (!schedule.StartExplicit(std::span<const PlaybackOccurrenceId>(physical), found->second)) return false;
    checkpointDirty = true;
    return true;
}

void PlaybackScheduler::Impl::OnPlaybackStarting(play_control::t_track_command command, bool paused) {
    Post([this, command, paused] { HandleNavigationStart(command, paused); });
}

void PlaybackScheduler::Impl::HandleNavigationStart(play_control::t_track_command command, bool paused) {
    startCommand = command;
    startPaused = paused;
    navigationTarget.reset();
    if (!modelInitialized ||
        (command != play_control::track_command_next && command != play_control::track_command_prev &&
         command != play_control::track_command_rand)) {
        return;
    }

    const auto current = schedule.Current();
    const auto target =
        command == play_control::track_command_prev ? schedule.Previous() : schedule.ManualNext();
    if (!current) return;
    // Previous with nothing before, or Next at the end of a radio station's currently loaded
    // queue: keep the current song instead of the core stopping playback.
    if (!target) {
        if (command == play_control::track_command_prev ||
            (command == play_control::track_command_next && radio::Active())) {
            navigationTarget = current;
        }
        return;
    }
    if (command == play_control::track_command_rand) {
        navigationTarget = target;
        return;
    }
    const auto currentAt = physicalIndex.find(*current);
    const auto targetAt = physicalIndex.find(*target);
    if (currentAt == physicalIndex.end() || targetAt == physicalIndex.end()) return;
    std::size_t expected = currentAt->second + 1;
    bool exists = expected < physical.size();
    if (command == play_control::track_command_prev) {
        exists = currentAt->second > 0;
        if (exists) expected = currentAt->second - 1;
    }
    const bool adjacent = exists && targetAt->second == expected;
    if (!adjacent) navigationTarget = target;
}

void PlaybackScheduler::Impl::OnTrackStarted(const metadb_handle_ptr&) {
    Post([this] {
        EnsureSidecar();
        const std::size_t current = CurrentQueueIndex(physical.size(), false);
        if (current == SIZE_MAX || current >= physical.size()) return;
        const auto occurrence = physical[current];
        if (navigationTarget && *navigationTarget != occurrence) {
            StartOccurrence(*navigationTarget, startPaused);
            return;
        }
        HandleTrackStarted(occurrence);
    });
}

void PlaybackScheduler::Impl::HandleTrackStarted(PlaybackOccurrenceId occurrence) {
    EnsureModel();
    const auto current = schedule.Current();
    const auto expectedNext = schedule.ManualNext();
    const bool linearFixed = !schedule.shuffle_enabled() && !schedule.HasPriority() &&
                             schedule.repeat_mode() != PlaybackRepeatMode::All;
    const bool explicitStart = linearFixed || (exactStart && *exactStart == occurrence) ||
                               startCommand == play_control::track_command_play ||
                               ((startCommand == play_control::track_command_default ||
                                 startCommand == play_control::track_command_settrack) &&
                                current != occurrence && (!expectedNext || *expectedNext != occurrence));
    const bool changed = explicitStart
                             ? StartExplicitAt(occurrence)
                             : schedule.Started(occurrence, startCommand == play_control::track_command_prev);
    if (!changed) {
        ScheduleSynchronize();
        return;
    }
    navigationTarget.reset();
    exactStart.reset();
    pendingClickStart.reset();
    failureBurst = 0;
    ApplyProjection();
    checkpointDirty = true;
    SaveCheckpoint();
    startCommand = play_control::track_command_default;
    startPaused = false;
}

void PlaybackScheduler::Impl::OnPlaybackStopped(play_control::t_stop_reason reason) {
    Post([this, reason] { HandleStop(reason); });
}

void PlaybackScheduler::Impl::HandleStop(play_control::t_stop_reason reason) {
    if (reason == play_control::stop_reason_starting_another ||
        reason == play_control::stop_reason_shutting_down) {
        return;
    }
    pendingClickStart.reset();

    if (navigationTarget) {
        const auto target = *navigationTarget;
        navigationTarget.reset();
        StartOccurrence(target, startPaused);
        return;
    }

    // Every reason but the two above counts as a failed start here: what foobar2000 reports for
    // a track that fails to open is not documented. A Stop through Glasswave's controls clears
    // the start first (StopRequested); foobar2000's own Stop pressed while a song is still
    // opening still reads as a failed start.
    if (exactStart) {
        exactStart.reset();
        ++failureBurst;
        if (failureBurst > 8) {
            failureBurst = 0;
            return;
        }
        if (const auto next = schedule.SkipFailedCurrent()) {
            StartOccurrence(*next);
        }
        return;
    }

    if (reason != play_control::stop_reason_eof) return;
    if (playback_control::get()->get_stop_after_current()) return;
    if (schedule.repeat_mode() == PlaybackRepeatMode::All) {
        schedule.Tick(0.0);
        const auto next = schedule.Next();
        if (next) StartOccurrence(*next);
    } else if (schedule.repeat_mode() == PlaybackRepeatMode::One) {
        const auto current = schedule.Current();
        if (current) StartOccurrence(*current);
    }
}

void PlaybackScheduler::Impl::OnPlaybackTime() {
    if (timeQueued) return;
    timeQueued = true;
    Post([this] {
        timeQueued = false;
        HandleTime();
    });
}

void PlaybackScheduler::Impl::HandleTime() {
    if (!modelInitialized) return;
    auto control = playback_control::get();
    const double length = control->playback_get_length_ex();
    const double position = control->playback_get_position();
    if (!std::isfinite(length) || !std::isfinite(position) || length <= 0 || position < 0) return;
    if (position > 0) failureBurst = 0;
    const auto oldRevision = schedule.revision();
    schedule.Tick(std::max(0.0, length - position));
    if (schedule.revision() != oldRevision) {
        ApplyProjection();
    }
}

void PlaybackScheduler::Impl::QueuePlaylistAdded(std::size_t base, const metadb_handle_list& items) {
    if (mapGuard) return;
    const auto generation = sourceGeneration;
    const metadb_handle_list copy = items;
    Post([this, generation, base, copy] {
        if (generation != sourceGeneration || mapGuard) return;
        if (physical.size() + copy.get_count() ==
            playlist_manager::get()->playlist_get_item_count(QueuePlaylist(false))) {
            const auto added = AllocateOccurrences(copy);
            physical.insert(physical.begin() + static_cast<std::ptrdiff_t>(std::min(base, physical.size())),
                            added.begin(), added.end());
            ScheduleSynchronize();
        } else {
            QueuePlaylistAvailabilityChanged();
        }
    });
}

void PlaybackScheduler::Impl::QueuePlaylistReordered(const t_size* order, std::size_t count) {
    if (mapGuard) return;
    const auto generation = sourceGeneration;
    const std::vector<t_size> copy(order, order + count);
    Post([this, generation, copy] {
        if (generation != sourceGeneration || mapGuard) return;
        if (copy.size() != physical.size()) {
            QueuePlaylistAvailabilityChanged();
            return;
        }
        std::vector<PlaybackOccurrenceId> updated(copy.size());
        for (std::size_t i = 0; i < copy.size(); ++i)
            if (copy[i] >= physical.size()) return;
        for (std::size_t i = 0; i < copy.size(); ++i)
            updated[i] = physical[copy[i]];
        const auto oldPhysical = physical;
        SetPhysical(std::move(updated));
        RemapOwnedCoreQueue(oldPhysical);
        reorderDirty = true;
        if (!ownedCoreQueue.empty() && !RemoveOwnedCoreQueueEntries()) {
            console::print("Glasswave: own priority queue rows could not be cleared after a manual drag.");
        }
        ScheduleSynchronize();
    });
}

void PlaybackScheduler::Impl::QueuePlaylistRemoved(const bit_array& mask, std::size_t old_count) {
    if (mapGuard) return;
    std::vector<std::uint8_t> removed(old_count, 0);
    std::size_t removedCount = 0;
    for (std::size_t i = 0; i < old_count; ++i)
        if (mask[i]) {
            removed[i] = 1;
            ++removedCount;
        }
    const bool whole_clear = old_count != 0 && removedCount == old_count;
    const auto generation = sourceGeneration;
    Post([this, generation, removed = std::move(removed), whole_clear] {
        if (generation != sourceGeneration || mapGuard) return;
        physical = ApplyPhysicalRemovalMask(std::span<const PlaybackOccurrenceId>(physical),
                                            std::span<const std::uint8_t>(removed));
        if (whole_clear) {
            RemoveOwnedCoreQueueEntries();
            ++contentGeneration;
            wholeReplaceDirty = true;
        }
        ScheduleSynchronize();
    });
}

void PlaybackScheduler::Impl::QueuePlaylistReplaced() {
    if (!mapGuard) QueuePlaylistAvailabilityChanged();
}

void PlaybackScheduler::Impl::QueuePlaylistAvailabilityChanged() {
    if (mapGuard) return;
    ScheduleSynchronize();
}

void PlaybackScheduler::Impl::ScheduleCoreQueueChanged() {
    if (!started) return;
    schedule.Touch();
    if (coreQueueSyncQueued) return;
    coreQueueSyncQueued = true;
    Post([this] {
        coreQueueSyncQueued = false;
        std::vector<t_playback_queue_item> snapshot = CoreQueueSnapshot();
        ReconcileOwnedCoreQueue(snapshot);
        RequestListenerNotification();
    });
}

void PlaybackScheduler::Impl::ScheduleSynchronize() {
    if (syncQueued) return;
    syncQueued = true;
    const auto generation = sourceGeneration;
    const auto token = ++syncGeneration;
    Post([this, generation, token] {
        if (generation != sourceGeneration || token != syncGeneration) return;
        syncQueued = false;
        Synchronize();
    });
}

void PlaybackScheduler::Impl::CancelSynchronize() {
    ++syncGeneration;
    syncQueued = false;
    reorderDirty = false;
}

void PlaybackScheduler::Impl::Synchronize() {
    if (!started || mapGuard) return;
    EnsureSidecar();
    if (!modelInitialized) {
        EnsureModel();
        reorderDirty = false;
        return;
    }

    if (wholeReplaceDirty) {
        wholeReplaceDirty = false;
        InvalidatePosted();
        const std::size_t current = CurrentQueueIndex(physical.size(), true);
        schedule.ResetFresh(std::span<const PlaybackOccurrenceId>(physical),
                            current == SIZE_MAX ? 0 : current, NextSeed());
        modelInitialized = true;
        checkpointDirty = true;
        reorderDirty = false;
        SaveCheckpoint();
        Post([this] { Synchronize(); });
        return;
    }

    const auto modelIds = schedule.AllOccurrenceIds();
    const std::unordered_set<PlaybackOccurrenceId> modelSet(modelIds.begin(), modelIds.end());
    const std::unordered_set<PlaybackOccurrenceId> physicalSet(physical.begin(), physical.end());
    if (physicalSet.empty() && !modelSet.empty()) {
        ++contentGeneration;
        InvalidatePosted();
        const std::size_t current = CurrentQueueIndex(physical.size(), true);
        schedule.ResetFresh(std::span<const PlaybackOccurrenceId>(physical),
                            current == SIZE_MAX ? 0 : current, NextSeed());
        modelInitialized = true;
        checkpointDirty = true;
        reorderDirty = false;
        SaveCheckpoint();
        Post([this] { Synchronize(); });
        return;
    }

    std::vector<PlaybackOccurrenceId> removed;
    for (const auto occurrence : modelIds)
        if (!physicalSet.contains(occurrence)) removed.push_back(occurrence);
    std::vector<PlaybackOccurrenceId> added;
    for (const auto occurrence : physical)
        if (!modelSet.contains(occurrence)) added.push_back(occurrence);
    if (!removed.empty()) {
        schedule.Remove(std::span<const PlaybackOccurrenceId>(removed));
        checkpointDirty = true;
    }
    if (!added.empty()) {
        schedule.AddOrdinary(std::span<const PlaybackOccurrenceId>(added));
        checkpointDirty = true;
    }

    const std::size_t current = CurrentQueueIndex(physical.size(), true);
    const std::optional<PlaybackOccurrenceId> currentOccurrence =
        current == SIZE_MAX ? std::nullopt : std::optional<PlaybackOccurrenceId>(physical[current]);
    if (currentOccurrence && schedule.Current() != currentOccurrence && !schedule.shuffle_enabled() &&
        !schedule.HasPriority() && current < physical.size()) {
        if (schedule.StartExplicit(std::span<const PlaybackOccurrenceId>(physical), current))
            checkpointDirty = true;
    } else {
        schedule.RestoreCurrent(currentOccurrence);
    }

    if (reorderDirty) {
        schedule.AdoptPhysicalOrder(std::span<const PlaybackOccurrenceId>(physical), current);
        checkpointDirty = true;
    }

    reorderDirty = false;
    ApplyProjection();
    if (checkpointDirty) SaveCheckpoint();
}

void PlaybackScheduler::Impl::OnNativeOrderMode() {
    auto manager = playlist_manager::get();
    const t_size active = manager->playback_order_get_active();
    const char* name =
        active < manager->playback_order_get_count() ? manager->playback_order_get_name(active) : nullptr;
    const std::string observedName = name ? name : "";
    if (applyingCoreOrder) {
        coreOrderCallbackSeen = true;
        return;
    }
    if (expectedOrderEchoes != 0 && pfc::stricmp_ascii(observedName.c_str(), expectedOrderName.c_str()) == 0) {
        --expectedOrderEchoes;
        return;
    }
    const auto generation = sourceGeneration;
    Post([this, generation, observedName] {
        if (generation != sourceGeneration) return;
        const char* name = observedName.c_str();
        if (pfc::stricmp_ascii(name, "Default") == 0) {
            SetPlaybackMode(false, 0);
        } else if (NameContains(name, "shuffle") || NameContains(name, "random")) {
            SetPlaybackMode(true, schedule.repeat_mode() == PlaybackRepeatMode::Off   ? 0
                                  : schedule.repeat_mode() == PlaybackRepeatMode::All ? 1
                                                                                      : 2);
        } else if (NameContains(name, "repeat") &&
                   (NameContains(name, "playlist") || NameContains(name, "all"))) {
            SetPlaybackMode(schedule.shuffle_enabled(), 1);
        } else if (NameContains(name, "repeat") &&
                   (NameContains(name, "track") || NameContains(name, "one"))) {
            SetPlaybackMode(schedule.shuffle_enabled(), 2);
        } else {
            ApplyCoreOrder();
        }
    });
}

bool PlaybackScheduler::Impl::QueueIsEngaged() const {
    auto manager = playlist_manager::get();
    const t_size queue = QueuePlaylist(false);
    return queue != SIZE_MAX &&
           (manager->get_active_playlist() == queue || manager->get_playing_playlist() == queue);
}

// The core's own order follows only what it must do itself: Default (the schedule projects the
// order into the playlist), or Repeat (track) while repeating one song, so the core replays the
// song at its end and Next / Previous still walk the list (owner, 2026-11-20: Next under repeat
// one replayed the song, and the list was laid out with the song last).
void PlaybackScheduler::Impl::ApplyCoreOrder() {
    if (!QueueIsEngaged() || applyingCoreOrder) return;
    auto manager = playlist_manager::get();
    const char* name = schedule.repeat_mode() == PlaybackRepeatMode::One ? "Repeat (track)" : "Default";
    t_size order = OrderIndex(name);
    if (order == SIZE_MAX) {
        name = "Default";
        order = OrderIndex(name);
    }
    if (order == SIZE_MAX || manager->playback_order_get_active() == order) return;
    if (expectedOrderName != name) expectedOrderEchoes = 0;
    expectedOrderName = name;
    ++expectedOrderEchoes;
    coreOrderCallbackSeen = false;
    applyingCoreOrder = true;
    manager->playback_order_set_active(order);
    applyingCoreOrder = false;
    if (coreOrderCallbackSeen) --expectedOrderEchoes;
}

void PlaybackScheduler::Impl::Post(std::function<void()> task) {
    const auto generation = sourceGeneration;
    std::weak_ptr<int> lifetimeToken = lifetime;
    fb2k::inMainThread([this, lifetimeToken, generation, task = std::move(task)] {
        if (lifetimeToken.expired()) return;
        if (!started || generation != sourceGeneration) return;
        const auto revisionBefore = schedule.revision();
        task();
        if (schedule.revision() != revisionBefore) RequestListenerNotification();
    });
}

void PlaybackScheduler::Impl::RequestListenerNotification() {
    PFC_ASSERT(core_api::is_main_thread());
    if (!started || listeners.Empty() || listenerNotifyQueued) return;
    listenerNotifyQueued = true;
    Post([this] {
        listenerNotifyQueued = false;
        listeners.Emit();
    });
}

void PlaybackScheduler::Impl::InvalidatePosted() {
    ++sourceGeneration;
    ++syncGeneration;
    syncQueued = false;
    timeQueued = false; // its post is dropped by the generation: a later tick must queue again
    coreQueueSyncQueued = false;
    listenerNotifyQueued = false;
    reorderDirty = false;
    navigationTarget.reset();
    exactStart.reset();
    pendingClickStart.reset();
}

bool PlaybackSchedulerProbeAccess::Install() {
    PFC_ASSERT(core_api::is_main_thread());
    PlaybackScheduler::Get();
    g_probeEnabled = true;
    g_probeCurrentIndex.reset();
    return PlaybackScheduler::Existing() != nullptr;
}

void PlaybackSchedulerProbeAccess::Uninstall() {
    PFC_ASSERT(core_api::is_main_thread());
    g_probeEnabled = false;
    g_probeCurrentIndex.reset();
}

PlaybackSchedulerProbeAccess::HostState PlaybackSchedulerProbeAccess::Capture() {
    PFC_ASSERT(core_api::is_main_thread());
    PlaybackScheduler::Get();
    if (!g_scheduler) return {};
    const auto& impl = *g_scheduler->m_impl;
    auto state = std::make_shared<ProbeHostSnapshot>();
    state->scheduleState = impl.schedule.Snapshot();
    state->scheduleSeed = impl.schedule.seed();
    state->physical = impl.physical;
    state->handles = impl.handles;
    state->ownedCoreQueue = impl.ownedCoreQueue;
    state->nextOccurrence = impl.nextOccurrence;
    state->contentGeneration = impl.contentGeneration;
    state->savedFingerprint = impl.savedFingerprint;
    state->savedState = impl.savedState;
    state->savedCheckpoint = impl.savedCheckpoint;
    state->cfgCheckpoint = cfg_checkpoint.get().c_str();
    state->cfgShuffle = static_cast<int>(cfg_shuffle.get());
    state->cfgRepeat = static_cast<int>(cfg_repeat.get());
    state->checkpointDirty = impl.checkpointDirty;
    state->checkpointValid = impl.checkpointValid;
    state->modelInitialized = impl.modelInitialized;
    state->probeCurrentIndex = g_probeCurrentIndex;
    return {std::move(state)};
}

bool PlaybackSchedulerProbeAccess::Restore(const HostState& state) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!g_scheduler || !state.data) return false;
    const auto saved = std::static_pointer_cast<const ProbeHostSnapshot>(state.data);
    auto& impl = *g_scheduler->m_impl;
    if (!impl.schedule.RestoreSnapshot(saved->scheduleState, saved->scheduleSeed)) return false;
    impl.SetPhysical(saved->physical);
    impl.handles = saved->handles;
    impl.ownedCoreQueue = saved->ownedCoreQueue;
    impl.nextOccurrence = saved->nextOccurrence;
    impl.contentGeneration = saved->contentGeneration;
    impl.savedFingerprint = saved->savedFingerprint;
    impl.savedState = saved->savedState;
    impl.savedCheckpoint = saved->savedCheckpoint;
    impl.checkpointDirty = saved->checkpointDirty;
    impl.checkpointValid = saved->checkpointValid;
    impl.modelInitialized = saved->modelInitialized;
    impl.syncQueued = false;
    impl.timeQueued = false;
    impl.reorderDirty = false;
    impl.wholeReplaceDirty = false;
    impl.navigationTarget.reset();
    impl.exactStart.reset();
    cfg_checkpoint = saved->cfgCheckpoint.c_str();
    cfg_shuffle = saved->cfgShuffle;
    cfg_repeat = saved->cfgRepeat;
    g_probeCurrentIndex = saved->probeCurrentIndex;
    ++impl.sourceGeneration;
    ++impl.syncGeneration;
    return true;
}

bool PlaybackSchedulerProbeAccess::Same(const HostState& left, const HostState& right) {
    if (!left.data || !right.data) return left.data == right.data;
    const auto a = std::static_pointer_cast<const ProbeHostSnapshot>(left.data);
    const auto b = std::static_pointer_cast<const ProbeHostSnapshot>(right.data);
    return SameScheduleState(a->scheduleState, b->scheduleState) && a->physical == b->physical &&
           a->handles == b->handles && a->ownedCoreQueue == b->ownedCoreQueue &&
           a->nextOccurrence == b->nextOccurrence && a->contentGeneration == b->contentGeneration &&
           a->savedFingerprint == b->savedFingerprint && SameScheduleState(a->savedState, b->savedState) &&
           a->savedCheckpoint == b->savedCheckpoint && a->cfgCheckpoint == b->cfgCheckpoint &&
           a->cfgShuffle == b->cfgShuffle && a->cfgRepeat == b->cfgRepeat &&
           a->checkpointDirty == b->checkpointDirty && a->checkpointValid == b->checkpointValid &&
           a->modelInitialized == b->modelInitialized && a->probeCurrentIndex == b->probeCurrentIndex;
}

std::optional<PlaybackOccurrenceId> PlaybackSchedulerProbeAccess::CurrentOccurrence() {
    PFC_ASSERT(core_api::is_main_thread());
    return g_scheduler ? g_scheduler->m_impl->schedule.Current() : std::nullopt;
}

std::vector<PlaybackOccurrenceId> PlaybackSchedulerProbeAccess::PhysicalOccurrences() {
    PFC_ASSERT(core_api::is_main_thread());
    return g_scheduler ? g_scheduler->m_impl->physical : std::vector<PlaybackOccurrenceId>();
}

PlaybackScheduleState PlaybackSchedulerProbeAccess::ScheduleState() {
    PFC_ASSERT(core_api::is_main_thread());
    return g_scheduler ? g_scheduler->m_impl->schedule.Snapshot() : PlaybackScheduleState();
}

std::uint64_t PlaybackSchedulerProbeAccess::ContentGeneration() {
    PFC_ASSERT(core_api::is_main_thread());
    return g_scheduler ? g_scheduler->m_impl->contentGeneration : 0;
}

bool PlaybackSchedulerProbeAccess::CheckpointUsesCurrentSchema() {
    PFC_ASSERT(core_api::is_main_thread());
    using namespace checkpoint_json;
    const auto json = core::json::Parse(std::string(cfg_checkpoint.get().c_str()));
    if (!json) return false;
    const auto version = Integer(*json, L"version");
    return version && *version == kCheckpointVersion && Indices(*json, L"canonical") &&
           Indices(*json, L"projection") && Indices(*json, L"history") && Indices(*json, L"future") &&
           Indices(*json, L"priority") && Indices(*json, L"prioritySizes") && Indices(*json, L"pending");
}

bool PlaybackSchedulerProbeAccess::AdvanceOccurrence(PlaybackOccurrenceId occurrence, bool from_history) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!g_scheduler) return false;
    auto& impl = *g_scheduler->m_impl;
    const auto found = impl.physicalIndex.find(occurrence);
    if (found == impl.physicalIndex.end()) return false;
    g_probeCurrentIndex = found->second;
    impl.startCommand = from_history ? play_control::track_command_prev : play_control::track_command_next;
    impl.HandleTrackStarted(occurrence);
    impl.startCommand = play_control::track_command_default;
    return impl.schedule.Current() == occurrence;
}

bool PlaybackSchedulerProbeAccess::AdvanceExplicit(PlaybackOccurrenceId occurrence) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!g_scheduler) return false;
    auto& impl = *g_scheduler->m_impl;
    const auto found = impl.physicalIndex.find(occurrence);
    if (found == impl.physicalIndex.end()) return false;
    g_probeCurrentIndex = found->second;
    impl.startCommand = play_control::track_command_default;
    impl.exactStart.reset();
    if (!impl.StartExplicitAt(occurrence)) return false;
    impl.navigationTarget.reset();
    impl.failureBurst = 0;
    if (!impl.ApplyProjection()) return false;
    impl.SaveCheckpoint();
    impl.startCommand = play_control::track_command_default;
    impl.startPaused = false;
    return impl.schedule.Current() == occurrence;
}

bool PlaybackSchedulerProbeAccess::TickModel(double remaining_seconds) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!g_scheduler) return false;
    auto& impl = *g_scheduler->m_impl;
    const bool changed = impl.schedule.Tick(remaining_seconds);
    if (changed) impl.ApplyProjection();
    return changed;
}

PlaybackScheduler::PlaybackScheduler(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

PlaybackScheduler::~PlaybackScheduler() = default;

void PlaybackScheduler::Start() {
    PFC_ASSERT(core_api::is_main_thread());
    if (g_scheduler || g_schedulerStopped || g_schedulerStarting) return;
    g_schedulerStarting = true;
    auto impl = std::make_unique<Impl>();
    g_scheduler.reset(new PlaybackScheduler(std::move(impl)));
    try {
        g_scheduler->m_impl->Start();
    } catch (...) {
        g_scheduler.reset();
        g_schedulerStarting = false;
        throw;
    }
    g_schedulerStarting = false;
}

void PlaybackScheduler::Shutdown() {
    PFC_ASSERT(core_api::is_main_thread());
    if (g_schedulerStopped) return;
    g_schedulerStopped = true;
    if (g_scheduler) g_scheduler->m_impl->Shutdown();
}

PlaybackScheduler& PlaybackScheduler::Get() {
    PFC_ASSERT(core_api::is_main_thread());
    if (!g_scheduler && !g_schedulerStopped) Start();
    PFC_ASSERT(g_scheduler);
    return *g_scheduler;
}

PlaybackScheduler* PlaybackScheduler::Existing() {
    return g_scheduler.get();
}

bool PlaybackScheduler::SetPlaybackMode(bool shuffle, int repeat) {
    return m_impl->SetPlaybackMode(shuffle, repeat);
}

bool PlaybackScheduler::QueueShuffleEnabled() const {
    return m_impl->schedule.shuffle_enabled();
}

int PlaybackScheduler::QueueRepeatMode() const {
    return static_cast<int>(m_impl->schedule.repeat_mode());
}

bool PlaybackScheduler::PlayQueue(const metadb_handle_list& items, std::size_t start) {
    return m_impl->PlayQueue(items, start);
}

bool PlaybackScheduler::AdoptPlaying(const metadb_handle_list& items, std::size_t start) {
    return m_impl->AdoptPlaying(items, start);
}

bool PlaybackScheduler::Editing() const {
    return m_impl->mapGuard;
}

bool PlaybackScheduler::PlayNext(const metadb_handle_list& items) {
    return m_impl->PlayNext(items);
}

bool PlaybackScheduler::AddToQueue(const metadb_handle_list& items) {
    return m_impl->AddToQueue(items);
}

bool PlaybackScheduler::AppendSourceBatch(const metadb_handle_list& items, std::uint64_t expectedGeneration) {
    return m_impl->AppendSourceBatch(items, expectedGeneration);
}

std::vector<std::size_t> PlaybackScheduler::ScheduledUpcomingIndices() {
    return m_impl->ScheduledUpcomingIndices();
}

std::vector<ScheduledUpcomingEntry> PlaybackScheduler::ScheduledUpcomingSnapshot() {
    return m_impl->ScheduledUpcomingSnapshot();
}

std::vector<ScheduledUpcomingEntry> PlaybackScheduler::DisplayUpcomingSnapshot() {
    return m_impl->ScheduledUpcomingSnapshot(true);
}

std::size_t PlaybackScheduler::RepeatCycleSize() {
    PFC_ASSERT(core_api::is_main_thread());
    return m_impl->modelInitialized ? m_impl->schedule.RepeatCycleSize() : 0;
}

bool PlaybackScheduler::ReorderUpcomingSnapshot(std::uint64_t expectedRevision,
                                                const std::vector<ScheduledUpcomingEntry>& ordered) {
    return m_impl->ReorderUpcomingSnapshot(expectedRevision, ordered);
}

std::optional<std::size_t> PlaybackScheduler::ScheduledNextIndex() {
    return m_impl->ScheduledNextIndex();
}

metadb_handle_ptr PlaybackScheduler::CurrentTrack() const {
    PFC_ASSERT(core_api::is_main_thread());
    const auto& impl = *m_impl;
    if (!impl.modelInitialized) return {};
    const auto cur = impl.schedule.Current();
    if (!cur) return {};
    const auto found = impl.handles.find(*cur);
    return found == impl.handles.end() ? metadb_handle_ptr() : found->second;
}

std::optional<std::size_t> PlaybackScheduler::CurrentQueuePosition() const {
    PFC_ASSERT(core_api::is_main_thread());
    const auto& impl = *m_impl;
    if (!impl.modelInitialized) return std::nullopt;
    const auto cur = impl.schedule.Current();
    if (!cur) return std::nullopt;
    const auto found = impl.physicalIndex.find(*cur);
    return found == impl.physicalIndex.end() ? std::nullopt : std::optional<std::size_t>(found->second);
}

bool PlaybackScheduler::AlignLinearCurrent(std::size_t playlistIndex) {
    PFC_ASSERT(core_api::is_main_thread());
    auto& impl = *m_impl;
    impl.EnsureSidecar();
    impl.EnsureModel();
    if (!impl.modelInitialized || playlistIndex >= impl.physical.size() || impl.schedule.shuffle_enabled() ||
        impl.schedule.HasPriority()) {
        return false;
    }
    if (impl.schedule.Current() == impl.physical[playlistIndex]) return true;
    if (!impl.schedule.StartExplicit(std::span<const PlaybackOccurrenceId>(impl.physical), playlistIndex))
        return false;
    impl.checkpointDirty = true;
    impl.ApplyProjection();
    impl.SaveCheckpoint();
    impl.RequestListenerNotification();
    return true;
}

metadb_handle_ptr PlaybackScheduler::StartingTrack(play_control::t_track_command command) {
    PFC_ASSERT(core_api::is_main_thread());
    auto& impl = *m_impl;
    if (!impl.modelInitialized) return {};
    std::optional<PlaybackOccurrenceId> target;
    if (command == play_control::track_command_next || command == play_control::track_command_rand)
        target = impl.schedule.ManualNext();
    else if (command == play_control::track_command_prev)
        target = impl.schedule.Previous();
    else
        target = impl.exactStart ? impl.exactStart : impl.pendingClickStart;
    if (!target) return {};
    const auto found = impl.handles.find(*target);
    return found == impl.handles.end() ? metadb_handle_ptr() : found->second;
}

std::optional<std::size_t> PlaybackScheduler::PendingStartIndex(play_control::t_track_command command) {
    PFC_ASSERT(core_api::is_main_thread());
    auto& impl = *m_impl;
    if (!impl.modelInitialized) return std::nullopt;
    std::optional<PlaybackOccurrenceId> target;
    if (command == play_control::track_command_next || command == play_control::track_command_rand)
        target = impl.schedule.ManualNext();
    else if (command == play_control::track_command_prev)
        target = impl.schedule.Previous();
    else
        target = impl.exactStart ? impl.exactStart : impl.pendingClickStart;
    if (!target) return std::nullopt;
    const auto found = impl.physicalIndex.find(*target);
    return found == impl.physicalIndex.end() ? std::nullopt : std::optional<std::size_t>(found->second);
}

void PlaybackScheduler::ExpectExactStart(std::size_t playlistIndex) {
    PFC_ASSERT(core_api::is_main_thread());
    auto& impl = *m_impl;
    if (!impl.modelInitialized || playlistIndex >= impl.physical.size()) {
        impl.pendingClickStart.reset();
        return;
    }
    impl.pendingClickStart = impl.physical[playlistIndex];
}

std::optional<StartingPreview> PlaybackScheduler::PreviewStarting(play_control::t_track_command command) {
    PFC_ASSERT(core_api::is_main_thread());
    auto& impl = *m_impl;
    if (!impl.modelInitialized) return std::nullopt;

    const auto queue = CoreQueueSnapshot();
    const t_size queuePlaylist = QueuePlaylist(false);
    auto owned = BucketQueueItems(impl.ownedCoreQueue);

    std::optional<PlaybackOccurrenceId> target;
    std::size_t consumedQueueIndex = SIZE_MAX;
    metadb_handle_ptr targetHandle;

    if (command == play_control::track_command_next || command == play_control::track_command_rand) {
        if (!queue.empty()) {
            const auto& head = queue.front();
            auto ownedCheck = owned;
            const bool isOwned = TakeMatchingQueueItem(ownedCheck, head);
            if (!isOwned) {
                consumedQueueIndex = 0;
                targetHandle = head.m_handle;
            }
        }
        if (!targetHandle.is_valid()) target = impl.schedule.ManualNext();
    } else if (command == play_control::track_command_prev) {
        target = impl.schedule.Previous();
    } else {
        target = impl.exactStart ? impl.exactStart : impl.pendingClickStart;
    }

    if (target) {
        const auto found = impl.handles.find(*target);
        if (found != impl.handles.end()) targetHandle = found->second;
    }
    if (!targetHandle.is_valid()) return std::nullopt;

    StartingPreview result;
    result.track = targetHandle;
    result.scheduleRevision = impl.schedule.revision();

    // Capture the song leaving the card so History can show it immediately.
    metadb_handle_ptr now;
    if (playback_control::get()->get_now_playing(now) && now.is_valid() && now != targetHandle) {
        result.previousTrack = now;
    } else if (const auto cur = impl.schedule.Current(); cur && (!target || *cur != *target)) {
        if (const auto found = impl.handles.find(*cur); found != impl.handles.end() && found->second != targetHandle)
            result.previousTrack = found->second;
    }

    PlaybackSchedule preview = impl.schedule;
    if (target) {
        const auto current = impl.schedule.Current();
        const auto targetAt = impl.physicalIndex.find(*target);
        if (targetAt != impl.physicalIndex.end()) result.playingIndex = targetAt->second;

        if (!current || *current != *target) {
            const auto expectedNext = impl.schedule.ManualNext();
            const bool fromHistory = command == play_control::track_command_prev;
            const bool explicitStart = (impl.exactStart && *impl.exactStart == *target) ||
                                       (impl.pendingClickStart && *impl.pendingClickStart == *target) ||
                                       command == play_control::track_command_play ||
                                       ((command == play_control::track_command_default ||
                                         command == play_control::track_command_settrack) &&
                                        (!expectedNext || *expectedNext != *target));
            if (explicitStart && targetAt != impl.physicalIndex.end()) {
                preview.StartExplicit(std::span<const PlaybackOccurrenceId>(impl.physical), targetAt->second);
            } else {
                preview.Started(*target, fromHistory);
            }
        }

        // If the starting target sits in the explicit SDK core queue, omit that consumed entry.
        if (targetAt != impl.physicalIndex.end() && !queue.empty()) {
            for (std::size_t i = 0; i < queue.size(); ++i) {
                if (queue[i].m_playlist == queuePlaylist && queue[i].m_item == targetAt->second &&
                    queue[i].m_handle == targetHandle) {
                    consumedQueueIndex = i;
                    break;
                }
            }
        }
    }

    const auto upcoming = preview.DisplayUpcoming();
    result.repeatCycle = preview.RepeatCycleSize();
    result.upcoming.reserve(queue.size() + upcoming.size());
    std::unordered_set<std::size_t> coveredPhysical;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const auto& item = queue[i];
        const bool isOwned = TakeMatchingQueueItem(owned, item);
        if (i == consumedQueueIndex) continue;
        result.upcoming.push_back(
            {ScheduledUpcomingSource::CoreQueue, i, isOwned ? item.m_item : SIZE_MAX, item.m_handle});
        if (item.m_playlist == queuePlaylist && item.m_item < impl.physical.size())
            coveredPhysical.insert(item.m_item);
    }
    for (const auto occurrence : upcoming) {
        const auto index = impl.physicalIndex.find(occurrence);
        const auto handle = impl.handles.find(occurrence);
        if (index != impl.physicalIndex.end() && !coveredPhysical.contains(index->second))
            result.upcoming.push_back({ScheduledUpcomingSource::Playlist, SIZE_MAX, index->second,
                                       handle == impl.handles.end() ? metadb_handle_ptr() : handle->second});
    }
    return result;
}

std::optional<StartingPreview> PlaybackScheduler::PreviewStartingAt(std::size_t playlistIndex,
                                                                    const metadb_handle_ptr& previousTrack) {
    PFC_ASSERT(core_api::is_main_thread());
    auto& impl = *m_impl;
    if (!impl.modelInitialized || playlistIndex >= impl.physical.size()) return std::nullopt;
    const auto target = impl.physical[playlistIndex];
    const auto handleFound = impl.handles.find(target);
    if (handleFound == impl.handles.end() || !handleFound->second.is_valid()) return std::nullopt;

    StartingPreview result;
    result.track = handleFound->second;
    result.previousTrack = previousTrack;
    result.playingIndex = playlistIndex;
    result.scheduleRevision = impl.schedule.revision();

    PlaybackSchedule preview = impl.schedule;
    preview.StartExplicit(std::span<const PlaybackOccurrenceId>(impl.physical), playlistIndex);
    const auto queue = CoreQueueSnapshot();
    const t_size queuePlaylist = QueuePlaylist(false);
    auto owned = BucketQueueItems(impl.ownedCoreQueue);
    const auto upcoming = preview.DisplayUpcoming();
    result.repeatCycle = preview.RepeatCycleSize();
    result.upcoming.reserve(queue.size() + upcoming.size());
    std::unordered_set<std::size_t> coveredPhysical;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const auto& item = queue[i];
        const bool isOwned = TakeMatchingQueueItem(owned, item);
        if (item.m_playlist == queuePlaylist && item.m_item == playlistIndex && item.m_handle == result.track)
            continue;
        result.upcoming.push_back(
            {ScheduledUpcomingSource::CoreQueue, i, isOwned ? item.m_item : SIZE_MAX, item.m_handle});
        if (item.m_playlist == queuePlaylist && item.m_item < impl.physical.size())
            coveredPhysical.insert(item.m_item);
    }
    for (const auto occurrence : upcoming) {
        const auto index = impl.physicalIndex.find(occurrence);
        const auto handle = impl.handles.find(occurrence);
        if (index != impl.physicalIndex.end() && !coveredPhysical.contains(index->second))
            result.upcoming.push_back({ScheduledUpcomingSource::Playlist, SIZE_MAX, index->second,
                                       handle == impl.handles.end() ? metadb_handle_ptr() : handle->second});
    }
    return result;
}

std::uint64_t PlaybackScheduler::ScheduleRevision() const {
    return m_impl->schedule.revision();
}

std::uint64_t PlaybackScheduler::AddListener(std::function<void()> listener) {
    return m_impl->AddListener(std::move(listener));
}

void PlaybackScheduler::RemoveListener(std::uint64_t listener) {
    m_impl->RemoveListener(listener);
}

std::uint64_t PlaybackScheduler::ContentGeneration() const {
    return m_impl->contentGeneration;
}

void PlaybackScheduler::OnPlaybackStarting(play_control::t_track_command command, bool paused) {
    m_impl->OnPlaybackStarting(command, paused);
}

void PlaybackScheduler::OnTrackStarted(const metadb_handle_ptr& track) {
    m_impl->OnTrackStarted(track);
}

void PlaybackScheduler::OnPlaybackStopped(play_control::t_stop_reason reason) {
    m_impl->OnPlaybackStopped(reason);
}

void PlaybackScheduler::OnPlaybackTime() {
    m_impl->OnPlaybackTime();
}

void PlaybackScheduler::StopRequested() {
    PFC_ASSERT(core_api::is_main_thread());
    m_impl->exactStart.reset();
    m_impl->navigationTarget.reset();
}

void PlaybackScheduler::QueuePlaylistAdded(std::size_t base, const metadb_handle_list& items) {
    m_impl->QueuePlaylistAdded(base, items);
}

void PlaybackScheduler::QueuePlaylistReordered(const t_size* order, std::size_t count) {
    m_impl->QueuePlaylistReordered(order, count);
}

void PlaybackScheduler::QueuePlaylistRemoved(const bit_array& mask, std::size_t old_count) {
    m_impl->QueuePlaylistRemoved(mask, old_count);
}

void PlaybackScheduler::QueuePlaylistReplaced() {
    m_impl->QueuePlaylistReplaced();
}

void PlaybackScheduler::QueuePlaylistAvailabilityChanged() {
    m_impl->QueuePlaylistAvailabilityChanged();
}

void PlaybackScheduler::OnCoreQueueChanged() {
    m_impl->ScheduleCoreQueueChanged();
}

void PlaybackScheduler::CorePlaybackOrderChanged() {
    m_impl->OnNativeOrderMode();
}

void PlaybackScheduler::OnNativeOrderMode() {
    CorePlaybackOrderChanged();
}

namespace {

class SchedulerPlaybackEvents : public play_callback_static {
  public:
    unsigned get_flags() override {
        return flag_on_playback_starting | flag_on_playback_new_track | flag_on_playback_stop |
               flag_on_playback_time;
    }
    void on_playback_starting(play_control::t_track_command command, bool paused) override {
        if (auto* scheduler = PlaybackScheduler::Existing()) scheduler->OnPlaybackStarting(command, paused);
    }
    void on_playback_new_track(metadb_handle_ptr track) override {
        if (auto* scheduler = PlaybackScheduler::Existing()) scheduler->OnTrackStarted(track);
    }
    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (auto* scheduler = PlaybackScheduler::Existing()) scheduler->OnPlaybackStopped(reason);
    }
    void on_playback_time(double) override {
        if (auto* scheduler = PlaybackScheduler::Existing()) scheduler->OnPlaybackTime();
    }
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info&) override {}
    void on_playback_dynamic_info_track(const file_info&) override {}
    void on_volume_change(float) override {}
};

class SchedulerQueueEvents : public playback_queue_callback {
  public:
    void on_changed(t_change_origin) override {
        if (auto* scheduler = PlaybackScheduler::Existing()) scheduler->OnCoreQueueChanged();
    }
};

class SchedulerLifecycle : public initquit {
  public:
    void on_init() override { PlaybackScheduler::Start(); }
    void on_quit() override { PlaybackScheduler::Shutdown(); }
};

initquit_factory_t<SchedulerLifecycle> g_schedulerLifecycle;
play_callback_static_factory_t<SchedulerPlaybackEvents> g_schedulerPlayback;
service_factory_single_t<SchedulerQueueEvents> g_schedulerQueue;

} // namespace

} // namespace glasswave::media
