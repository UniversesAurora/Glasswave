#pragma once
#include "cache/images.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>

namespace glasswave::cache::art_absence {
// A picture the service confirmed absent is not asked for again for a day, also across
// restarts; an explicit refresh (Invalidate) asks at once.
inline constexpr int64_t kFreshSeconds = 24 * 60 * 60;
struct Entry {
    uint64_t account = 0;
    int64_t checked = 0;
    bool invalidated = false;
};
inline std::mutex mutex;
inline std::map<std::string, Entry> entries;
inline std::atomic_uint64_t generation = 0;
inline uint64_t Epoch() {
    return generation.load();
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
    if (epoch != Epoch()) return false;
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
        return Remember(identity, account, checked, epoch);
    } catch (...) {
        return false;
    }
}
inline void StoreMissing(const ImageIdentity& identity, uint64_t account, uint64_t epoch = Epoch()) {
    if (epoch != Epoch()) return;
    if (AccountRevision(identity.provider) != account) return;
    const auto now = Store::Now();
    if (!Remember(identity, account, now, epoch)) return;
    try {
        auto writer = GetStore().Begin(StorageKey(identity));
        if (!writer) return;
        std::ofstream output(writer->Path(), std::ios::binary);
        output << "missing";
        output.close();
        if (!output || AccountRevision(identity.provider) != account || epoch != Epoch()) return;
        Metadata metadata;
        metadata.extra = "confirmed-missing/1";
        metadata.validatedAt = now;
        writer->Commit(std::move(metadata));
    } catch (...) {
    }
}
inline void Clear(const ImageIdentity& identity, uint64_t account, uint64_t epoch = Epoch()) {
    if (AccountRevision(identity.provider) != account || epoch != Epoch()) return;
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
    ++generation;
    for (auto& [key, entry] : entries)
        if (key.starts_with(prefix)) entry.invalidated = true;
}
} // namespace glasswave::cache::art_absence
