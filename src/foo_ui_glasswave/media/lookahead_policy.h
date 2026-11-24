#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace glasswave::media {

// Staged non-audio lookahead horizons (audio prefetch remains owned by cache::PrefetchHost at
// N+1 near track end):
//   Tier 1 (N+1 .. N+5): streaming metadata links + regional display names
//   Tier 2 (N+1 .. N+4): low-priority cover art warming (once album art key is stable)
//   Tier 3 (N+1 only):   cancellable lyrics prefetch (once display tags are settled)
inline constexpr size_t kLookaheadMetadataHorizon = 5;
inline constexpr size_t kLookaheadArtHorizon = 4;
inline constexpr size_t kLookaheadLyricsHorizon = 1;

struct LookaheadCandidate {
    std::wstring identity;          // stable deduplication key (path|subsong or provider id)
    bool isStreaming = false;       // true for provider-backed streams, false for local files
    bool itemIdentified = false;    // tags name the track (media::ItemIdentified)
    bool metadataComplete = false;  // local file or streaming track with links (!LacksLinks)
    bool albumArtKeyStable = false; // local file or streaming track with known albumId / settled links
    bool regionalSettled = true;    // false while regional display overlay is still pending
};

struct LookaheadPlan {
    std::vector<size_t> selected;         // deduplicated upcoming candidate indices (up to 5)
    std::vector<size_t> wantMetadata;     // indices needing metadata / regional display reads
    std::vector<size_t> warmArt;          // indices ready for low-priority ArtCache::Warm
    std::optional<size_t> prefetchLyrics; // index of N+1 when ready for lyrics prefetch
};

// Pure lookahead horizon planner: enforces idle = idle when playback is inactive, deduplicates
// upcoming tracks against the currently playing track and each other, and gates cover art and
// lyrics warming behind metadata / regional display readiness.
inline LookaheadPlan PlanLookahead(const std::vector<LookaheadCandidate>& candidates, bool activePlayback,
                                   std::wstring_view currentIdentity = {}) {
    LookaheadPlan plan;
    if (!activePlayback) return plan;

    std::unordered_set<std::wstring_view> seen;
    if (!currentIdentity.empty()) seen.insert(currentIdentity);

    size_t rank = 0;
    for (size_t i = 0; i < candidates.size() && rank < kLookaheadMetadataHorizon; ++i) {
        const LookaheadCandidate& c = candidates[i];
        if (c.identity.empty() || !seen.insert(c.identity).second) continue;

        plan.selected.push_back(i);
        if (c.isStreaming && (!c.metadataComplete || !c.regionalSettled)) {
            plan.wantMetadata.push_back(i);
        }
        if (rank < kLookaheadArtHorizon && c.albumArtKeyStable) {
            plan.warmArt.push_back(i);
        }
        if (rank < kLookaheadLyricsHorizon && c.itemIdentified && c.metadataComplete && c.regionalSettled) {
            plan.prefetchLyrics = i;
        }
        ++rank;
    }
    return plan;
}

} // namespace glasswave::media
