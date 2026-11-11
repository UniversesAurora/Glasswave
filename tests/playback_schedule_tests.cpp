#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <span>
#include <unordered_set>
#include <vector>

#include "media/playback_schedule.h"

namespace {

using glasswave::media::PlaybackOccurrenceId;
using glasswave::media::PlaybackRepeatMode;
using glasswave::media::PlaybackSchedule;
using Id = PlaybackOccurrenceId;

std::size_t checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

template <typename Container> std::span<const typename Container::value_type> Span(const Container& values) {
    return std::span<const typename Container::value_type>(values.data(), values.size());
}

void CheckUnique(const std::vector<Id>& values) {
    std::unordered_set<Id> seen;
    for (const auto value : values)
        Check(seen.insert(value).second, "An occurrence was duplicated");
}

void CheckResetAndMode() {
    PlaybackSchedule schedule(7);
    const std::vector<Id> imported{101, 202, 303, 404};
    schedule.ResetFresh(Span(imported), 1, 7);
    Check(schedule.Current() == 202, "Reset lost the selected current occurrence");
    Check(schedule.History() == std::vector<Id>{101}, "Only the selected prefix belongs to history");
    Check(schedule.ProjectionOrder() == imported, "Ordered playback must preserve imported order");

    Check(schedule.SetMode(true, PlaybackRepeatMode::Off), "Shuffle mode was rejected");
    const auto shuffled = schedule.ProjectionOrder();
    Check(shuffled.size() == imported.size(), "Shuffle lost an occurrence");
    Check(shuffled[0] == 101 && shuffled[1] == 202, "Shuffle moved a played or current occurrence");
    Check(std::unordered_set<Id>(shuffled.begin(), shuffled.end()) ==
              std::unordered_set<Id>(imported.begin(), imported.end()),
          "Shuffle changed occurrence identity");

    Check(schedule.SetMode(false, PlaybackRepeatMode::Off), "Ordered mode was rejected");
    Check(schedule.ProjectionOrder() == imported, "Undo shuffle did not restore original ranks");
}

void CheckFreshShuffleAndSourceGeneration() {
    PlaybackSchedule schedule(11);
    Check(schedule.SetMode(true, PlaybackRepeatMode::Off), "Shuffle mode was rejected");
    const std::vector<Id> imported{1, 2, 3, 4, 5, 6};
    schedule.ResetFresh(Span(imported), 3, 11);
    Check(schedule.History().empty(), "Fresh shuffled playback must not mark the clicked prefix as played");
    const auto first = schedule.ProjectionOrder();
    Check(first.size() == imported.size() && first.front() == 4,
          "Fresh shuffle must start at the clicked row");
    CheckUnique(first);
    for (const auto before_click : {1, 2, 3})
        Check(std::find(first.begin(), first.end(), before_click) != first.end(),
              "Fresh shuffle omitted a row before the clicked row");

    const auto old_generation = schedule.generation();
    const std::vector<Id> replacement{90, 91};
    schedule.ResetFresh(Span(replacement), 1, 12);
    Check(schedule.generation() > old_generation, "Replacement did not advance source generation");
    Check(schedule.AllOccurrenceIds() == replacement, "Replacement retained old occurrences");
    Check(schedule.ProjectionOrder().front() == 91, "Replacement did not start at its clicked row");
    for (const auto old : imported)
        Check(std::find(replacement.begin(), replacement.end(), old) == replacement.end(),
              "Old source identity survived Replace and Play");
}

void CheckDeterministicSeed() {
    const std::vector<Id> imported{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    PlaybackSchedule left(0x123456789abcdef0ULL);
    PlaybackSchedule right(0x123456789abcdef0ULL);
    PlaybackSchedule other(0xfedcba9876543210ULL);
    left.ResetFresh(Span(imported), 0, 0x123456789abcdef0ULL);
    right.ResetFresh(Span(imported), 0, 0x123456789abcdef0ULL);
    other.ResetFresh(Span(imported), 0, 0xfedcba9876543210ULL);
    Check(left.SetMode(true, PlaybackRepeatMode::All), "Shuffle mode was rejected");
    Check(right.SetMode(true, PlaybackRepeatMode::All), "Shuffle mode was rejected");
    Check(other.SetMode(true, PlaybackRepeatMode::All), "Shuffle mode was rejected");
    Check(left.ProjectionOrder() == right.ProjectionOrder(), "Equal seeds were not deterministic");
    Check(left.ProjectionOrder() != other.ProjectionOrder(), "Different seeds produced the same permutation");
}

void CheckPriorityBlocks() {
    PlaybackSchedule schedule(3);
    const std::vector<Id> imported{1, 2, 3};
    schedule.ResetFresh(Span(imported), 0, 3);
    const std::vector<Id> older{10, 11};
    const std::vector<Id> newer{12, 13};
    Check(schedule.AddPlayNext(Span(older)), "First PlayNext batch was rejected");
    Check(schedule.AddPlayNext(Span(newer)), "Second PlayNext batch was rejected");
    Check(schedule.ProjectionOrder() == std::vector<Id>({1, 12, 13, 10, 11, 2, 3}),
          "PlayNext must put the newest call first and preserve each block's input order");
    Check(schedule.Upcoming() == std::vector<Id>({12, 13, 10, 11, 2, 3}),
          "Upcoming disagrees with priority projection");
}

void CheckOrdinaryAdd() {
    PlaybackSchedule schedule(0x44);
    const std::vector<Id> imported{1, 2, 3, 4, 5, 6};
    schedule.ResetFresh(Span(imported), 1, 0x44);
    Check(schedule.SetMode(true, PlaybackRepeatMode::All), "Shuffle mode was rejected");
    const std::vector<Id> priority{90, 91};
    Check(schedule.AddPlayNext(Span(priority)), "Priority batch was rejected");
    const auto before = schedule.ProjectionOrder();
    const auto current = schedule.Current();
    const auto priority_before = schedule.FutureOrder();
    const std::vector<Id> added{70, 71, 72, 73, 74};
    Check(schedule.AddOrdinary(Span(added)), "Ordinary add was rejected");
    const auto after = schedule.ProjectionOrder();
    Check(schedule.Current() == current, "Ordinary add moved the current occurrence");
    Check(schedule.FutureOrder()[0] == priority_before[0] && schedule.FutureOrder()[1] == priority_before[1],
          "Ordinary add disturbed a PlayNext priority block");
    Check(after.size() == before.size() + added.size(), "Ordinary add lost an occurrence");
    CheckUnique(after);
    for (const auto occurrence : added)
        Check(std::find(after.begin(), after.end(), occurrence) != after.end(),
              "Ordinary add lost a new row");
}

void CheckManualReorder() {
    PlaybackSchedule schedule(5);
    const std::vector<Id> imported{1, 2, 3, 4};
    schedule.ResetFresh(Span(imported), 0, 5);
    const std::vector<Id> priority{9, 10};
    Check(schedule.AddPlayNext(Span(priority)), "Priority batch was rejected");
    const std::vector<Id> reordered{4, 3, 10, 9, 2};
    Check(schedule.ReorderFuture(Span(reordered)), "Valid manual future reorder was rejected");
    Check(schedule.FutureOrder() == reordered, "Manual future drag was not respected");
    Check(schedule.ProjectionOrder() == std::vector<Id>({1, 4, 3, 10, 9, 2}),
          "Projection disagrees with the manual future order");

    const std::vector<Id> invalid{4, 4, 3, 10, 9};
    Check(!schedule.ReorderFuture(Span(invalid)), "Duplicate manual reorder was accepted");
}

void CheckManualOverrideUpdatesCanonicalOrder() {
    PlaybackSchedule schedule(0x71);
    const std::vector<Id> imported{1, 2, 3, 4};
    schedule.ResetFresh(Span(imported), 0, 0x71);
    Check(schedule.SetMode(true, PlaybackRepeatMode::All), "Shuffle mode was rejected");
    const std::vector<Id> dragged{4, 2, 3};
    Check(schedule.ReorderFuture(Span(dragged)), "Shuffled future drag was rejected");
    Check(schedule.FutureOrder() == dragged, "Dragged shuffled future was not preserved");
    Check(schedule.SetMode(false, PlaybackRepeatMode::All), "Disabling shuffle was rejected");
    Check(schedule.FutureOrder() == dragged,
          "Disabling shuffle silently undid the user's ordered future drag");
    Check(schedule.CanonicalOrder() == std::vector<Id>({1, 4, 2, 3}),
          "Visible future drag did not update canonical slots");
}

void CheckRestoredCanonicalOrder() {
    PlaybackSchedule original(0x72);
    const std::vector<Id> imported{1, 2, 3, 4, 5};
    original.ResetFresh(Span(imported), 0, 0x72);
    Check(original.SetMode(true, PlaybackRepeatMode::All), "Shuffle mode was rejected");
    const std::vector<Id> dragged{5, 3, 4, 2};
    Check(original.ReorderFuture(Span(dragged)), "Shuffle drag was rejected");
    const auto canonical = original.CanonicalOrder();
    const auto physical = original.ProjectionOrder();
    const auto current = original.Current();
    const auto currentAt = std::find(physical.begin(), physical.end(), *current);

    PlaybackSchedule restored(0x73);
    Check(restored.SetMode(true, PlaybackRepeatMode::All), "Restored shuffle mode was rejected");
    restored.Restore(Span(canonical), Span(physical),
                     static_cast<std::size_t>(std::distance(physical.begin(), currentAt)), 0x73);
    Check(restored.CanonicalOrder() == canonical, "Restart lost canonical occurrence order");
    Check(restored.SetMode(false, PlaybackRepeatMode::All), "Restored shuffle could not be disabled");
    Check(restored.FutureOrder() == dragged,
          "Shuffle OFF after restart did not restore the dragged natural order");
}

void CheckStreamingSourceAppend() {
    PlaybackSchedule schedule(0x74);
    Check(schedule.SetMode(true, PlaybackRepeatMode::Off), "Paged source shuffle was rejected");
    const std::vector<Id> first_page{1, 2, 3};
    schedule.ResetFresh(Span(first_page), 1, 0x74);
    const auto generation = schedule.generation();
    const std::vector<Id> later_pages{4, 5, 6};
    Check(schedule.AddOrdinary(Span(later_pages)), "Later source page was rejected");
    Check(schedule.generation() == generation, "Normal source append changed content generation");
    Check(schedule.CanonicalOrder() == std::vector<Id>({1, 2, 3, 4, 5, 6}),
          "Later pages were not appended at natural canonical positions");
    CheckUnique(schedule.ProjectionOrder());

    const std::vector<Id> replacement{9, 10};
    schedule.ResetFresh(Span(replacement), 0, 0x75);
    Check(schedule.generation() > generation, "Replacement did not reset the paged source token");
    Check(schedule.AllOccurrenceIds() == replacement, "Replacement retained paged source rows");
}

void CheckScheduleRoleRoundTrip() {
    PlaybackSchedule schedule(0x75);
    const std::vector<Id> imported{1, 2, 3, 4, 5, 6, 7, 8};
    schedule.ResetFresh(Span(imported), 2, 0x75);
    Check(schedule.SetMode(true, PlaybackRepeatMode::All), "Initial shuffle was rejected");
    Check(schedule.Started(*schedule.Next()), "First role-changing step failed");
    Check(schedule.Started(*schedule.Next()), "Second role-changing step failed");
    Check(schedule.SetMode(true, PlaybackRepeatMode::One), "Repeat-one for role round-trip failed");
    const auto repeatOneProjection = schedule.ProjectionOrder();
    Check(!repeatOneProjection.empty() && repeatOneProjection.back() == *schedule.Current(),
          "Repeat-one setup did not place future before current");
    const std::vector<Id> priority{90, 91};
    Check(schedule.AddPlayNext(Span(priority)), "Priority for role round-trip failed");

    const auto saved = schedule.Snapshot();
    const auto future = schedule.FutureOrder();
    Check(saved.priorityBlocks.size() == 1 && saved.priorityBlocks.front() == priority,
          "Priority blocks were not captured");

    PlaybackSchedule restored(0x76);
    Check(restored.RestoreSnapshot(saved, 0x76), "Full schedule role snapshot was rejected");
    const auto roundTrip = restored.Snapshot();
    Check(roundTrip.canonicalOrder == saved.canonicalOrder, "Round-trip canonical order changed");
    Check(roundTrip.projectionOrder == saved.projectionOrder, "Round-trip projection order changed");
    Check(roundTrip.history == saved.history, "Round-trip history changed");
    Check(roundTrip.current == saved.current, "Round-trip current changed");
    Check(roundTrip.priorityBlocks == saved.priorityBlocks, "Round-trip priority blocks changed");
    Check(roundTrip.ordinaryFuture == saved.ordinaryFuture, "Round-trip ordinary future changed");
    Check(roundTrip.pendingCycle == saved.pendingCycle, "Round-trip pending cycle changed");
    Check(roundTrip.shuffle == saved.shuffle && roundTrip.repeat == saved.repeat, "Round-trip modes changed");
    Check(restored.FutureOrder() == future, "Round-trip future order changed");

    auto invalid = saved;
    invalid.projectionOrder.pop_back();
    Check(!restored.RestoreSnapshot(invalid, 0x77), "Invalid projection order was restored");

    Check(restored.SetMode(false, PlaybackRepeatMode::One), "Shuffle OFF after restore failed");
    const auto restoredFuture = restored.FutureOrder();
    for (const auto past : roundTrip.history)
        Check(std::find(restoredFuture.begin(), restoredFuture.end(), past) == restoredFuture.end(),
              "Shuffle OFF after restore replayed a historical occurrence");
}

void CheckNavigationAndFailure() {
    PlaybackSchedule schedule(6);
    const std::vector<Id> imported{1, 2, 3, 4};
    schedule.ResetFresh(Span(imported), 1, 6);
    Check(schedule.Started(4), "Forward jump was rejected");
    Check(schedule.Current() == 4, "Forward jump did not change current");
    Check(schedule.History() == std::vector<Id>({1, 2}), "Forward jump history is wrong");
    Check(schedule.Previous() == 2, "Previous must use actual navigation history");
    Check(schedule.Started(2, true), "Backward navigation was rejected");
    Check(schedule.Current() == 2 && schedule.Previous() == 1, "Backward navigation history is wrong");
    Check(schedule.SkipFailedCurrent() == 4, "Failure fallback ignored the restored forward occurrence");
    Check(schedule.Current() == 4, "Failure fallback did not advance current");
    Check(schedule.Next() == 3, "Failure fallback did not retain the remaining future order");
}

void CheckPreviousRestoresForwardFuture() {
    PlaybackSchedule schedule(0x67);
    const std::vector<Id> imported{1, 2, 3, 4};
    schedule.ResetFresh(Span(imported), 0, 0x67);
    Check(schedule.Started(*schedule.Next()), "ABC forward B failed");
    Check(schedule.Started(*schedule.Next()), "ABC forward C failed");
    Check(schedule.Started(2, true), "ABC previous B failed");
    Check(schedule.FutureOrder() == std::vector<Id>({3, 4}), "Previous B did not restore C before D");
    Check(schedule.Started(*schedule.Next()), "ABC next after previous failed");
    Check(schedule.Current() == 3, "Next after Previous B did not return to C");

    PlaybackSchedule twice(0x68);
    twice.ResetFresh(Span(imported), 0, 0x68);
    Check(twice.Started(*twice.Next()), "Twice forward B failed");
    Check(twice.Started(*twice.Next()), "Twice forward C failed");
    Check(twice.Started(2, true), "Twice previous B failed");
    Check(twice.Started(1, true), "Twice previous A failed");
    Check(twice.FutureOrder() == std::vector<Id>({2, 3, 4}),
          "Two Previous steps did not preserve withdrawn B then C/D");
    Check(twice.Started(*twice.Next()), "Twice next after previous failed");
    Check(twice.Current() == 2, "Two Previous steps did not return Next to B");

    PlaybackSchedule priority(0x69);
    priority.ResetFresh(Span(imported), 0, 0x69);
    Check(priority.Started(*priority.Next()), "Priority forward B failed");
    Check(priority.Started(*priority.Next()), "Priority forward C failed");
    const std::vector<Id> playNext{90};
    Check(priority.AddPlayNext(Span(playNext)), "Priority insert failed");
    Check(priority.Started(2, true), "Priority previous B failed");
    Check(priority.Next() == 90, "Previous navigation displaced a priority PlayNext item");
    Check(priority.Started(*priority.Next()), "Priority item did not start after previous");
    Check(priority.Next() == 3, "Restored C was not next after the priority item");
}

void CheckTrimmedHistoryStaysBeforeCurrent() {
    PlaybackSchedule schedule(0x70);
    std::vector<Id> imported(1500);
    for (std::size_t i = 0; i < imported.size(); ++i)
        imported[i] = i + 1;
    schedule.ResetFresh(Span(imported), 0, 0x70);
    for (std::size_t i = 0; i < 1200; ++i) {
        const auto next = schedule.Next();
        Check(next.has_value(), "Trimmed-history progression ran out of future");
        Check(schedule.Started(*next), "Trimmed-history progression failed");
    }

    const auto state = schedule.Snapshot();
    Check(state.history.size() == PlaybackSchedule::kHistoryLimit,
          "Trimmed-history test did not reach the history bound");
    const auto projection = schedule.ProjectionOrder();
    Check(projection.size() == imported.size(), "Projection omitted a known occurrence");
    CheckUnique(projection);
    const auto current = std::find(projection.begin(), projection.end(), *state.current);
    Check(current != projection.end(), "Projection omitted current");

    std::unordered_set<Id> future;
    for (const auto& block : state.priorityBlocks)
        future.insert(block.begin(), block.end());
    future.insert(state.ordinaryFuture.begin(), state.ordinaryFuture.end());
    future.insert(state.pendingCycle.begin(), state.pendingCycle.end());
    for (auto it = std::next(current); it != projection.end(); ++it)
        Check(future.contains(*it), "An inactive trimmed occurrence appeared after current");

    std::unordered_set<Id> represented(state.history.begin(), state.history.end());
    represented.insert(*state.current);
    represented.insert(future.begin(), future.end());
    std::size_t inactiveBefore = 0;
    for (auto it = projection.begin(); it != current; ++it)
        if (!represented.contains(*it)) ++inactiveBefore;
    Check(inactiveBefore > 0, "Trimmed-history test produced no inactive canonical prefix");

    Check(schedule.SetMode(false, PlaybackRepeatMode::Off), "Disabling shuffle after trim failed");
    const auto unplayed = schedule.FutureOrder();
    for (const auto past : state.history)
        Check(std::find(unplayed.begin(), unplayed.end(), past) == unplayed.end(),
              "Disabling shuffle replayed retained history");
    for (const auto occurrence : projection)
        if (!represented.contains(occurrence))
            Check(std::find(unplayed.begin(), unplayed.end(), occurrence) == unplayed.end(),
                  "Disabling shuffle replayed an inactive trimmed occurrence");
}

void CheckRepeatOnePriority() {
    PlaybackSchedule schedule(8);
    const std::vector<Id> imported{1, 2, 3};
    schedule.ResetFresh(Span(imported), 0, 8);
    Check(schedule.SetMode(false, PlaybackRepeatMode::One), "Repeat-one was rejected");
    Check(schedule.Upcoming() == std::vector<Id>{1}, "Repeat-one must normally repeat current");
    Check(schedule.ProjectionOrder() == std::vector<Id>({2, 3, 1}),
          "Repeat-one projection must let core Default stop at current");

    const std::vector<Id> priority{9};
    Check(schedule.AddPlayNext(Span(priority)), "Repeat-one PlayNext was rejected");
    Check(schedule.Upcoming() == std::vector<Id>({9, 1}), "Repeat-one priority must override current");
    Check(schedule.Started(9), "Repeat-one priority did not start");
    Check(schedule.Upcoming() == std::vector<Id>{9}, "Repeat-one did not resume after the priority row");
}

void CheckDuplicateOccurrencesAndRemoval() {
    PlaybackSchedule schedule(0x30);
    const std::vector<Id> imported{10, 11, 20, 21};
    schedule.ResetFresh(Span(imported), 1, 0x30);
    Check(schedule.SetMode(true, PlaybackRepeatMode::All), "Shuffle mode was rejected");
    const auto projection = schedule.ProjectionOrder();
    Check(std::find(projection.begin(), projection.end(), 10) != projection.end(),
          "First duplicate-handle occurrence was lost");
    Check(std::find(projection.begin(), projection.end(), 11) != projection.end(),
          "Second duplicate-handle occurrence was lost");

    const std::vector<Id> remove_current{11};
    Check(schedule.Remove(Span(remove_current)), "Removing current duplicate occurrence failed");
    Check(!schedule.Current().has_value(), "Removing current did not clear current");
    Check(schedule.AllOccurrenceIds() == std::vector<Id>({10, 20, 21}),
          "Removing one duplicate occurrence removed another");
    const std::vector<Id> priority{30};
    Check(schedule.AddPlayNext(Span(priority)), "PlayNext after deleting current failed");
    Check(schedule.Next() == 30, "New priority was not next after deleting current");
    CheckUnique(schedule.ProjectionOrder());
}

void CheckPhysicalRemovalMaskWithDuplicateOccurrences() {
    using glasswave::media::ApplyPhysicalRemovalMask;
    const std::vector<Id> physical{10, 11, 10, 12, 11};
    const std::vector<std::uint8_t> removed{0, 1, 1, 0, 0};
    const auto compacted = ApplyPhysicalRemovalMask(Span(physical), Span(removed));
    Check(compacted == std::vector<Id>({10, 12, 11}),
          "Old-index removal mask changed occurrence order or removed the wrong duplicate");
}

void CheckRepeatCyclePrefetch() {
    PlaybackSchedule schedule(0xabc);
    const std::vector<Id> imported{1, 2, 3};
    schedule.ResetFresh(Span(imported), 2, 0xabc);
    Check(schedule.SetMode(true, PlaybackRepeatMode::All), "Repeat-all shuffle was rejected");
    const auto revision = schedule.revision();
    Check(!schedule.Tick(31.0), "Repeat cycle was materialized too early");
    Check(schedule.revision() == revision, "Early tick changed schedule");
    Check(schedule.Tick(30.0), "Repeat cycle was not precomputed at the 30 second boundary");
    const auto next = schedule.Next();
    Check(next.has_value() && *next != 3, "Repeat cycle did not choose a new first occurrence");
    const auto upcoming = schedule.Upcoming();
    Check(upcoming.size() == imported.size(), "Upcoming lost repeat-cycle rows");
    Check(upcoming.back() == 3, "Repeat cycle must preserve current until the cycle closes");
    const auto after_prefetch = schedule.revision();
    Check(!schedule.Tick(0.0), "Repeat cycle was generated twice");
    Check(schedule.revision() == after_prefetch, "Prefetched cycle changed after reaching end");
}

void CheckSingleRowRepeatAll() {
    for (const bool shuffle : {false, true}) {
        PlaybackSchedule schedule(shuffle ? 0x881 : 0x880);
        const std::vector<Id> imported{42};
        schedule.ResetFresh(Span(imported), 0, shuffle ? 0x881 : 0x880);
        Check(schedule.SetMode(shuffle, PlaybackRepeatMode::All), "Single-row repeat-all mode was rejected");
        Check(schedule.Tick(30.0), "Single-row repeat-all did not prepare its cycle");
        Check(schedule.Snapshot().pendingCycle == imported,
              "Single-row repeat-all prepared the wrong pending set");
        Check(schedule.ProjectionOrder() == imported,
              "Single-row repeat-all duplicated its physical projection");

        for (std::size_t repeat = 0; repeat < 4; ++repeat) {
            Check(schedule.Next() == 42, "Single-row repeat-all lost its automatic next row");
            Check(schedule.ManualNext() == 42, "Single-row repeat-all lost its manual next row");
            Check(schedule.Upcoming() == imported,
                  "Single-row repeat-all Upcoming must expose exactly one row");
            Check(schedule.Started(42), "Single-row repeat-all EOF restart failed");
        }
        Check(schedule.AllOccurrenceIds() == imported,
              "Single-row repeat-all appended a synthetic occurrence");
        CheckUnique(schedule.ProjectionOrder());
    }
}

void CheckBoundedRepeatCycles() {
    PlaybackSchedule schedule(0x55);
    const std::vector<Id> imported{1, 2, 3, 4, 5};
    schedule.ResetFresh(Span(imported), 0, 0x55);
    Check(schedule.SetMode(true, PlaybackRepeatMode::All), "Repeat-all shuffle was rejected");

    for (std::size_t cycle = 0; cycle < 1200; ++cycle) {
        while (const auto next = schedule.ManualNext())
            Check(schedule.Started(*next), "Cycle step failed");
        Check(schedule.Current().has_value(), "Cycle lost current");
        Check(schedule.Tick(0.0), "A new cycle was not prepared at the boundary");
        const auto next = schedule.Next();
        Check(next.has_value(), "Prepared cycle has no next row");
        Check(schedule.Started(*next), "Prepared cycle did not start");
        Check(schedule.AllOccurrenceIds().size() == imported.size(), "Repeat cycle appended occurrences");
        Check(schedule.History().size() <= PlaybackSchedule::kHistoryLimit,
              "History memory grew without bound");
        CheckUnique(schedule.ProjectionOrder());
    }
}

void CheckAdoptPhysicalOrder() {
    PlaybackSchedule schedule(9);
    const std::vector<Id> imported{1, 2, 3, 4};
    schedule.ResetFresh(Span(imported), 1, 9);
    const std::vector<Id> physical{3, 1, 2, 4};
    Check(schedule.AdoptPhysicalOrder(Span(physical), 2), "Physical reorder was rejected");
    Check(schedule.Current() == 2, "Physical reorder lost current");
    Check(schedule.History() == std::vector<Id>({3, 1}), "Physical prefix did not become history");
    Check(schedule.FutureOrder() == std::vector<Id>{4}, "Physical suffix did not become future");

    const std::vector<Id> remove{1};
    Check(schedule.Remove(Span(remove)), "Removal was rejected");
    Check(schedule.AllOccurrenceIds() == std::vector<Id>({2, 3, 4}),
          "Removal did not preserve occurrence identity");
    CheckUnique(schedule.ProjectionOrder());
}

void CheckExplicitStartPreservesProjectedOrder() {
    const std::vector<Id> imported{1, 2, 3, 4, 5, 6, 7};
    for (const bool shuffle : {false, true}) {
        PlaybackSchedule schedule(shuffle ? 0x902 : 0x901);
        schedule.ResetFresh(Span(imported), 1, shuffle ? 0x902 : 0x901);
        Check(schedule.SetMode(shuffle, PlaybackRepeatMode::Off), "Explicit-start shuffle mode was rejected");
        const auto physical = schedule.ProjectionOrder();
        const auto canonical = schedule.CanonicalOrder();
        constexpr std::size_t selected = 4;
        Check(schedule.StartExplicit(Span(physical), selected), "Explicit physical start was rejected");
        Check(schedule.Current() == physical[selected],
              "Explicit physical start selected the wrong occurrence");
        Check(schedule.ProjectionOrder() == physical,
              "Explicit start moved the selected row out of physical order");
        Check(schedule.CanonicalOrder() == canonical, "Explicit start rewrote canonical order");
        const auto selectedIndex = static_cast<std::size_t>(
            std::find(physical.begin(), physical.end(), *schedule.Current()) - physical.begin());
        Check(schedule.History() == std::vector<Id>(physical.begin(), physical.begin() + selected),
              "Explicit start did not preserve the physical prefix as history");
        Check(schedule.FutureOrder() == std::vector<Id>(physical.begin() + selected + 1, physical.end()),
              "Explicit start did not preserve the physical suffix as future");

        const auto selectedOccurrence = *schedule.Current();
        Check(schedule.SetMode(false, PlaybackRepeatMode::Off),
              "Disabling shuffle after explicit start was rejected");
        Check(schedule.Current() == selectedOccurrence,
              "Disabling shuffle moved the explicitly selected occurrence");
        const auto projectionAfterDisable = schedule.ProjectionOrder();
        Check(std::find(projectionAfterDisable.begin(), projectionAfterDisable.end(), selectedOccurrence) -
                      projectionAfterDisable.begin() ==
                  static_cast<std::ptrdiff_t>(selectedIndex),
              "Disabling shuffle moved the explicitly selected row");
        Check(schedule.CanonicalOrder() == canonical,
              "Shuffle toggle after explicit start rewrote canonical order");
        Check(schedule.SetMode(true, PlaybackRepeatMode::Off),
              "Re-enabling shuffle after explicit start was rejected");
        const auto projectionAfterEnable = schedule.ProjectionOrder();
        Check(std::find(projectionAfterEnable.begin(), projectionAfterEnable.end(), selectedOccurrence) -
                      projectionAfterEnable.begin() ==
                  static_cast<std::ptrdiff_t>(selectedIndex),
              "Re-enabling shuffle moved the explicitly selected row");
    }
}

void CheckExplicitFailureSkipKeepsRowsInPlace() {
    PlaybackSchedule schedule(0x903);
    const std::vector<Id> imported{11, 12, 13, 14, 15, 16};
    schedule.ResetFresh(Span(imported), 0, 0x903);
    const auto physical = schedule.ProjectionOrder();
    Check(schedule.StartExplicit(Span(physical), 3), "Explicit failed-start setup was rejected");
    const auto failed = *schedule.Current();
    const auto next = schedule.SkipFailedCurrent();
    Check(next == physical[4], "Failed explicit start did not advance to the next physical row");
    Check(schedule.StartExplicit(Span(physical), 4), "Skip after failed explicit start was rejected");
    Check(schedule.ProjectionOrder() == physical, "Failed start or its automatic skip moved physical rows");
    Check(std::find(schedule.History().begin(), schedule.History().end(), failed) != schedule.History().end(),
          "Failed explicit row disappeared from its physical place");
}

void CheckExplicitFailureSkipPreservesPlayNext() {
    PlaybackSchedule schedule(0x907);
    const std::vector<Id> imported{51, 52, 53, 54, 55, 56};
    schedule.ResetFresh(Span(imported), 0, 0x907);
    const std::vector<Id> priority{90};
    Check(schedule.AddPlayNext(Span(priority)), "Failure-skip priority setup was rejected");
    const auto physical = schedule.ProjectionOrder();
    const auto selectedAt = std::find(physical.begin(), physical.end(), 54);
    Check(selectedAt != physical.end(), "Failure-skip fixture lost its selected row");
    const auto position = static_cast<std::size_t>(selectedAt - physical.begin());
    Check(schedule.StartExplicit(Span(physical), position), "Failure-skip explicit start was rejected");
    Check(schedule.Next() == 90, "Failure-skip setup lost its PlayNext occurrence");
    Check(schedule.SkipFailedCurrent() == 90, "Failure skip displaced the PlayNext occurrence");
    Check(schedule.Started(90), "Failure skip could not start the preserved PlayNext occurrence");
    Check(schedule.Next() == 55, "Failure skip did not resume after the selected physical row");
    Check(schedule.ProjectionOrder() == physical, "Failure skip or its PlayNext resume moved visible rows");
}

void CheckExplicitStartPreservesPlayNextPriority() {
    PlaybackSchedule schedule(0x904);
    const std::vector<Id> imported{21, 22, 23, 24, 25};
    schedule.ResetFresh(Span(imported), 1, 0x904);
    const std::vector<Id> priority{90, 91};
    Check(schedule.AddPlayNext(Span(priority)), "Explicit-start priority setup was rejected");
    const auto physical = schedule.ProjectionOrder();
    const auto selectedAt = std::find(physical.begin(), physical.end(), 24);
    Check(selectedAt != physical.end(), "Explicit-start fixture lost its selected row");
    const auto position = static_cast<std::size_t>(selectedAt - physical.begin());
    Check(schedule.StartExplicit(Span(physical), position),
          "Explicit start with temporary priority was rejected");
    Check(schedule.ProjectionOrder() == physical,
          "Temporary PlayNext priority rearranged an explicit selection");
    Check(schedule.Current() == 24, "Explicit start selected the wrong occurrence with PlayNext queued");
    Check(schedule.Next() == 90, "Explicit start displaced a user PlayNext occurrence");
    Check(schedule.FutureOrder().size() >= 2 && schedule.FutureOrder()[0] == 90 &&
              schedule.FutureOrder()[1] == 91,
          "Explicit start changed the queued PlayNext block order");
    Check(schedule.SetMode(true, PlaybackRepeatMode::Off), "Enabling shuffle with preserved PlayNext failed");
    auto toggledProjection = schedule.ProjectionOrder();
    Check(std::find(toggledProjection.begin(), toggledProjection.end(), 24) - toggledProjection.begin() ==
                  static_cast<std::ptrdiff_t>(position) &&
              schedule.Next() == 90,
          "Shuffle toggle moved the selected row or displaced PlayNext");
    Check(schedule.SetMode(false, PlaybackRepeatMode::Off),
          "Disabling shuffle with preserved PlayNext failed");
    toggledProjection = schedule.ProjectionOrder();
    Check(std::find(toggledProjection.begin(), toggledProjection.end(), 24) - toggledProjection.begin() ==
                  static_cast<std::ptrdiff_t>(position) &&
              schedule.Next() == 90,
          "Shuffle-off moved the selected row or displaced PlayNext");
    const std::vector<Id> added{92};
    Check(schedule.AddOrdinary(Span(added)), "Paged append after explicit start failed");
    toggledProjection = schedule.ProjectionOrder();
    Check(std::find(toggledProjection.begin(), toggledProjection.end(), 24) - toggledProjection.begin() ==
                  static_cast<std::ptrdiff_t>(position) &&
              std::find(toggledProjection.begin(), toggledProjection.end(), 92) != toggledProjection.end() &&
              schedule.Next() == 90,
          "Paged append moved the selected row or displaced PlayNext");
    const auto projectionBeforePriorityStart = schedule.ProjectionOrder();
    Check(schedule.Started(90), "Starting the preserved PlayNext occurrence failed");
    Check(schedule.Next() == 91, "Starting one PlayNext occurrence skipped the rest of its block");
    Check(schedule.ProjectionOrder() == projectionBeforePriorityStart,
          "Playing a preserved PlayNext occurrence moved visible rows");
}

void CheckExplicitStartConsumesOnlySelectedPriorityOccurrence() {
    PlaybackSchedule schedule(0x906);
    const std::vector<Id> imported{41, 42, 43, 44};
    schedule.ResetFresh(Span(imported), 1, 0x906);
    const std::vector<Id> priority{90, 91};
    Check(schedule.AddPlayNext(Span(priority)), "Selected-priority setup was rejected");
    const auto physical = schedule.ProjectionOrder();
    const auto selectedAt = std::find(physical.begin(), physical.end(), 90);
    Check(selectedAt != physical.end(), "Selected-priority fixture lost the selected row");
    const auto position = static_cast<std::size_t>(selectedAt - physical.begin());
    Check(schedule.StartExplicit(Span(physical), position),
          "Explicit start on a PlayNext occurrence was rejected");
    Check(schedule.Current() == 90, "Explicit start did not select the priority occurrence");
    Check(schedule.Next() == 91, "Selecting one priority occurrence consumed another");
    Check(schedule.ProjectionOrder() == physical, "Selecting a priority occurrence moved visible rows");
    Check(schedule.Started(91), "Starting the remaining priority occurrence failed");
    Check(schedule.Current() == 91 && schedule.Next() == 43,
          "Priority consumption did not resume the ordinary future");
    Check(schedule.ProjectionOrder() == physical,
          "Starting the remaining priority occurrence moved visible rows");
}

void CheckExplicitStartAndManualReorderKeepCurrent() {
    PlaybackSchedule schedule(0x905);
    // Occurrence ids, not handles, define identity: repeated tracks remain distinct rows.
    const std::vector<Id> imported{31, 32, 33, 34, 35, 36};
    schedule.ResetFresh(Span(imported), 0, 0x905);
    Check(schedule.SetMode(true, PlaybackRepeatMode::Off),
          "Manual-reorder explicit-start shuffle was rejected");
    const auto physical = schedule.ProjectionOrder();
    Check(schedule.StartExplicit(Span(physical), 3), "Manual-reorder explicit-start setup was rejected");
    const auto current = *schedule.Current();
    auto future = schedule.FutureOrder();
    std::reverse(future.begin(), future.end());
    Check(schedule.ReorderFuture(Span(future)), "Manual future reorder after explicit start was rejected");
    Check(schedule.Current() == current, "Manual future reorder moved the explicitly selected occurrence");
    CheckUnique(schedule.ProjectionOrder());
    Check(schedule.ProjectionOrder().size() == imported.size(), "Manual future reorder lost an occurrence");
}

void CheckTogglePerformance() {
    std::vector<Id> imported(64);
    for (std::size_t i = 0; i < imported.size(); ++i)
        imported[i] = i + 1;
    PlaybackSchedule schedule(0x99);
    schedule.ResetFresh(Span(imported), 0, 0x99);
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < 100000; ++i)
        Check(schedule.SetMode((i & 1) == 0, PlaybackRepeatMode::All), "Toggle benchmark rejected a mode");
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
    std::cout << "100k toggle ms: " << elapsed.count() << '\n';
    Check(elapsed < std::chrono::seconds(10), "One hundred thousand shuffle toggles exceeded ten seconds");
    Check(schedule.CanonicalOrder() == imported, "Repeated shuffle toggles damaged canonical order");
}

void CheckBatchAddAndRemovePerformance() {
    std::vector<Id> imported(100000);
    for (std::size_t i = 0; i < imported.size(); ++i)
        imported[i] = i + 1;
    PlaybackSchedule schedule(0xa1);
    schedule.ResetFresh(Span(imported), 0, 0xa1);
    Check(schedule.SetMode(true, PlaybackRepeatMode::All), "Batch-add shuffle was rejected");
    const auto oldFuture = schedule.FutureOrder();

    std::vector<Id> batch(100000);
    for (std::size_t i = 0; i < batch.size(); ++i)
        batch[i] = imported.size() + i + 1;
    const auto addBegin = std::chrono::steady_clock::now();
    Check(schedule.AddOrdinary(Span(batch)), "One hundred thousand row add failed");
    const auto addElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - addBegin);
    std::cout << "100k batch add ms: " << addElapsed.count() << '\n';
    Check(addElapsed < std::chrono::seconds(5), "One hundred thousand row add exceeded five seconds");
    const auto merged = schedule.FutureOrder();
    Check(merged.size() == oldFuture.size() + batch.size(), "Batch add lost an occurrence");
    std::size_t oldAt = 0;
    for (const auto occurrence : merged)
        if (oldAt < oldFuture.size() && occurrence == oldFuture[oldAt]) ++oldAt;
    Check(oldAt == oldFuture.size(), "Batch add changed the relative order of ordinary future rows");

