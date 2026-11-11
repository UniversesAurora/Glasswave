#include "media/playback_schedule.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace glasswave::media {

namespace {

constexpr std::uint64_t kRandomGolden = 0x9e3779b97f4a7c15ULL;

bool IsUnique(std::span<const PlaybackOccurrenceId> occurrences) {
    std::unordered_set<PlaybackOccurrenceId> seen;
    seen.reserve(occurrences.size());
    for (const auto occurrence : occurrences) {
        if (occurrence == 0 || !seen.insert(occurrence).second) return false;
    }
    return true;
}

std::unordered_set<PlaybackOccurrenceId> AsSet(std::span<const PlaybackOccurrenceId> occurrences) {
    return std::unordered_set<PlaybackOccurrenceId>(occurrences.begin(), occurrences.end());
}

} // namespace

PlaybackSchedule::PlaybackSchedule(std::uint64_t seed) : m_seed(seed), m_randomState(seed) {}

void PlaybackSchedule::ResetFresh(std::span<const PlaybackOccurrenceId> imported_order, std::size_t start,
                                  std::uint64_t seed) {
    ++m_generation;
    m_known.clear();
    m_canonicalOrder.clear();
    m_projectionOrder.clear();
    m_history.clear();
    m_current.reset();
    m_priorityBlocks.clear();
    m_ordinaryFuture.clear();
    m_pendingCycle.clear();
    m_seed = seed;
    m_randomState = seed;
    if (!IsUnique(imported_order)) {
        BumpRevision();
        return;
    }

    m_canonicalOrder.assign(imported_order.begin(), imported_order.end());
    m_known.insert(imported_order.begin(), imported_order.end());
    if (m_canonicalOrder.empty()) {
        BumpRevision();
        return;
    }

    start = std::min(start, m_canonicalOrder.size() - 1);
    m_current = m_canonicalOrder[start];
    if (m_shuffle) {
        for (const auto occurrence : m_canonicalOrder)
            if (occurrence != *m_current) m_ordinaryFuture.push_back(occurrence);
        Shuffle(m_ordinaryFuture);
    } else {
        for (std::size_t i = 0; i < start; ++i)
            PushHistory(m_canonicalOrder[i]);
        for (std::size_t i = start + 1; i < m_canonicalOrder.size(); ++i)
            m_ordinaryFuture.push_back(m_canonicalOrder[i]);
    }
    m_projectionOrder = RoleProjectionOrder();
    BumpRevision();
}

void PlaybackSchedule::Restore(std::span<const PlaybackOccurrenceId> canonical_order,
                               std::span<const PlaybackOccurrenceId> physical_order,
                               std::size_t current_position, std::uint64_t seed) {
    ++m_generation;
    m_known.clear();
    m_canonicalOrder.clear();
    m_projectionOrder.clear();
    m_history.clear();
    m_current.reset();
    m_priorityBlocks.clear();
    m_ordinaryFuture.clear();
    m_pendingCycle.clear();
    m_seed = seed;
    m_randomState = seed;
    if (!IsUnique(canonical_order) || !IsUnique(physical_order) ||
        canonical_order.size() != physical_order.size()) {
        BumpRevision();
        return;
    }

    const auto canonical = AsSet(canonical_order);
    const auto physical = AsSet(physical_order);
    if (canonical != physical) {
        BumpRevision();
        return;
    }

    m_canonicalOrder.assign(canonical_order.begin(), canonical_order.end());
    m_projectionOrder.assign(physical_order.begin(), physical_order.end());
    m_known = canonical;
    if (!physical_order.empty() && current_position != SIZE_MAX) {
        current_position = std::min(current_position, physical_order.size() - 1);
        m_current = physical_order[current_position];
        for (std::size_t i = 0; i < physical_order.size(); ++i)
            if (i != current_position) m_ordinaryFuture.push_back(physical_order[i]);
    }
    BumpRevision();
}

