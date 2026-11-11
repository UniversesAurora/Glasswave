#include "cache/prefetch.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace glasswave::cache {
namespace {} // namespace

std::optional<std::size_t> DeterministicNextIndex(std::size_t current, std::size_t count, OrderMode mode,
                                                  bool stopAfterCurrent) {
    if (stopAfterCurrent || count == 0 || current >= count) return std::nullopt;

    switch (mode) {
    case OrderMode::Sequential:
        return current + 1 < count ? std::optional<std::size_t>(current + 1) : std::nullopt;
    case OrderMode::RepeatAll:
        return (current + 1) % count;
    case OrderMode::RepeatOne:
        return current;
    case OrderMode::Shuffle:
    case OrderMode::Random:
    case OrderMode::Other:
        return std::nullopt;
    }
    return std::nullopt;
}

PrefetchCoordinator::CandidateStamp PrefetchCoordinator::CandidateStamp::From(
    const PrefetchRequest& request) {
    return {
        request.contextGeneration,
        request.trackGeneration,
        request.candidateGeneration,
        request.itemPath,
    };
}

bool PrefetchCoordinator::SameStamp(const CandidateStamp& a, const CandidateStamp& b) {
    return a.contextGeneration == b.contextGeneration && a.trackGeneration == b.trackGeneration &&
           a.candidateGeneration == b.candidateGeneration && a.itemPath == b.itemPath;
}

PrefetchCoordinator::PrefetchCoordinator(PrefetchPolicy policy) : m_policy(policy) {}

void PrefetchCoordinator::SetPolicy(PrefetchPolicy policy) {
    std::lock_guard lock(m_mutex);
    m_policy = policy;
}

bool PrefetchCoordinator::ShouldStart(const PlaybackSnapshot& snapshot) const {
    if (!snapshot.playing || snapshot.stopAfterCurrent || !snapshot.speculativePrefetchAllowed) return false;
    if (snapshot.paused && !m_policy.allowWhilePaused) return false;
    if (!snapshot.networkAllowed && !m_policy.allowOnConstrainedNetwork) return false;
    if (snapshot.next.proof == CandidateProof::None || snapshot.next.itemPath.empty()) return false;
    if (!std::isfinite(snapshot.remainingSeconds) || snapshot.remainingSeconds < 0) return false;

    const double lead = std::max(0.0, m_policy.leadSeconds);
    return snapshot.remainingSeconds <= lead;
}

bool PrefetchCoordinator::MatchesActive(const PrefetchRequest& request) const {
    return m_snapshot.has_value() && request.contextGeneration == m_snapshot->contextGeneration &&
           request.trackGeneration == m_snapshot->trackGeneration &&
           request.candidateGeneration == m_snapshot->candidateGeneration &&
           request.itemPath == m_snapshot->next.itemPath;
}

bool PrefetchCoordinator::AlreadyAttempted(const PrefetchRequest& request) const {
    return m_attempted.has_value() && SameStamp(*m_attempted, CandidateStamp::From(request));
}

PrefetchDecision PrefetchCoordinator::EvaluateLocked() {
    if (!m_snapshot.has_value()) return {};

    if (m_active.has_value()) {
        if (ShouldStart(*m_snapshot) && MatchesActive(*m_active)) return {};
        if (m_cancelIssued) return {};
        m_cancelIssued = true;
        return {.start = std::nullopt, .cancelRequestId = m_active->id};
    }

    if (!ShouldStart(*m_snapshot)) return {};

    PrefetchRequest request;
    request.id = m_nextRequestId++;
    request.contextGeneration = m_snapshot->contextGeneration;
    request.trackGeneration = m_snapshot->trackGeneration;
    request.candidateGeneration = m_snapshot->candidateGeneration;
    request.itemPath = m_snapshot->next.itemPath;
    request.proof = m_snapshot->next.proof;
    request.remainingSeconds = m_snapshot->remainingSeconds;

    if (AlreadyAttempted(request)) return {};

    m_active = request;
    m_cancelIssued = false;
    return {.start = std::move(request), .cancelRequestId = std::nullopt};
}

PrefetchDecision PrefetchCoordinator::Evaluate(PlaybackSnapshot snapshot) {
    std::lock_guard lock(m_mutex);
    m_snapshot = std::move(snapshot);
    return EvaluateLocked();
}

PrefetchDecision PrefetchCoordinator::Finish(uint64_t requestId, PrefetchOutcome outcome) {
    std::lock_guard lock(m_mutex);
    if (!m_active.has_value() || m_active->id != requestId) return {};

    if (outcome != PrefetchOutcome::Cancelled) m_attempted = CandidateStamp::From(*m_active);
    m_active.reset();
    m_cancelIssued = false;

    // A context change that arrived while the previous transfer was winding down can now
    // start its replacement. This remains one active work item for the whole coordinator.
    return EvaluateLocked();
}

void PrefetchCoordinator::AllowRetry() {
    std::lock_guard lock(m_mutex);
    m_attempted.reset();
}

uint64_t PrefetchCoordinator::ActiveRequestId() const {
    std::lock_guard lock(m_mutex);
    return m_active.has_value() ? m_active->id : 0;
}

} // namespace glasswave::cache
