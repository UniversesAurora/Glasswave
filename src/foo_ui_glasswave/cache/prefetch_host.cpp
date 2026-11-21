#include "pch.h"
#include "core/text.h"

#include "cache/prefetch.h"
#include "cache/prefetch_host.h"

#include "applemusic/config.h"
#include "cache/audio.h"
#include "netease/config.h"
#include "media/playback_scheduler.h"
#include "media/special_playlists.h"
#include "stream/provider.h"

#include <SDK/skipTrack.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace glasswave::cache {
namespace {

using core::Utf8;

std::string TrackToken(const metadb_handle_ptr& track) {
    if (!track.is_valid()) return {};
    std::string token = track->get_path();
    token.push_back('|');
    token += std::to_string(track->get_subsong_index());
    return token;
}

OrderMode OrderModeFromName(std::string_view name) {
    const auto equals = [name](std::string_view expected) {
        return pfc::stricmp_ascii_ex(name.data(), name.size(), expected.data(), expected.size()) == 0;
    };
    if (equals("Default")) return OrderMode::Sequential;
    if (equals("Repeat (playlist)")) return OrderMode::RepeatAll;
    if (equals("Repeat (track)")) return OrderMode::RepeatOne;
    if (name.find("Shuffle") != std::string_view::npos) return OrderMode::Shuffle;
    if (name.find("Random") != std::string_view::npos) return OrderMode::Random;
    return OrderMode::Other;
}

void AppendPart(std::string& output, std::string_view value) {
    output += std::to_string(value.size());
    output.push_back(':');
    output.append(value);
    output.push_back(';');
}

std::string ContextFingerprint(std::string_view orderName) {
    std::string value;
    AppendPart(value, orderName);
    if (auto* scheduler = media::PlaybackScheduler::Existing())
        AppendPart(value, std::to_string(scheduler->ScheduleRevision()));
    AppendPart(value, std::to_string(static_cast<int>(netease::GetQuality())));
    AppendPart(value, applemusic::GetAtmosEnabled() ? "atmos" : "lossless");
    AppendPart(value, std::to_string(static_cast<int>(applemusic::GetAudioQuality())));
    AppendPart(value, std::to_string(static_cast<int>(applemusic::GetAtmosOutput())));
    for (stream::Provider* provider : stream::Providers()) {
        const std::string key = Utf8(provider->Key());
        AppendPart(value, key);
        AppendPart(value, std::to_string(AccountRevision(key)));
    }
    return value;
}

struct PrimeJob {
    uint64_t requestId = 0;
    uint64_t contextGeneration = 0;
    uint64_t trackGeneration = 0;
    uint64_t candidateGeneration = 0;
    std::string itemPath;
    CandidateProof proof = CandidateProof::None;
    abort_callback_impl abort;
    std::atomic_bool initialized = false;
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool releaseRequested = false;
    bool cancelRequested = false;
    bool probeOnly = false;

    void Release() noexcept {
        {
            std::lock_guard lock(mutex);
            if (releaseRequested) return;
            releaseRequested = true;
        }
        abort.abort();
        changed.notify_all();
    }
};

void PrimeTarget(const std::shared_ptr<PrimeJob>& job) {
    bool failed = false;
    try {
        audio::PrefetchScope prefetchScope;
        service_ptr_t<input_decoder> decoder;
        input_entry::g_open_for_decoding(decoder, nullptr, job->itemPath.c_str(), job->abort, true);
        decoder->initialize(0, input_flag_simpledecode, job->abort);
        job->initialized.store(true, std::memory_order_release);

        std::unique_lock lock(job->mutex);
        job->changed.wait(lock, [&] { return job->releaseRequested || job->abort.is_aborting(); });
    } catch (...) {
        failed = !job->abort.is_aborting();
    }

    const uint64_t requestId = job->requestId;
    fb2k::inMainThread([requestId, failed] { PrefetchHost::ReportPrimeExit(requestId, failed); });
}

class ProviderEvents final : public stream::Provider::Listener {
  public:
    ProviderEvents(std::function<void()> accountChanged, std::function<void()> languageChanged)
        : m_accountChanged(std::move(accountChanged)), m_languageChanged(std::move(languageChanged)) {}

