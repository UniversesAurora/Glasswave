#pragma once

#include "cache/store.h"
#include "stream/model.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::cache::provider_snapshot {

struct NavigationTargetBinding {
    std::string key;
    std::string resourceId;
    bool operator==(const NavigationTargetBinding&) const = default;
};

// Minimal, provider-owned navigation state used for an immediate cache-first start.
// Authentication remains unknown until the provider verifies the current account.
struct Snapshot {
    std::string provider;
    std::string sourceOrigin;
    std::string context;
    std::string locale;
    std::string accountId;
    std::string accountName;
    std::string sourceStorefront;
    int displayRegion = 0;
    std::vector<stream::Playlist> created;
    std::vector<stream::Playlist> saved;
    std::vector<NavigationTargetBinding> bindings;

    bool operator==(const Snapshot&) const = default;
};

std::string Context(std::span<const std::string_view> parts);
std::optional<Snapshot> Load(Store& store, std::string accountScope, std::string_view provider,
                             std::string_view sourceOrigin, std::string_view context);
bool Save(Store& store, std::string accountScope, const Snapshot& snapshot);
void Erase(Store& store, std::string accountScope, std::string_view provider, std::string_view sourceOrigin,
           std::string_view context);

} // namespace glasswave::cache::provider_snapshot
