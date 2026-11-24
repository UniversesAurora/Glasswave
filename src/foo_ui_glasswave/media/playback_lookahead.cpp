#include "pch.h"
#include "media/playback_lookahead.h"
#include "lyrics/lyrics_service.h"
#include "media/lookahead_policy.h"
#include "media/path_behaviour.h"
#include "media/playback_scheduler.h"
#include "media/radio.h"
#include "media/special_playlists.h"
#include "stream/catalog.h"
#include "stream/provider.h"
#include "ui/art_cache.h"

#include <SDK/skipTrack.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::media {

namespace {

// 500 px matches NetEase's 500y500 rendition boundary and Apple Music's 1200 px disk cache
// rendition while serving QueueView, Transport, Taskbar and PlayerWindow without re-decoding.
constexpr int kWarmArtSizePx = 500;

std::unique_ptr<PlaybackLookahead> g_lookahead;

enum class PlaylistOrder {
    Sequential,
    RepeatPlaylist,
    Other,
};

PlaylistOrder OrderFromName(std::string_view name) {
    if (name == "Default") return PlaylistOrder::Sequential;
    if (name == "Repeat (playlist)") return PlaylistOrder::RepeatPlaylist;
    return PlaylistOrder::Other;
}

} // namespace

struct PlaybackLookahead::Impl final : stream::Provider::Listener {
    std::atomic<bool> scheduled{false};
    std::atomic<bool> alive{true};
    bool starting = false;
    std::optional<StartingPreview> startingPreview;

    uint64_t schedulerToken = 0;
    int radioToken = 0;
    std::vector<stream::Provider*> providers;
    std::vector<metadb_handle_ptr> watchedHandles;

    void Attach() {
        PlaybackScheduler::Start();
        if (auto* scheduler = PlaybackScheduler::Existing()) {
            schedulerToken = scheduler->AddListener([this] { RequestRefresh(); });
        }
        radioToken = radio::Subscribe([this] { RequestRefresh(); });
        for (stream::Provider* provider : stream::Providers()) {
            if (!provider) continue;
            provider->AddListener(this);
            providers.push_back(provider);
        }
    }

    void Detach() {
        alive.store(false);
        if (auto* scheduler = PlaybackScheduler::Existing(); scheduler && schedulerToken) {
            scheduler->RemoveListener(schedulerToken);
            schedulerToken = 0;
        }
        if (radioToken) {
            radio::Unsubscribe(radioToken);
            radioToken = 0;
        }
        for (stream::Provider* provider : providers) {
            if (provider) provider->RemoveListener(this);
        }
        providers.clear();
        watchedHandles.clear();
        starting = false;
        startingPreview.reset();
    }

    // stream::Provider::Listener
    void OnAccountObserved(stream::Provider&) override { RequestRefresh(); }
    void OnAccountChanged(stream::Provider&) override { RequestRefresh(); }
    void OnLibraryChanged(stream::Provider&) override { RequestRefresh(); }
    void OnDisplayChanged(stream::Provider&) override { RequestRefresh(); }

    void RequestRefresh() {
        if (!alive.load()) return;
        if (scheduled.exchange(true)) return;
        fb2k::inMainThread([] {
            if (auto* host = PlaybackLookahead::Existing()) host->m_impl->RefreshMain();
        });
    }