    void OnAccountChanged(stream::Provider&) override { m_accountChanged(); }
    void OnLanguageChanged(stream::Provider&) override { m_languageChanged(); }

  private:
    std::function<void()> m_accountChanged;
    std::function<void()> m_languageChanged;
};

std::unique_ptr<PrefetchHost> g_host;

} // namespace

struct PrefetchHost::Impl {
    struct BuiltSnapshot {
        PlaybackSnapshot snapshot;
        std::string currentToken;
        std::string candidateToken;
        std::string contextFingerprint;
        bool capacityFull = false;
    };

    struct ProbeHook {
        PrefetchHostProbeSnapshot capture;
        std::vector<PrefetchHostProbeEvent> events;
        std::string savedTrack;
        std::string savedCandidate;
        std::string savedContext;
        bool savedCapacityFull = false;

        void RecordStarted(const PrimeJob& job) {
            events.push_back({
                PrefetchHostProbeEventKind::Started,
                job.requestId,
                job.contextGeneration,
                job.trackGeneration,
                job.candidateGeneration,
                job.itemPath,
                job.proof,
            });
        }

        void RecordCancelled(const PrimeJob& job) {
            events.push_back({
                PrefetchHostProbeEventKind::Cancelled,
                job.requestId,
                job.contextGeneration,
                job.trackGeneration,
                job.candidateGeneration,
                job.itemPath,
                job.proof,
            });
        }
    };

    Impl();
    ~Impl();

    void StartMain();
    void StopMain();
    void RefreshMain();
    void ScheduleRefreshMain();
    void RunScheduledRefresh();
    void OnTrackStartedMain(std::string_view itemPath);
    void OnPlaybackStoppedMain();
    void NotifyContextChangedMain();
    void PrimeExited(uint64_t requestId, bool failed);
    void ApplyDecision(PrefetchDecision decision);
    bool StartPrime(const PrefetchRequest& request);
    bool CancelActive(uint64_t requestId) noexcept;
    void ReleaseAllJobs() noexcept;
    void ReleaseHandedOff() noexcept;
    bool UninstallProbeMain();

    BuiltSnapshot BuildSnapshotMain() const;
    std::string ActiveOrderName() const;

    mutable std::mutex mutex;
    PrefetchCoordinator coordinator;
    std::unique_ptr<ProviderEvents> providerEvents;
    std::shared_ptr<PrimeJob> activeJob;
    std::vector<std::shared_ptr<PrimeJob>> handedOffJobs;
    std::string lastTrack;
    std::string lastCandidate;
    std::string lastContext;
    uint64_t contextGeneration = 0;
    uint64_t trackGeneration = 0;
    uint64_t candidateGeneration = 0;
    bool enabled = true;
    bool networkAllowed = true;
    bool started = false;
    bool stopping = false;
    bool capacityWasFull = false;
    uint64_t schedulerListener = 0;
    std::atomic_bool refreshQueued = false;
    std::unique_ptr<ProbeHook> probe;
};

PrefetchHost::Impl::Impl() = default;

PrefetchHost::Impl::~Impl() {
    StopMain();
}

void PrefetchHost::Impl::StartMain() {
    PFC_ASSERT(core_api::is_main_thread());
    {
        std::lock_guard lock(mutex);
        if (started || stopping) return;
        started = true;
    }
    providerEvents = std::make_unique<ProviderEvents>([this] { NotifyContextChangedMain(); },
                                                      [this] { ScheduleRefreshMain(); });
    media::PlaybackScheduler::Start();
    if (auto* scheduler = media::PlaybackScheduler::Existing())
        schedulerListener = scheduler->AddListener([] {
            if (auto* host = PrefetchHost::Existing()) host->ScheduleRefresh();
        });
    for (stream::Provider* provider : stream::Providers())
        provider->AddListener(providerEvents.get());
    RefreshMain();
}