PlaybackScheduleState PlaybackSchedule::Snapshot() const {
    PlaybackScheduleState state;
    state.canonicalOrder = m_canonicalOrder;
    state.projectionOrder = m_projectionOrder;
    state.history.assign(m_history.begin(), m_history.end());
    state.current = m_current;
    state.priorityBlocks = m_priorityBlocks;
    state.ordinaryFuture = m_ordinaryFuture;
    state.pendingCycle = m_pendingCycle;
    state.shuffle = m_shuffle;
    state.repeat = m_repeat;
    return state;
}

bool PlaybackSchedule::RestoreSnapshot(const PlaybackScheduleState& state, std::uint64_t seed) {
    if (!IsUnique(state.canonicalOrder)) return false;
    if (!IsUnique(state.projectionOrder) || state.projectionOrder.size() != state.canonicalOrder.size())
        return false;
    if (state.repeat != PlaybackRepeatMode::Off && state.repeat != PlaybackRepeatMode::All &&
        state.repeat != PlaybackRepeatMode::One) {
        return false;
    }

    const auto known = AsSet(state.canonicalOrder);
    if (AsSet(state.projectionOrder) != known) return false;
    for (const auto occurrence : state.history)
        if (!known.contains(occurrence)) return false;
    if (state.current && !known.contains(*state.current)) return false;
    std::unordered_set<PlaybackOccurrenceId> future;
    for (const auto& block : state.priorityBlocks) {
        if (block.empty()) return false;
        for (const auto occurrence : block)
            if (!known.contains(occurrence) || !future.insert(occurrence).second) return false;
    }
    for (const auto occurrence : state.ordinaryFuture)
        if (!known.contains(occurrence) || !future.insert(occurrence).second) return false;
    if (state.current && future.contains(*state.current)) return false;

    std::unordered_set<PlaybackOccurrenceId> pending;
    for (const auto occurrence : state.pendingCycle)
        if (!known.contains(occurrence) || !pending.insert(occurrence).second || future.contains(occurrence))
            return false;

    ++m_generation;
    m_known = known;
    m_canonicalOrder = state.canonicalOrder;
    m_projectionOrder = state.projectionOrder;
    m_history.assign(state.history.begin(), state.history.end());
    TrimHistory();
    m_current = state.current;
    m_priorityBlocks = state.priorityBlocks;
    m_ordinaryFuture = state.ordinaryFuture;
    m_pendingCycle = state.pendingCycle;
    m_shuffle = state.shuffle;
    m_repeat = state.repeat;
    m_seed = seed;
    m_randomState = seed;
    BumpRevision();
    return true;
}

bool PlaybackSchedule::SetMode(bool shuffle, PlaybackRepeatMode repeat) {
    const bool valid_repeat = repeat == PlaybackRepeatMode::Off || repeat == PlaybackRepeatMode::All ||
                              repeat == PlaybackRepeatMode::One;
    if (!valid_repeat) return false;
    if (m_shuffle == shuffle && m_repeat == repeat) return true;

    CollapsePendingCycle();
    if (m_shuffle != shuffle) {
        m_shuffle = shuffle;
        if (m_shuffle) {
            Shuffle(m_ordinaryFuture);
        } else {
            const auto unplayed = AsSet(m_ordinaryFuture);
            m_ordinaryFuture.clear();
            m_ordinaryFuture.reserve(unplayed.size());
            for (const auto occurrence : m_canonicalOrder)
                if (unplayed.contains(occurrence)) m_ordinaryFuture.push_back(occurrence);
        }
    }
    if (m_repeat != repeat) {
        m_repeat = repeat;
        if (m_repeat != PlaybackRepeatMode::All) m_pendingCycle.clear();
    }
    m_projectionOrder = RoleProjectionOrder(repeat != PlaybackRepeatMode::One);
    BumpRevision();
    return true;
}

bool PlaybackSchedule::AddPlayNext(std::span<const PlaybackOccurrenceId> occurrences) {
    if (occurrences.empty() || !IsUnique(occurrences)) return false;
    for (const auto occurrence : occurrences)
        if (Contains(occurrence)) return false;

    CollapsePendingCycle();
    if (!Register(occurrences)) return false;
    m_priorityBlocks.insert(m_priorityBlocks.begin(),
                            std::vector<PlaybackOccurrenceId>(occurrences.begin(), occurrences.end()));
    m_projectionOrder = RoleProjectionOrder();
    BumpRevision();
    return true;
}