    void RefreshMain() {
        scheduled.store(false);
        if (!alive.load()) return;

        auto pc = playback_control::get();
        const bool activePlayback = starting || pc->is_playing();
        if (!activePlayback) {
            watchedHandles.clear();
            startingPreview.reset();
            return;
        }
        if (pc->is_playing() && !starting) startingPreview.reset();

        metadb_handle_ptr current;
        if (starting && startingPreview && startingPreview->track.is_valid()) {
            current = startingPreview->track;
        } else {
            pc->get_now_playing(current);
        }
        const std::wstring currentIdentity =
            current.is_valid() ? lyrics::Service::TrackIdentity(current) : std::wstring();

        std::vector<metadb_handle_ptr> handles = CollectUpcomingHandles(current);

        std::vector<LookaheadCandidate> candidates;
        candidates.reserve(handles.size());
        for (const auto& handle : handles) {
            LookaheadCandidate candidate;
            if (!handle.is_valid()) {
                candidates.push_back(std::move(candidate));
                continue;
            }
            candidate.identity = lyrics::Service::TrackIdentity(handle);
            candidate.itemIdentified = ItemIdentified(handle);
            if (auto* provider = stream::ProviderOf(handle)) {
                candidate.isStreaming = true;
                const stream::Id id = provider->TrackId(handle);
                const stream::Catalog* records = provider->Records();
                const auto known = records && !id.empty() ? records->FindTrack(id) : std::nullopt;
                candidate.metadataComplete = known && !stream::LacksLinks(*known);
                candidate.albumArtKeyStable =
                    known && (!known->albumId.empty() || !stream::LacksLinks(*known));
                candidate.regionalSettled = !id.empty() && !provider->TrackDisplayPending(id);
            } else {
                candidate.isStreaming = false;
                candidate.metadataComplete = true;
                candidate.albumArtKeyStable = true;
                candidate.regionalSettled = true;
            }
            candidates.push_back(std::move(candidate));
        }

        const LookaheadPlan plan = PlanLookahead(candidates, activePlayback, currentIdentity);

        watchedHandles.clear();
        watchedHandles.reserve(plan.selected.size());
        for (size_t idx : plan.selected) {
            watchedHandles.push_back(handles[idx]);
        }

        // Tier 1: Metadata links + regional display names (coalesced via stream::Catalog::Want).
        for (size_t idx : plan.wantMetadata) {
            const metadb_handle_ptr& handle = handles[idx];
            if (auto* provider = stream::ProviderOf(handle)) {
                const stream::Id id = provider->TrackId(handle);
                if (!id.empty()) (void)provider->FindTrack(id);
            }
        }

        // Tier 2: Low-priority cover art warming (only once album art key is stable).
        if (!plan.warmArt.empty()) {
            auto& artCache = ui::ArtCache::Get();
            for (size_t idx : plan.warmArt) {
                artCache.Warm(handles[idx], kWarmArtSizePx);
            }
        }

        // Tier 3: Single-slot cancellable lyrics prefetch for immediate N+1 once tags settle.
        if (plan.prefetchLyrics) {
            lyrics::Service::Get().Prefetch(handles[*plan.prefetchLyrics]);
        } else {
            lyrics::Service::Get().CancelPrefetch();
        }
    }