void PrefetchHost::Impl::StopMain() {
    {
        std::lock_guard lock(mutex);
        if (stopping) return;
        stopping = true;
        started = false;
    }
    if (providerEvents)
        for (stream::Provider* provider : stream::Providers())
            provider->RemoveListener(providerEvents.get());
    if (auto* scheduler = media::PlaybackScheduler::Existing())
        scheduler->RemoveListener(std::exchange(schedulerListener, 0));
    providerEvents.reset();
    ReleaseAllJobs();
    audio::Shutdown();
}

std::string PrefetchHost::Impl::ActiveOrderName() const {
    auto manager = playlist_manager::get();
    const t_size count = manager->playback_order_get_count();
    const t_size active = manager->playback_order_get_active();
    return active < count ? manager->playback_order_get_name(active) : std::string();
}

PrefetchHost::Impl::BuiltSnapshot PrefetchHost::Impl::BuildSnapshotMain() const {
    PFC_ASSERT(core_api::is_main_thread());

    BuiltSnapshot built;
    {
        std::lock_guard lock(mutex);
        if (probe) {
            built.snapshot = probe->capture.playback;
            built.currentToken = probe->capture.currentToken;
            built.candidateToken = probe->capture.candidateToken;
            built.contextFingerprint = probe->capture.contextFingerprint;
            built.capacityFull = false;
            return built;
        }
    }

    PlaybackSnapshot& snapshot = built.snapshot;
    const std::string orderName = ActiveOrderName();

    auto control = playback_control::get();
    metadb_handle_ptr now;
    const bool hasNow = control->get_now_playing(now);
    snapshot.playing = control->is_playing();
    snapshot.paused = control->is_paused();
    snapshot.stopAfterCurrent = control->get_stop_after_current();

    {
        std::lock_guard lock(mutex);
        snapshot.networkAllowed = networkAllowed;
        snapshot.speculativePrefetchAllowed = enabled;
    }

    const double length = control->playback_get_length_ex();
    const double position = control->playback_get_position();
    if (std::isfinite(length) && length > 0 && std::isfinite(position)) {
        snapshot.remainingSeconds = std::max(0.0, length - position);
    }

    built.currentToken = TrackToken(now);
    auto manager = playlist_manager::get();
    const auto filters = fb2k::skipTrack::enumerate();
    const auto acceptedByCore = [&filters](const metadb_handle_ptr& handle, t_size playlist, t_size index) {
        fb2k::skipTrackParam parameter{handle, playlist, index};
        for (const auto& filter : filters) {
            try {
                if (!filter->testTrack(parameter)) return false;
            } catch (...) {
                return false;
            }
        }
        return true;
    };
    t_size playingPlaylist = SIZE_MAX;
    t_size playingIndex = SIZE_MAX;
    if (!snapshot.stopAfterCurrent) {
        bool queueDecided = false;
        pfc::list_t<t_playback_queue_item> queue;
        manager->queue_get_contents(queue);
        for (const auto& item : queue) {
            if (!item.m_handle.is_valid()) continue;
            if (!acceptedByCore(item.m_handle, item.m_playlist, item.m_item)) continue;
            queueDecided = true;
            if (stream::ProviderOf(item.m_handle)) {
                snapshot.next.itemPath = item.m_handle->get_path();
                snapshot.next.proof = CandidateProof::CoreQueue;
            }
            break;
        }

        if (!queueDecided && snapshot.next.proof == CandidateProof::None && hasNow &&
            manager->get_playing_item_location(&playingPlaylist, &playingIndex)) {
            const OrderMode mode = OrderModeFromName(orderName);
            const t_size count = manager->playlist_get_item_count(playingPlaylist);
            auto consider = [&](t_size index, CandidateProof proof) {
                metadb_handle_ptr candidate;
                if (!manager->playlist_get_item_handle(candidate, playingPlaylist, index)) return false;
                if (!acceptedByCore(candidate, playingPlaylist, index)) return true;
                if (!stream::ProviderOf(candidate)) return false;
                snapshot.next.itemPath = candidate->get_path();
                snapshot.next.proof = proof;
                return true;
            };

            // Skip filtered items only within the order's proven range. Repeat-one has no
            // fallback: a filtered current item makes the next selection unknown.
            if (auto* scheduler = media::PlaybackScheduler::Existing();
                scheduler && media::IsQueue(playingPlaylist)) {
                if (const auto next = scheduler->ScheduledNextIndex(); next && *next < count)
                    consider(*next, CandidateProof::LinearPlaylist);
            } else if (mode == OrderMode::RepeatOne) {
                if (playingIndex < count) consider(playingIndex, CandidateProof::RepeatOne);
            } else if (mode == OrderMode::Sequential && playingIndex < count) {
                for (t_size index = playingIndex + 1; index < count; ++index) {
                    if (!consider(index, CandidateProof::LinearPlaylist)) break;
                    if (snapshot.next.proof != CandidateProof::None) break;
                }
            } else if (mode == OrderMode::RepeatAll && count != 0 && playingIndex < count) {
                t_size index = playingIndex;
                for (t_size scanned = 0; scanned < count; ++scanned) {
                    index = (index + 1) % count;
                    if (!consider(index, CandidateProof::LinearPlaylist)) break;
                    if (snapshot.next.proof != CandidateProof::None) break;
                }
            }
        }
    }

    built.candidateToken =
        snapshot.next.itemPath + "|" + std::to_string(static_cast<int>(snapshot.next.proof));
    built.contextFingerprint = ContextFingerprint(orderName);
    built.capacityFull = audio::ActivePrefetchFlights() >= audio::kMaxPrefetchFlights;
    return built;
}

