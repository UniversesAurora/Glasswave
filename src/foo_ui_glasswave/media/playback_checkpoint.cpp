#include "pch.h"
#include "media/playback_scheduler_impl.h"

// The scheduler's checkpoint: the schedule saved with the queue it belongs to (its
// fingerprint) in the configuration, and restored at start-up when the queue is the same.
namespace glasswave::media {

using namespace scheduler;

namespace {

// Identifies the queue's content so a checkpoint is applied only to the queue it was taken
// of. It was the hex of every path, which wrote megabytes into the configuration for a long
// queue on every change; a count and a 64-bit FNV-1a digest of the same length-prefixed
// identities keep the comparison and stay a few bytes. (The digest is an identity, not an
// integrity check: the checkpoint's indices are validated separately before use.)
std::string QueueFingerprint(const metadb_handle_list& items) {
    uint64_t digest = 0xcbf29ce484222325ull;
    const auto mix = [&digest](std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            digest ^= byte;
            digest *= 0x100000001b3ull;
        }
    };
    std::string part;
    for (const auto& handle : items) {
        part.clear();
        if (handle.is_empty()) {
            part = "!empty;";
        } else {
            const char* path = handle->get_path();
            const std::string_view text = path ? std::string_view(path) : std::string_view();
            part.append(std::to_string(text.size()));
            part.push_back(':');
            part.append(text);
            AppendLe64(part, handle->get_subsong_index());
            part.push_back(';');
        }
        mix(part);
    }
    std::string count;
    AppendLe64(count, items.get_count());
    std::string bytes;
    AppendLe64(bytes, digest);
    return core::HexLower(count) + core::HexLower(bytes);
}

} // namespace

void PlaybackScheduler::Impl::SaveCheckpoint(bool force) {
    if (!started || !modelInitialized || (!force && !checkpointDirty)) return;
    const t_size queue = QueuePlaylist(false);
    if (queue == SIZE_MAX) {
        if (!std::string(cfg_checkpoint.get().c_str()).empty()) cfg_checkpoint = "";
        checkpointValid = false;
        checkpointDirty = false;
        return;
    }
    metadb_handle_list items;
    playlist_manager::get()->playlist_get_all_items(queue, items);
    if (items.get_count() != physical.size()) return;

    const auto state = schedule.Snapshot();
    const auto fingerprint = QueueFingerprint(items);
    if (checkpointValid && fingerprint == savedFingerprint && SameScheduleState(state, savedState)) {
        checkpointDirty = false;
        return;
    }

    const auto physicalIndexOf = [this](PlaybackOccurrenceId occurrence, std::optional<std::size_t>& result) {
        const auto found = physicalIndex.find(occurrence);
        if (found == physicalIndex.end()) return false;
        result = found->second;
        return true;
    };
    const auto indicesOf = [&](const std::vector<PlaybackOccurrenceId>& ids,
                               std::vector<std::size_t>& result) {
        result.clear();
        result.reserve(ids.size());
        for (const auto occurrence : ids) {
            std::optional<std::size_t> index;
            if (!physicalIndexOf(occurrence, index)) return false;
            result.push_back(*index);
        }
        return true;
    };

    std::vector<std::size_t> canonical, projection, history, future, priority, pending;
    std::vector<std::size_t> prioritySizes;
    std::optional<std::size_t> currentIndex;
    if (!indicesOf(state.canonicalOrder, canonical) || !indicesOf(state.projectionOrder, projection) ||
        !indicesOf(state.history, history) || !indicesOf(state.ordinaryFuture, future) ||
        !indicesOf(state.pendingCycle, pending)) {
        return;
    }
    for (const auto& block : state.priorityBlocks) {
        std::vector<std::size_t> blockIndices;
        if (!indicesOf(block, blockIndices)) return;
        prioritySizes.push_back(blockIndices.size());
        priority.insert(priority.end(), blockIndices.begin(), blockIndices.end());
    }
    if (state.current && !physicalIndexOf(*state.current, currentIndex)) return;

    using namespace checkpoint_json;
    core::json::Object json;
    json.SetNamedValue(L"version", wdj::JsonValue::CreateNumberValue(kCheckpointVersion));
    json.SetNamedValue(L"shuffle", wdj::JsonValue::CreateBooleanValue(state.shuffle));
    json.SetNamedValue(L"repeat", wdj::JsonValue::CreateNumberValue(static_cast<int>(state.repeat)));
    json.SetNamedValue(L"fingerprint", wdj::JsonValue::CreateStringValue(core::Wide(fingerprint)));
    json.SetNamedValue(L"current", wdj::JsonValue::CreateNumberValue(
                                       currentIndex ? static_cast<double>(*currentIndex) : -1.0));
    json.SetNamedValue(L"canonical", IndexArray(canonical));
    json.SetNamedValue(L"projection", IndexArray(projection));
    json.SetNamedValue(L"history", IndexArray(history));
    json.SetNamedValue(L"future", IndexArray(future));
    json.SetNamedValue(L"priority", IndexArray(priority));
    json.SetNamedValue(L"prioritySizes", IndexArray(prioritySizes));
    json.SetNamedValue(L"pending", IndexArray(pending));
    auto text = core::Utf8(core::json::Stringify(json));
    if (text != savedCheckpoint) cfg_checkpoint = text.c_str();
    savedFingerprint = fingerprint;
    savedState = state;
    savedCheckpoint = std::move(text);
    checkpointValid = true;
    checkpointDirty = false;
}

