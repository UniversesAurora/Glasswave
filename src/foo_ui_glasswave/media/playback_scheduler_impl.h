#pragma once

// The playback scheduler's implementation, shared by media/playback_scheduler.cpp,
// playback_checkpoint.cpp and playback_core_queue.cpp: Impl, its settings and helpers.

#include "core/hex.h"
#include "core/json.h"
#include "core/text.h"
#include "core/signal.h"
#include "media/playback_scheduler.h"
#include "media/special_playlists.h"
#include <chrono>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace glasswave::media {

namespace scheduler {

// {6B42D913-0F86-4AA7-9C35-8A2F3D1E7054} / {6B42D914-...}
extern cfg_var_modern::cfg_int cfg_shuffle;

extern cfg_var_modern::cfg_int cfg_repeat;

extern cfg_var_modern::cfg_string cfg_checkpoint;

// The checkpoint's format; a checkpoint of another version is dropped.
constexpr int kCheckpointVersion = 3;

inline std::uint64_t NextSeed() {
    static std::uint64_t counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<std::uint64_t>(now) ^ (++counter * 0x9e3779b97f4a7c15ULL);
}

inline bool IsValidRepeat(int repeat) {
    return repeat >= 0 && repeat <= 2;
}

inline void AppendLe64(std::string& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

inline std::string HandleIdentityKey(const metadb_handle_ptr& handle) {
    if (handle.is_empty()) return {};
    std::string key;
    const char* path = handle->get_path();
    const std::string_view text = path ? std::string_view(path) : std::string_view();
    key.append(std::to_string(text.size()));
    key.push_back(':');
    key.append(text);
    AppendLe64(key, handle->get_subsong_index());
    return key;
}

using OwnedQueueBuckets = std::unordered_map<std::string, std::deque<t_playback_queue_item>>;

inline std::string QueueItemIdentityKey(const t_playback_queue_item& item) {
    std::string key = item.m_handle.is_empty() ? "0" : "1";
    if (!item.m_handle.is_empty()) key += HandleIdentityKey(item.m_handle);
    AppendLe64(key, item.m_playlist);
    AppendLe64(key, item.m_item);
    return key;
}

inline OwnedQueueBuckets BucketQueueItems(const std::vector<t_playback_queue_item>& items) {
    OwnedQueueBuckets buckets;
    buckets.reserve(items.size());
    for (const auto& item : items)
        buckets[QueueItemIdentityKey(item)].push_back(item);
    return buckets;
}

inline bool TakeMatchingQueueItem(OwnedQueueBuckets& buckets, const t_playback_queue_item& item) {
    const auto found = buckets.find(QueueItemIdentityKey(item));
    if (found == buckets.end() || found->second.empty() || found->second.front() != item) return false;
    found->second.pop_front();
    return true;
}

// The checkpoint's fields (core::json). A field that is missing or of another type reads as
// nothing, so a checkpoint of another shape is dropped instead of half applied.
namespace checkpoint_json {

namespace wdj = winrt::Windows::Data::Json;

inline std::optional<wdj::IJsonValue> Field(const core::json::Object& o, const wchar_t* key, wdj::JsonValueType type) {
    if (!o.HasKey(key)) return std::nullopt;
    auto value = o.GetNamedValue(key);
    if (value.ValueType() != type) return std::nullopt;
    return value;
}

inline std::optional<std::int64_t> Integer(const core::json::Object& o, const wchar_t* key) {
    const auto value = Field(o, key, wdj::JsonValueType::Number);
    if (!value) return std::nullopt;
    const double number = value->GetNumber();
    if (number != std::floor(number) || std::abs(number) > 9007199254740992.0) return std::nullopt;
    return static_cast<std::int64_t>(number);
}

inline std::optional<bool> Boolean(const core::json::Object& o, const wchar_t* key) {
    const auto value = Field(o, key, wdj::JsonValueType::Boolean);
    return value ? std::optional<bool>(value->GetBoolean()) : std::nullopt;
}

inline std::optional<std::string> Text(const core::json::Object& o, const wchar_t* key) {
    const auto value = Field(o, key, wdj::JsonValueType::String);
    return value ? std::optional<std::string>(core::Utf8(std::wstring(value->GetString()))) : std::nullopt;
}

// An array of row indices (whole numbers from 0).
inline std::optional<std::vector<std::size_t>> Indices(const core::json::Object& o, const wchar_t* key) {
    const auto value = Field(o, key, wdj::JsonValueType::Array);
    if (!value) return std::nullopt;
    std::vector<std::size_t> out;
    for (const auto& item : value->GetArray()) {
        if (item.ValueType() != wdj::JsonValueType::Number) return std::nullopt;
        const double number = item.GetNumber();
        if (number < 0 || number != std::floor(number) || number > 9007199254740992.0) return std::nullopt;
        out.push_back(static_cast<std::size_t>(number));
    }
    return out;
}

inline wdj::JsonArray IndexArray(const std::vector<std::size_t>& values) {
    wdj::JsonArray array;
    for (const auto value : values) array.Append(wdj::JsonValue::CreateNumberValue(static_cast<double>(value)));
    return array;
}

} // namespace checkpoint_json

// foobar2000's playback queue as it is now.
inline std::vector<t_playback_queue_item> CoreQueueSnapshot() {
    pfc::list_t<t_playback_queue_item> queue;
    playlist_manager::get()->queue_get_contents(queue);
    std::vector<t_playback_queue_item> items;
    items.reserve(queue.get_count());
    for (const auto& item : queue)
        items.push_back(item);
    return items;
}

inline bool SameScheduleState(const PlaybackScheduleState& left, const PlaybackScheduleState& right) {
    return left.canonicalOrder == right.canonicalOrder && left.projectionOrder == right.projectionOrder &&
           left.history == right.history && left.current == right.current &&
           left.priorityBlocks == right.priorityBlocks && left.ordinaryFuture == right.ordinaryFuture &&
           left.pendingCycle == right.pendingCycle && left.shuffle == right.shuffle &&
           left.repeat == right.repeat;
}

} // namespace scheduler

using namespace scheduler;

struct PlaybackScheduler::Impl {
    Impl();
    ~Impl();