void PrefetchHost::Impl::RefreshMain() {
    if (!core_api::is_main_thread()) {
        fb2k::inMainThread([] {
            if (auto* host = PrefetchHost::Existing()) host->Refresh();
        });
        return;
    }

    BuiltSnapshot built = BuildSnapshotMain();
    {
        std::lock_guard lock(mutex);
        if (!started || stopping) return;
        if (built.contextFingerprint != lastContext) {
            lastContext = std::move(built.contextFingerprint);
            ++contextGeneration;
            ++candidateGeneration;
        }
        if (built.currentToken != lastTrack) {
            lastTrack = std::move(built.currentToken);
            ++trackGeneration;
            ++candidateGeneration;
        }
        if (built.candidateToken != lastCandidate) {
            lastCandidate = std::move(built.candidateToken);
            ++candidateGeneration;
        }
        if (capacityWasFull && !built.capacityFull) coordinator.AllowRetry();
        capacityWasFull = built.capacityFull;
        built.snapshot.contextGeneration = contextGeneration;
        built.snapshot.trackGeneration = trackGeneration;
        built.snapshot.candidateGeneration = candidateGeneration;
    }

    ApplyDecision(coordinator.Evaluate(std::move(built.snapshot)));
}

void PrefetchHost::Impl::ScheduleRefreshMain() {
    {
        std::lock_guard lock(mutex);
        if (!started || stopping) return;
    }
    if (refreshQueued.exchange(true)) return;
    try {
        fb2k::inMainThread([] {
            if (auto* host = PrefetchHost::Existing()) host->m_impl->RunScheduledRefresh();
        });
    } catch (...) {
        refreshQueued.store(false);
    }
}

void PrefetchHost::Impl::RunScheduledRefresh() {
    refreshQueued.store(false);
    RefreshMain();
}

bool PrefetchHost::Impl::StartPrime(const PrefetchRequest& request) {
    auto job = std::make_shared<PrimeJob>();
    job->requestId = request.id;
    job->contextGeneration = request.contextGeneration;
    job->trackGeneration = request.trackGeneration;
    job->candidateGeneration = request.candidateGeneration;
    job->itemPath = request.itemPath;
    job->proof = request.proof;
    {
        std::lock_guard lock(mutex);
        if (!started || stopping || activeJob) return false;
        activeJob = job;
        if (probe) {
            job->probeOnly = true;
            probe->RecordStarted(*job);
            return true;
        }
    }
    try {
        fb2k::splitTask([job] { PrimeTarget(job); });
        return true;
    } catch (...) {
        std::lock_guard lock(mutex);
        if (activeJob == job) activeJob.reset();
        return false;
    }
}