bool PlaybackScheduler::Impl::LoadCheckpoint(const metadb_handle_list& items) {
    const std::string text(cfg_checkpoint.get().c_str());
    if (text.empty()) return false;
    // A checkpoint that does not fit the queue as it is now is dropped.
    const auto reject = [this] {
        cfg_checkpoint = "";
        checkpointValid = false;
        checkpointDirty = false;
        return false;
    };
    using namespace checkpoint_json;
    const auto parsed = core::json::Parse(text);
    if (!parsed) return reject();
    const auto& json = *parsed;
    const auto version = Integer(json, L"version");
    const auto shuffle = Boolean(json, L"shuffle");
    const auto repeat = Integer(json, L"repeat");
    const auto fingerprint = Text(json, L"fingerprint");
    const auto currentIndex = Integer(json, L"current");
    const auto canonical = Indices(json, L"canonical");
    const auto projection = Indices(json, L"projection");
    const auto history = Indices(json, L"history");
    const auto future = Indices(json, L"future");
    const auto priority = Indices(json, L"priority");
    const auto prioritySizes = Indices(json, L"prioritySizes");
    const auto pending = Indices(json, L"pending");
    if (!version || *version != kCheckpointVersion || !shuffle || !repeat || !IsValidRepeat(static_cast<int>(*repeat)) || !fingerprint ||
        !currentIndex || !canonical || !projection || !history || !future || !priority || !prioritySizes ||
        !pending || *fingerprint != QueueFingerprint(items) || canonical->size() != items.get_count() ||
        projection->size() != items.get_count()) {
        return reject();
    }
    std::vector<bool> seen(canonical->size(), false);
    for (const auto index : *canonical) {
        if (index >= seen.size() || seen[index]) {
            return reject();
        }
        seen[index] = true;
    }
    std::fill(seen.begin(), seen.end(), false);
    for (const auto index : *projection) {
        if (index >= seen.size() || seen[index]) {
            return reject();
        }
        seen[index] = true;
    }

    const auto physicalOccurrences = AllocateOccurrences(items);
    std::vector<PlaybackOccurrenceId> canonicalOccurrences;
    canonicalOccurrences.reserve(canonical->size());
    for (const auto index : *canonical)
        canonicalOccurrences.push_back(physicalOccurrences[index]);
    SetPhysical(physicalOccurrences);

    const auto idsOf = [&](const std::vector<std::size_t>& indices,
                           std::vector<PlaybackOccurrenceId>& result) {
        result.clear();
        result.reserve(indices.size());
        for (const auto index : indices) {
            if (index >= physicalOccurrences.size()) return false;
            result.push_back(physicalOccurrences[index]);
        }
        return true;
    };

    PlaybackScheduleState state;
    state.canonicalOrder = std::move(canonicalOccurrences);
    state.shuffle = *shuffle;
    state.repeat = static_cast<PlaybackRepeatMode>(*repeat);
    if (!idsOf(*projection, state.projectionOrder) || !idsOf(*history, state.history) ||
        !idsOf(*future, state.ordinaryFuture) || !idsOf(*pending, state.pendingCycle)) {
        return reject();
    }
    if (*currentIndex >= 0) {
        if (static_cast<std::size_t>(*currentIndex) >= physicalOccurrences.size()) {
            return reject();
        }
        state.current = physicalOccurrences[static_cast<std::size_t>(*currentIndex)];
    } else if (*currentIndex < -1) {
        return reject();
    }

    std::size_t priorityAt = 0;
    for (const auto size : *prioritySizes) {
        if (priorityAt + size > priority->size()) {
            return reject();
        }
        std::vector<PlaybackOccurrenceId> block;
        std::vector<std::size_t> blockIndices(priority->begin() + static_cast<std::ptrdiff_t>(priorityAt),
                                              priority->begin() +
                                                  static_cast<std::ptrdiff_t>(priorityAt + size));
        if (!idsOf(blockIndices, block)) {
            return reject();
        }
        state.priorityBlocks.push_back(std::move(block));
        priorityAt += size;
    }
    if (priorityAt != priority->size() || !schedule.RestoreSnapshot(state, NextSeed())) {
        return reject();
    }
    cfg_shuffle = state.shuffle ? 1 : 0;
    cfg_repeat = static_cast<int>(state.repeat);
    savedState = std::move(state);
    savedFingerprint = *fingerprint;
    savedCheckpoint = text;
    checkpointValid = true;
    checkpointDirty = false;
    return true;
}

} // namespace glasswave::media