bool PlaybackSchedule::AddOrdinary(std::span<const PlaybackOccurrenceId> occurrences) {
    if (occurrences.empty() || !IsUnique(occurrences)) return false;
    for (const auto occurrence : occurrences)
        if (Contains(occurrence)) return false;

    CollapsePendingCycle();
    if (!Register(occurrences)) return false;
    if (!m_shuffle) {
        m_ordinaryFuture.insert(m_ordinaryFuture.end(), occurrences.begin(), occurrences.end());
    } else {
        std::vector<PlaybackOccurrenceId> oldFuture = std::move(m_ordinaryFuture);
        std::vector<PlaybackOccurrenceId> newFuture(occurrences.begin(), occurrences.end());
        Shuffle(newFuture);
        m_ordinaryFuture.clear();
        m_ordinaryFuture.reserve(oldFuture.size() + newFuture.size());
        std::size_t oldAt = 0, newAt = 0;
        while (oldAt < oldFuture.size() || newAt < newFuture.size()) {
            const std::size_t oldRemaining = oldFuture.size() - oldAt;
            const std::size_t newRemaining = newFuture.size() - newAt;
            if (oldRemaining == 0) {
                m_ordinaryFuture.push_back(newFuture[newAt++]);
            } else if (newRemaining == 0) {
                m_ordinaryFuture.push_back(oldFuture[oldAt++]);
            } else if (RandomBelow(oldRemaining + newRemaining) < newRemaining) {
                m_ordinaryFuture.push_back(newFuture[newAt++]);
            } else {
                m_ordinaryFuture.push_back(oldFuture[oldAt++]);
            }
        }
    }
    m_projectionOrder = RoleProjectionOrder();
    BumpRevision();
    return true;
}

bool PlaybackSchedule::ReorderFuture(std::span<const PlaybackOccurrenceId> occurrences) {
    if (!IsUnique(occurrences)) return false;
    CollapsePendingCycle();
    const auto expected = FutureOrder();
    if (expected.size() != occurrences.size()) return false;
    if (AsSet(expected) != AsSet(occurrences)) return false;
    if (!ApplyCanonicalOverride(occurrences)) return false;

    m_priorityBlocks.clear();
    m_ordinaryFuture.assign(occurrences.begin(), occurrences.end());
    m_projectionOrder = RoleProjectionOrder();
    BumpRevision();
    return true;
}

bool PlaybackSchedule::AdoptPhysicalOrder(std::span<const PlaybackOccurrenceId> physical_order,
                                          std::size_t current_position) {
    if (!IsUnique(physical_order) || physical_order.size() != m_known.size() ||
        AsSet(physical_order) != m_known) {
        return false;
    }

    m_priorityBlocks.clear();
    m_pendingCycle.clear();
    m_history.clear();
    m_ordinaryFuture.clear();
    m_current.reset();

    if (!physical_order.empty() && current_position != SIZE_MAX) {
        current_position = std::min(current_position, physical_order.size() - 1);
        m_current = physical_order[current_position];
        if (m_repeat == PlaybackRepeatMode::One && m_priorityBlocks.empty()) {
            for (std::size_t i = 0; i < physical_order.size(); ++i)
                if (i != current_position) m_ordinaryFuture.push_back(physical_order[i]);
        } else {
            for (std::size_t i = 0; i < current_position; ++i)
                PushHistory(physical_order[i]);
            for (std::size_t i = current_position + 1; i < physical_order.size(); ++i)
                m_ordinaryFuture.push_back(physical_order[i]);
        }
    } else {
        m_ordinaryFuture.assign(physical_order.begin(), physical_order.end());
    }
    if (!ApplyCanonicalOverride(std::span<const PlaybackOccurrenceId>(m_ordinaryFuture))) return false;
    m_projectionOrder.assign(physical_order.begin(), physical_order.end());
    BumpRevision();
    return true;
}