bool PrefetchHost::Impl::CancelActive(uint64_t requestId) noexcept {
    std::shared_ptr<PrimeJob> job;
    {
        std::lock_guard lock(mutex);
        if (!activeJob || activeJob->requestId != requestId) return false;
        job = activeJob;
        job->cancelRequested = true;
        if (probe && job->probeOnly) probe->RecordCancelled(*job);
    }
    job->Release();
    return true;
}

void PrefetchHost::Impl::ApplyDecision(PrefetchDecision decision) {
    for (unsigned attempt = 0; attempt < 4 && decision; ++attempt) {
        if (decision.cancelRequestId) {
            const uint64_t requestId = *decision.cancelRequestId;
            if (CancelActive(requestId)) return; // PrimeExited completes the coordinator step.
            decision = coordinator.Finish(requestId, PrefetchOutcome::Cancelled);
            continue;
        }
        if (!decision.start) continue;

        const PrefetchRequest request = *decision.start;
        bool capacityFull = false;
        {
            std::lock_guard lock(mutex);
            capacityFull = !probe && audio::ActivePrefetchFlights() >= audio::kMaxPrefetchFlights;
            capacityWasFull = capacityFull;
        }
        if (capacityFull || !StartPrime(request)) {
            decision = coordinator.Finish(request.id, PrefetchOutcome::Failed);
            continue;
        }
        return;
    }
}

void PrefetchHost::Impl::PrimeExited(uint64_t requestId, bool failed) {
    bool cancelled = false;
    {
        std::lock_guard lock(mutex);
        if (!activeJob || activeJob->requestId != requestId) return;
        cancelled = activeJob->cancelRequested || !failed;
        activeJob.reset();
    }
    ApplyDecision(
        coordinator.Finish(requestId, cancelled ? PrefetchOutcome::Cancelled : PrefetchOutcome::Failed));
}

void PrefetchHost::Impl::OnTrackStartedMain(std::string_view itemPath) {
    ReleaseHandedOff();

    std::shared_ptr<PrimeJob> handedOff;
    uint64_t cancelSlowPrime = 0;
    {
        std::lock_guard lock(mutex);
        if (activeJob && activeJob->itemPath == itemPath) {
            if (!activeJob->cancelRequested && activeJob->initialized.load(std::memory_order_acquire)) {
                handedOff = std::move(activeJob);
            } else if (!activeJob->cancelRequested) {
                activeJob->cancelRequested = true;
                cancelSlowPrime = activeJob->requestId;
            }
        }
        if (handedOff) handedOffJobs.push_back(handedOff);
    }

    if (cancelSlowPrime) CancelActive(cancelSlowPrime);
    if (handedOff) {
        ApplyDecision(coordinator.Finish(handedOff->requestId, PrefetchOutcome::Complete));
    }
    RefreshMain();
}

void PrefetchHost::Impl::OnPlaybackStoppedMain() {
    ReleaseHandedOff();
    RefreshMain();
}

void PrefetchHost::Impl::NotifyContextChangedMain() {
    ReleaseHandedOff();
    {
        std::lock_guard lock(mutex);
        ++contextGeneration;
        ++candidateGeneration;
    }
    coordinator.AllowRetry();
    ScheduleRefreshMain();
}

void PrefetchHost::Impl::ReleaseHandedOff() noexcept {
    std::vector<std::shared_ptr<PrimeJob>> release;
    {
        std::lock_guard lock(mutex);
        release.swap(handedOffJobs);
    }
    for (const auto& job : release)
        job->Release();
}

void PrefetchHost::Impl::ReleaseAllJobs() noexcept {
    std::shared_ptr<PrimeJob> active;
    std::vector<std::shared_ptr<PrimeJob>> handedOff;
    {
        std::lock_guard lock(mutex);
        active = std::move(activeJob);
        handedOff.swap(handedOffJobs);
    }
    if (active) active->Release();
    for (const auto& job : handedOff)
        job->Release();
}

