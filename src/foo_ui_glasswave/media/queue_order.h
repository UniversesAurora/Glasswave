#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace glasswave::media {

// The sidebar follows the stored playlist order, including under the core's native
// shuffle mode. The playing row is represented by the song card above the list.
inline std::vector<size_t> FollowingQueueIndices(size_t count, size_t playing, bool repeatAll) {
    const bool located = playing < count;
    const size_t first = located ? playing + 1 : 0;
    const size_t remaining = count - first;
    const size_t shown = remaining + (repeatAll && located ? playing : 0);
    std::vector<size_t> indices;
    indices.reserve(shown);
    for (size_t offset = 0; offset < shown; ++offset)
        indices.push_back(offset < remaining ? first + offset : offset - remaining);
    return indices;
}

// Align the first common playlist slot. Native shuffle can jump in either direction.
inline std::ptrdiff_t QueueRowShift(std::span<const size_t> before, std::span<const size_t> after) {
    for (size_t i = 0; i < after.size(); ++i) {
        const auto found = std::find(before.begin(), before.end(), after[i]);
        if (found != before.end())
            return static_cast<std::ptrdiff_t>(i) - std::distance(before.begin(), found);
    }
    // Replacing a whole list uses a short entrance instead of travelling its old length.
    return before.empty() ? 1 : -1;
}

// A wrapped sidebar has non-contiguous slots. Reorder only those slots and leave the
// playing song and every other playlist position in place.
inline std::optional<std::vector<size_t>> ReorderQueueSlots(size_t count, std::span<const size_t> slots,
                                                          std::span<const size_t> items) {
    if (slots.size() != items.size()) return std::nullopt;
    std::vector<bool> available(count, false);
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; ++i) order[i] = i;
    for (size_t slot : slots) {
        if (slot >= count || available[slot]) return std::nullopt;
        available[slot] = true;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i] >= count || !available[items[i]]) return std::nullopt;
        available[items[i]] = false;
        order[slots[i]] = items[i];
    }
    return order;
}

}  // namespace glasswave::media