    const auto removeBegin = std::chrono::steady_clock::now();
    std::vector<Id> remove;
    remove.reserve(imported.size() / 2);
    for (std::size_t i = 1; i < imported.size(); i += 2)
        remove.push_back(imported[i]);
    Check(schedule.Remove(Span(remove)), "One hundred thousand mask removal failed");
    const auto removeElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - removeBegin);
    std::cout << "50k remove ms: " << removeElapsed.count() << '\n';
    Check(removeElapsed < std::chrono::seconds(5), "Fifty thousand row removal exceeded five seconds");
    Check(schedule.AllOccurrenceIds().size() == 150000, "Batch removal lost the wrong occurrence count");
}

} // namespace

int main() {
    CheckResetAndMode();
    CheckFreshShuffleAndSourceGeneration();
    CheckDeterministicSeed();
    CheckPriorityBlocks();
    CheckOrdinaryAdd();
    CheckManualReorder();
    CheckManualOverrideUpdatesCanonicalOrder();
    CheckRestoredCanonicalOrder();
    CheckStreamingSourceAppend();
    CheckScheduleRoleRoundTrip();
    CheckNavigationAndFailure();
    CheckPreviousRestoresForwardFuture();
    CheckTrimmedHistoryStaysBeforeCurrent();
    CheckRepeatOnePriority();
    CheckDuplicateOccurrencesAndRemoval();
    CheckPhysicalRemovalMaskWithDuplicateOccurrences();
    CheckRepeatCyclePrefetch();
    CheckSingleRowRepeatAll();
    CheckBoundedRepeatCycles();
    CheckAdoptPhysicalOrder();
    CheckExplicitStartPreservesProjectedOrder();
    CheckExplicitFailureSkipKeepsRowsInPlace();
    CheckExplicitFailureSkipPreservesPlayNext();
    CheckExplicitStartPreservesPlayNextPriority();
    CheckExplicitStartConsumesOnlySelectedPriorityOccurrence();
    CheckExplicitStartAndManualReorderKeepCurrent();
    CheckTogglePerformance();
    CheckBatchAddAndRemovePerformance();
    std::cout << checks << " playback schedule checks passed.\n";
}