bool PrefetchHost::Impl::UninstallProbeMain() {
    PFC_ASSERT(core_api::is_main_thread());
    {
        std::lock_guard lock(mutex);
        if (!probe) return false;
        PlaybackSnapshot stopped;
        stopped.speculativePrefetchAllowed = false;
        stopped.remainingSeconds = -1;
        probe->capture.playback = std::move(stopped);
        probe->capture.currentToken = "prefetch-probe-stop";
        probe->capture.candidateToken.clear();
        probe->capture.contextFingerprint = "prefetch-probe-stop";
    }

    RefreshMain();

    std::shared_ptr<PrimeJob> active;
    uint64_t cancelledRequest = 0;
    {
        std::lock_guard lock(mutex);
        active = std::move(activeJob);
        if (active) cancelledRequest = active->requestId;
    }
    if (active) active->Release();
    if (cancelledRequest) coordinator.Finish(cancelledRequest, PrefetchOutcome::Cancelled);

    {
        std::lock_guard lock(mutex);
        if (!probe) return false;
        lastTrack = std::move(probe->savedTrack);
        lastCandidate = std::move(probe->savedCandidate);
        lastContext = std::move(probe->savedContext);
        capacityWasFull = probe->savedCapacityFull;
        probe.reset();
    }
    coordinator.AllowRetry();
    return true;
}

PrefetchHost::PrefetchHost(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
PrefetchHost::~PrefetchHost() = default;

void PrefetchHost::Start() {
    if (!core_api::is_main_thread()) {
        fb2k::inMainThread([] { PrefetchHost::Start(); });
        return;
    }
    if (g_host) return;
    g_host = std::unique_ptr<PrefetchHost>(new PrefetchHost(std::make_unique<Impl>()));
    g_host->m_impl->StartMain();
}

void PrefetchHost::Shutdown() {
    if (!core_api::is_main_thread()) {
        fb2k::inMainThread([] { PrefetchHost::Shutdown(); });
        return;
    }
    g_host.reset();
}

PrefetchHost& PrefetchHost::Get() {
    PFC_ASSERT(g_host);
    return *g_host;
}

PrefetchHost* PrefetchHost::Existing() {
    return g_host.get();
}

void PrefetchHost::Refresh() {
    m_impl->RefreshMain();
}

void PrefetchHost::ScheduleRefresh() {
    m_impl->ScheduleRefreshMain();
}

void PrefetchHost::OnTrackStarted(std::string_view itemPath) {
    if (!core_api::is_main_thread()) {
        const std::string path(itemPath);
        fb2k::inMainThread([path] {
            if (auto* host = PrefetchHost::Existing()) host->OnTrackStarted(path);
        });
        return;
    }
    m_impl->OnTrackStartedMain(itemPath);
}

void PrefetchHost::OnPlaybackStopped() {
    PFC_ASSERT(core_api::is_main_thread());
    m_impl->OnPlaybackStoppedMain();
}

void PrefetchHost::SetEnabled(bool value) {
    PFC_ASSERT(core_api::is_main_thread());
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->enabled == value) return;
        m_impl->enabled = value;
    }
    if (value) m_impl->coordinator.AllowRetry();
    Refresh();
}

void PrefetchHost::SetNetworkAllowed(bool value) {
    PFC_ASSERT(core_api::is_main_thread());
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->networkAllowed == value) return;
        m_impl->networkAllowed = value;
    }
    if (value) m_impl->coordinator.AllowRetry();
    Refresh();
}

void PrefetchHost::NotifyContextChanged() {
    PFC_ASSERT(core_api::is_main_thread());
    m_impl->NotifyContextChangedMain();
}

void PrefetchHost::AllowRetry() {
    PFC_ASSERT(core_api::is_main_thread());
    m_impl->coordinator.AllowRetry();
    Refresh();
}

void PrefetchHost::ReportPrimeExit(uint64_t requestId, bool failed) {
    if (auto* host = PrefetchHost::Existing()) host->m_impl->PrimeExited(requestId, failed);
}