    class PlaylistEvents;

    void Start();
    void Shutdown();
    bool SetPlaybackMode(bool shuffle, int repeat);
    bool PlayQueue(const metadb_handle_list& items, std::size_t start);
    bool AdoptPlaying(const metadb_handle_list& items, std::size_t start);
    bool PlayNext(const metadb_handle_list& items);
    bool AddToQueue(const metadb_handle_list& items);
    bool AppendSourceBatch(const metadb_handle_list& items, std::uint64_t expectedGeneration);
    bool InsertNewOccurrences(const metadb_handle_list& items, bool playNext,
                              std::optional<std::uint64_t> expectedGeneration = std::nullopt);
    std::vector<std::size_t> ScheduledUpcomingIndices();
    std::vector<ScheduledUpcomingEntry> ScheduledUpcomingSnapshot(bool display = false);
    bool ReorderUpcomingSnapshot(std::uint64_t expectedRevision,
                                 const std::vector<ScheduledUpcomingEntry>& ordered);
    std::optional<std::size_t> ScheduledNextIndex();
    std::uint64_t AddListener(std::function<void()> listener);
    void RemoveListener(std::uint64_t listener);

    void OnPlaybackStarting(play_control::t_track_command command, bool paused);
    void OnTrackStarted(const metadb_handle_ptr& track);
    void OnPlaybackStopped(play_control::t_stop_reason reason);
    void OnPlaybackTime();

    void QueuePlaylistAdded(std::size_t base, const metadb_handle_list& items);
    void QueuePlaylistReordered(const t_size* order, std::size_t count);
    void QueuePlaylistRemoved(const bit_array& mask, std::size_t old_count);
    void QueuePlaylistReplaced();
    void QueuePlaylistAvailabilityChanged();
    void ScheduleCoreQueueChanged();
    void OnNativeOrderMode();

    void ScheduleSynchronize();
    void CancelSynchronize();
    void Synchronize();
    void ApplyCoreOrder();
    void SaveCheckpoint(bool force = false);
    bool LoadCheckpoint(const metadb_handle_list& items);

    std::size_t EnsureSidecar();
    void RebuildSidecar(const metadb_handle_list& items);
    PlaybackOccurrenceId AllocateOccurrence();
    std::vector<PlaybackOccurrenceId> AllocateOccurrences(const metadb_handle_list& items);
    metadb_handle_list HandlesFor(std::span<const PlaybackOccurrenceId> occurrences) const;
    bool ApplyProjection();
    bool ApplyPhysicalOrder(const std::vector<PlaybackOccurrenceId>& desired);
    void SetPhysical(std::vector<PlaybackOccurrenceId> order);
    void ReindexPhysical();
    std::vector<PlaybackOccurrenceId> ResetModelFresh(const metadb_handle_list& items, std::size_t start);
    void RemapOwnedCoreQueue(const std::vector<PlaybackOccurrenceId>& oldPhysical);
    bool RebuildCoreQueue(const std::vector<t_playback_queue_item>& items);
    bool PrependCoreQueue(std::span<const PlaybackOccurrenceId> occurrences);
    bool RemoveOwnedCoreQueueEntries();
    bool RemoveCoreQueueItem(const t_playback_queue_item& item);
    void ReconcileOwnedCoreQueue(const std::vector<t_playback_queue_item>& queue) const;
    bool StartOccurrence(PlaybackOccurrenceId occurrence, bool paused = false);
    bool StartExplicitAt(PlaybackOccurrenceId occurrence);
    void HandleNavigationStart(play_control::t_track_command command, bool paused);
    void HandleTrackStarted(PlaybackOccurrenceId occurrence);
    void HandleStop(play_control::t_stop_reason reason);
    void HandleTime();
    bool QueueIsEngaged() const;
    void Post(std::function<void()> task);
    void RequestListenerNotification();
    void InvalidatePosted();
    void EnsureModel();

