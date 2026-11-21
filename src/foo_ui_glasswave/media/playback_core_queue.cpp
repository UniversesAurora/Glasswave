#include "pch.h"
#include "media/playback_scheduler_impl.h"

// The scheduler's mirror of foobar2000's playback queue: the entries it owns there, kept
// pointing at the right rows when the queue playlist moves, and rebuilt or pruned with it.
namespace glasswave::media {

using namespace scheduler;

namespace {

bool CanReplayQueueItem(const t_playback_queue_item& item) {
    if (item.m_handle.is_empty() && item.m_playlist == SIZE_MAX) return false;
    auto manager = playlist_manager::get();
    if (item.m_playlist < manager->get_playlist_count() &&
        item.m_item < manager->playlist_get_item_count(item.m_playlist)) {
        metadb_handle_ptr handle;
        if (manager->playlist_get_item_handle(handle, item.m_playlist, item.m_item) &&
            handle == item.m_handle)
            return true;
    }
    return item.m_handle.is_valid();
}

} // namespace

void PlaybackScheduler::Impl::RemapOwnedCoreQueue(const std::vector<PlaybackOccurrenceId>& oldPhysical) {
    const t_size queue = QueuePlaylist(false);
    std::vector<t_playback_queue_item> remapped;
    remapped.reserve(ownedCoreQueue.size());
    for (auto item : ownedCoreQueue) {
        if (queue == SIZE_MAX || item.m_playlist != queue || item.m_item >= oldPhysical.size()) continue;
        const auto occurrence = oldPhysical[item.m_item];
        const auto handle = handles.find(occurrence);
        const auto index = physicalIndex.find(occurrence);
        if (handle == handles.end() || index == physicalIndex.end() || handle->second != item.m_handle)
            continue;
        item.m_item = index->second;
        remapped.push_back(std::move(item));
    }
    ownedCoreQueue = std::move(remapped);
}

bool PlaybackScheduler::Impl::RebuildCoreQueue(const std::vector<t_playback_queue_item>& items) {
    for (const auto& item : items)
        if (!CanReplayQueueItem(item)) return false;

    auto manager = playlist_manager::get();
    // The queue as it is now, to put back if the rebuild fails half way.
    const std::vector<t_playback_queue_item> before = CoreQueueSnapshot();
    const auto replace = [&](const std::vector<t_playback_queue_item>& list) {
        manager->queue_remove_mask(pfc::bit_array_true());
        for (const auto& item : list) {
            if (item.m_playlist < manager->get_playlist_count() &&
                item.m_item < manager->playlist_get_item_count(item.m_playlist)) {
                metadb_handle_ptr handle;
                if (manager->playlist_get_item_handle(handle, item.m_playlist, item.m_item) &&
                    handle == item.m_handle) {
                    manager->queue_add_item_playlist(item.m_playlist, item.m_item);
                    continue;
                }
            }
            manager->queue_add_item(item.m_handle);
        }
    };
    try {
        replace(items);
        return true;
    } catch (...) {
        try {
            replace(before);
        } catch (...) {
        }
        return false;
    }
}

void PlaybackScheduler::Impl::ReconcileOwnedCoreQueue(const std::vector<t_playback_queue_item>& queue) const {
    auto buckets = BucketQueueItems(ownedCoreQueue);
    std::vector<t_playback_queue_item> retained;
    for (const auto& item : queue) {
        if (TakeMatchingQueueItem(buckets, item)) retained.push_back(item);
    }
    ownedCoreQueue = std::move(retained);
}

bool PlaybackScheduler::Impl::PrependCoreQueue(std::span<const PlaybackOccurrenceId> occurrences) {
    const t_size queue = QueuePlaylist(false);
    if (queue == SIZE_MAX) return false;
    std::vector<t_playback_queue_item> snapshot = CoreQueueSnapshot();
    ReconcileOwnedCoreQueue(snapshot);

    std::vector<t_playback_queue_item> block;
    block.reserve(occurrences.size());
    for (const auto occurrence : occurrences) {
        const auto index = physicalIndex.find(occurrence);
        const auto handle = handles.find(occurrence);
        if (index == physicalIndex.end() || handle == handles.end()) return false;
        block.push_back({handle->second, queue, index->second});
    }

    std::vector<t_playback_queue_item> rebuilt = block;
    rebuilt.reserve(block.size() + snapshot.size());
    rebuilt.insert(rebuilt.end(), snapshot.begin(), snapshot.end());
    if (!RebuildCoreQueue(rebuilt)) return false;

    auto previousOwned = BucketQueueItems(ownedCoreQueue);
    ownedCoreQueue = std::move(block);
    for (const auto& item : snapshot) {
        if (TakeMatchingQueueItem(previousOwned, item)) ownedCoreQueue.push_back(item);
    }
    return true;
}

bool PlaybackScheduler::Impl::RemoveOwnedCoreQueueEntries() {
    std::vector<t_playback_queue_item> snapshot = CoreQueueSnapshot();
    ReconcileOwnedCoreQueue(snapshot);
    if (ownedCoreQueue.empty()) return true;

    auto owned = BucketQueueItems(ownedCoreQueue);
    std::vector<t_playback_queue_item> retained;
    for (const auto& item : snapshot) {
        if (!TakeMatchingQueueItem(owned, item)) retained.push_back(item);
    }
    if (!RebuildCoreQueue(retained)) return false;
    ownedCoreQueue.clear();
    return true;
}

bool PlaybackScheduler::Impl::RemoveCoreQueueItem(const t_playback_queue_item& item) {
    std::vector<t_playback_queue_item> updated;
    bool removed = false;
    for (const auto& current : CoreQueueSnapshot()) {
        if (!removed && current == item) {
            removed = true;
            continue;
        }
        updated.push_back(current);
    }
    if (!removed || !RebuildCoreQueue(updated)) return false;
    ReconcileOwnedCoreQueue(updated);
    return true;
}

} // namespace glasswave::media
