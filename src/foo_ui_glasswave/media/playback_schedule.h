#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace glasswave::media {

using PlaybackOccurrenceId = std::uint64_t;

inline constexpr double kRepeatCyclePrefetchSeconds = 30.0;

inline std::vector<PlaybackOccurrenceId> ApplyPhysicalRemovalMask(
    std::span<const PlaybackOccurrenceId> physical, std::span<const std::uint8_t> removed) {
    std::vector<PlaybackOccurrenceId> out;
    out.reserve(physical.size());
    for (std::size_t i = 0; i < physical.size(); ++i)
        if (i >= removed.size() || removed[i] == 0) out.push_back(physical[i]);
    return out;
}

enum class PlaybackRepeatMode : int {
    Off = 0,
    All = 1,
    One = 2,
};

struct PlaybackScheduleState {
    std::vector<PlaybackOccurrenceId> canonicalOrder;
    // Visible Now Playing row order. Playback roles below may refer to positions elsewhere.
    std::vector<PlaybackOccurrenceId> projectionOrder;
    std::vector<PlaybackOccurrenceId> history;
    std::optional<PlaybackOccurrenceId> current;
    std::vector<std::vector<PlaybackOccurrenceId>> priorityBlocks;
    std::vector<PlaybackOccurrenceId> ordinaryFuture;
    std::vector<PlaybackOccurrenceId> pendingCycle;
    bool shuffle = false;
    PlaybackRepeatMode repeat = PlaybackRepeatMode::Off;
};

// Deterministic queue-order state for Glasswave's protected Now Playing playlist.
//
// Occurrence ids name physical queue entries, not tracks. Two queue rows with the same
// metadb handle therefore get different ids and survive reorder/remove operations without
// being collapsed. Canonical order, visible row projection and playback roles are separate:
// explicit selection can preserve every physical row while a PlayNext row still plays next.
class PlaybackSchedule final {
  public:
    static constexpr std::size_t kHistoryLimit = 1024;

    explicit PlaybackSchedule(std::uint64_t seed = 0);

    // Replaces the occurrence set with a freshly chosen source list. This always advances the
    // source generation. With shuffle enabled, every other row (including rows before the
    // clicked row) joins the random future; with shuffle disabled, the prefix is historical.
    void ResetFresh(std::span<const PlaybackOccurrenceId> imported_order, std::size_t start,
                    std::uint64_t seed);

    // Restores an existing physical queue after component startup. `canonical_order` is the
    // independent unshuffled rank; `physical_order` need not match it under shuffle.
    void Restore(std::span<const PlaybackOccurrenceId> canonical_order,
                 std::span<const PlaybackOccurrenceId> physical_order, std::size_t current_position,
                 std::uint64_t seed);
    PlaybackScheduleState Snapshot() const;
    bool RestoreSnapshot(const PlaybackScheduleState& state, std::uint64_t seed);

    bool SetMode(bool shuffle, PlaybackRepeatMode repeat);
    bool shuffle_enabled() const { return m_shuffle; }
    PlaybackRepeatMode repeat_mode() const { return m_repeat; }

    // Successive calls prepend a new stable block. The newest call is next, while rows
    // inside one call retain input order.
    bool AddPlayNext(std::span<const PlaybackOccurrenceId> occurrences);

    // Adds only to the ordinary future. Shuffle inserts each occurrence at a uniformly
    // selected slot in that section, so priority, current and history are never changed.
    bool AddOrdinary(std::span<const PlaybackOccurrenceId> occurrences);

    // Explicit user reorder of the complete future. This is authoritative: it collapses
    // PlayNext priority back into ordinary order because the user has named the new order.
    bool ReorderFuture(std::span<const PlaybackOccurrenceId> occurrences);

    // Applies an explicit reorder of the complete physical playlist delivered by the core.
    // Rows before the current occurrence become history and the rest become ordinary
    // future. Repeat-one projection keeps its ordinary future before the current row.
    bool AdoptPhysicalOrder(std::span<const PlaybackOccurrenceId> physical_order,
                            std::size_t current_position);