class PrefetchLifecycle : public initquit {
  public:
    void on_init() override { PrefetchHost::Start(); }
    void on_quit() override { PrefetchHost::Shutdown(); }
};

class PrefetchPlaybackEvents : public play_callback_static {
  public:
    unsigned get_flags() override {
        return flag_on_playback_new_track | flag_on_playback_stop | flag_on_playback_seek |
               flag_on_playback_pause | flag_on_playback_edited | flag_on_playback_dynamic_info |
               flag_on_playback_time;
    }

    void on_playback_starting(play_control::t_track_command, bool) override {}
    void on_playback_new_track(metadb_handle_ptr track) override {
        const std::string path = track.is_valid() ? track->get_path() : "";
        fb2k::inMainThread([path] {
            if (auto* host = PrefetchHost::Existing()) host->OnTrackStarted(path);
        });
    }
    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (reason == play_control::stop_reason_starting_another) return;
        fb2k::inMainThread([] {
            if (auto* host = PrefetchHost::Existing()) host->OnPlaybackStopped();
        });
    }
    void on_playback_seek(double) override {
        if (auto* host = PrefetchHost::Existing()) host->ScheduleRefresh();
    }
    void on_playback_pause(bool) override {
        if (auto* host = PrefetchHost::Existing()) host->ScheduleRefresh();
    }
    void on_playback_edited(metadb_handle_ptr) override {
        if (auto* host = PrefetchHost::Existing()) host->ScheduleRefresh();
    }
    void on_playback_dynamic_info(const file_info&) override {
        if (auto* host = PrefetchHost::Existing()) host->ScheduleRefresh();
    }
    void on_playback_dynamic_info_track(const file_info&) override {}
    void on_playback_time(double) override {
        if (auto* host = PrefetchHost::Existing()) host->ScheduleRefresh();
    }
    void on_volume_change(float) override {}
};

class PrefetchPlaylistEvents : public playlist_callback_static {
  public:
    unsigned get_flags() override {
        return flag_on_items_added | flag_on_items_reordered | flag_on_items_removing |
               flag_on_items_removed | flag_on_items_replaced | flag_on_playlist_activate |
               flag_on_playlists_reorder | flag_on_playlists_removing | flag_on_playlists_removed |
               flag_on_playback_order_changed;
    }

    void Refresh() {
        if (auto* host = PrefetchHost::Existing()) host->ScheduleRefresh();
    }
    void on_items_added(t_size, t_size, metadb_handle_list_cref, const bit_array&) override { Refresh(); }
    void on_items_reordered(t_size, const t_size*, t_size) override { Refresh(); }
    void on_items_removing(t_size, const bit_array&, t_size, t_size) override { Refresh(); }
    void on_items_removed(t_size, const bit_array&, t_size, t_size) override { Refresh(); }
    void on_items_selection_change(t_size, const bit_array&, const bit_array&) override {}
    void on_item_focus_change(t_size, t_size, t_size) override {}
    void on_items_modified(t_size, const bit_array&) override {}
    void on_items_modified_fromplayback(t_size, const bit_array&, play_control::t_display_level) override {}
    void on_items_replaced(t_size, const bit_array&,
                           const pfc::list_base_const_t<t_on_items_replaced_entry>&) override {
        Refresh();
    }
    void on_item_ensure_visible(t_size, t_size) override {}
    void on_playlist_activate(t_size, t_size) override { Refresh(); }
    void on_playlist_created(t_size, const char*, t_size) override {}
    void on_playlists_reorder(const t_size*, t_size) override { Refresh(); }
    void on_playlists_removing(const bit_array&, t_size, t_size) override { Refresh(); }
    void on_playlists_removed(const bit_array&, t_size, t_size) override { Refresh(); }
    void on_playlist_renamed(t_size, const char*, t_size) override {}
    void on_default_format_changed() override {}
    void on_playback_order_changed(t_size) override { Refresh(); }
    void on_playlist_locked(t_size, bool) override {}
};

