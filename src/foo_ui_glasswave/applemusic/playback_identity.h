#pragma once

#include "applemusic/ids.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glasswave::applemusic {

inline bool IsCatalogSongId(std::wstring_view id) {
    if (id.empty() || id.size() > 256) return false;
    return std::all_of(id.begin(), id.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
}

// The catalog asset id to try first for playback and catalog API calls: a known live
// reportingId supersedes a potentially stale original catalogId, while keeping the
// song's stable track.id (URI) intact.
inline std::wstring PreferredCatalogAssetId(std::wstring_view trackId, std::wstring_view catalogId,
                                            std::wstring_view reportingId) {
    if (IsCatalogSongId(reportingId)) return std::wstring(reportingId);
    if (!catalogId.empty()) return std::wstring(catalogId);
    return std::wstring(trackId);
}

struct LibrarySongIdentity {
    std::wstring id;
    std::wstring type;
    bool playParamsPresent = false;
    bool isLibrary = false;
    bool reporting = false;
    std::wstring playParamsId;
    std::wstring playParamsKind;
    std::wstring playParamsCatalogId;
    std::wstring playParamsReportingId;
    std::wstring embeddedCatalogId;
};

// Selects the current live catalog id reported by Apple for one library-songs item,
// skipping any ids already known to fail (e.g. a stale catalogId or an older reportingId
// after a subsequent re-release).
inline std::optional<std::wstring> SelectLibraryReplacementId(const LibrarySongIdentity& item,
                                                            std::wstring_view expectedLibraryId,
                                                            const std::set<std::wstring>& exclude) {
    if (expectedLibraryId.empty() || item.id != expectedLibraryId || item.type != L"library-songs" ||
        !item.playParamsPresent || !item.isLibrary || item.playParamsId != expectedLibraryId ||
        item.playParamsKind != L"song") {
        return std::nullopt;
    }
    const auto acceptable = [&exclude](const std::wstring& candidate) {
        return IsCatalogSongId(candidate) && !exclude.contains(candidate);
    };
    if (item.reporting && acceptable(item.playParamsReportingId)) return item.playParamsReportingId;
    if (acceptable(item.embeddedCatalogId)) return item.embeddedCatalogId;
    if (acceptable(item.playParamsCatalogId)) return item.playParamsCatalogId;
    return std::nullopt;
}

struct CatalogSongCandidate {
    std::wstring id;
    std::wstring reportingId;
    bool playable = false;
    std::wstring title;
    std::wstring album;
    std::wstring contentRating;
    int durationMs = 0;
};

struct MatchHints {
    std::wstring title;
    std::wstring album;
    std::wstring contentRating;
    int durationMs = 0;
};

namespace detail {

inline bool EqualIgnoreAsciiCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t ca = a[i];
        wchar_t cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
        if (ca != cb) return false;
    }
    return true;
}

} // namespace detail

// Ranks replacement catalog songs returned by filter[equivalents] or filter[isrc],
// preferring streamable entries matching the original track's content rating, album,
// duration and title, and excluding already-tried ids.
inline std::vector<std::wstring> RankCatalogCandidates(const std::vector<CatalogSongCandidate>& candidates,
                                                       const MatchHints& hints,
                                                       const std::set<std::wstring>& exclude) {
    struct Scored {
        std::wstring id;
        int score = 0;
        size_t index = 0;
    };
    std::vector<Scored> scored;
    scored.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        std::wstring effective =
            IsCatalogSongId(c.reportingId) && !exclude.contains(c.reportingId) ? c.reportingId : c.id;
        if (!IsCatalogSongId(effective) || exclude.contains(effective)) continue;

        int score = 0;
        if (c.playable) score += 100;
        if (!hints.contentRating.empty()) {
            if (detail::EqualIgnoreAsciiCase(c.contentRating, hints.contentRating)) score += 10;
        } else if (c.contentRating.empty()) {
            score += 2;
        }
        if (!hints.album.empty() && detail::EqualIgnoreAsciiCase(c.album, hints.album)) score += 6;
        if (hints.durationMs > 0 && c.durationMs > 0 && std::abs(hints.durationMs - c.durationMs) <= 3000)
            score += 4;
        if (!hints.title.empty() && detail::EqualIgnoreAsciiCase(c.title, hints.title)) score += 2;
        scored.push_back({std::move(effective), score, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        return a.score != b.score ? a.score > b.score : a.index < b.index;
    });

    std::vector<std::wstring> ranked;
    std::set<std::wstring> seen;
    for (auto& item : scored) {
        if (seen.insert(item.id).second) ranked.push_back(std::move(item.id));
    }
    return ranked;
}