    std::vector<metadb_handle_ptr> CollectUpcomingHandles(const metadb_handle_ptr& current) const {
        std::vector<metadb_handle_ptr> out;
        auto pm = playlist_manager::get();
        if (playback_control::get()->get_stop_after_current()) return out;

        if (starting && startingPreview) {
            out.reserve(startingPreview->upcoming.size() + 5);
            for (const auto& entry : startingPreview->upcoming) {
                if (entry.handle.is_valid()) out.push_back(entry.handle);
                if (out.size() >= kLookaheadMetadataHorizon + 2) break;
            }
            if (radio::Active()) {
                for (auto& item : radio::PendingItems()) {
                    if (item.is_valid()) out.push_back(std::move(item));
                }
            }
            return out;
        }

        t_size playingPlaylist = SIZE_MAX;
        t_size playingIndex = SIZE_MAX;
        const bool hasPlayingLocation = pm->get_playing_item_location(&playingPlaylist, &playingIndex);

        if (auto* scheduler = PlaybackScheduler::Existing();
            scheduler && ((hasPlayingLocation && IsQueue(playingPlaylist)) || radio::Active())) {
            const auto upcoming = scheduler->ScheduledUpcomingSnapshot();
            out.reserve(upcoming.size() + 5);
            for (const auto& entry : upcoming) {
                if (entry.handle.is_valid()) out.push_back(entry.handle);
                if (out.size() >= kLookaheadMetadataHorizon + 2) break;
            }
            if (radio::Active()) {
                for (auto& item : radio::PendingItems()) {
                    if (item.is_valid()) out.push_back(std::move(item));
                }
            }
            return out;
        }

        // Non-queue playlist: honor explicit core queue first, then deterministic playlist order.
        const auto filters = fb2k::skipTrack::enumerate();
        const auto accepted = [&filters](const metadb_handle_ptr& handle, t_size playlist, t_size index) {
            fb2k::skipTrackParam param{handle, playlist, index};
            for (const auto& filter : filters) {
                try {
                    if (!filter->testTrack(param)) return false;
                } catch (...) {
                    return false;
                }
            }
            return true;
        };

        pfc::list_t<t_playback_queue_item> coreQueue;
        pm->queue_get_contents(coreQueue);
        for (const auto& item : coreQueue) {
            if (!item.m_handle.is_valid()) continue;
            if (!accepted(item.m_handle, item.m_playlist, item.m_item)) continue;
            out.push_back(item.m_handle);
            if (out.size() >= kLookaheadMetadataHorizon + 2) return out;
        }

        if (!hasPlayingLocation || !current.is_valid()) return out;
        const t_size count = pm->playlist_get_item_count(playingPlaylist);
        if (count == 0 || playingIndex >= count) return out;

        const char* orderName = pm->playback_order_get_name(pm->playback_order_get_active());
        const PlaylistOrder order = OrderFromName(orderName ? orderName : "");

        auto pushIndex = [&](t_size idx) {
            metadb_handle_ptr handle;
            if (!pm->playlist_get_item_handle(handle, playingPlaylist, idx) || !handle.is_valid()) return;
            if (!accepted(handle, playingPlaylist, idx)) return;
            out.push_back(std::move(handle));
        };

        if (order == PlaylistOrder::Sequential) {
            for (t_size idx = playingIndex + 1; idx < count && out.size() < kLookaheadMetadataHorizon + 2;
                 ++idx) {
                pushIndex(idx);
            }
        } else if (order == PlaylistOrder::RepeatPlaylist) {
            t_size idx = playingIndex;
            for (t_size scanned = 0;
                 scanned + 1 < count && out.size() < kLookaheadMetadataHorizon + 2; ++scanned) {
                idx = (idx + 1) % count;
                pushIndex(idx);
            }
        }
        return out;
    }
};

PlaybackLookahead::PlaybackLookahead() : m_impl(std::make_unique<Impl>()) {}

PlaybackLookahead::~PlaybackLookahead() {
    if (m_impl) m_impl->Detach();
}

void PlaybackLookahead::Start() {
    if (!core_api::is_main_thread()) {
        fb2k::inMainThread([] { PlaybackLookahead::Start(); });
        return;
    }
    if (g_lookahead) return;
    g_lookahead.reset(new PlaybackLookahead());
    g_lookahead->m_impl->Attach();
    g_lookahead->Schedule();
}

void PlaybackLookahead::Shutdown() {
    if (!g_lookahead) return;
    g_lookahead.reset();
}

PlaybackLookahead* PlaybackLookahead::Existing() {
    return g_lookahead.get();
}

void PlaybackLookahead::Schedule() {
    if (m_impl) m_impl->RequestRefresh();
}

void PlaybackLookahead::OnPlaybackStarting(play_control::t_track_command command) {
    if (!m_impl) return;
    m_impl->starting = true;
    m_impl->startingPreview.reset();
    if (auto* scheduler = PlaybackScheduler::Existing()) {
        m_impl->startingPreview = scheduler->PreviewStarting(command);
    }
    m_impl->RequestRefresh();
}

void PlaybackLookahead::OnPlaybackNewTrack() {
    if (!m_impl) return;
    m_impl->starting = false;
    m_impl->startingPreview.reset();
    m_impl->RequestRefresh();
}

void PlaybackLookahead::OnPlaybackStopped() {
    if (!m_impl) return;
    m_impl->starting = false;
    m_impl->startingPreview.reset();
    m_impl->watchedHandles.clear();
    lyrics::Service::Get().CancelPrefetch();
}

void PlaybackLookahead::OnMetadbChanged(metadb_handle_list_cref items) {
    if (!m_impl || m_impl->watchedHandles.empty()) return;
    const t_size count = items.get_count();
    if (count == 0) return;
    for (const auto& watched : m_impl->watchedHandles) {
        const metadb_handle* target = watched.get_ptr();
        if (!target) continue;
        t_size lo = 0;
        t_size hi = count;
        while (lo < hi) {
            const t_size mid = lo + (hi - lo) / 2;
            if (items[mid].get_ptr() < target)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < count && items[lo].get_ptr() == target) {
            m_impl->RequestRefresh();
            return;
        }
    }
}