bool PlaybackSchedule::StartExplicit(std::span<const PlaybackOccurrenceId> physical_order,
                                     std::size_t current_position) {
    if (!IsUnique(physical_order) || physical_order.size() != m_known.size() ||
        AsSet(physical_order) != m_known || physical_order.empty() ||
        current_position >= physical_order.size()) {
        return false;
    }

    std::unordered_set<PlaybackOccurrenceId> priority;
    std::vector<std::vector<PlaybackOccurrenceId>> preservedPriority;
    preservedPriority.reserve(m_priorityBlocks.size());
    for (auto block : m_priorityBlocks) {
        std::erase(block, physical_order[current_position]);
        if (block.empty()) continue;
        priority.insert(block.begin(), block.end());
        preservedPriority.push_back(std::move(block));
    }

    m_history.clear();
    m_priorityBlocks = std::move(preservedPriority);
    m_ordinaryFuture.clear();
    m_pendingCycle.clear();
    m_current = physical_order[current_position];
    for (std::size_t i = 0; i < current_position; ++i)
        if (!priority.contains(physical_order[i])) PushHistory(physical_order[i]);
    for (std::size_t i = current_position + 1; i < physical_order.size(); ++i)
        if (!priority.contains(physical_order[i])) m_ordinaryFuture.push_back(physical_order[i]);
    m_projectionOrder.assign(physical_order.begin(), physical_order.end());
    BumpRevision();
    return true;
}

bool PlaybackSchedule::Remove(std::span<const PlaybackOccurrenceId> occurrences) {
    if (occurrences.empty()) return false;
    const auto removed = AsSet(occurrences);
    bool changed = false;
    const auto before_known = m_known.size();
    for (const auto occurrence : removed)
        changed |= m_known.erase(occurrence) != 0;
    if (!changed) return false;

    const auto removeIf = [&removed](PlaybackOccurrenceId occurrence) {
        return removed.contains(occurrence);
    };
    std::erase_if(m_canonicalOrder, removeIf);
    std::erase_if(m_projectionOrder, removeIf);
    std::erase_if(m_ordinaryFuture, removeIf);
    for (auto& block : m_priorityBlocks)
        std::erase_if(block, removeIf);
    std::erase_if(m_priorityBlocks, [](const auto& block) { return block.empty(); });
    std::erase_if(m_pendingCycle, removeIf);
    std::erase_if(m_history, removeIf);
    if (m_current && removed.contains(*m_current)) m_current.reset();
    BumpRevision();
    return before_known != m_known.size();
}

bool PlaybackSchedule::Started(PlaybackOccurrenceId occurrence, bool from_history) {
    if (!Contains(occurrence)) return false;
    if (m_current == occurrence) return true;

    const auto old_current = m_current;
    const auto pending = std::find(m_pendingCycle.begin(), m_pendingCycle.end(), occurrence);
    if (pending != m_pendingCycle.end()) {
        if (old_current) PushHistory(*old_current);
        std::vector<PlaybackOccurrenceId> remainder;
        for (auto it = pending + 1; it != m_pendingCycle.end(); ++it)
            if (*it != occurrence) remainder.push_back(*it);
        m_priorityBlocks.clear();
        m_ordinaryFuture = std::move(remainder);
        m_pendingCycle.clear();
        m_current = occurrence;
        BumpRevision();
        return true;
    }

    if (from_history) {
        const auto in_history = std::find(m_history.rbegin(), m_history.rend(), occurrence);
        if (in_history != m_history.rend()) {
            const auto target = std::next(in_history).base();
            std::vector<PlaybackOccurrenceId> replay;
            replay.reserve(static_cast<std::size_t>(std::distance(target, m_history.end())) + 1);
            for (auto it = target; it != m_history.end(); ++it)
                if (*it != occurrence) replay.push_back(*it);
            if (old_current) replay.push_back(*old_current);
            m_history.erase(std::next(in_history).base(), m_history.end());
            std::unordered_set<PlaybackOccurrenceId> replaySet(replay.begin(), replay.end());
            std::erase_if(m_ordinaryFuture,
                          [&replaySet](PlaybackOccurrenceId value) { return replaySet.contains(value); });
            m_pendingCycle.clear();
            m_ordinaryFuture.insert(m_ordinaryFuture.begin(), replay.begin(), replay.end());
            m_current = occurrence;
            BumpRevision();
            return true;
        }
    }

    if (old_current) PushHistory(*old_current);
    RemoveFromFuture(occurrence);
    m_pendingCycle.clear();
    m_current = occurrence;
    BumpRevision();
    return true;
}