struct SongIdentityHint {
    std::wstring trackId;
    std::wstring catalogId;
    std::wstring libraryId;
    std::wstring reportingId;
    std::wstring isrc;
    MatchHints match;
};

struct RecoveryCallbacks {
    // Returns true if assetId succeeded; false if it answered 404; throws on any other failure/abort.
    std::function<bool(const std::wstring& assetId)> tryOpen;
    // Tier 1: live /v1/me/library/songs/{libraryId}?include=catalog
    std::function<std::optional<std::wstring>(std::wstring_view libraryId,
                                              const std::set<std::wstring>& exclude)>
        fromLibrary;
    // Tier 2: live /v1/catalog/{storefront}/songs?filter[equivalents]={catalogId}
    std::function<std::vector<std::wstring>(std::wstring_view catalogId,
                                            const std::set<std::wstring>& exclude)>
        fromEquivalents;
    // Tier 3: live /v1/catalog/{storefront}/songs?filter[isrc]={isrc}
    std::function<std::vector<std::wstring>(std::wstring_view isrc, const std::set<std::wstring>& exclude)>
        fromIsrc;
};

// Recovers a playable catalog asset id after an initial 404 across up to four ordered tiers:
//   1. Live library-songs lookup (handles repeated re-releases even if reportingId changed again)
//   2. Apple Music catalog filter[equivalents] (works without libraryId or across storefront switches)
//   3. Apple Music catalog filter[isrc] (same master re-uploaded without an equivalents link)
//   4. Original catalogId fallback (when the initial 404 came from a stale cached reportingId)
// Every candidate is tried at most once.
inline std::optional<std::wstring> RecoverAfterNotFound(const SongIdentityHint& hint,
                                                        const std::wstring& failedInitialAsset,
                                                        bool allowCatalogDiscovery,
                                                        const RecoveryCallbacks& cb) {
    if (!cb.tryOpen) return std::nullopt;
    std::set<std::wstring> tried;
    if (!failedInitialAsset.empty()) tried.insert(failedInitialAsset);

    const auto tryCandidate = [&](const std::wstring& candidate) -> std::optional<std::wstring> {
        if (!IsCatalogSongId(candidate) || !tried.insert(candidate).second) return std::nullopt;
        if (cb.tryOpen(candidate)) return candidate;
        return std::nullopt;
    };

    // Tier 1: authoritative library item lookup.
    if (!hint.libraryId.empty() && cb.fromLibrary) {
        if (const auto candidate = cb.fromLibrary(hint.libraryId, tried)) {
            if (auto resolved = tryCandidate(*candidate)) return resolved;
        }
    }

    // Tier 2: catalog equivalents for the failed asset and original catalog id.
    if (allowCatalogDiscovery && cb.fromEquivalents) {
        std::vector<std::wstring> seeds;
        const auto addSeed = [&seeds](std::wstring_view id) {
            if (!IsCatalogSongId(id)) return;
            if (std::find(seeds.begin(), seeds.end(), id) == seeds.end()) seeds.emplace_back(id);
        };
        addSeed(failedInitialAsset);
        addSeed(hint.catalogId);
        addSeed(hint.trackId);

        for (const auto& seed : seeds) {
            const auto candidates = cb.fromEquivalents(seed, tried);
            size_t attempted = 0;
            for (const auto& candidate : candidates) {
                if (attempted >= 2) break;
                if (!IsCatalogSongId(candidate) || tried.contains(candidate)) continue;
                ++attempted;
                if (auto resolved = tryCandidate(candidate)) return resolved;
            }
        }
    }

    // Tier 3: ISRC lookup for the exact recording under a new catalog release.
    if (allowCatalogDiscovery && !hint.isrc.empty() && cb.fromIsrc) {
        const auto candidates = cb.fromIsrc(hint.isrc, tried);
        size_t attempted = 0;
        for (const auto& candidate : candidates) {
            if (attempted >= 2) break;
            if (!IsCatalogSongId(candidate) || tried.contains(candidate)) continue;
            ++attempted;
            if (auto resolved = tryCandidate(candidate)) return resolved;
        }
    }

    // Tier 4: original catalogId if we started from a stale cached reportingId.
    if (auto resolved = tryCandidate(hint.catalogId)) return resolved;
    return std::nullopt;
}

} // namespace glasswave::applemusic
