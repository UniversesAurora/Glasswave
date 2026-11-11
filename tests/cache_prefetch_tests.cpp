#include "cache/prefetch.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using glasswave::cache::CandidateProof;
using glasswave::cache::OrderMode;
using glasswave::cache::PrefetchCoordinator;
using glasswave::cache::PrefetchOutcome;
using glasswave::cache::PrefetchPolicy;

int g_checks = 0;

void Check(bool value, const char* message) {
    ++g_checks;
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void CheckIndex(const std::optional<std::size_t>& actual, std::size_t expected, const char* message) {
    Check(actual == expected, message);
}

void CheckNoIndex(const std::optional<std::size_t>& actual, const char* message) {
    Check(!actual.has_value(), message);
}

glasswave::cache::PlaybackSnapshot Snapshot(std::string identity, double remaining = 12.0,
                                            CandidateProof proof = CandidateProof::CoreLookahead) {
    glasswave::cache::PlaybackSnapshot snapshot;
    snapshot.playing = true;
    snapshot.remainingSeconds = remaining;
    snapshot.next.itemPath = identity;
    snapshot.next.proof = proof;
    return snapshot;
}

void TestEightModeEdges() {
    CheckIndex(glasswave::cache::DeterministicNextIndex(1, 5, OrderMode::Sequential), 2,
               "sequential advances within the list");
    CheckNoIndex(glasswave::cache::DeterministicNextIndex(4, 5, OrderMode::Sequential),
                 "sequential stops at the list end");
    CheckIndex(glasswave::cache::DeterministicNextIndex(4, 5, OrderMode::RepeatAll), 0,
               "repeat-all wraps to the beginning");
    CheckIndex(glasswave::cache::DeterministicNextIndex(0, 1, OrderMode::RepeatAll), 0,
               "repeat-all handles a one-item list");
    CheckIndex(glasswave::cache::DeterministicNextIndex(3, 5, OrderMode::RepeatOne), 3,
               "repeat-one selects the current item");
    CheckNoIndex(glasswave::cache::DeterministicNextIndex(0, 1, OrderMode::Shuffle),
                 "shuffle is never approximated, including one-item lists");
    CheckNoIndex(glasswave::cache::DeterministicNextIndex(2, 5, OrderMode::Random),
                 "random is never approximated");
    CheckNoIndex(glasswave::cache::DeterministicNextIndex(1, 5, OrderMode::Other),
                 "unknown playback orders are never approximated");
    CheckNoIndex(glasswave::cache::DeterministicNextIndex(1, 5, OrderMode::Sequential, true),
                 "stop-after-current suppresses sequential work");
    CheckNoIndex(glasswave::cache::DeterministicNextIndex(1, 5, OrderMode::RepeatOne, true),
                 "stop-after-current suppresses repeat-one work");
    CheckNoIndex(glasswave::cache::DeterministicNextIndex(5, 5, OrderMode::Sequential),
                 "an invalid current index is rejected");
    CheckNoIndex(glasswave::cache::DeterministicNextIndex(0, 0, OrderMode::RepeatAll),
                 "an empty list is rejected");
}

void TestThresholdPauseAndSingleFlight() {
    PrefetchCoordinator coordinator;

    auto decision = coordinator.Evaluate(Snapshot("next", 30.001));
    Check(!decision, "the default policy does nothing before 30 seconds");

    decision = coordinator.Evaluate(Snapshot("next", 30.0));
    Check(decision.start && !decision.cancelRequestId, "the default policy starts at 30 seconds");
    Check(decision.start->proof == CandidateProof::CoreLookahead, "the proof is carried to the request");
    const uint64_t first = decision.start->id;

    decision = coordinator.Finish(first + 1, PrefetchOutcome::Complete);
    Check(!decision && coordinator.ActiveRequestId() == first, "an out-of-order completion is ignored");

    decision = coordinator.Evaluate(Snapshot("next", 29.5));
    Check(!decision, "progress updates do not duplicate an active flight");

    auto paused = Snapshot("next", 29.0);
    paused.paused = true;
    decision = coordinator.Evaluate(paused);
    Check(decision.cancelRequestId == first, "pause cancels speculative work");
    decision = coordinator.Evaluate(paused);
    Check(!decision, "the same cancellation is not emitted repeatedly");

    decision = coordinator.Finish(first, PrefetchOutcome::Cancelled);
    Check(!decision && coordinator.ActiveRequestId() == 0,
          "finishing cancellation does not restart while paused");

    decision = coordinator.Evaluate(Snapshot("next", 28.0));
    Check(decision.start && decision.start->id != first, "unpause can start after cancellation");
    const uint64_t second = decision.start->id;

    decision = coordinator.Finish(second, PrefetchOutcome::Complete);
    Check(!decision, "a completed exact candidate is not requested again");
    decision = coordinator.Evaluate(Snapshot("next", 27.0));
    Check(!decision, "the completed candidate remains suppressed");
}

void TestGenerationOrderingAndRetry() {
    PrefetchCoordinator coordinator;

    auto current = Snapshot("next", 20.0);
    auto decision = coordinator.Evaluate(current);
    Check(decision.start.has_value(), "generation test starts an initial flight");
    const uint64_t first = decision.start->id;

    current.contextGeneration = 1;
    decision = coordinator.Evaluate(current);
    Check(decision.cancelRequestId == first, "an account or quality generation change cancels work");
    decision = coordinator.Finish(first, PrefetchOutcome::Cancelled);
    Check(decision.start && decision.start->itemPath == "next",
          "the new generation starts only after cancellation finishes");
    const uint64_t second = decision.start->id;

    auto changed = current;
    changed.trackGeneration = 1;
    changed.candidateGeneration = 1;
    changed.next.itemPath = "changed";
    decision = coordinator.Evaluate(changed);
    Check(decision.cancelRequestId == second, "track and candidate generation changes cancel stale work");
    decision = coordinator.Finish(second, PrefetchOutcome::Cancelled);
    Check(decision.start && decision.start->itemPath == "changed",
          "the replacement candidate starts after stale work stops");
    const uint64_t third = decision.start->id;

    auto constrained = changed;
    constrained.networkAllowed = false;
    decision = coordinator.Evaluate(constrained);
    Check(decision.cancelRequestId == third, "a constrained network cancels speculative work");
    decision = coordinator.Finish(third, PrefetchOutcome::Failed);
    Check(!decision, "failure does not retry while constrained");

    decision = coordinator.Evaluate(changed);
    Check(!decision, "failure is not retried by progress alone");
    coordinator.AllowRetry();
    decision = coordinator.Evaluate(changed);
    Check(decision.start && decision.start->itemPath == "changed",
          "an explicit retry starts one new attempt");
    const uint64_t fourth = decision.start->id;

    auto stopping = changed;
    stopping.stopAfterCurrent = true;
    decision = coordinator.Evaluate(stopping);
    Check(decision.cancelRequestId == fourth, "stop-after-current cancels speculative work");
}

void TestOptInPolicyAndProofs() {
    PrefetchPolicy policy;
    policy.allowWhilePaused = true;
    policy.allowOnConstrainedNetwork = true;

    PrefetchCoordinator coordinator(policy);
    auto snapshot = Snapshot("queue", 10.0, CandidateProof::CoreQueue);
    snapshot.paused = true;
    snapshot.networkAllowed = false;
    auto decision = coordinator.Evaluate(snapshot);
    Check(decision.start && decision.start->proof == CandidateProof::CoreQueue,
          "CoreQueue is accepted when the explicit policy allows paused constrained work");
    coordinator.Finish(decision.start->id, PrefetchOutcome::Cancelled);

    PrefetchCoordinator invalid(policy);
    auto noProof = Snapshot("invalid", 10.0, CandidateProof::None);
    Check(!invalid.Evaluate(noProof), "unproven candidates are never prefetched");

    auto emptyPath = Snapshot("", 10.0, CandidateProof::CoreQueue);
    Check(!invalid.Evaluate(emptyPath), "empty identities are never prefetched");

    auto disabled = Snapshot("disabled", 10.0);
    disabled.speculativePrefetchAllowed = false;
    Check(!invalid.Evaluate(disabled), "disabled speculative prefetch starts nothing");

    auto stopped = Snapshot("stopped", 10.0);
    stopped.playing = false;
    Check(!invalid.Evaluate(stopped), "a stopped player starts nothing");

    auto unknownRemaining = Snapshot("unknown", -1.0);
    Check(!invalid.Evaluate(unknownRemaining), "unknown duration starts nothing");
    unknownRemaining.remainingSeconds = std::numeric_limits<double>::quiet_NaN();
    Check(!invalid.Evaluate(unknownRemaining), "non-finite duration starts nothing");
}

void TestConcurrentEvaluationStartsOneFlight() {
    PrefetchCoordinator coordinator;
    std::atomic<int> ready = 0;
    std::atomic<bool> go = false;
    std::mutex decisionsMutex;
    std::vector<glasswave::cache::PrefetchDecision> decisions;

    const auto worker = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire))
            std::this_thread::yield();
        auto decision = coordinator.Evaluate(Snapshot("concurrent", 10.0));
        std::lock_guard lock(decisionsMutex);
        decisions.push_back(std::move(decision));
    };

    std::thread first(worker);
    std::thread second(worker);
    while (ready.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    go.store(true, std::memory_order_release);
    first.join();
    second.join();

    Check(decisions.size() == 2, "both concurrent evaluations return");
    const auto starts = static_cast<int>(std::count_if(
        decisions.begin(), decisions.end(), [](const auto& decision) { return decision.start.has_value(); }));
    Check(starts == 1, "two concurrent evaluations start exactly one flight");
    Check(coordinator.ActiveRequestId() != 0, "the single winner remains active");
}

} // namespace

int main() {
    TestEightModeEdges();
    TestThresholdPauseAndSingleFlight();
    TestGenerationOrderingAndRetry();
    TestOptInPolicyAndProofs();
    TestConcurrentEvaluationStartsOneFlight();

    std::cout << "Cache prefetch checks passed: " << g_checks << '\n';
    return 0;
}