bool PlaybackSchedule::RestoreCurrent(std::optional<PlaybackOccurrenceId> occurrence) {
    if (occurrence && !Contains(*occurrence)) return false;
    if (m_current == occurrence) return true;
    if (occurrence) RemoveFromFuture(*occurrence);
    m_current = occurrence;
    BumpRevision();
    return true;
}

bool PlaybackSchedule::Tick(double remaining_seconds) {
    if (!std::isfinite(remaining_seconds)) return false;
    return MakeRepeatCycleIfDue(remaining_seconds);
}

std::optional<PlaybackOccurrenceId> PlaybackSchedule::SkipFailedCurrent() {
    if (!m_current) return std::nullopt;
    std::optional<PlaybackOccurrenceId> next;
    const auto priority = PriorityOrder();
    if (!priority.empty()) {
        next = priority.front();
    } else if (!m_ordinaryFuture.empty()) {
        next = m_ordinaryFuture.front();
    } else {
        const auto pending = PendingUpcoming();
        if (!pending.empty()) next = pending.front();
    }
    if (!next || next == m_current) return std::nullopt;

    m_current = next;
    RemoveFromFuture(*next);
    m_pendingCycle.clear();
    BumpRevision();
    return next;
}

std::optional<PlaybackOccurrenceId> PlaybackSchedule::Previous() const {
    if (!m_history.empty()) return m_history.back();
    return std::nullopt;
}

std::optional<PlaybackOccurrenceId> PlaybackSchedule::Next() const {
    if (!m_priorityBlocks.empty()) return m_priorityBlocks.front().front();
    if (m_repeat == PlaybackRepeatMode::One) return m_current;
    if (!m_ordinaryFuture.empty()) return m_ordinaryFuture.front();
    const auto pending = PendingUpcoming();
    if (!pending.empty()) return pending.front();
    if (m_repeat == PlaybackRepeatMode::All && m_pendingCycle.size() == 1 && m_current &&
        m_pendingCycle.front() == *m_current) {
        return m_current;
    }
    return std::nullopt;
}

std::optional<PlaybackOccurrenceId> PlaybackSchedule::ManualNext() const {
    if (!m_priorityBlocks.empty()) return m_priorityBlocks.front().front();
    if (!m_ordinaryFuture.empty()) return m_ordinaryFuture.front();
    const auto pending = PendingUpcoming();
    if (!pending.empty()) return pending.front();
    if (m_repeat == PlaybackRepeatMode::All && m_pendingCycle.size() == 1 && m_current &&
        m_pendingCycle.front() == *m_current) {
        return m_current;
    }
    return std::nullopt;
}

std::vector<PlaybackOccurrenceId> PlaybackSchedule::Upcoming() const {
    auto out = PriorityOrder();
    if (m_repeat == PlaybackRepeatMode::One) {
        if (m_current) out.push_back(*m_current);
    } else {
        out.insert(out.end(), m_ordinaryFuture.begin(), m_ordinaryFuture.end());
        const auto pending = PendingUpcoming();
        out.insert(out.end(), pending.begin(), pending.end());
        if (!pending.empty() && m_current) {
            out.push_back(*m_current);
        } else if (m_repeat == PlaybackRepeatMode::All && m_pendingCycle.size() == 1 && m_current &&
                   m_pendingCycle.front() == *m_current) {
            out.push_back(*m_current);
        }
    }
    return out;
}

std::vector<PlaybackOccurrenceId> PlaybackSchedule::FutureOrder() const {
    auto out = PriorityOrder();
    out.insert(out.end(), m_ordinaryFuture.begin(), m_ordinaryFuture.end());
    const auto pending = PendingUpcoming();
    out.insert(out.end(), pending.begin(), pending.end());
    return out;
}

