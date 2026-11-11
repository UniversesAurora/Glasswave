#include "pch.h"

#include "media/playback_scheduler.h"
#include "media/special_playlists.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace glasswave::media::scheduler_probe {
namespace {

namespace fs = std::filesystem;
namespace json = winrt::Windows::Data::Json;

constexpr std::string_view kProbe = "playback-scheduler";
constexpr std::size_t kMaxMarkerBytes = 4096;
constexpr auto kMainCallTimeout = std::chrono::seconds(15);

struct Paths {
    fs::path request;
    fs::path claimed;
    fs::path result;
};

struct CaseResult {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct ProbeResult {
    std::string runId;
    std::string error;
    bool installed = false;
    bool restored = false;
    std::vector<CaseResult> cases;
};

struct NamedHandle {
    const char* name;
    metadb_handle_ptr handle;
};

struct MainCall {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    bool value = false;
};

struct RunControl {
    std::mutex mutex;
    std::shared_ptr<MainCall> active;
    std::atomic_bool stopping = false;
};

struct OriginalState {
    t_size queuePlaylist = SIZE_MAX;
    metadb_handle_list items;
    std::vector<bool> selected;
    t_size focus = SIZE_MAX;
    std::vector<t_playback_queue_item> coreQueue;
    t_size activePlaylist = SIZE_MAX;
    t_size playingPlaylist = SIZE_MAX;
    t_size coreOrder = SIZE_MAX;
    PlaybackSchedulerProbeAccess::HostState host;
};

struct RestoreOutcome {
    bool ok = false;
    std::string detail;
};

bool SameName(const fs::path& left, const fs::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::optional<fs::path> FinalPath(const fs::path& path) {
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (!length || length >= buffer.size()) return std::nullopt;
    std::wstring value(buffer.data(), length);
    if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
    while (value.size() > value.find_first_not_of(L"\\/") &&
           (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return fs::path(value).lexically_normal();
}

std::optional<Paths> ProbePaths() {
    pfc::string8 native;
    try {
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    const auto profile =
        FinalPath(fs::path(pfc::stringcvt::string_wide_from_utf8(native.get_ptr()).get_ptr()));
    if (!profile || !SameName(profile->filename(), L"profile") ||
        !SameName(profile->parent_path().filename(), L"fb2k-dev") ||
        !SameName(profile->parent_path().parent_path().filename(), L".local")) {
        return std::nullopt;
    }
    const auto directory = *profile / L"glasswave" / L"scheduler-probe";
    return Paths{directory / L"request.json", directory / L"request.claimed.json",
                 directory / L"result.json"};
}

std::optional<std::string> ReadBounded(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > kMaxMarkerBytes) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) return std::nullopt;
    return bytes;
}

std::optional<std::string> ReadMarker(const fs::path& path) {
    const auto bytes = ReadBounded(path);
    if (!bytes) return std::nullopt;
    try {
        const auto object = json::JsonObject::Parse(winrt::to_hstring(*bytes));
        if (object.GetNamedString(L"probe") != L"playback-scheduler") return std::nullopt;
        const auto runId = winrt::to_string(object.GetNamedString(L"runId"));
        if (runId.size() < 8 || runId.size() > 64) return std::nullopt;
        return runId;
    } catch (...) {
        return std::nullopt;
    }
}

bool OnMain(RunControl& control, abort_callback& abort, std::function<bool()> action) {
    auto call = std::make_shared<MainCall>();
    {
        std::lock_guard lock(control.mutex);
        if (control.stopping || abort.is_aborting() || control.active) return false;
        control.active = call;
    }
    try {
        fb2k::inMainThread([call, action = std::move(action)]() mutable {
            bool ok = false;
            try {
                ok = action();
            } catch (...) {
            }
            {
                std::lock_guard lock(call->mutex);
                call->value = ok;
                call->done = true;
            }
            call->changed.notify_all();
        });
    } catch (...) {
        std::lock_guard lock(control.mutex);
        control.active.reset();
        return false;
    }
    std::unique_lock lock(call->mutex);
    const bool finished =
        call->changed.wait_until(lock, std::chrono::steady_clock::now() + kMainCallTimeout,
                                 [&] { return call->done || control.stopping || abort.is_aborting(); });
    {
        std::lock_guard controlLock(control.mutex);
        if (control.active == call) control.active.reset();
    }
    return finished && call->done && call->value && !control.stopping && !abort.is_aborting();
}

bool PumpMain(RunControl& control, abort_callback& abort) {
    return OnMain(control, abort, [] { return true; });
}

metadb_handle_ptr MakeHandle(const std::string& runId, const char* name) {
    const std::string path = "glasswave-probe://" + runId + "/" + name;
    return metadb::get()->handle_create(path.c_str(), 0);
}

void Append(metadb_handle_list& list, const metadb_handle_ptr& handle) {
    list.add_item(handle);
}

std::vector<t_playback_queue_item> CoreQueueSnapshot() {
    pfc::list_t<t_playback_queue_item> queue;
    playlist_manager::get()->queue_get_contents(queue);
    std::vector<t_playback_queue_item> out;
    out.reserve(queue.get_count());
    for (const auto& item : queue)
        out.push_back(item);
    return out;
}

bool CanReplay(const t_playback_queue_item& item) {
    auto manager = playlist_manager::get();
    if (item.m_handle.is_empty() && item.m_playlist == SIZE_MAX) return false;
    if (item.m_playlist < manager->get_playlist_count() &&
        item.m_item < manager->playlist_get_item_count(item.m_playlist)) {
        metadb_handle_ptr handle;
        if (manager->playlist_get_item_handle(handle, item.m_playlist, item.m_item) &&
            handle == item.m_handle) {
            return true;
        }
    }
    return item.m_handle.is_valid();
}

bool RebuildQueue(const std::vector<t_playback_queue_item>& items) {
    for (const auto& item : items)
        if (!CanReplay(item)) return false;
    auto manager = playlist_manager::get();
    manager->queue_remove_mask(pfc::bit_array_true());
    for (const auto& item : items) {
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
    return true;
}

OriginalState CaptureOriginal() {
    OriginalState state;
    auto manager = playlist_manager::get();
    state.queuePlaylist = QueuePlaylist(false);
    if (state.queuePlaylist != SIZE_MAX) {
        manager->playlist_get_all_items(state.queuePlaylist, state.items);
        pfc::bit_array_bittable mask(state.items.get_count());
        manager->playlist_get_selection_mask(state.queuePlaylist, mask);
        state.selected.reserve(state.items.get_count());
        for (t_size i = 0; i < state.items.get_count(); ++i)
            state.selected.push_back(mask[i]);
        state.focus = manager->playlist_get_focus_item(state.queuePlaylist);
    }
    state.coreQueue = CoreQueueSnapshot();
    state.activePlaylist = manager->get_active_playlist();
    state.playingPlaylist = manager->get_playing_playlist();
    state.coreOrder = manager->playback_order_get_active();
    state.host = PlaybackSchedulerProbeAccess::Capture();
    return state;
}

RestoreOutcome RestoreOriginal(const OriginalState& state) {
    if (state.queuePlaylist == SIZE_MAX) return {false, "queue_playlist_missing"};
    auto manager = playlist_manager::get();
    std::string stage = "queue_playlist_missing";
    try {
        stage = "playlist_clear";
        manager->playlist_undo_backup(state.queuePlaylist);
        manager->playlist_clear(state.queuePlaylist);
        stage = "playlist_add";
        if (!manager->playlist_add_items(state.queuePlaylist, state.items, pfc::bit_array_false()))
            return {false, stage};
        stage = "selection";
        pfc::bit_array_bittable selected(state.items.get_count());
        for (t_size i = 0; i < state.selected.size(); ++i)
            selected.set(i, state.selected[i]);
        manager->playlist_set_selection(state.queuePlaylist, pfc::bit_array_true(), selected);
        stage = "focus";
        manager->playlist_set_focus_item(state.queuePlaylist, state.focus);
        stage = "core_queue";
        if (!RebuildQueue(state.coreQueue)) return {false, stage};
        stage = "core_order";
        manager->playback_order_set_active(state.coreOrder);
        stage = "active_playlist";
        manager->set_active_playlist(state.activePlaylist);
        stage = "playing_playlist";
        manager->set_playing_playlist(state.playingPlaylist);
        stage = "host_state";
        if (!PlaybackSchedulerProbeAccess::Restore(state.host)) return {false, stage};
    } catch (...) {
        return {false, stage + "_exception"};
    }
    return {true, {}};
}

bool SameOriginalState(const OriginalState& state, std::string& detail) {
    const auto fail = [&](std::string value) {
        detail = std::move(value);
        return false;
    };
    auto manager = playlist_manager::get();
    if (QueuePlaylist(false) != state.queuePlaylist) return fail("queue_playlist_id");
    metadb_handle_list items;
    manager->playlist_get_all_items(state.queuePlaylist, items);
    if (items.get_count() != state.items.get_count()) return fail("playlist_item_count");
    for (t_size i = 0; i < items.get_count(); ++i)
        if (items[i] != state.items[i]) return fail("playlist_item_" + std::to_string(i));

    pfc::bit_array_bittable selection(items.get_count());
    manager->playlist_get_selection_mask(state.queuePlaylist, selection);
    if (state.selected.size() != items.get_count()) return fail("selection_count");
    for (t_size i = 0; i < items.get_count(); ++i)
        if (selection[i] != state.selected[i]) return fail("selection_" + std::to_string(i));
    if (manager->playlist_get_focus_item(state.queuePlaylist) != state.focus) return fail("focus");
    const auto queue = CoreQueueSnapshot();
    if (queue.size() != state.coreQueue.size()) return fail("core_queue_size");
    for (std::size_t i = 0; i < queue.size(); ++i)
        if (queue[i] != state.coreQueue[i]) return fail("core_queue_" + std::to_string(i));
    if (manager->get_active_playlist() != state.activePlaylist) return fail("active_playlist");
    if (manager->get_playing_playlist() != state.playingPlaylist) return fail("playing_playlist");
    if (manager->playback_order_get_active() != state.coreOrder) return fail("core_order");
    if (!PlaybackSchedulerProbeAccess::Same(state.host, PlaybackSchedulerProbeAccess::Capture()))
        return fail("host_state");
    return true;
}

bool RestoreAndCompare(const OriginalState& state, std::string& detail) {
    const auto outcome = RestoreOriginal(state);
    if (!outcome.ok) {
        detail = outcome.detail;
        return false;
    }
    return SameOriginalState(state, detail);
}

bool SnapshotPlaylist(metadb_handle_list& items) {
    const t_size queue = QueuePlaylist(false);
    if (queue == SIZE_MAX) return false;
    playlist_manager::get()->playlist_get_all_items(queue, items);
    return true;
}

bool QueueHandlesEqual(const std::vector<metadb_handle_ptr>& expected) {
    const auto queue = CoreQueueSnapshot();
    if (queue.size() != expected.size()) return false;
    for (std::size_t i = 0; i < queue.size(); ++i)
        if (queue[i].m_handle != expected[i]) return false;
    return true;
}

std::string HandleSymbol(const metadb_handle_ptr& handle, const std::vector<NamedHandle>& names) {
    for (const auto& item : names)
        if (item.handle == handle) return item.name;
    return "unknown";
}

std::string QueueDescription(const std::vector<NamedHandle>& names) {
    std::string detail = "queue=[";
    const auto queue = CoreQueueSnapshot();
    for (std::size_t i = 0; i < queue.size(); ++i) {
        if (i) detail += ',';
        detail += HandleSymbol(queue[i].m_handle, names);
    }
    return detail + ']';
}

std::string SnapshotDescription(const std::vector<NamedHandle>& names) {
    const auto snapshot = PlaybackScheduler::Get().ScheduledUpcomingSnapshot();
    std::string detail = "snapshot=[";
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        if (i) detail += ',';
        detail += snapshot[i].source == ScheduledUpcomingSource::CoreQueue ? "core:" : "playlist:";
        detail += HandleSymbol(snapshot[i].handle, names);
        detail += ":queue=" + std::to_string(snapshot[i].queueIndex);
        detail += ":playlist=" + std::to_string(snapshot[i].playlistIndex);
    }
    return detail + ']';
}

std::string ModeDescription() {
    auto manager = playlist_manager::get();
    const t_size order = manager->playback_order_get_active();
    return "shuffle=" + std::to_string(PlaybackScheduler::Get().QueueShuffleEnabled() ? 1 : 0) +
           ",repeat=" + std::to_string(PlaybackScheduler::Get().QueueRepeatMode()) + ",order=" +
           (order < manager->playback_order_get_count() ? manager->playback_order_get_name(order)
                                                        : "invalid");
}

std::size_t HandleIndex(const metadb_handle_list& items, const metadb_handle_ptr& handle) {
    for (std::size_t i = 0; i < items.get_count(); ++i)
        if (items[i] == handle) return i;
    return SIZE_MAX;
}

std::size_t OrderIndex(const char* name) {
    auto manager = playlist_manager::get();
    for (t_size i = 0; i < manager->playback_order_get_count(); ++i)
        if (pfc::stricmp_ascii(manager->playback_order_get_name(i), name) == 0) return i;
    return SIZE_MAX;
}

void AddCase(ProbeResult& result, const char* name, bool ok, std::string detail = {}) {
    result.cases.push_back({name, ok, std::move(detail)});
}

bool AllCasesPass(const ProbeResult& result) {
    return !result.cases.empty() &&
           std::all_of(result.cases.begin(), result.cases.end(), [](const auto& item) { return item.ok; });
}

bool WriteResultFile(const fs::path& path, const ProbeResult& result, bool ok) {
    json::JsonObject object;
    object.Insert(L"probe", json::JsonValue::CreateStringValue(L"playback-scheduler"));
    object.Insert(L"runId", json::JsonValue::CreateStringValue(winrt::to_hstring(result.runId)));
    object.Insert(L"ok", json::JsonValue::CreateBooleanValue(ok));
    object.Insert(L"error", result.error.empty()
                                ? json::JsonValue::CreateNullValue()
                                : json::JsonValue::CreateStringValue(winrt::to_hstring(result.error)));
    object.Insert(L"installed", json::JsonValue::CreateBooleanValue(result.installed));
    object.Insert(L"restored", json::JsonValue::CreateBooleanValue(result.restored));
    json::JsonArray cases;
    for (const auto& item : result.cases) {
        json::JsonObject row;
        row.Insert(L"name", json::JsonValue::CreateStringValue(winrt::to_hstring(item.name)));
        row.Insert(L"ok", json::JsonValue::CreateBooleanValue(item.ok));
        row.Insert(L"detail", item.detail.empty()
                                  ? json::JsonValue::CreateNullValue()
                                  : json::JsonValue::CreateStringValue(winrt::to_hstring(item.detail)));
        cases.Append(row);
    }
    object.Insert(L"cases", cases);
    object.Insert(L"caseCount", json::JsonValue::CreateNumberValue(static_cast<double>(result.cases.size())));
    const auto temporary = path.wstring() + L".tmp";
    const auto bytes = winrt::to_string(object.Stringify());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
           FALSE;
}

void RunCases(const std::string& runId, RunControl& control, abort_callback& abort, ProbeResult& result) {
    const auto a = MakeHandle(runId, "a");
    const auto b = MakeHandle(runId, "b");
    const auto c = MakeHandle(runId, "c");
    const auto d = MakeHandle(runId, "play-next-d");
    const auto e = MakeHandle(runId, "play-next-e");
    const auto f1 = MakeHandle(runId, "foreign-1");
    const auto f2 = MakeHandle(runId, "foreign-2");
    const auto x = MakeHandle(runId, "replacement-x");
    const auto repeatNext = MakeHandle(runId, "repeat-one-priority");
    const std::vector<NamedHandle> symbols{{"a", a},   {"b", b}, {"c", c},
                                           {"d", d},   {"e", e}, {"f1", f1},
                                           {"f2", f2}, {"x", x}, {"repeat", repeatNext}};

    if (!OnMain(control, abort, [] { return PlaybackSchedulerProbeAccess::Install(); })) {
        result.error = "probe_install_failed";
        return;
    }
    result.installed = true;

    std::string emptyStateDetail;
    const bool emptyStateOk = OnMain(control, abort, [a, c, &emptyStateDetail] {
        const auto valid = CaptureOriginal();
        auto manager = playlist_manager::get();
        bool ok = false;
        try {
            manager->set_active_playlist(SIZE_MAX);
            manager->set_playing_playlist(SIZE_MAX);
            const auto empty = CaptureOriginal();
            if (empty.activePlaylist != SIZE_MAX) {
                emptyStateDetail = "sdk_active_playlist_sentinel_rejected";
            } else if (empty.playingPlaylist != SIZE_MAX) {
                emptyStateDetail = "sdk_playing_playlist_sentinel_rejected";
            } else {
                metadb_handle_list replacement;
                Append(replacement, a);
                Append(replacement, c);
                manager->playlist_clear(valid.queuePlaylist);
                if (!manager->playlist_add_items(valid.queuePlaylist, replacement, pfc::bit_array_false())) {
                    emptyStateDetail = "empty_case_mutation_failed";
                } else {
                    manager->set_active_playlist(valid.queuePlaylist);
                    manager->set_playing_playlist(valid.queuePlaylist);
                    ok = RestoreAndCompare(empty, emptyStateDetail);
                    if (ok && (manager->get_active_playlist() != SIZE_MAX ||
                               manager->get_playing_playlist() != SIZE_MAX)) {
                        emptyStateDetail = "empty_state_getter_mismatch";
                        ok = false;
                    }
                }
            }
        } catch (...) {
            emptyStateDetail = "empty_state_case_exception";
            ok = false;
        }

        std::string cleanupDetail;
        if (!RestoreAndCompare(valid, cleanupDetail)) {
            ok = false;
            emptyStateDetail = cleanupDetail.empty() ? "valid_checkpoint_restore_failed"
                                                     : "valid_checkpoint_restore_failed:" + cleanupDetail;
        } else if (ok) {
            emptyStateDetail = "active=SIZE_MAX;playing=SIZE_MAX";
        }
        return ok;
    });
    AddCase(result, "empty_active_playing_playlist_restore", emptyStateOk, emptyStateDetail);

    metadb_handle_list first;
    Append(first, a);
    Append(first, b);
    Append(first, c);
    if (!OnMain(control, abort, [first] { return PlaybackScheduler::Get().PlayQueue(first, 0); })) {
        result.error = "queue_setup_failed";
        return;
    }

    metadb_handle_list explicitItems;
    Append(explicitItems, a);
    Append(explicitItems, b);
    Append(explicitItems, c);
    Append(explicitItems, a);
    Append(explicitItems, c);
    const auto explicitStartKeepsOrder = [&](bool shuffle) {
        return OnMain(control, abort, [explicitItems, shuffle] {
            auto& scheduler = PlaybackScheduler::Get();
            if (!scheduler.SetPlaybackMode(shuffle, 0)) return false;
            if (!scheduler.PlayQueue(explicitItems, 1)) return false;
            const auto physical = PlaybackSchedulerProbeAccess::PhysicalOccurrences();
            if (physical.size() != explicitItems.get_count()) return false;
            const std::size_t selected = shuffle ? 3 : 4;
            if (!PlaybackSchedulerProbeAccess::AdvanceExplicit(physical[selected])) return false;
            return PlaybackSchedulerProbeAccess::PhysicalOccurrences() == physical &&
                   PlaybackSchedulerProbeAccess::ScheduleState().current == physical[selected];
        });
    };
    AddCase(result, "explicit_start_keeps_duplicate_rows_in_place", explicitStartKeepsOrder(false));
    AddCase(result, "explicit_start_keeps_shuffled_rows_in_place", explicitStartKeepsOrder(true));
    if (!OnMain(control, abort,
                [first] {
                    return PlaybackScheduler::Get().SetPlaybackMode(false, 0) &&
                           PlaybackScheduler::Get().PlayQueue(first, 0);
                }) ||
        !OnMain(control, abort, [f1, f2] {
            auto manager = playlist_manager::get();
            manager->queue_add_item(f1);
            manager->queue_add_item(f1);
            manager->queue_add_item(f2);
            return true;
        })) {
        result.error = "queue_setup_failed";
        return;
    }

    metadb_handle_list priority;
    Append(priority, d);
    Append(priority, e);
    if (!OnMain(control, abort, [priority] { return PlaybackScheduler::Get().PlayNext(priority); })) {
        result.error = "play_next_failed";
        return;
    }
    AddCase(result, "play_next_prepends_foreign_queue",
            OnMain(control, abort, [d, e, f1, f2] { return QueueHandlesEqual({d, e, f1, f1, f2}); }));
    AddCase(result, "owned_queue_mirror_follows_shuffle_reorder", OnMain(control, abort, [e] {
                auto& scheduler = PlaybackScheduler::Get();
                if (!scheduler.SetPlaybackMode(true, 1) || !scheduler.SetPlaybackMode(false, 1)) return false;
                const auto snapshot = scheduler.ScheduledUpcomingSnapshot();
                bool ownedFound = false;
                std::size_t foreignCount = 0;
                for (const auto& row : snapshot) {
                    if (row.source != ScheduledUpcomingSource::CoreQueue) continue;
                    if (row.handle == e) ownedFound = row.playlistIndex != SIZE_MAX;
                    if (row.playlistIndex == SIZE_MAX) ++foreignCount;
                }
                return ownedFound && foreignCount == 3;
            }));
    const bool explicitPriorityStart = OnMain(control, abort, [b, d, e, f1, f2] {
        const auto physical = PlaybackSchedulerProbeAccess::PhysicalOccurrences();
        const auto projection = PlaybackSchedulerProbeAccess::ScheduleState().projectionOrder;
        const auto snapshot = PlaybackScheduler::Get().ScheduledUpcomingSnapshot();
        const auto selected = std::find_if(snapshot.begin(), snapshot.end(), [b](const auto& row) {
            return row.source == ScheduledUpcomingSource::Playlist && row.handle == b;
        });
        if (selected == snapshot.end() || selected->playlistIndex >= physical.size()) return false;
        const auto occurrence = physical[selected->playlistIndex];
        if (!PlaybackSchedulerProbeAccess::AdvanceExplicit(occurrence)) return false;
        return PlaybackSchedulerProbeAccess::PhysicalOccurrences() == physical &&
               PlaybackSchedulerProbeAccess::ScheduleState().projectionOrder == projection &&
               PlaybackSchedulerProbeAccess::ScheduleState().current == occurrence &&
               QueueHandlesEqual({d, e, f1, f1, f2});
    });
    AddCase(result, "explicit_start_preserves_rows_and_play_next_queue", explicitPriorityStart);
    AddCase(result, "explicit_start_consumes_only_selected_priority_row",
            OnMain(control, abort, [d, e, f1, f2] {
                const auto physical = PlaybackSchedulerProbeAccess::PhysicalOccurrences();
                const auto before = PlaybackSchedulerProbeAccess::ScheduleState();
                const auto snapshot = PlaybackScheduler::Get().ScheduledUpcomingSnapshot();
                const auto selected = std::find_if(snapshot.begin(), snapshot.end(), [d](const auto& row) {
                    return row.source == ScheduledUpcomingSource::CoreQueue && row.handle == d;
                });
                if (selected == snapshot.end() || selected->playlistIndex >= physical.size()) return false;
                const auto occurrence = physical[selected->playlistIndex];
                if (!PlaybackSchedulerProbeAccess::AdvanceExplicit(occurrence)) return false;
                const auto after = PlaybackSchedulerProbeAccess::ScheduleState();
                return PlaybackSchedulerProbeAccess::PhysicalOccurrences() == physical &&
                       after.projectionOrder == before.projectionOrder && after.current == occurrence &&
                       QueueHandlesEqual({e, f1, f1, f2});
            }));
    if (!OnMain(control, abort, [first, priority] {
            auto& scheduler = PlaybackScheduler::Get();
            return scheduler.SetPlaybackMode(false, 0) && scheduler.PlayQueue(first, 0) &&
                   scheduler.PlayNext(priority);
        })) {
        result.error = "explicit_priority_restore_failed";
        return;
    }

    if (!OnMain(control, abort,
                [b, e] {
                    metadb_handle_list items;
                    if (!SnapshotPlaylist(items)) return false;
                    const auto bAt = HandleIndex(items, b);
                    const auto eAt = HandleIndex(items, e);
                    if (bAt == SIZE_MAX || eAt == SIZE_MAX) return false;
                    std::vector<t_size> order(items.get_count());
                    for (std::size_t i = 0; i < order.size(); ++i)
                        order[i] = i;
                    std::swap(order[bAt], order[eAt]);
                    return playlist_manager::get()->playlist_reorder_items(QueuePlaylist(false), order.data(),
                                                                           order.size());
                }) ||
        !PumpMain(control, abort) || !PumpMain(control, abort)) {
        result.error = "priority_drag_failed";
        return;
    }
    std::string dragDetail;
    const bool dragOk = OnMain(control, abort, [&dragDetail, &symbols, f1, f2] {
        dragDetail = QueueDescription(symbols) + ';' + SnapshotDescription(symbols);
        return QueueHandlesEqual({f1, f1, f2});
    });
    AddCase(result, "own_priority_drag_overrides_sdk_queue", dragOk, dragDetail);

    std::uint64_t crossRevision = 0;
    std::vector<ScheduledUpcomingEntry> crossOrder;
    metadb_handle_ptr crossProjectedHandle;
    if (!OnMain(control, abort, [&crossRevision, &crossOrder] {
            auto& scheduler = PlaybackScheduler::Get();
            crossRevision = scheduler.ScheduleRevision();
            crossOrder = scheduler.ScheduledUpcomingSnapshot();
            const auto physical = std::find_if(crossOrder.begin(), crossOrder.end(), [](const auto& row) {
                return row.source == ScheduledUpcomingSource::Playlist;
            });
            if (physical == crossOrder.end()) return false;
            auto row = *physical;
            crossOrder.erase(physical);
            crossOrder.insert(crossOrder.begin(), std::move(row));
            return scheduler.ReorderUpcomingSnapshot(crossRevision, crossOrder);
        })) {
        result.error = "foreign_cross_boundary_drag_failed";
        return;
    }
    if (!OnMain(control, abort, [&crossProjectedHandle] {
            const auto snapshot = PlaybackScheduler::Get().ScheduledUpcomingSnapshot();
            if (snapshot.empty() || snapshot.front().source != ScheduledUpcomingSource::CoreQueue ||
                snapshot.front().playlistIndex == SIZE_MAX) {
                return false;
            }
            crossProjectedHandle = snapshot.front().handle;
            return crossProjectedHandle.is_valid();
        })) {
        result.error = "foreign_cross_projection_missing";
        return;
    }
    std::string crossDetail;
    const bool crossOk = OnMain(control, abort, [&crossDetail, &symbols, crossProjectedHandle, f1, f2] {
        crossDetail = QueueDescription(symbols) + ';' + SnapshotDescription(symbols);
        return QueueHandlesEqual({crossProjectedHandle, f1, f1, f2});
    });
    AddCase(result, "foreign_cross_boundary_drag_projects_prefix", crossOk, crossDetail);
    AddCase(result, "stale_snapshot_reorder_rejected", OnMain(control, abort, [crossRevision, crossOrder] {
                return !PlaybackScheduler::Get().ReorderUpcomingSnapshot(crossRevision, crossOrder);
            }));

    if (!OnMain(control, abort, [] { return PlaybackScheduler::Get().SetPlaybackMode(true, 1); })) {
        result.error = "shuffle_enable_failed";
        return;
    }
    const auto contentBefore = ([&] {
        std::uint64_t value = 0;
        OnMain(control, abort, [&value] {
            value = PlaybackSchedulerProbeAccess::ContentGeneration();
            return true;
        });
        return value;
    })();
    metadb_handle_list added;
    Append(added, MakeHandle(runId, "shuffle-add"));
    if (!OnMain(control, abort, [added] { return PlaybackScheduler::Get().AddToQueue(added); }) ||
        !PumpMain(control, abort)) {
        result.error = "shuffle_add_failed";
        return;
    }
    if (!OnMain(control, abort,
                [handle = added[0]] {
                    metadb_handle_list items;
                    if (!SnapshotPlaylist(items)) return false;
                    const auto index = HandleIndex(items, handle);
                    if (index == SIZE_MAX) return false;
                    pfc::bit_array_one mask(index);
                    return playlist_manager::get()->playlist_remove_items(QueuePlaylist(false), mask);
                }) ||
        !PumpMain(control, abort)) {
        result.error = "shuffle_delete_failed";
        return;
    }
    AddCase(result, "add_delete_during_shuffle_preserve_content_generation",
            OnMain(control, abort, [contentBefore] {
                return PlaybackSchedulerProbeAccess::ContentGeneration() == contentBefore;
            }));

    metadb_handle_list replacement;
    Append(replacement, x);
    Append(replacement, MakeHandle(runId, "replacement-y"));
    const auto staleGeneration = ([&] {
        std::uint64_t value = 0;
        OnMain(control, abort, [&value] {
            value = PlaybackSchedulerProbeAccess::ContentGeneration();
            return true;
        });
        return value;
    })();
    if (!OnMain(control, abort,
                [replacement] { return PlaybackScheduler::Get().PlayQueue(replacement, 0); })) {
        result.error = "replacement_failed";
        return;
    }
    metadb_handle_list stale;
    Append(stale, MakeHandle(runId, "stale-append"));
    AddCase(result, "replace_cancels_stale_append", OnMain(control, abort, [stale, staleGeneration] {
                return !PlaybackScheduler::Get().AppendSourceBatch(stale, staleGeneration);
            }));

    const std::size_t shuffleOrder = OrderIndex("Shuffle (tracks)");
    const std::size_t defaultOrder = OrderIndex("Default");
    if (shuffleOrder == SIZE_MAX || defaultOrder == SIZE_MAX || !OnMain(control, abort, [shuffleOrder] {
            playlist_manager::get()->playback_order_set_active(shuffleOrder);
            return true;
        })) {
        result.error = "native_shuffle_import_failed";
        return;
    }
    if (!PumpMain(control, abort) || !PumpMain(control, abort) || !PumpMain(control, abort)) {
        result.error = "native_shuffle_echo_drain_failed";
        return;
    }
    std::string modeDetail;
    const bool shuffleImportOk = OnMain(control, abort, [&modeDetail, defaultOrder] {
        auto manager = playlist_manager::get();
        modeDetail = ModeDescription();
        return PlaybackScheduler::Get().QueueShuffleEnabled() &&
               manager->playback_order_get_active() == defaultOrder;
    });
    AddCase(result, "native_shuffle_imports_and_core_returns_default", shuffleImportOk, modeDetail);

    AddCase(result, "own_default_echo_does_not_disable_shuffle", OnMain(control, abort, [defaultOrder] {
                auto manager = playlist_manager::get();
                if (manager->playback_order_get_active() != defaultOrder ||
                    !PlaybackScheduler::Get().SetPlaybackMode(true, 1)) {
                    return false;
                }
                return manager->playback_order_get_active() == defaultOrder &&
                       PlaybackScheduler::Get().QueueShuffleEnabled();
            }));
    if (!OnMain(control, abort,
                [defaultOrder] {
                    playlist_manager::get()->playback_order_set_active(defaultOrder);
                    PlaybackScheduler::Get().CorePlaybackOrderChanged();
                    return true;
                }) ||
        !PumpMain(control, abort)) {
        result.error = "native_default_import_failed";
        return;
    }
    AddCase(result, "native_default_disables_custom_shuffle",
            OnMain(control, abort, [] { return !PlaybackScheduler::Get().QueueShuffleEnabled(); }));

    if (!OnMain(control, abort, [] {
            auto& scheduler = PlaybackScheduler::Get();
            metadb_handle_list items;
            return SnapshotPlaylist(items) && scheduler.PlayQueue(items, 0) &&
                   scheduler.SetPlaybackMode(false, 2);
        })) {
        result.error = "repeat_one_setup_failed";
        return;
    }
    metadb_handle_list repeatPriority;
    Append(repeatPriority, repeatNext);
    if (!OnMain(control, abort,
                [repeatPriority] { return PlaybackScheduler::Get().PlayNext(repeatPriority); })) {
        result.error = "repeat_one_priority_failed";
        return;
    }
    std::string repeatDetail;
    const bool repeatPriorityOk = OnMain(control, abort, [&repeatDetail, &symbols, repeatNext] {
        const auto snapshot = PlaybackScheduler::Get().ScheduledUpcomingSnapshot();
        repeatDetail = SnapshotDescription(symbols) + ';' + ModeDescription();
        return !snapshot.empty() && snapshot.front().handle == repeatNext;
    });
    AddCase(result, "repeat_one_priority_is_actual_next", repeatPriorityOk, repeatDetail);
    if (!OnMain(control, abort,
                [repeatNext] {
                    const auto current = PlaybackSchedulerProbeAccess::CurrentOccurrence();
                    if (!current) return false;
                    const auto physical = PlaybackSchedulerProbeAccess::PhysicalOccurrences();
                    auto it = std::find(physical.begin(), physical.end(), *current);
                    if (it == physical.end()) return false;
                    const auto next =
                        physical[(static_cast<std::size_t>(it - physical.begin()) + 1) % physical.size()];
                    return PlaybackSchedulerProbeAccess::AdvanceOccurrence(next, false);
                }) ||
        !OnMain(control, abort, [] { return PlaybackScheduler::Get().SetPlaybackMode(true, 1); }) ||
        !OnMain(control, abort, [] {
            const auto physical = PlaybackSchedulerProbeAccess::PhysicalOccurrences();
            for (std::size_t i = 0; i < physical.size(); ++i) {
                const auto state = PlaybackSchedulerProbeAccess::ScheduleState();
                std::optional<PlaybackOccurrenceId> next;
                if (!state.priorityBlocks.empty())
                    next = state.priorityBlocks.front().front();
                else if (!state.ordinaryFuture.empty())
                    next = state.ordinaryFuture.front();
                if (!next) break;
                if (!PlaybackSchedulerProbeAccess::AdvanceOccurrence(*next, false)) return false;
            }
            return PlaybackSchedulerProbeAccess::TickModel(30.0);
        })) {
        result.error = "repeat_cycle_peek_failed";
        return;
    }
    AddCase(result, "repeat_cycle_peek_visible", OnMain(control, abort, [] {
                return !PlaybackSchedulerProbeAccess::ScheduleState().pendingCycle.empty() &&
                       !PlaybackScheduler::Get().ScheduledUpcomingSnapshot().empty();
            }));

    metadb_handle_list originalForCheckpoint;
    OriginalState checkpoint;
    if (!OnMain(control, abort, [&originalForCheckpoint, &checkpoint] {
            checkpoint = CaptureOriginal();
            return SnapshotPlaylist(originalForCheckpoint);
        })) {
        result.error = "checkpoint_capture_failed";
        return;
    }
    metadb_handle_list checkpointAdd;
    Append(checkpointAdd, MakeHandle(runId, "checkpoint-add"));
    bool currentCheckpointSchema = false;
    std::string checkpointRestoreDetail;
    if (!OnMain(control, abort,
                [checkpointAdd] { return PlaybackScheduler::Get().AddToQueue(checkpointAdd); }) ||
        !OnMain(control, abort,
                [&currentCheckpointSchema] {
                    currentCheckpointSchema = PlaybackSchedulerProbeAccess::CheckpointUsesCurrentSchema();
                    return true;
                }) ||
        !OnMain(control, abort,
                [checkpoint, &checkpointRestoreDetail] {
                    const auto outcome = RestoreOriginal(checkpoint);
                    if (!outcome.ok) {
                        checkpointRestoreDetail = outcome.detail;
                        return false;
                    }
                    return SameOriginalState(checkpoint, checkpointRestoreDetail);
                }) ||
        !PumpMain(control, abort)) {
        result.error = "checkpoint_restore_failed";
        if (!checkpointRestoreDetail.empty()) result.error += ':' + checkpointRestoreDetail;
        return;
    }
    AddCase(result, "checkpoint_roles_and_host_maps_roundtrip",
            currentCheckpointSchema && OnMain(control, abort, [checkpoint] {
                const auto restored = PlaybackSchedulerProbeAccess::Capture();
                return PlaybackSchedulerProbeAccess::Same(checkpoint.host, restored);
            }));
}

void RunWorker(const Paths& paths, const std::string& runId, RunControl& control, abort_callback& abort,
               const OriginalState& original, ProbeResult& result, std::atomic_bool& restoreRequired) {
    SetThreadDescription(GetCurrentThread(), L"Glasswave playback scheduler probe");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        RunCases(runId, control, abort, result);
    } catch (const exception_aborted&) {
        result.error = "cancelled";
    } catch (...) {
        result.error = "probe_exception";
    }

    std::string restoreDetail;
    const bool restored = OnMain(control, abort, [original, &restoreDetail] {
        const auto outcome = RestoreOriginal(original);
        if (!outcome.ok) {
            restoreDetail = outcome.detail;
            return false;
        }
        return SameOriginalState(original, restoreDetail);
    });
    if (!restored) {
        const std::string restoreError =
            restoreDetail.empty() ? "restore_failed" : "restore_failed:" + restoreDetail;
        result.error = result.error.empty() ? restoreError : result.error + ';' + restoreError;
    } else {
        result.restored = true;
        restoreRequired.store(false, std::memory_order_release);
    }
    if (OnMain(control, abort, [] {
            PlaybackSchedulerProbeAccess::Uninstall();
            return true;
        })) {
    } else if (result.error.empty()) {
        result.error = "probe_uninstall_failed";
    }
    const bool ok = result.error.empty() && result.installed && result.restored && AllCasesPass(result);
    WriteResultFile(paths.result, result, ok);
    winrt::uninit_apartment();
}

class SchedulerProbeLifecycle : public initquit {
  public:
    ~SchedulerProbeLifecycle() { Stop(); }

    void on_init() override {
        const auto paths = ProbePaths();
        if (!paths) return;
        const auto runId = ReadMarker(paths->request);
        if (!runId) return;
        DeleteFileW(paths->result.c_str());
        if (!MoveFileExW(paths->request.c_str(), paths->claimed.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return;
        }
        m_original = CaptureOriginal();
        m_result.runId = *runId;
        m_control = std::make_shared<RunControl>();
        m_abort = std::make_unique<abort_callback_impl>();
        auto control = m_control;
        auto abort = m_abort.get();
        const auto original = m_original;
        m_worker = std::thread([paths = *paths, runId = *runId, control, abort, original, this] {
            RunWorker(paths, runId, *control, *abort, original, m_result, m_restoreRequired);
        });
    }

    void on_quit() override { Stop(); }

  private:
    void Stop() noexcept {
        if (m_control) {
            std::shared_ptr<MainCall> active;
            {
                std::lock_guard lock(m_control->mutex);
                m_control->stopping = true;
                active = m_control->active;
            }
            if (active) active->changed.notify_all();
        }
        if (m_abort) m_abort->abort();
        if (m_worker.joinable()) m_worker.join();
        if (m_original.queuePlaylist != SIZE_MAX && m_restoreRequired.exchange(false)) {
            try {
                const auto outcome = RestoreOriginal(m_original);
                std::string detail;
                if (outcome.ok && SameOriginalState(m_original, detail))
                    PlaybackSchedulerProbeAccess::Uninstall();
            } catch (...) {
            }
        }
    }

    OriginalState m_original;
    ProbeResult m_result;
    std::shared_ptr<RunControl> m_control;
    std::unique_ptr<abort_callback_impl> m_abort;
    std::thread m_worker;
    std::atomic_bool m_restoreRequired = true;
};

FB2K_SERVICE_FACTORY(SchedulerProbeLifecycle);

} // namespace
} // namespace glasswave::media::scheduler_probe