class PrefetchQueueEvents : public playback_queue_callback {
  public:
    void on_changed(t_change_origin) override {
        if (auto* host = PrefetchHost::Existing()) host->ScheduleRefresh();
    }
};

initquit_factory_t<PrefetchLifecycle> g_prefetchLifecycle;
play_callback_static_factory_t<PrefetchPlaybackEvents> g_prefetchPlayback;
service_factory_single_t<PrefetchPlaylistEvents> g_prefetchPlaylist;
service_factory_single_t<PrefetchQueueEvents> g_prefetchQueue;

bool PrefetchHostProbeAccess::Install(PrefetchHostProbeSnapshot snapshot) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!g_host) PrefetchHost::Start();
    if (!g_host) return false;

    auto& impl = *g_host->m_impl;
    std::lock_guard lock(impl.mutex);
    if (impl.probe || impl.activeJob || !impl.handedOffJobs.empty() ||
        impl.coordinator.ActiveRequestId() != 0)
        return false;

    auto hook = std::make_unique<PrefetchHost::Impl::ProbeHook>();
    hook->capture = std::move(snapshot);
    hook->savedTrack = impl.lastTrack;
    hook->savedCandidate = impl.lastCandidate;
    hook->savedContext = impl.lastContext;
    hook->savedCapacityFull = impl.capacityWasFull;
    impl.probe = std::move(hook);
    return true;
}

bool PrefetchHostProbeAccess::SetSnapshot(PrefetchHostProbeSnapshot snapshot) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!g_host) return false;
    std::lock_guard lock(g_host->m_impl->mutex);
    if (!g_host->m_impl->probe) return false;
    g_host->m_impl->probe->capture = std::move(snapshot);
    return true;
}

bool PrefetchHostProbeAccess::DispatchPlaybackTime() {
    PFC_ASSERT(core_api::is_main_thread());
    if (!g_host || !IsInstalled()) return false;

    g_prefetchPlayback.get_static_instance().on_playback_time(0.0);
    auto& impl = *g_host->m_impl;
    if (impl.refreshQueued.exchange(false)) impl.RunScheduledRefresh();
    return true;
}

bool PrefetchHostProbeAccess::NotifyContextChanged() {
    PFC_ASSERT(core_api::is_main_thread());
    if (!g_host || !IsInstalled()) return false;

    g_host->NotifyContextChanged();
    auto& impl = *g_host->m_impl;
    if (impl.refreshQueued.exchange(false)) impl.RunScheduledRefresh();
    return true;
}

bool PrefetchHostProbeAccess::CompletePrime(uint64_t requestId) {
    PFC_ASSERT(core_api::is_main_thread());
    if (!g_host) return false;
    {
        std::lock_guard lock(g_host->m_impl->mutex);
        if (!g_host->m_impl->probe || !g_host->m_impl->activeJob ||
            g_host->m_impl->activeJob->requestId != requestId || !g_host->m_impl->activeJob->probeOnly)
            return false;
    }
    g_host->m_impl->PrimeExited(requestId, false);
    return true;
}

bool PrefetchHostProbeAccess::Uninstall() {
    PFC_ASSERT(core_api::is_main_thread());
    return g_host && g_host->m_impl->UninstallProbeMain();
}

bool PrefetchHostProbeAccess::IsInstalled() {
    if (!g_host) return false;
    std::lock_guard lock(g_host->m_impl->mutex);
    return g_host->m_impl->probe != nullptr;
}

uint64_t PrefetchHostProbeAccess::ActivePrimeRequestId() {
    if (!g_host) return 0;
    std::lock_guard lock(g_host->m_impl->mutex);
    return g_host->m_impl->activeJob ? g_host->m_impl->activeJob->requestId : 0;
}

std::vector<PrefetchHostProbeEvent> PrefetchHostProbeAccess::Events() {
    if (!g_host) return {};
    std::lock_guard lock(g_host->m_impl->mutex);
    return g_host->m_impl->probe ? g_host->m_impl->probe->events : std::vector<PrefetchHostProbeEvent>();
}

} // namespace glasswave::cache