std::vector<PlaybackOccurrenceId> PlaybackSchedule::RoleProjectionOrder(bool anchor_current) const {
    std::vector<PlaybackOccurrenceId> out;
    std::unordered_set<PlaybackOccurrenceId> seen;
    out.reserve(m_known.size());
    seen.reserve(m_known.size());
    const auto append = [&](PlaybackOccurrenceId occurrence) {
        if (seen.insert(occurrence).second) out.push_back(occurrence);
    };
    const auto append_vector = [&](const std::vector<PlaybackOccurrenceId>& values) {
        for (const auto occurrence : values)
            append(occurrence);
    };

    const auto future = FutureOrder();
    const auto pending = PendingUpcoming();
    const auto future_set = AsSet(std::span<const PlaybackOccurrenceId>(future));
    std::unordered_set<PlaybackOccurrenceId> represented = future_set;
    represented.insert(m_history.begin(), m_history.end());
    if (m_current) represented.insert(*m_current);
    for (const auto occurrence : m_canonicalOrder)
        if (!represented.contains(occurrence)) append(occurrence);

    if (m_repeat == PlaybackRepeatMode::One && m_priorityBlocks.empty() && m_current) {
        for (const auto& occurrence : m_history)
            if (!future_set.contains(occurrence) && occurrence != m_current) append(occurrence);
        append_vector(m_ordinaryFuture);
        append_vector(pending);
        append(*m_current);
    } else {
        for (const auto& occurrence : m_history)
            if (!future_set.contains(occurrence) && occurrence != m_current) append(occurrence);
        if (m_current) append(*m_current);
        append_vector(PriorityOrder());
        append_vector(m_ordinaryFuture);
        append_vector(pending);
    }

    // Automatic role changes and additions may not move the current physical row. Shuffle
    // toggles, queue insertions and paged appends can change surrounding rows, but the row
    // the owner selected stays anchored until a deliberate drag, replacement, or repeat-one
    // projection change.
    if (anchor_current && m_repeat != PlaybackRepeatMode::One && m_current) {
        const auto oldCurrent = std::find(m_projectionOrder.begin(), m_projectionOrder.end(), *m_current);
        if (oldCurrent != m_projectionOrder.end()) {
            const auto target =
                static_cast<std::size_t>(std::distance(m_projectionOrder.begin(), oldCurrent));
            std::erase(out, *m_current);
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(std::min(target, out.size())), *m_current);
        }
    }
    return out;
}

std::vector<PlaybackOccurrenceId> PlaybackSchedule::ProjectionOrder() const {
    return m_projectionOrder;
}

std::vector<PlaybackOccurrenceId> PlaybackSchedule::AllOccurrenceIds() const {
    return std::vector<PlaybackOccurrenceId>(m_canonicalOrder.begin(), m_canonicalOrder.end());
}

std::vector<PlaybackOccurrenceId> PlaybackSchedule::CanonicalOrder() const {
    return std::vector<PlaybackOccurrenceId>(m_canonicalOrder.begin(), m_canonicalOrder.end());
}

std::vector<PlaybackOccurrenceId> PlaybackSchedule::History() const {
    return std::vector<PlaybackOccurrenceId>(m_history.begin(), m_history.end());
}

bool PlaybackSchedule::Contains(PlaybackOccurrenceId occurrence) const {
    return m_known.contains(occurrence);
}

bool PlaybackSchedule::Register(std::span<const PlaybackOccurrenceId> occurrences) {
    m_known.reserve(m_known.size() + occurrences.size());
    m_canonicalOrder.reserve(m_canonicalOrder.size() + occurrences.size());
    for (const auto occurrence : occurrences) {
        m_known.insert(occurrence);
        m_canonicalOrder.push_back(occurrence);
    }
    return true;
}

bool PlaybackSchedule::RemoveFromFuture(PlaybackOccurrenceId occurrence) {
    bool changed = false;
    for (auto& block : m_priorityBlocks) {
        const auto old = block.size();
        std::erase(block, occurrence);
        changed |= old != block.size();
    }
    std::erase_if(m_priorityBlocks, [](const auto& block) { return block.empty(); });
    const auto old_future = m_ordinaryFuture.size();
    std::erase(m_ordinaryFuture, occurrence);
    changed |= old_future != m_ordinaryFuture.size();
    const auto old_pending = m_pendingCycle.size();
    std::erase(m_pendingCycle, occurrence);
    changed |= old_pending != m_pendingCycle.size();
    return changed;
}

