#pragma once

#include "cache/prefetch.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::cache {

struct PrefetchHostProbeSnapshot {
    PlaybackSnapshot playback;
    std::string currentToken;
    std::string candidateToken;
    std::string contextFingerprint;
};

enum class PrefetchHostProbeEventKind {
    Started,
    Cancelled,
};

struct PrefetchHostProbeEvent {
    PrefetchHostProbeEventKind kind = PrefetchHostProbeEventKind::Started;
    uint64_t requestId = 0;
    uint64_t contextGeneration = 0;
    uint64_t trackGeneration = 0;
    uint64_t candidateGeneration = 0;
    std::string itemPath;
    CandidateProof proof = CandidateProof::None;
};

// Narrow process-local seam for cache/prefetch_probe.cpp. The probe installs it only after
// claiming a marker under the repository's .local/fb2k-dev profile and removes it afterwards.
class PrefetchHostProbeAccess final {
  public:
    static bool Install(PrefetchHostProbeSnapshot snapshot);
    static bool SetSnapshot(PrefetchHostProbeSnapshot snapshot);
    static bool DispatchPlaybackTime();
    static bool NotifyContextChanged();
    static bool CompletePrime(uint64_t requestId);
    static bool Uninstall();
    static bool IsInstalled();
    static uint64_t ActivePrimeRequestId();
    static std::vector<PrefetchHostProbeEvent> Events();
};

// Process-wide foobar2000 bridge and prefetch lifecycle. All methods that inspect or mutate
// playback/core state are main-thread calls. Existing() returns null before initquit startup
// and after shutdown.
class PrefetchHost final {
  public:
    static void Start();
    static void Shutdown();
    static PrefetchHost& Get();
    static PrefetchHost* Existing();

    ~PrefetchHost();
    PrefetchHost(const PrefetchHost&) = delete;
    PrefetchHost& operator=(const PrefetchHost&) = delete;

    // Re-snapshots the current core state. Normally called by the registered callbacks.
    void Refresh();
    void ScheduleRefresh();

    // Called from the real play callback after the new input has opened successfully.
    void OnTrackStarted(std::string_view itemPath);
    void OnPlaybackStopped();

    // Explicit controls for configuration or future UI. Changes are handled without a timer.
    void SetEnabled(bool enabled);
    void SetNetworkAllowed(bool allowed);
    void NotifyContextChanged();
    void AllowRetry();

    // Internal split-task completion hop. Public only so the worker does not retain a host
    // pointer across shutdown.
    static void ReportPrimeExit(uint64_t requestId, bool failed);

  private:
    struct Impl;
    explicit PrefetchHost(std::unique_ptr<Impl> impl);

    friend class PrefetchHostProbeAccess;
    std::unique_ptr<Impl> m_impl;
};

} // namespace glasswave::cache