    PlaybackSchedule schedule;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
    std::unique_ptr<PlaylistEvents> playlistEvents;
    std::vector<PlaybackOccurrenceId> physical;
    std::unordered_map<PlaybackOccurrenceId, std::size_t> physicalIndex;
    std::map<PlaybackOccurrenceId, metadb_handle_ptr> handles;
    mutable std::vector<t_playback_queue_item> ownedCoreQueue;
    core::Signal<> listeners;
    PlaybackScheduleState savedState;
    std::string savedFingerprint;
    std::string savedCheckpoint;
    std::uint64_t nextOccurrence = 1;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t contentGeneration = 0;
    std::uint64_t syncGeneration = 0;
    std::uint64_t expectedOrderEchoes = 0;
    std::string expectedOrderName; // the core order we set, whose own callback is its echo
    bool started = false;
    bool modelInitialized = false;
    bool mapGuard = false;
    bool applyingCoreOrder = false;
    bool coreOrderCallbackSeen = false;
    bool syncQueued = false;
    bool coreQueueSyncQueued = false;
    bool listenerNotifyQueued = false;
    bool reorderDirty = false;
    bool wholeReplaceDirty = false;
    bool timeQueued = false;
    bool checkpointDirty = false;
    bool checkpointValid = false;
    play_control::t_track_command startCommand = play_control::track_command_default;
    bool startPaused = false;
    std::optional<PlaybackOccurrenceId> navigationTarget;
    std::optional<PlaybackOccurrenceId> exactStart;
    std::optional<PlaybackOccurrenceId> pendingClickStart;
    unsigned failureBurst = 0;
};

class PlaybackScheduler::Impl::PlaylistEvents final : public playlist_callback_impl_base {
  public:
    explicit PlaylistEvents(Impl& owner)
        : playlist_callback_impl_base(flag_on_items_added | flag_on_items_reordered | flag_on_items_removed |
                                      flag_on_items_replaced | flag_on_playlist_activate |
                                      flag_on_playlists_reorder | flag_on_playlists_removing |
                                      flag_on_playlists_removed | flag_on_playlist_locked),
          impl(owner) {}

    void on_items_added(t_size playlist, t_size base, metadb_handle_list_cref data,
                        const bit_array&) override {
        if (IsQueue(playlist)) impl.QueuePlaylistAdded(base, data);
    }
    void on_items_reordered(t_size playlist, const t_size* order, t_size count) override {
        if (IsQueue(playlist)) impl.QueuePlaylistReordered(order, count);
    }
    void on_items_removed(t_size playlist, const bit_array& mask, t_size old_count, t_size) override {
        if (IsQueue(playlist)) impl.QueuePlaylistRemoved(mask, old_count);
    }
    void on_items_replaced(
        t_size playlist, const bit_array&,
        const pfc::list_base_const_t<playlist_callback::t_on_items_replaced_entry>&) override {
        if (IsQueue(playlist)) impl.QueuePlaylistReplaced();
    }
    void on_playlist_activate(t_size, t_size) override { impl.QueuePlaylistAvailabilityChanged(); }
    void on_playlists_reorder(const t_size*, t_size) override { impl.QueuePlaylistAvailabilityChanged(); }
    void on_playlists_removing(const bit_array&, t_size, t_size) override {
        impl.QueuePlaylistAvailabilityChanged();
    }
    void on_playlists_removed(const bit_array&, t_size, t_size) override {
        impl.QueuePlaylistAvailabilityChanged();
    }
    void on_playlist_locked(t_size playlist, bool) override {
        if (IsQueue(playlist)) impl.QueuePlaylistAvailabilityChanged();
    }

  private:
    Impl& impl;
};

} // namespace glasswave::media