bool PlaybackSchedule::ApplyCanonicalOverride(std::span<const PlaybackOccurrenceId> occurrences) {
    const auto affected = AsSet(occurrences);
    std::vector<std::size_t> slots;
    slots.reserve(occurrences.size());
    for (std::size_t i = 0; i < m_canonicalOrder.size(); ++i)
        if (affected.contains(m_canonicalOrder[i])) slots.push_back(i);
    if (slots.size() != occurrences.size()) return false;
    for (std::size_t i = 0; i < occurrences.size(); ++i)
        m_canonicalOrder[slots[i]] = occurrences[i];
    return true;
}

void PlaybackSchedule::PushHistory(PlaybackOccurrenceId occurrence) {
    if (!m_history.empty() && m_history.back() == occurrence) return;
    m_history.push_back(occurrence);
    TrimHistory();
}

void PlaybackSchedule::TrimHistory() {
    while (m_history.size() > kHistoryLimit)
        m_history.pop_front();
}

void PlaybackSchedule::BumpRevision() {
    ++m_revision;
}

std::vector<PlaybackOccurrenceId> PlaybackSchedule::PriorityOrder() const {
    std::vector<PlaybackOccurrenceId> out;
    for (const auto& block : m_priorityBlocks)
        out.insert(out.end(), block.begin(), block.end());
    return out;
}

std::vector<PlaybackOccurrenceId> PlaybackSchedule::PendingUpcoming() const {
    std::vector<PlaybackOccurrenceId> out;
    if (m_pendingCycle.empty()) return out;
    out.reserve(m_pendingCycle.size());
    for (const auto occurrence : m_pendingCycle)
        if (!m_current || occurrence != *m_current) out.push_back(occurrence);
    return out;
}

void PlaybackSchedule::CollapsePendingCycle() {
    if (m_pendingCycle.empty()) return;
    const auto pending = PendingUpcoming();
    std::unordered_set<PlaybackOccurrenceId> present(m_ordinaryFuture.begin(), m_ordinaryFuture.end());
    for (const auto occurrence : pending)
        if (present.insert(occurrence).second) m_ordinaryFuture.push_back(occurrence);
    m_pendingCycle.clear();
}

bool PlaybackSchedule::HasFuture() const {
    return !m_priorityBlocks.empty() || !m_ordinaryFuture.empty() || !m_pendingCycle.empty();
}

bool PlaybackSchedule::MakeRepeatCycleIfDue(double remaining_seconds) {
    if (m_repeat != PlaybackRepeatMode::All || !m_current || HasFuture() ||
        remaining_seconds > kRepeatCyclePrefetchSeconds || m_canonicalOrder.empty()) {
        return false;
    }

    m_pendingCycle = m_canonicalOrder;
    if (m_shuffle) Shuffle(m_pendingCycle);
    const auto current = std::find(m_pendingCycle.begin(), m_pendingCycle.end(), *m_current);
    if (current != m_pendingCycle.end()) std::rotate(current, current + 1, m_pendingCycle.end());
    BumpRevision();
    return true;
}

void PlaybackSchedule::Shuffle(std::vector<PlaybackOccurrenceId>& occurrences) {
    for (std::size_t i = occurrences.size(); i > 1; --i) {
        const auto at = RandomBelow(i);
        std::swap(occurrences[i - 1], occurrences[at]);
    }
}

std::size_t PlaybackSchedule::RandomBelow(std::size_t upper_bound) {
    if (upper_bound <= 1) return 0;
    const auto bound = static_cast<std::uint64_t>(upper_bound);
    const auto threshold = (std::uint64_t{0} - bound) % bound;
    std::uint64_t value;
    do {
        m_randomState += kRandomGolden;
        value = m_randomState;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        value ^= value >> 31;
    } while (value < threshold);
    return static_cast<std::size_t>(value % bound);
}

} // namespace glasswave::media