    // Starts an explicitly selected row while preserving the current projected playback
    // order and existing PlayNext priority blocks. Unlike AdoptPhysicalOrder this does not
    // rewrite canonical rank, and unlike Started it does not put the clicked row before
    // rows the user skipped over.
    bool StartExplicit(std::span<const PlaybackOccurrenceId> physical_order, std::size_t current_position);

    bool Remove(std::span<const PlaybackOccurrenceId> occurrences);

    // A successful decode callback. `from_history` distinguishes a Previous navigation;
    // repeat-one at end of file is handled by the host and does not call this method.
    bool Started(PlaybackOccurrenceId occurrence, bool from_history = false);

    // Reconcile an external/core change without adding the old current row to history.
    bool RestoreCurrent(std::optional<PlaybackOccurrenceId> occurrence);

    // Called from playback-time events. It precomputes repeat-all's next permutation when
    // the current row is the last scheduled row and at most 30 seconds remain.
    bool Tick(double remaining_seconds);

    // Track failure fallback. Failed rows are not written to history.
    std::optional<PlaybackOccurrenceId> SkipFailedCurrent();

    std::optional<PlaybackOccurrenceId> Current() const { return m_current; }
    std::optional<PlaybackOccurrenceId> Previous() const;
    std::optional<PlaybackOccurrenceId> Next() const;
    std::optional<PlaybackOccurrenceId> ManualNext() const;

    // Actual automatic next items. Repeat-one therefore reports priority followed by the
    // current row; ordinary future remains available through FutureOrder().
    std::vector<PlaybackOccurrenceId> Upcoming() const;
    std::vector<PlaybackOccurrenceId> FutureOrder() const;
    std::vector<PlaybackOccurrenceId> ProjectionOrder() const;
    std::vector<PlaybackOccurrenceId> AllOccurrenceIds() const;
    std::vector<PlaybackOccurrenceId> CanonicalOrder() const;
    std::vector<PlaybackOccurrenceId> History() const;

    std::uint64_t revision() const { return m_revision; }
    std::uint64_t seed() const { return m_seed; }
    std::uint64_t generation() const { return m_generation; }
    void Touch() { BumpRevision(); }

  private:
    bool Contains(PlaybackOccurrenceId occurrence) const;
    bool Register(std::span<const PlaybackOccurrenceId> occurrences);
    bool RemoveFromFuture(PlaybackOccurrenceId occurrence);
    bool ApplyCanonicalOverride(std::span<const PlaybackOccurrenceId> occurrences);
    void PushHistory(PlaybackOccurrenceId occurrence);
    void TrimHistory();
    void BumpRevision();

    std::vector<PlaybackOccurrenceId> RoleProjectionOrder(bool anchor_current = true) const;
    std::vector<PlaybackOccurrenceId> PriorityOrder() const;
    std::vector<PlaybackOccurrenceId> PendingUpcoming() const;
    void CollapsePendingCycle();
    bool HasFuture() const;
    bool MakeRepeatCycleIfDue(double remaining_seconds);
    void Shuffle(std::vector<PlaybackOccurrenceId>& occurrences);
    std::size_t RandomBelow(std::size_t upper_bound);

    std::unordered_set<PlaybackOccurrenceId> m_known;
    std::vector<PlaybackOccurrenceId> m_canonicalOrder;
    std::vector<PlaybackOccurrenceId> m_projectionOrder;
    std::deque<PlaybackOccurrenceId> m_history;
    std::optional<PlaybackOccurrenceId> m_current;
    std::vector<std::vector<PlaybackOccurrenceId>> m_priorityBlocks;
    std::vector<PlaybackOccurrenceId> m_ordinaryFuture;
    std::vector<PlaybackOccurrenceId> m_pendingCycle;
    bool m_shuffle = false;
    PlaybackRepeatMode m_repeat = PlaybackRepeatMode::Off;
    std::uint64_t m_seed = 0;
    std::uint64_t m_randomState = 0;
    std::uint64_t m_revision = 0;
    std::uint64_t m_generation = 0;
};

} // namespace glasswave::media
