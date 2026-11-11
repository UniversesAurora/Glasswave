#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

// Event-driven policy for warming one exact next track. This module deliberately cannot
// advance playback or create a random order. The host supplies either a deterministic
// playlist result or a candidate confirmed by an authoritative core lookahead bridge.
namespace glasswave::cache {

enum class OrderMode {
    Sequential,
    RepeatAll,
    RepeatOne,
    Shuffle,
    Random,
    Other
};

// Returns the next playlist index only when the supplied mode alone proves it. The caller
// must map the index to its item and mark CandidateProof::LinearPlaylist. Shuffle/random
// return nullopt even for a one-item list; the core may choose it, but this helper must
// not pretend that it knows the core's private order state. The separate playback queue is
// ordered front-first and is represented by CandidateProof::CoreQueue.
std::optional<std::size_t> DeterministicNextIndex(std::size_t current, std::size_t count, OrderMode mode,
                                                  bool stopAfterCurrent = false);

enum class CandidateProof {
    None,
    LinearPlaylist, // DeterministicNextIndex supplied the index.
    RepeatOne,
    CoreQueue,     // First queue entry accepted by every registered skipTrack service.
    CoreLookahead, // Host bridge confirmed the actual next item without advancing core.
};

struct PrefetchCandidate {
    std::string itemPath; // Stable metadb location; the real input resolves the audio key.
    CandidateProof proof = CandidateProof::None;
};

// The host snapshots core state on the main thread, then may call Evaluate from a decoder
// or transfer worker. Every identity-changing event (track, next candidate, account/quality,
// order, queue edit or manual navigation) must advance at least one generation.
// Progress-only updates leave the generations unchanged.
struct PlaybackSnapshot {
    uint64_t contextGeneration = 0;
    uint64_t trackGeneration = 0;
    uint64_t candidateGeneration = 0;
    bool playing = false;
    bool paused = false;
    bool stopAfterCurrent = false;
    bool networkAllowed = true;
    bool speculativePrefetchAllowed = true;
    // Negative while unknown, for example a live stream or an unopened current item.
    double remainingSeconds = -1.0;
    PrefetchCandidate next;
};

struct PrefetchPolicy {
    // Evaluated only when the host delivers an event. This module creates no timer.
    double leadSeconds = 30.0;
    bool allowWhilePaused = false;
    bool allowOnConstrainedNetwork = false;
};

struct PrefetchRequest {
    uint64_t id = 0;
    uint64_t contextGeneration = 0;
    uint64_t trackGeneration = 0;
    uint64_t candidateGeneration = 0;
    std::string itemPath;
    CandidateProof proof = CandidateProof::None;
    double remainingSeconds = -1.0;
};

struct PrefetchDecision {
    std::optional<PrefetchRequest> start;
    std::optional<uint64_t> cancelRequestId;

    explicit operator bool() const { return start.has_value() || cancelRequestId.has_value(); }
};

enum class PrefetchOutcome {
    Complete,
    Failed,
    Cancelled
};

// Pure coordinator. At most one request is active. Evaluate() is safe to call from the
// main thread, decoder worker or transfer scheduler. The caller must report every emitted
// Start through Finish(); a changed evaluation requests cancellation rather than launching
// a second transfer. Progress events do not reschedule an already active request.
class PrefetchCoordinator {
  public:
    explicit PrefetchCoordinator(PrefetchPolicy policy = {});

    void SetPolicy(PrefetchPolicy policy);
    PrefetchDecision Evaluate(PlaybackSnapshot snapshot);
    PrefetchDecision Finish(uint64_t requestId, PrefetchOutcome outcome);

    // A failed transfer is not polled or retried automatically. Call after an explicit
    // network/auth transition; the next progress/context event can then try once more.
    void AllowRetry();

    uint64_t ActiveRequestId() const;

  private:
    struct CandidateStamp {
        uint64_t contextGeneration = 0;
        uint64_t trackGeneration = 0;
        uint64_t candidateGeneration = 0;
        std::string itemPath;

        static CandidateStamp From(const PrefetchRequest& request);
    };

    bool ShouldStart(const PlaybackSnapshot& snapshot) const;
    bool MatchesActive(const PrefetchRequest& request) const;
    bool AlreadyAttempted(const PrefetchRequest& request) const;
    static bool SameStamp(const CandidateStamp& a, const CandidateStamp& b);
    PrefetchDecision EvaluateLocked();

    mutable std::mutex m_mutex;
    PrefetchPolicy m_policy;
    std::optional<PlaybackSnapshot> m_snapshot;
    std::optional<PrefetchRequest> m_active;
    std::optional<CandidateStamp> m_attempted;
    uint64_t m_nextRequestId = 1;
    bool m_cancelIssued = false;
};

} // namespace glasswave::cache
