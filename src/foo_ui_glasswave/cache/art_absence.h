#pragma once
#include "cache/images.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::cache::art_absence {
// A picture the service confirmed absent is not asked for again for a day, also across
// restarts; an explicit refresh (Invalidate) asks at once.
inline constexpr int64_t kFreshSeconds = 24 * 60 * 60;
struct Entry {
    uint64_t account = 0;
    int64_t checked = 0;
    bool invalidated = false;
};
// One explicit refresh: every art key under `prefix` (a whole key segment, see ArtKeyHasPrefix)
// is asked again, whatever a record on disk or an answer still in flight says.
struct Invalidation {
    std::string prefix;
    uint64_t sequence = 0;
    int64_t at = 0; // Store::Now()
};
inline std::mutex mutex;
inline std::map<std::string, Entry> entries;
inline std::vector<Invalidation> invalidations; // oldest first, bounded
// The invalidations dropped to keep the list bounded, summed up as if they had covered every key:
// an answer asked for before the newest of them and a disk record written before it are not
// trusted. That costs one more question for such a picture and never keeps a stale answer.
inline uint64_t droppedThrough = 0; // the sequence of the newest dropped invalidation
inline int64_t droppedAt = 0;       // and its time
inline std::atomic_uint64_t generation = 0; // the sequence of the latest invalidation
// A worker captures this before it asks the service; an answer is kept only if no invalidation
// of its own key has been made since (one of another cover or service does not discard it).
inline uint64_t Epoch() {
    return generation.load();
}
// Under `mutex`.
inline bool InvalidatedSinceLocked(std::string_view artKey, uint64_t epoch) {
    if (epoch < droppedThrough) return true;
    for (const auto& item : invalidations)
        if (item.sequence > epoch && ArtKeyHasPrefix(artKey, item.prefix)) return true;
    return false;
}
inline int64_t InvalidatedAtLocked(std::string_view artKey) {
    int64_t at = droppedAt;
    for (const auto& item : invalidations)
        if (ArtKeyHasPrefix(artKey, item.prefix)) at = std::max(at, item.at);
    return at;
}
inline bool Stale(const ImageIdentity& identity, uint64_t epoch) {
    std::lock_guard lock(mutex);
    return InvalidatedSinceLocked(ImageArtKey(identity), epoch);
}
inline Key StorageKey(const ImageIdentity& identity) {
    return {Kind::Data, identity.provider, AccountScope(identity.provider),
            "art-absence/" + ImageArtKey(identity), "confirmed-missing/1"};
}
inline bool Fresh(std::string_view artKey) {
    const auto colon = artKey.find(':');
    if (colon == std::string_view::npos) return false;
    const auto account = AccountRevision(std::string(artKey.substr(0, colon)));
    const auto now = Store::Now();
    std::lock_guard lock(mutex);
    const auto found = entries.find(std::string(artKey));
    return found != entries.end() && found->second.account == account && !found->second.invalidated &&
           now >= found->second.checked && now - found->second.checked < kFreshSeconds;
}
inline bool Remember(const ImageIdentity& identity, uint64_t account, int64_t checked, uint64_t epoch) {
    if (AccountRevision(identity.provider) != account) return false;
    std::lock_guard lock(mutex);
    if (InvalidatedSinceLocked(ImageArtKey(identity), epoch)) return false;
    if (entries.size() >= 4096) {
        auto oldest = std::min_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.second.checked < b.second.checked;
        });
        if (oldest != entries.end()) entries.erase(oldest);
    }
    entries[ImageArtKey(identity)] = {account, checked, false};
    return true;
}
// Worker only. Only a successful provider metadata response may establish absence.
inline bool KnownMissing(const ImageIdentity& identity) {
    const auto key = ImageArtKey(identity);
    {
        std::lock_guard lock(mutex);
        if (entries.contains(key)) {
            // Do not restore a stale or explicitly invalidated disk entry below.
            const auto& entry = entries.at(key);
            const auto now = Store::Now();
            return entry.account == AccountRevision(identity.provider) && !entry.invalidated &&
                   now >= entry.checked && now - entry.checked < kFreshSeconds;
        }
    }
    const auto account = AccountRevision(identity.provider);
    const auto epoch = Epoch();
    try {
        const auto record = GetStore().Lookup(StorageKey(identity));
        if (!record || record->Info().extra != "confirmed-missing/1") return false;
        const auto checked = record->Info().validatedAt;
        const auto now = Store::Now();
        if (checked <= 0 || now < checked || now - checked >= kFreshSeconds) return false;
        {
            // Written before (or in the second of) an explicit refresh: the refresh asked for
            // the service's answer again, and the disk record is that stale answer.
            std::lock_guard lock(mutex);
            if (checked <= InvalidatedAtLocked(key)) return false;
        }
        return Remember(identity, account, checked, epoch);
    } catch (...) {
        return false;
    }
}
inline void StoreMissing(const ImageIdentity& identity, uint64_t account, uint64_t epoch = Epoch()) {
    if (Stale(identity, epoch)) return;
    if (AccountRevision(identity.provider) != account) return;
    const auto now = Store::Now();
    if (!Remember(identity, account, now, epoch)) return;
    try {
        auto writer = GetStore().Begin(StorageKey(identity));
        if (!writer) return;
        std::ofstream output(writer->Path(), std::ios::binary);
        output << "missing";
        output.close();
        if (!output || AccountRevision(identity.provider) != account || Stale(identity, epoch)) return;
        Metadata metadata;
        metadata.extra = "confirmed-missing/1";
        metadata.validatedAt = now;
        writer->Commit(std::move(metadata));
    } catch (...) {
    }
}
inline void Clear(const ImageIdentity& identity, uint64_t account, uint64_t epoch = Epoch()) {
    if (AccountRevision(identity.provider) != account || Stale(identity, epoch)) return;
    {
        std::lock_guard lock(mutex);
        if (auto found = entries.find(ImageArtKey(identity)); found != entries.end())
            found->second.invalidated = true;
    }
    try {
        GetStore().Erase(StorageKey(identity));
    } catch (...) {
    }
}
inline void Invalidate(std::string_view prefix) {
    std::lock_guard lock(mutex);
    invalidations.push_back({std::string(prefix), ++generation, Store::Now()});
    constexpr size_t kMaxInvalidations = 256;
    if (invalidations.size() > kMaxInvalidations) {
        const auto drop = invalidations.begin() + static_cast<std::ptrdiff_t>(invalidations.size() - kMaxInvalidations);
        for (auto it = invalidations.begin(); it != drop; ++it) {
            droppedThrough = std::max(droppedThrough, it->sequence);
            droppedAt = std::max(droppedAt, it->at);
        }
        invalidations.erase(invalidations.begin(), drop);
    }
    for (auto& [key, entry] : entries)
        if (ArtKeyHasPrefix(key, prefix)) entry.invalidated = true;
}
} // namespace glasswave::cache::art_absence
