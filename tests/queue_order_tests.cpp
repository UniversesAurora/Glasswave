#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

#include "media/queue_order.h"
#include "stream/playback_source.h"
#include "ui/spring.h"

namespace {

size_t checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

void CheckIndices() {
    using glasswave::media::FollowingQueueIndices;
    const size_t none = std::numeric_limits<size_t>::max();
    Check(FollowingQueueIndices(0, none, true).empty(), "Empty queue");
    Check(FollowingQueueIndices(1, 0, true).empty(), "Current song belongs in the card only");
    Check(FollowingQueueIndices(5, 2, false) == std::vector<size_t>({3, 4}), "Remaining tail");
    Check(FollowingQueueIndices(5, 2, true) == std::vector<size_t>({3, 4, 0, 1}), "Repeat wrap");
    Check(FollowingQueueIndices(5, 4, false).empty(), "End without repeat");
    Check(FollowingQueueIndices(5, none, true) == std::vector<size_t>({0, 1, 2, 3, 4}), "No cursor");
    for (size_t count = 0; count < 40; ++count) {
        // Includes stale indices after deleting songs or replacing the queue.
        for (size_t playing = 0; playing <= count + 2; ++playing) {
            for (bool repeat : {false, true}) {
                const auto rows = FollowingQueueIndices(count, playing, repeat);
                const bool located = playing < count;
                const size_t expected = !located ? count : repeat ? count - 1 : count - playing - 1;
                Check(rows.size() == expected, "Unexpected row count");
                std::vector<bool> seen(count, false);
                for (size_t index : rows) {
                    Check(index < count && !seen[index], "Invalid or duplicate slot");
                    Check(!located || index != playing, "Current song duplicated below card");
                    seen[index] = true;
                }
            }
        }
    }
}

void CheckReordering() {
    using namespace glasswave::media;
    const auto wrapped = ReorderQueueSlots(5, std::vector<size_t>{3, 4, 0, 1},
                                          std::vector<size_t>{4, 0, 3, 1});
    Check(wrapped && *wrapped == std::vector<size_t>({3, 1, 2, 4, 0}), "Wrapped drag order");
    Check(!ReorderQueueSlots(3, std::vector<size_t>{0, 0}, std::vector<size_t>{0, 1}), "Duplicate slots");
    Check(!ReorderQueueSlots(3, std::vector<size_t>{0, 1}, std::vector<size_t>{0, 0}), "Duplicate items");
    Check(!ReorderQueueSlots(3, std::vector<size_t>{0, 1}, std::vector<size_t>{0, 2}), "Foreign item");
    Check(!ReorderQueueSlots(3, std::vector<size_t>{0, 3}, std::vector<size_t>{0, 3}), "Stale slot");
    Check(!ReorderQueueSlots(3, std::vector<size_t>{0}, std::vector<size_t>{}), "Mismatched sizes");
    for (size_t count = 1; count < 40; ++count) {
        for (size_t playing = 0; playing < count; ++playing) {
            for (bool repeat : {false, true}) {
                const auto slots = FollowingQueueIndices(count, playing, repeat);
                auto items = slots;
                std::reverse(items.begin(), items.end());
                const auto order = ReorderQueueSlots(count, slots, items);
                Check(order.has_value(), "Valid reorder rejected");
                Check((*order)[playing] == playing, "Reorder moved the current song");
                auto sorted = *order;
                std::sort(sorted.begin(), sorted.end());
                for (size_t i = 0; i < count; ++i) Check(sorted[i] == i, "Reorder lost a song");
                for (size_t i = 0; i < slots.size(); ++i)
                    Check((*order)[slots[i]] == items[i], "Displayed order disagrees with core order");
            }
        }
    }
}

void CheckMotion() {
    using glasswave::media::QueueRowShift;
    Check(QueueRowShift(std::vector<size_t>{3, 4, 5}, std::vector<size_t>{5}) == -2, "Forward jump");
    Check(QueueRowShift(std::vector<size_t>{3, 4, 5}, std::vector<size_t>{1, 2, 3, 4, 5}) == 2,
          "Native shuffle backward jump");
    Check(QueueRowShift(std::vector<size_t>{3, 4, 0, 1}, std::vector<size_t>{4, 0, 1, 2}) == -1,
          "Forward repeat wrap");
    Check(QueueRowShift(std::vector<size_t>{3, 4, 0, 1}, std::vector<size_t>{2, 3, 4, 0}) == 1,
          "Backward repeat wrap");
    Check(QueueRowShift(std::vector<size_t>{}, std::vector<size_t>{1, 2}) > 0, "Enter from empty tail");
    Check(QueueRowShift(std::vector<size_t>{1, 2}, std::vector<size_t>{}) < 0, "Leave at end");
    Check(QueueRowShift(std::vector<size_t>{99, 100}, std::vector<size_t>{1, 2}) == -1,
          "Whole-list replacement must have a short entrance");

    glasswave::ui::Spring before, shifted;
    before.SetTarget(100, 0.1f);
    shifted = before;
    shifted.Shift(56);
    for (int frame = 0; frame < 600; ++frame) {
        before.Step(1.0f / 120);
        shifted.Step(1.0f / 120);
        Check(std::abs(shifted.Position() - before.Position() - 56) < 0.001f,
              "Growing history changed relative spring motion");
    }
    Check(!before.Moving() && !shifted.Moving(), "Settled springs must stop requesting frames");
}

void CheckPlaybackSources() {
    using namespace glasswave::stream;
    for (const auto* url : {L"", L" ", L"\t\r\n"})
        Check(CheckSourceUrl(url) == SourceStatus::Unavailable, "Empty source must not reach HTTP");
    for (const auto* url : {L"https://cdn.example/song.flac", L"http://cdn.example/a?token=123",
                            L"HTTPS://cdn.example/song.mp3"})
        Check(CheckSourceUrl(url) == SourceStatus::Playable, "Valid CDN URL rejected");
    for (const auto* url : {L"https://", L"https:///song.mp3", L"file://song.mp3", L"ncm://song/1",
                            L"https://cdn.example/\nfile", L"not a URL"})
        Check(CheckSourceUrl(url) == SourceStatus::Failed, "Malformed source must retain error reporting");
    for (const auto before : {SourceStatus::Playable, SourceStatus::Unavailable, SourceStatus::Failed}) {
        for (const auto after : {SourceStatus::Playable, SourceStatus::Unavailable, SourceStatus::Failed}) {
            const auto combined = CombineSourceStatus(before, after);
            const auto expected = before == SourceStatus::Playable || after == SourceStatus::Playable
                                      ? SourceStatus::Playable
                                      : before == SourceStatus::Failed || after == SourceStatus::Failed
                                            ? SourceStatus::Failed
                                            : SourceStatus::Unavailable;
            Check(combined == expected, "Quality fallback lost a request failure or playable result");
        }
    }
    SourceAvailability availability;
    const auto oldEpoch = availability.Epoch();
    Check(availability.CanPlay("ncm://song/1"), "Unknown tracks must be tried");
    availability.MarkUnavailable(oldEpoch, "ncm://song/1");
    Check(!availability.CanPlay("ncm://song/1"), "Unavailable source must be skipped on native repeat");
    Check(availability.CanPlay("other://song/1"), "Provider identities must remain separate");
    availability.Reset();
    Check(availability.CanPlay("ncm://song/1"), "Explicit retry must recheck availability");
    availability.MarkUnavailable(oldEpoch, "ncm://song/1");
    Check(availability.CanPlay("ncm://song/1"), "Cancelled old worker poisoned a new playback attempt");
    availability.MarkUnavailable(availability.Epoch(), "other://song/1");
    Check(!availability.CanPlay("other://song/1"), "Shared policy must accept another provider");
}

}  // namespace

int main() {
    CheckIndices();
    CheckReordering();
    CheckMotion();
    CheckPlaybackSources();
    std::cout << checks << " queue, motion and playback source checks passed.\n";
}