namespace {

class LookaheadLifecycle : public initquit {
  public:
    void on_init() override { PlaybackLookahead::Start(); }
    void on_quit() override { PlaybackLookahead::Shutdown(); }
};

class LookaheadPlaybackEvents : public play_callback_static {
  public:
    unsigned get_flags() override {
        return flag_on_playback_starting | flag_on_playback_new_track | flag_on_playback_stop;
    }
    void on_playback_starting(play_control::t_track_command command, bool) override {
        fb2k::inMainThread([command] {
            if (auto* host = PlaybackLookahead::Existing()) host->OnPlaybackStarting(command);
        });
    }
    void on_playback_new_track(metadb_handle_ptr) override {
        fb2k::inMainThread([] {
            if (auto* host = PlaybackLookahead::Existing()) host->OnPlaybackNewTrack();
        });
    }
    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (reason == play_control::stop_reason_starting_another) return;
        fb2k::inMainThread([] {
            if (auto* host = PlaybackLookahead::Existing()) host->OnPlaybackStopped();
        });
    }
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info&) override {}
    void on_playback_dynamic_info_track(const file_info&) override {}
    void on_playback_time(double) override {}
    void on_volume_change(float) override {}
};

class LookaheadPlaylistEvents : public playlist_callback_static {
  public:
    unsigned get_flags() override {
        return flag_on_items_added | flag_on_items_removed | flag_on_items_reordered |
               flag_on_playback_order_changed;
    }
    void on_items_added(t_size, t_size, metadb_handle_list_cref, const bit_array&) override { Trigger(); }
    void on_items_reordered(t_size, const t_size*, t_size) override { Trigger(); }
    void on_items_removed(t_size, const bit_array&, t_size, t_size) override { Trigger(); }
    void on_playback_order_changed(t_size) override { Trigger(); }
    void on_items_removing(t_size, const bit_array&, t_size, t_size) override {}
    void on_items_selection_change(t_size, const bit_array&, const bit_array&) override {}
    void on_item_focus_change(t_size, t_size, t_size) override {}
    void on_items_modified(t_size, const bit_array&) override {}
    void on_items_modified_fromplayback(t_size, const bit_array&, play_control::t_display_level) override {}
    void on_items_replaced(t_size, const bit_array&,
                           const pfc::list_base_const_t<t_on_items_replaced_entry>&) override {}
    void on_item_ensure_visible(t_size, t_size) override {}
    void on_playlist_activate(t_size, t_size) override {}
    void on_playlist_created(t_size, const char*, t_size) override {}
    void on_playlists_reorder(const t_size*, t_size) override {}
    void on_playlists_removing(const bit_array&, t_size, t_size) override {}
    void on_playlists_removed(const bit_array&, t_size, t_size) override {}
    void on_playlist_renamed(t_size, const char*, t_size) override {}
    void on_default_format_changed() override {}
    void on_playlist_locked(t_size, bool) override {}

  private:
    static void Trigger() {
        if (auto* host = PlaybackLookahead::Existing()) host->Schedule();
    }
};

class LookaheadQueueEvents : public playback_queue_callback {
  public:
    void on_changed(t_change_origin) override {
        if (auto* host = PlaybackLookahead::Existing()) host->Schedule();
    }
};

class LookaheadMetadbEvents : public metadb_io_callback {
  public:
    void on_changed_sorted(metadb_handle_list_cref items, bool fromHook) override {
        if (fromHook) return;
        if (auto* host = PlaybackLookahead::Existing()) host->OnMetadbChanged(items);
    }
};

initquit_factory_t<LookaheadLifecycle> g_lookaheadLifecycle;
play_callback_static_factory_t<LookaheadPlaybackEvents> g_lookaheadPlaybackEvents;
service_factory_single_t<LookaheadPlaylistEvents> g_lookaheadPlaylistEvents;
service_factory_single_t<LookaheadQueueEvents> g_lookaheadQueueEvents;
service_factory_single_t<LookaheadMetadbEvents> g_lookaheadMetadbEvents;

} // namespace

} // namespace glasswave::media
