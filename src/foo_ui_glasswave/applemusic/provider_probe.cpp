#include "pch.h"

#include "applemusic/catalog.h"
#include "core/paths.h"
#include "applemusic/client.h"
#include "applemusic/config.h"
#include "applemusic/display_metadata.h"
#include "net/http.h"
#include "core/json.h"
#include "stream/list_session.h"
#include "stream/provider.h"

#include <SDK/initquit.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <thread>

namespace glasswave::applemusic {
namespace {

namespace fs = std::filesystem;
namespace json = core::json;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

constexpr wchar_t kSongCatalog[] = L"song.contract";
constexpr wchar_t kAlbumCatalog[] = L"album.contract";
constexpr wchar_t kAlbumLibrary[] = L"l.album.contract";
constexpr wchar_t kPlaylistCatalog[] = L"playlist.contract";
constexpr wchar_t kPlaylistLibrary[] = L"p.saved.contract";
constexpr wchar_t kPlaylistPerformance[] = L"p.performance.contract";
constexpr wchar_t kPlaylistPerformanceCatalog[] = L"playlist.performance.catalog";
constexpr wchar_t kPlaylistPagedPerformance[] = L"p.glasswave-paged-performance";
constexpr wchar_t kMutationAlbumCatalog[] = L"1900000001";
constexpr wchar_t kMutationAlbumLibrary[] = L"l.mutation.contract";
constexpr wchar_t kMutationCatalogSong[] = L"1900002600";
constexpr wchar_t kMutationPlaylistCatalog[] = L"1900000002";
constexpr wchar_t kMutationPlaylistLibrary[] = L"p.mutation.contract";
constexpr wchar_t kSourcePageArtistCatalog[] = L"1900002500";
constexpr wchar_t kSourcePageArtistLibrary[] = L"r.sourcepage.contract";
constexpr wchar_t kStaleArtistCatalog[] = L"1900002501";
constexpr wchar_t kStaleArtistLibrary[] = L"r.staleread.contract";
constexpr wchar_t kCatalogArtistAll[] = L"159260351";
constexpr wchar_t kCatalogArtistEmpty[] = L"1147783278";
constexpr wchar_t kCatalogArtistUnknown[] = L"1900003998";
constexpr wchar_t kCatalogArtistUnauthorized[] = L"1900003997";
constexpr wchar_t kCatalogArtistNetworkError[] = L"1900003996";
constexpr wchar_t kCatalogArtistContinuationError[] = L"1900003995";
constexpr wchar_t kCatalogArtistConcurrentEmpty[] = L"1900003994";
constexpr wchar_t kScratchName[] = L"provider-contract-scratch";
constexpr wchar_t kRenamedName[] = L"provider-contract-renamed";
constexpr wchar_t kFailRename[] = L"fail-rename";
constexpr size_t kMaxRequestBytes = 4096;
constexpr size_t kPerformanceTrackCount = 359;
constexpr size_t kPerformanceUniqueCount = 356;
constexpr size_t kEnrichmentTrackCount = 5;
constexpr size_t kEnrichmentUniqueCount = 4;
constexpr size_t kEnrichmentMissingUniqueCount = 3;
constexpr size_t kEnrichmentPageLimit = 20;
constexpr size_t kEnrichmentTagLength = 16;
constexpr size_t kEnrichmentKnownIndex = 0;
constexpr size_t kEnrichmentIndexes[kEnrichmentTrackCount] = {2, 0, 2, 1, 3};

struct Paths {
    fs::path request;
    fs::path claimed;
    fs::path result;
};

struct ProbeRequest {
    std::string nonce;
    std::string scenario;
};

bool IsSupportedProbeScenario(std::string_view scenario) {
    return scenario == "baseline" || scenario == "mutation-reconciliation" ||
           scenario == "catalog-regression" || scenario == "catalog-suggestions" ||
           scenario == "startup-cache-seed" || scenario == "startup-cache-same" ||
           scenario == "startup-cache-b" || scenario == "startup-cache-signed-out" ||
           scenario == "startup-cache-transient" || scenario == "startup-cache-origin";
}

struct Operation {
    std::wstring name;
    bool ok = false;
    std::wstring error;
};

struct Snapshot {
    std::vector<std::wstring> created;
    std::vector<std::wstring> saved;
    std::vector<std::wstring> albums;
    std::vector<std::wstring> artists;
    std::vector<std::wstring> favorites;
    bool operator==(const Snapshot&) const = default;
};

struct PerformanceResult {
    size_t coalescedCallbacks = 0;
    size_t firstRows = 0;
    size_t secondRows = 0;
    size_t uniqueTracks = 0;
    size_t cachedRows = 0;
    size_t fallbackRows = 0;
    size_t catalogRows = 0;
    double coalescedMs = 0;
    double cachedMs = 0;
    double fallbackMs = 0;
    bool firstValid = false;
    bool secondValid = false;
    bool cachedValid = false;
    bool fallbackValid = false;
    bool catalogValid = false;
};

struct PagingResult {
    size_t firstRows = 0;
    size_t secondRows = 0;
    size_t thirdRows = 0;
    size_t fourthRows = 0;
    size_t totalRows = 0;
    size_t uniqueTracks = 0;
    bool pausedAtFirstPage = false;
    bool firstMore = false;
    bool firstOffset = false;
    bool firstOpaque = false;
    bool explicitNextOffset = false;
    bool mismatchedCursorRejected = false;
    bool crossQueryRejected = false;
    bool exhausted = false;
    bool creditsValid = false;
    bool restorePreserved = false;
};

struct PagingFixtureCounters {
    size_t primaryRequests = 0;
    size_t pageRequests = 0;
    size_t zeroOffsetRequests = 0;
    size_t exactNestedIncludes = 0;
    size_t opaqueQueryRetained = 0;
    size_t invalidPrimaryRequests = 0;
    size_t invalidPageRequests = 0;
    size_t pageRows = 0;
    std::vector<size_t> offsets;
};

struct EnrichmentResult {
    size_t baselineRows = 0;
    size_t enrichedRows = 0;
    size_t uniqueTracks = 0;
    size_t hydratedUniqueTracks = 0;
    size_t callbackCount = 0;
    size_t cancellationCallbacks = 0;
    size_t fullyKnownCallbacks = 0;
    double callbackMs = 0;
    bool freshCatalogIds = false;
    bool baselineUsable = false;
    bool baselinePending = false;
    bool noHydrationBeforeBaseline = false;
    bool cancellationRejected = false;
    bool cancellationNoRequests = false;
    bool explicitAccepted = false;
    bool callbackNotInline = false;
    bool occurrenceIdentityPreserved = false;
    bool relationsValid = false;
    bool pendingCleared = false;
    bool knownRecordSkipped = false;
    bool fullyKnownPageSkipped = false;
};

struct EnrichmentFixtureCounters {
    std::wstring tag;
    size_t trackRequests = 0;
    size_t invalidTrackRequests = 0;
    size_t trackRows = 0;
    size_t hydrationRequests = 0;
    size_t hydrationRows = 0;
    size_t invalidHydrationRequests = 0;
    size_t knownIdRequests = 0;
    size_t hydrationDuplicateIds = 0;
    size_t maxHydrationConcurrency = 0;
    size_t activeHydrationRequests = 0;
    size_t hydrationDelayMs = 0;
    std::vector<size_t> hydrationBatchSizes;
    std::vector<stream::Id> hydrationIds;
};

struct RegionalFixtureCounters {
    size_t sourceLibraryAlbumRequests = 0;
    size_t sourceLibraryAlbumTrackRequests = 0;
    size_t sourceLibraryArtistRequests = 0;
    size_t sourceCatalogArtistRequests = 0;
    size_t sourceRegionalAlbumRequests = 0;
    size_t sourceRegionalArtistRequests = 0;
    size_t sourceRetryArtistRequests = 0;
    size_t japanAlbumRequests = 0;
    size_t japanSongRequests = 0;
    size_t japanArtistRequests = 0;
    size_t chinaAlbumRequests = 0;
    size_t chinaSongRequests = 0;
    size_t chinaArtistRequests = 0;
    size_t retryArtistRequests = 0;
    size_t retryArtistFailures = 0;
    size_t retryArtistSuccesses = 0;
    size_t invalidRequests = 0;
};

struct ArtistFixtureCounters {
    size_t originalLibraryReads = 0;
    size_t sourcePageCanonicalReads = 0;
    size_t staleCanonicalReads = 0;
    bool staleReadArmed = false;
    bool staleReadHeld = false;
    bool staleReadCompleted = false;
    std::vector<std::pair<std::wstring, stream::Id>> writes;
};

struct AlbumResolverFixtureCounters {
    size_t catalogDetailReads = 0;
    size_t libraryDetailReads = 0;
    size_t libraryTrackReads = 0;
};

struct PlaylistAuthorityFixtureCounters {
    bool staleServed = false;
    bool absentServed = false;
    bool externalPresent = false;
};

struct RegionalResult {
    bool directCacheContract = false;
    bool staleWritesIsolated = false;
    bool sourceStorefrontIsolated = false;
    bool restoreCanonicalRecords = false;
    bool japanProviderFetch = false;
    bool chinaProviderFetch = false;
    bool accountProviderFetch = false;
    bool missingCounterpartFallback = false;
    bool albumDescriptionOverlay = false;
    bool artistBioOverlay = false;
    bool identityDurationPreserved = false;
    bool sourceUsRequestsOnly = false;
    bool regionalListPersistent = false;
    bool regionalCacheContextsSeparated = false;
    bool transientRetryRecovered = false;
};

struct StartupResult {
    std::string sourceIdentity;
    size_t cachedRows = 0;
    size_t listRequestsBeforeAuth = 0;
    size_t enrichmentRequestsBeforeAuth = 0;
    bool cachedSidebarBeforeAuth = false;
    bool cachedListBeforeAuth = false;
    bool restoringBeforeAuth = false;
    bool accountUnverifiedBeforeAuth = false;
    bool authPublishedBeforeMembership = false;
    bool playlistPageAfterAuthBeforeMembership = false;
    bool sameQueryIdentity = false;
    bool sameAccountPrefixPreserved = false;
    bool differentAccountRevokedA = false;
    bool signedOutCleared = false;
    bool transientPreserved = false;
    bool originIsolation = false;
    bool favoriteCachedBeforeAuth = false;
    bool favoriteWarmPageAfterAuth = false;
    bool favoriteColdPendingBeforeDiscovery = false;
    bool favoriteColdPageAfterDiscovery = false;
    bool favoriteNoFavoriteEmptyAfterDiscovery = false;
    bool favoriteBindingClearedOnAccountChange = false;
};

struct State {
    explicit State(fs::path path) : result_path(std::move(path)) {}
    fs::path result_path;
    std::vector<Operation> operations;
    Snapshot before_rename;
    PerformanceResult performance;
    PagingResult paging;
    stream::ListQuery pagingQuery;
    stream::ListCursor pagingFirstCursor;
    stream::ListCursor pagingCurrentCursor;
    std::vector<stream::Track> pagingTracks;
    EnrichmentResult enrichment;
    RegionalResult regional;
    StartupResult startup;
    std::vector<std::shared_ptr<stream::ListSession>> startupSessions;
    stream::ListQuery enrichmentQuery;
    stream::ListPage enrichmentBaseline;
    stream::ListPage enrichmentFinal;
    std::wstring enrichmentTag;
    std::chrono::steady_clock::time_point performanceStarted;
    std::chrono::steady_clock::time_point enrichmentStarted;
    stream::Id scratch;
    std::wstring error;
    bool enrichmentCallReturned = false;
    std::optional<AppleMusicDisplayRegion> displayRegionToRestore;
    std::optional<AppleMusicDisplayRegion> fixtureDisplayRegion;
    std::optional<AppleMusicDisplayRegion> restoredDisplayRegion;
    bool displayRegionRestoreAttempted = false;
    bool displayRegionRestored = false;
    bool displayRegionRestoredByProvider = false;
    std::string scenario;
    bool accountSwitchRequested = false;
    bool accountSwitchCompleted = false;
    bool accountOldRefreshCompleted = false;
    std::wstring accountOldRefreshError;
    std::future<void> accountSwitchFuture;
    std::atomic_bool mutationOldRefreshCompleted = false;
    std::wstring mutationOldRefreshError;
    std::future<void> mutationWaitFuture;
    std::future<void> playlistAuthorityWaitFuture;
    std::future<void> artistPreferenceWaitFuture;
    std::future<void> artistPreferenceFinalizeFuture;
    std::vector<std::future<void>> utilityFutures;
    size_t rapidMutationCallbacks = 0;
    bool rapidMutationFailed = false;
    size_t rapidArtistCallbacks = 0;
    bool rapidArtistFailed = false;
    bool stop = false;
    bool done = false;
};

std::string Utf8(std::wstring_view text) {
    return pfc::stringcvt::string_utf8_from_wide(text.data(), text.size()).get_ptr();
}

std::wstring Wide(std::string_view utf8) {
    return pfc::stringcvt::string_wide_from_utf8(utf8.data(), utf8.size()).get_ptr();
}

void WriteResult(const State& state) noexcept {
    try {
        JsonArray operations;
        for (const auto& operation : state.operations) {
            JsonObject row;
            row.Insert(L"name", JsonValue::CreateStringValue(operation.name));
            row.Insert(L"ok", JsonValue::CreateBooleanValue(operation.ok));
            row.Insert(L"error", JsonValue::CreateStringValue(operation.error));
            operations.Append(row);
        }
        JsonObject result;
        result.Insert(L"ok", JsonValue::CreateBooleanValue(state.error.empty()));
        result.Insert(L"error", JsonValue::CreateStringValue(state.error));
        result.Insert(L"operations", operations);
        JsonObject displayRegion;
        if (state.displayRegionToRestore) {
            displayRegion.Insert(
                L"saved", JsonValue::CreateNumberValue(static_cast<double>(*state.displayRegionToRestore)));
        } else {
            displayRegion.Insert(L"saved", JsonValue::CreateNullValue());
        }
        if (state.fixtureDisplayRegion) {
            displayRegion.Insert(
                L"test", JsonValue::CreateNumberValue(static_cast<double>(*state.fixtureDisplayRegion)));
        } else {
            displayRegion.Insert(L"test", JsonValue::CreateNullValue());
        }
        if (state.restoredDisplayRegion) {
            displayRegion.Insert(
                L"restored", JsonValue::CreateNumberValue(static_cast<double>(*state.restoredDisplayRegion)));
        } else {
            displayRegion.Insert(L"restored", JsonValue::CreateNullValue());
        }
        displayRegion.Insert(L"restoreAttempted",
                             JsonValue::CreateBooleanValue(state.displayRegionRestoreAttempted));
        displayRegion.Insert(L"restoredOk", JsonValue::CreateBooleanValue(state.displayRegionRestored));
        displayRegion.Insert(L"restoredByProvider",
                             JsonValue::CreateBooleanValue(state.displayRegionRestoredByProvider));
        result.Insert(L"displayRegion", displayRegion);
        JsonObject accountSwitch;
        accountSwitch.Insert(L"requested", JsonValue::CreateBooleanValue(state.accountSwitchRequested));
        accountSwitch.Insert(L"completed", JsonValue::CreateBooleanValue(state.accountSwitchCompleted));
        accountSwitch.Insert(L"oldRefreshCompleted",
                             JsonValue::CreateBooleanValue(state.accountOldRefreshCompleted));
        accountSwitch.Insert(L"oldRefreshError", JsonValue::CreateStringValue(state.accountOldRefreshError));
        result.Insert(L"accountSwitch", accountSwitch);
        JsonObject mutationRegression;
        mutationRegression.Insert(L"scenario", JsonValue::CreateStringValue(Wide(state.scenario)));
        mutationRegression.Insert(
            L"oldReadCompleted",
            JsonValue::CreateBooleanValue(state.mutationOldRefreshCompleted.load(std::memory_order_acquire)));
        mutationRegression.Insert(L"oldReadError",
                                  JsonValue::CreateStringValue(state.mutationOldRefreshError));
        result.Insert(L"mutationRegression", mutationRegression);
        JsonObject performance;
        performance.Insert(L"coalescedCallbacks", JsonValue::CreateNumberValue(static_cast<double>(
                                                      state.performance.coalescedCallbacks)));
        performance.Insert(L"firstRows",
                           JsonValue::CreateNumberValue(static_cast<double>(state.performance.firstRows)));
        performance.Insert(L"secondRows",
                           JsonValue::CreateNumberValue(static_cast<double>(state.performance.secondRows)));
        performance.Insert(L"uniqueTracks",
                           JsonValue::CreateNumberValue(static_cast<double>(state.performance.uniqueTracks)));
        performance.Insert(L"cachedRows",
                           JsonValue::CreateNumberValue(static_cast<double>(state.performance.cachedRows)));
        performance.Insert(L"fallbackRows",
                           JsonValue::CreateNumberValue(static_cast<double>(state.performance.fallbackRows)));
        performance.Insert(L"catalogRows",
                           JsonValue::CreateNumberValue(static_cast<double>(state.performance.catalogRows)));
        performance.Insert(L"coalescedMs", JsonValue::CreateNumberValue(state.performance.coalescedMs));
        performance.Insert(L"cachedMs", JsonValue::CreateNumberValue(state.performance.cachedMs));
        performance.Insert(L"fallbackMs", JsonValue::CreateNumberValue(state.performance.fallbackMs));
        performance.Insert(L"firstValid", JsonValue::CreateBooleanValue(state.performance.firstValid));
        performance.Insert(L"secondValid", JsonValue::CreateBooleanValue(state.performance.secondValid));
        performance.Insert(L"cachedValid", JsonValue::CreateBooleanValue(state.performance.cachedValid));
        performance.Insert(L"fallbackValid", JsonValue::CreateBooleanValue(state.performance.fallbackValid));
        performance.Insert(L"catalogValid", JsonValue::CreateBooleanValue(state.performance.catalogValid));
        result.Insert(L"performance", performance);
        JsonObject paging;
        paging.Insert(L"firstRows",
                      JsonValue::CreateNumberValue(static_cast<double>(state.paging.firstRows)));
        paging.Insert(L"secondRows",
                      JsonValue::CreateNumberValue(static_cast<double>(state.paging.secondRows)));
        paging.Insert(L"thirdRows",
                      JsonValue::CreateNumberValue(static_cast<double>(state.paging.thirdRows)));
        paging.Insert(L"fourthRows",
                      JsonValue::CreateNumberValue(static_cast<double>(state.paging.fourthRows)));
        paging.Insert(L"totalRows",
                      JsonValue::CreateNumberValue(static_cast<double>(state.paging.totalRows)));
        paging.Insert(L"uniqueTracks",
                      JsonValue::CreateNumberValue(static_cast<double>(state.paging.uniqueTracks)));
        paging.Insert(L"pausedAtFirstPage", JsonValue::CreateBooleanValue(state.paging.pausedAtFirstPage));
        paging.Insert(L"firstMore", JsonValue::CreateBooleanValue(state.paging.firstMore));
        paging.Insert(L"firstOffset", JsonValue::CreateBooleanValue(state.paging.firstOffset));
        paging.Insert(L"firstOpaque", JsonValue::CreateBooleanValue(state.paging.firstOpaque));
        paging.Insert(L"explicitNextOffset", JsonValue::CreateBooleanValue(state.paging.explicitNextOffset));
        paging.Insert(L"mismatchedCursorRejected",
                      JsonValue::CreateBooleanValue(state.paging.mismatchedCursorRejected));
        paging.Insert(L"crossQueryRejected", JsonValue::CreateBooleanValue(state.paging.crossQueryRejected));
        paging.Insert(L"exhausted", JsonValue::CreateBooleanValue(state.paging.exhausted));
        paging.Insert(L"creditsValid", JsonValue::CreateBooleanValue(state.paging.creditsValid));
        paging.Insert(L"restorePreserved", JsonValue::CreateBooleanValue(state.paging.restorePreserved));
        result.Insert(L"paging", paging);
        JsonObject enrichment;
        enrichment.Insert(L"tag", JsonValue::CreateStringValue(state.enrichmentTag));
        enrichment.Insert(L"baselineRows",
                          JsonValue::CreateNumberValue(static_cast<double>(state.enrichment.baselineRows)));
        enrichment.Insert(L"enrichedRows",
                          JsonValue::CreateNumberValue(static_cast<double>(state.enrichment.enrichedRows)));
        enrichment.Insert(L"uniqueTracks",
                          JsonValue::CreateNumberValue(static_cast<double>(state.enrichment.uniqueTracks)));
        enrichment.Insert(L"hydratedUniqueTracks", JsonValue::CreateNumberValue(static_cast<double>(
                                                       state.enrichment.hydratedUniqueTracks)));
        enrichment.Insert(L"callbackCount",
                          JsonValue::CreateNumberValue(static_cast<double>(state.enrichment.callbackCount)));
        enrichment.Insert(L"cancellationCallbacks", JsonValue::CreateNumberValue(static_cast<double>(
                                                        state.enrichment.cancellationCallbacks)));
        enrichment.Insert(L"fullyKnownCallbacks", JsonValue::CreateNumberValue(static_cast<double>(
                                                      state.enrichment.fullyKnownCallbacks)));
        enrichment.Insert(L"callbackMs", JsonValue::CreateNumberValue(state.enrichment.callbackMs));
        enrichment.Insert(L"freshCatalogIds",
                          JsonValue::CreateBooleanValue(state.enrichment.freshCatalogIds));
        enrichment.Insert(L"baselineUsable", JsonValue::CreateBooleanValue(state.enrichment.baselineUsable));
        enrichment.Insert(L"baselinePending",
                          JsonValue::CreateBooleanValue(state.enrichment.baselinePending));
        enrichment.Insert(L"noHydrationBeforeBaseline",
                          JsonValue::CreateBooleanValue(state.enrichment.noHydrationBeforeBaseline));
        enrichment.Insert(L"cancellationRejected",
                          JsonValue::CreateBooleanValue(state.enrichment.cancellationRejected));
        enrichment.Insert(L"cancellationNoRequests",
                          JsonValue::CreateBooleanValue(state.enrichment.cancellationNoRequests));
        enrichment.Insert(L"explicitAccepted",
                          JsonValue::CreateBooleanValue(state.enrichment.explicitAccepted));
        enrichment.Insert(L"callbackNotInline",
                          JsonValue::CreateBooleanValue(state.enrichment.callbackNotInline));
        enrichment.Insert(L"occurrenceIdentityPreserved",
                          JsonValue::CreateBooleanValue(state.enrichment.occurrenceIdentityPreserved));
        enrichment.Insert(L"relationsValid", JsonValue::CreateBooleanValue(state.enrichment.relationsValid));
        enrichment.Insert(L"pendingCleared", JsonValue::CreateBooleanValue(state.enrichment.pendingCleared));
        enrichment.Insert(L"knownRecordSkipped",
                          JsonValue::CreateBooleanValue(state.enrichment.knownRecordSkipped));
        enrichment.Insert(L"fullyKnownPageSkipped",
                          JsonValue::CreateBooleanValue(state.enrichment.fullyKnownPageSkipped));
        result.Insert(L"enrichment", enrichment);
        JsonObject regional;
        regional.Insert(L"directCacheContract",
                        JsonValue::CreateBooleanValue(state.regional.directCacheContract));
        regional.Insert(L"staleWritesIsolated",
                        JsonValue::CreateBooleanValue(state.regional.staleWritesIsolated));
        regional.Insert(L"sourceStorefrontIsolated",
                        JsonValue::CreateBooleanValue(state.regional.sourceStorefrontIsolated));
        regional.Insert(L"restoreCanonicalRecords",
                        JsonValue::CreateBooleanValue(state.regional.restoreCanonicalRecords));
        regional.Insert(L"japanProviderFetch",
                        JsonValue::CreateBooleanValue(state.regional.japanProviderFetch));
        regional.Insert(L"chinaProviderFetch",
                        JsonValue::CreateBooleanValue(state.regional.chinaProviderFetch));
        regional.Insert(L"accountProviderFetch",
                        JsonValue::CreateBooleanValue(state.regional.accountProviderFetch));
        regional.Insert(L"missingCounterpartFallback",
                        JsonValue::CreateBooleanValue(state.regional.missingCounterpartFallback));
        regional.Insert(L"albumDescriptionOverlay",
                        JsonValue::CreateBooleanValue(state.regional.albumDescriptionOverlay));
        regional.Insert(L"artistBioOverlay", JsonValue::CreateBooleanValue(state.regional.artistBioOverlay));
        regional.Insert(L"identityDurationPreserved",
                        JsonValue::CreateBooleanValue(state.regional.identityDurationPreserved));
        regional.Insert(L"sourceUsRequestsOnly",
                        JsonValue::CreateBooleanValue(state.regional.sourceUsRequestsOnly));
        regional.Insert(L"regionalListPersistent",
                        JsonValue::CreateBooleanValue(state.regional.regionalListPersistent));
        regional.Insert(L"regionalCacheContextsSeparated",
                        JsonValue::CreateBooleanValue(state.regional.regionalCacheContextsSeparated));
        regional.Insert(L"transientRetryRecovered",
                        JsonValue::CreateBooleanValue(state.regional.transientRetryRecovered));
        result.Insert(L"regional", regional);
        JsonObject startup;
        startup.Insert(L"sourceIdentity", JsonValue::CreateStringValue(Wide(state.startup.sourceIdentity)));
        startup.Insert(L"cachedRows",
                       JsonValue::CreateNumberValue(static_cast<double>(state.startup.cachedRows)));
        startup.Insert(L"listRequestsBeforeAuth", JsonValue::CreateNumberValue(static_cast<double>(
                                                      state.startup.listRequestsBeforeAuth)));
        startup.Insert(L"enrichmentRequestsBeforeAuth", JsonValue::CreateNumberValue(static_cast<double>(
                                                            state.startup.enrichmentRequestsBeforeAuth)));
        startup.Insert(L"cachedSidebarBeforeAuth",
                       JsonValue::CreateBooleanValue(state.startup.cachedSidebarBeforeAuth));
        startup.Insert(L"cachedListBeforeAuth",
                       JsonValue::CreateBooleanValue(state.startup.cachedListBeforeAuth));
        startup.Insert(L"restoringBeforeAuth",
                       JsonValue::CreateBooleanValue(state.startup.restoringBeforeAuth));
        startup.Insert(L"accountUnverifiedBeforeAuth",
                       JsonValue::CreateBooleanValue(state.startup.accountUnverifiedBeforeAuth));
        startup.Insert(L"authPublishedBeforeMembership",
                       JsonValue::CreateBooleanValue(state.startup.authPublishedBeforeMembership));
        startup.Insert(L"playlistPageAfterAuthBeforeMembership",
                       JsonValue::CreateBooleanValue(state.startup.playlistPageAfterAuthBeforeMembership));
        startup.Insert(L"sameQueryIdentity", JsonValue::CreateBooleanValue(state.startup.sameQueryIdentity));
        startup.Insert(L"sameAccountPrefixPreserved",
                       JsonValue::CreateBooleanValue(state.startup.sameAccountPrefixPreserved));
        startup.Insert(L"differentAccountRevokedA",
                       JsonValue::CreateBooleanValue(state.startup.differentAccountRevokedA));
        startup.Insert(L"signedOutCleared", JsonValue::CreateBooleanValue(state.startup.signedOutCleared));
        startup.Insert(L"transientPreserved",
                       JsonValue::CreateBooleanValue(state.startup.transientPreserved));
        startup.Insert(L"originIsolation", JsonValue::CreateBooleanValue(state.startup.originIsolation));
        startup.Insert(L"favoriteCachedBeforeAuth",
                       JsonValue::CreateBooleanValue(state.startup.favoriteCachedBeforeAuth));
        startup.Insert(L"favoriteWarmPageAfterAuth",
                       JsonValue::CreateBooleanValue(state.startup.favoriteWarmPageAfterAuth));
        startup.Insert(L"favoriteColdPendingBeforeDiscovery",
                       JsonValue::CreateBooleanValue(state.startup.favoriteColdPendingBeforeDiscovery));
        startup.Insert(L"favoriteColdPageAfterDiscovery",
                       JsonValue::CreateBooleanValue(state.startup.favoriteColdPageAfterDiscovery));
        startup.Insert(L"favoriteNoFavoriteEmptyAfterDiscovery",
                       JsonValue::CreateBooleanValue(state.startup.favoriteNoFavoriteEmptyAfterDiscovery));
        startup.Insert(L"favoriteBindingClearedOnAccountChange",
                       JsonValue::CreateBooleanValue(state.startup.favoriteBindingClearedOnAccountChange));
        result.Insert(L"startup", startup);

        const fs::path temporary = state.result_path.wstring() + L".tmp";
        const std::string bytes = Utf8(result.Stringify().c_str());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (output) {
            MoveFileExW(temporary.c_str(), state.result_path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    } catch (...) {
    }
}

bool SetProviderDisplayRegion(stream::Provider& provider, AppleMusicDisplayRegion region);

bool RestoreDisplayRegion(State& state) noexcept {
    if (!state.displayRegionToRestore) return false;
    if (state.displayRegionRestoreAttempted) return state.displayRegionRestored;
    state.displayRegionRestoreAttempted = true;
    try {
        if (auto* provider = stream::FindProvider(L"apple")) {
            state.displayRegionRestored = SetProviderDisplayRegion(*provider, *state.displayRegionToRestore);
            state.displayRegionRestoredByProvider = state.displayRegionRestored;
        }
        if (!state.displayRegionRestored) {
            SetAppleMusicDisplayRegion(*state.displayRegionToRestore);
            state.displayRegionRestored = GetAppleMusicDisplayRegion() == *state.displayRegionToRestore;
        }
        state.restoredDisplayRegion = GetAppleMusicDisplayRegion();
    } catch (...) {
        state.displayRegionRestored = false;
    }
    state.displayRegionRestored =
        state.restoredDisplayRegion && *state.restoredDisplayRegion == *state.displayRegionToRestore;
    return state.displayRegionRestored;
}

void Finish(const std::shared_ptr<State>& state, std::wstring error = {}) {
    if (!state || state->stop || state->done) return;
    state->error = std::move(error);
    state->done = true;
    if (!RestoreDisplayRegion(*state) && state->error.empty())
        state->error = L"display_region_restore_failed";
    WriteResult(*state);
}

void AddOperation(const std::shared_ptr<State>& state, std::wstring name, bool ok, std::wstring error = {}) {
    state->operations.push_back({std::move(name), ok, std::move(error)});
}

void Fail(const std::shared_ptr<State>& state, std::wstring operation, std::wstring error) {
    AddOperation(state, operation, false, error);
    Finish(state, std::move(error));
}

bool Live(const std::shared_ptr<State>& state) {
    return state && !state->stop && !state->done;
}

void PostAfter(const std::shared_ptr<State>& state, unsigned milliseconds, std::function<void()> callback) {
    state->utilityFutures.emplace_back(
        std::async(std::launch::async, [state, milliseconds, callback = std::move(callback)] {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
            fb2k::inMainThread([state, callback = std::move(callback)] {
                if (Live(state)) callback();
            });
        }));
}

stream::Provider* Provider(const std::shared_ptr<State>& state) {
    if (!Live(state)) return nullptr;
    auto* provider = stream::FindProvider(L"apple");
    if (!provider) Finish(state, L"apple_provider_missing");
    return provider;
}

std::optional<Paths> ProbePaths() {
    const fs::path directory = core::ProfileDir(L"applemusic");
    if (directory.empty()) return std::nullopt;
    return Paths{directory / L"provider-test-request.json", directory / L"provider-test-request.claimed.json",
                 directory / L"provider-test-result.json"};
}

std::optional<ProbeRequest> ReadRequest(const fs::path& path) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size == 0 || size > kMaxRequestBytes) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || static_cast<size_t>(input.gcount()) != bytes.size()) return std::nullopt;
    try {
        const JsonObject request = JsonObject::Parse(winrt::to_hstring(bytes));
        const auto value = request.Lookup(L"nonce");
        if (!value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::String)
            return std::nullopt;
        const std::string nonce = winrt::to_string(value.GetString());
        if (nonce.size() < 32 || nonce.size() > 128 ||
            !std::all_of(nonce.begin(), nonce.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            })) {
            return std::nullopt;
        }
        const auto scenarioValue = request.Lookup(L"scenario");
        if (!scenarioValue || scenarioValue.ValueType() != winrt::Windows::Data::Json::JsonValueType::String)
            return std::nullopt;
        const std::string scenario = winrt::to_string(scenarioValue.GetString());
        if (!IsSupportedProbeScenario(scenario)) return std::nullopt;
        return ProbeRequest{nonce, std::move(scenario)};
    } catch (...) {
        return std::nullopt;
    }
}

bool TestBridgeUrl(std::wstring_view url) {
    constexpr std::wstring_view prefix = L"http://127.0.0.1:";
    if (url.size() <= prefix.size() || url.substr(0, prefix.size()) != prefix || url.back() == L'/')
        return false;
    unsigned port = 0;
    for (wchar_t c : url.substr(prefix.size())) {
        if (c < L'0' || c > L'9') return false;
        port = port * 10 + static_cast<unsigned>(c - L'0');
        if (port > 65535) return false;
    }
    return port > 0 && port != 20025;
}

std::optional<json::Object> ReadMockTestingState() {
    try {
        const std::wstring url = BridgeUrl();
        if (!TestBridgeUrl(url)) return std::nullopt;
        net::HttpOptions options;
        options.method = L"GET";
        options.noProxy = true;
        options.timeoutMs = 2000;
        options.maxBodyBytes = 64u * 1024u;
        options.headers = {{L"X-API-Token", Wide(BridgeToken())}, {L"Accept", L"application/json"}};
        abort_callback_dummy abort;
        const auto response = net::SendHttp(url + L"/api/v1/glasswave/testing", options, abort);
        if (!response.ok() || response.body.empty()) return std::nullopt;
        const auto parsed = json::Parse(response.body);
        if (!parsed || !json::GetBool(*parsed, L"ok")) return std::nullopt;
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<json::Object> ReadStartupFixture() {
    const auto state = ReadMockTestingState();
    if (!state) return std::nullopt;
    return json::GetObject(*state, L"startup");
}

bool VerifyMockBridge(std::string_view expected_nonce) {
    const auto parsed = ReadMockTestingState();
    return parsed && json::GetString(*parsed, L"nonce") == Wide(expected_nonce);
}

std::optional<PagingFixtureCounters> ReadPagingFixtureCounters() {
    const auto state = ReadMockTestingState();
    if (!state) return std::nullopt;
    const auto paged = json::GetObject(*state, L"paged");
    if (!paged) return std::nullopt;
    PagingFixtureCounters counters;
    counters.primaryRequests = static_cast<size_t>(json::GetInt64(*paged, L"primaryRequests", -1));
    counters.pageRequests = static_cast<size_t>(json::GetInt64(*paged, L"pageRequests", -1));
    counters.zeroOffsetRequests = static_cast<size_t>(json::GetInt64(*paged, L"zeroOffsetRequests", -1));
    counters.exactNestedIncludes = static_cast<size_t>(json::GetInt64(*paged, L"exactNestedIncludes", -1));
    counters.opaqueQueryRetained = static_cast<size_t>(json::GetInt64(*paged, L"opaqueQueryRetained", -1));
    counters.invalidPrimaryRequests =
        static_cast<size_t>(json::GetInt64(*paged, L"invalidPrimaryRequests", -1));
    counters.invalidPageRequests = static_cast<size_t>(json::GetInt64(*paged, L"invalidPageRequests", -1));
    counters.pageRows = static_cast<size_t>(json::GetInt64(*paged, L"pageRows", -1));
    if (const auto offsets = json::GetArray(*paged, L"offsets")) {
        for (uint32_t index = 0; index < offsets->Size(); ++index) {
            const auto value = offsets->GetAt(index);
            if (value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
                counters.offsets.push_back(static_cast<size_t>(value.GetNumber()));
        }
    }
    return counters;
}

std::optional<EnrichmentFixtureCounters> ReadEnrichmentFixtureCounters() {
    const auto state = ReadMockTestingState();
    if (!state) return std::nullopt;
    const auto enrichment = json::GetObject(*state, L"enrichment");
    if (!enrichment) return std::nullopt;
    EnrichmentFixtureCounters counters;
    counters.tag = json::GetString(*enrichment, L"tag");
    counters.trackRequests = static_cast<size_t>(json::GetInt64(*enrichment, L"trackRequests", -1));
    counters.invalidTrackRequests =
        static_cast<size_t>(json::GetInt64(*enrichment, L"invalidTrackRequests", -1));
    counters.trackRows = static_cast<size_t>(json::GetInt64(*enrichment, L"trackRows", -1));
    counters.hydrationRequests = static_cast<size_t>(json::GetInt64(*enrichment, L"hydrationRequests", -1));
    counters.hydrationRows = static_cast<size_t>(json::GetInt64(*enrichment, L"hydrationRows", -1));
    counters.invalidHydrationRequests =
        static_cast<size_t>(json::GetInt64(*enrichment, L"invalidHydrationRequests", -1));
    counters.knownIdRequests = static_cast<size_t>(json::GetInt64(*enrichment, L"knownIdRequests", -1));
    counters.hydrationDuplicateIds =
        static_cast<size_t>(json::GetInt64(*enrichment, L"hydrationDuplicateIds", -1));
    counters.maxHydrationConcurrency =
        static_cast<size_t>(json::GetInt64(*enrichment, L"maxHydrationConcurrency", -1));
    counters.activeHydrationRequests =
        static_cast<size_t>(json::GetInt64(*enrichment, L"activeHydrationRequests", -1));
    counters.hydrationDelayMs = static_cast<size_t>(json::GetInt64(*enrichment, L"hydrationDelayMs", -1));
    if (const auto sizes = json::GetArray(*enrichment, L"hydrationBatchSizes")) {
        for (uint32_t index = 0; index < sizes->Size(); ++index) {
            const auto value = sizes->GetAt(index);
            if (value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number)
                counters.hydrationBatchSizes.push_back(static_cast<size_t>(value.GetNumber()));
        }
    }
    if (const auto ids = json::GetArray(*enrichment, L"hydrationIds")) {
        for (uint32_t index = 0; index < ids->Size(); ++index) {
            const auto value = ids->GetAt(index);
            if (value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                counters.hydrationIds.push_back(value.GetString().c_str());
        }
    }
    return counters;
}

std::optional<RegionalFixtureCounters> ReadRegionalFixtureCounters() {
    const auto state = ReadMockTestingState();
    if (!state) return std::nullopt;
    const auto regional = json::GetObject(*state, L"regional");
    if (!regional) return std::nullopt;
    RegionalFixtureCounters counters;
    counters.sourceLibraryAlbumRequests =
        static_cast<size_t>(json::GetInt64(*regional, L"sourceLibraryAlbumRequests", -1));
    counters.sourceLibraryAlbumTrackRequests =
        static_cast<size_t>(json::GetInt64(*regional, L"sourceLibraryAlbumTrackRequests", -1));
    counters.sourceLibraryArtistRequests =
        static_cast<size_t>(json::GetInt64(*regional, L"sourceLibraryArtistRequests", -1));
    counters.sourceCatalogArtistRequests =
        static_cast<size_t>(json::GetInt64(*regional, L"sourceCatalogArtistRequests", -1));
    counters.sourceRegionalAlbumRequests =
        static_cast<size_t>(json::GetInt64(*regional, L"sourceRegionalAlbumRequests", -1));
    counters.sourceRegionalArtistRequests =
        static_cast<size_t>(json::GetInt64(*regional, L"sourceRegionalArtistRequests", -1));
    counters.sourceRetryArtistRequests =
        static_cast<size_t>(json::GetInt64(*regional, L"sourceRetryArtistRequests", -1));
    counters.japanAlbumRequests = static_cast<size_t>(json::GetInt64(*regional, L"japanAlbumRequests", -1));
    counters.japanSongRequests = static_cast<size_t>(json::GetInt64(*regional, L"japanSongRequests", -1));
    counters.japanArtistRequests = static_cast<size_t>(json::GetInt64(*regional, L"japanArtistRequests", -1));
    counters.chinaAlbumRequests = static_cast<size_t>(json::GetInt64(*regional, L"chinaAlbumRequests", -1));
    counters.chinaSongRequests = static_cast<size_t>(json::GetInt64(*regional, L"chinaSongRequests", -1));
    counters.chinaArtistRequests = static_cast<size_t>(json::GetInt64(*regional, L"chinaArtistRequests", -1));
    counters.retryArtistRequests = static_cast<size_t>(json::GetInt64(*regional, L"retryArtistRequests", -1));
    counters.retryArtistFailures = static_cast<size_t>(json::GetInt64(*regional, L"retryArtistFailures", -1));
    counters.retryArtistSuccesses =
        static_cast<size_t>(json::GetInt64(*regional, L"retryArtistSuccesses", -1));
    counters.invalidRequests = static_cast<size_t>(json::GetInt64(*regional, L"invalidRequests", -1));
    return counters;
}

std::optional<ArtistFixtureCounters> ReadArtistFixtureCounters() {
    const auto state = ReadMockTestingState();
    if (!state) return std::nullopt;
    const auto mutation = json::GetObject(*state, L"mutation");
    const auto fixture = mutation ? json::GetObject(*mutation, L"artistFixture") : std::nullopt;
    const auto canonical = fixture ? json::GetObject(*fixture, L"canonicalReads") : std::nullopt;
    if (!mutation || !fixture || !canonical) return std::nullopt;
    ArtistFixtureCounters counters;
    counters.originalLibraryReads =
        static_cast<size_t>(json::GetInt64(*fixture, L"originalLibraryReads", -1));
    counters.sourcePageCanonicalReads =
        static_cast<size_t>(json::GetInt64(*canonical, kSourcePageArtistCatalog, -1));
    counters.staleCanonicalReads = static_cast<size_t>(json::GetInt64(*canonical, kStaleArtistCatalog, -1));
    counters.staleReadArmed = json::GetBool(*fixture, L"staleReadArmed");
    counters.staleReadHeld = json::GetBool(*fixture, L"staleReadHeld");
    counters.staleReadCompleted = json::GetBool(*fixture, L"staleReadCompleted");
    if (const auto writes = json::GetArray(*fixture, L"writes")) {
        for (uint32_t index = 0; index < writes->Size(); ++index) {
            const auto value = writes->GetAt(index);
            if (!value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
            const JsonObject item = value.GetObject();
            counters.writes.emplace_back(json::GetString(item, L"method"), json::GetString(item, L"id"));
        }
    }
    return counters;
}

std::optional<AlbumResolverFixtureCounters> ReadAlbumResolverFixtureCounters() {
    const auto state = ReadMockTestingState();
    if (!state) return std::nullopt;
    const auto isolation = json::GetObject(*state, L"isolation");
    if (!isolation) return std::nullopt;
    AlbumResolverFixtureCounters counters;
    counters.catalogDetailReads =
        static_cast<size_t>(json::GetInt64(*isolation, L"mutationCatalogAlbumReads", -1));
    counters.libraryDetailReads =
        static_cast<size_t>(json::GetInt64(*isolation, L"mutationLibraryAlbumReads", -1));
    counters.libraryTrackReads =
        static_cast<size_t>(json::GetInt64(*isolation, L"mutationLibraryAlbumTrackReads", -1));
    return counters;
}

std::optional<PlaylistAuthorityFixtureCounters> ReadPlaylistAuthorityFixtureCounters() {
    const auto state = ReadMockTestingState();
    if (!state) return std::nullopt;
    const auto mutation = json::GetObject(*state, L"mutation");
    const auto fixture = mutation ? json::GetObject(*mutation, L"playlistAuthority") : std::nullopt;
    if (!fixture) return std::nullopt;
    PlaylistAuthorityFixtureCounters counters;
    counters.staleServed = json::GetBool(*fixture, L"staleServed", false);
    counters.absentServed = json::GetBool(*fixture, L"absentServed", false);
    counters.externalPresent = json::GetBool(*fixture, L"externalPresent", false);
    return counters;
}

std::wstring PlaylistRow(const stream::Playlist& playlist) {
    return playlist.id + L"|" + playlist.name + L"|" + std::to_wstring(playlist.trackCount);
}

std::wstring AlbumRow(const stream::Album& album) {
    return album.id + L"|" + album.name + L"|" + std::to_wstring(album.trackCount);
}

std::wstring ArtistRow(const stream::Artist& artist) {
    return artist.id + L"|" + artist.name;
}

bool StartsWith(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::vector<stream::Id> ExpectedPerformanceTrackIds() {
    std::vector<stream::Id> ids;
    ids.reserve(kPerformanceTrackCount);
    for (size_t index = 0; index < kPerformanceUniqueCount; ++index)
        ids.push_back(L"song.performance." + std::to_wstring(index));
    ids.push_back(L"song.performance.7");
    ids.push_back(L"song.performance.128");
    ids.push_back(L"song.performance.277");
    return ids;
}

bool ValidPerformanceTracks(const stream::Playlist& info, const std::vector<stream::Track>& tracks,
                            size_t& uniqueTracks) {
    uniqueTracks = 0;
    if (info.id != kPlaylistPerformance || info.trackCount != static_cast<int>(kPerformanceTrackCount) ||
        tracks.size() != kPerformanceTrackCount)
        return false;

    const auto expectedIds = ExpectedPerformanceTrackIds();
    if (!std::equal(
            expectedIds.begin(), expectedIds.end(), tracks.begin(), tracks.end(),
            [](const stream::Id& expected, const stream::Track& track) { return expected == track.id; }))
        return false;

    std::set<stream::Id> unique;
    constexpr std::wstring_view trackPrefix = L"song.performance.";
    constexpr std::wstring_view albumPrefix = L"l.album.performance.";
    constexpr std::wstring_view artistPrefix = L"r.artist.performance.";
    constexpr std::wstring_view artistNamePrefix = L"Performance Artist ";
    for (const auto& track : tracks) {
        if (!StartsWith(track.id, trackPrefix)) return false;
        const std::wstring suffix(track.id.substr(trackPrefix.size()));
        if (track.album != L"Performance Album " + suffix ||
            track.albumId != std::wstring(albumPrefix) + suffix || track.artists.size() != 1 ||
            track.artists.front().id != std::wstring(artistPrefix) + suffix ||
            track.artists.front().name != std::wstring(artistNamePrefix) + suffix)
            return false;
        unique.emplace(track.id);
    }
    uniqueTracks = unique.size();
    return uniqueTracks == kPerformanceUniqueCount;
}

bool ValidCatalogPerformanceTracks(const stream::Playlist& info, const std::vector<stream::Track>& tracks) {
    if (info.id != kPlaylistPerformanceCatalog || info.trackCount != 3 || tracks.size() != 3) return false;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const stream::Track& track = tracks[index];
        const std::wstring suffix = std::to_wstring(index);
        if (track.id != L"song.catalog.performance." + suffix ||
            track.album != L"Catalog Performance Album " + suffix ||
            track.albumId != L"album.catalog.performance." + suffix || track.artists.size() != 1 ||
            track.artists.front().id != L"artist.catalog.performance." + suffix ||
            track.artists.front().name != L"Catalog Performance Artist " + suffix)
            return false;
    }
    return true;
}

std::vector<stream::Id> ExpectedPagedTrackIds() {
    std::vector<stream::Id> ids;
    ids.reserve(kPerformanceTrackCount);
    for (size_t index = 0; index < kPerformanceUniqueCount; ++index)
        ids.push_back(L"song.glasswave-paged-performance." + std::to_wstring(index));
    ids.push_back(L"song.glasswave-paged-performance.7");
    ids.push_back(L"song.glasswave-paged-performance.128");
    ids.push_back(L"song.glasswave-paged-performance.277");
    return ids;
}

bool ValidPagedTracks(const std::vector<stream::Track>& tracks, size_t& uniqueTracks) {
    uniqueTracks = 0;
    if (tracks.size() != kPerformanceTrackCount) return false;
    const auto expectedIds = ExpectedPagedTrackIds();
    if (!std::equal(
            expectedIds.begin(), expectedIds.end(), tracks.begin(), tracks.end(),
            [](const stream::Id& expected, const stream::Track& track) { return expected == track.id; }))
        return false;

    std::set<stream::Id> unique;
    constexpr std::wstring_view trackPrefix = L"song.glasswave-paged-performance.";
    constexpr std::wstring_view albumPrefix = L"l.album.glasswave-paged-performance.";
    constexpr std::wstring_view artistPrefix = L"r.artist.glasswave-paged-performance.";
    for (const auto& track : tracks) {
        if (!StartsWith(track.id, trackPrefix)) return false;
        const std::wstring suffix(track.id.substr(trackPrefix.size()));
        if (track.album != L"Glasswave Paged Album " + suffix ||
            track.albumId != std::wstring(albumPrefix) + suffix || track.artists.size() != 1 ||
            track.artists.front().id != std::wstring(artistPrefix) + suffix ||
            track.artists.front().name != L"Glasswave Paged Artist " + suffix)
            return false;
        unique.emplace(track.id);
    }
    uniqueTracks = unique.size();
    return uniqueTracks == kPerformanceUniqueCount;
}

const std::vector<stream::Track>* PagedRows(const stream::ListPage& page) {
    return std::get_if<std::vector<stream::Track>>(&page.rows);
}

std::wstring EnrichmentPlaylistId(const State& state) {
    return L"p.glasswave-enrichment." + state.enrichmentTag;
}

std::wstring EnrichmentTrackId(const State& state, size_t index) {
    return L"song.glasswave-enrichment." + state.enrichmentTag + L"." + std::to_wstring(index);
}

std::wstring EnrichmentLibraryId(const State& state, size_t index) {
    return L"i.song.glasswave-enrichment." + state.enrichmentTag + L"." + std::to_wstring(index);
}

std::wstring EnrichmentAlbumId(const State& state, size_t index) {
    return L"l.album.glasswave-enrichment." + state.enrichmentTag + L"." + std::to_wstring(index);
}

std::wstring EnrichmentArtistId(const State& state, size_t index) {
    return L"r.artist.glasswave-enrichment." + state.enrichmentTag + L"." + std::to_wstring(index);
}

std::wstring EnrichmentTitle(size_t index) {
    return L"Glasswave Enrichment Track " + std::to_wstring(index);
}

std::wstring EnrichmentArtistName(size_t index) {
    return L"Glasswave Enrichment Artist " + std::to_wstring(index);
}

std::wstring EnrichmentAlbumName(size_t index) {
    return L"Glasswave Enrichment Album " + std::to_wstring(index);
}

bool FreshEnrichmentCatalogIds(const State& state) {
    for (size_t index = 0; index < kEnrichmentUniqueCount; ++index)
        if (Catalog::Get().FindSong(EnrichmentTrackId(state, index))) return false;
    return true;
}

bool ValidEnrichmentBaseline(const State& state, const stream::ListPage& page) {
    const auto* rows = PagedRows(page);
    if (!rows || rows->size() != kEnrichmentTrackCount || !page.enrichmentPending || page.more ||
        !page.next.value.empty())
        return false;

    for (size_t occurrence = 0; occurrence < kEnrichmentTrackCount; ++occurrence) {
        const size_t index = kEnrichmentIndexes[occurrence];
        const stream::Track& track = (*rows)[occurrence];
        if (track.id != EnrichmentTrackId(state, index) || track.title != EnrichmentTitle(index) ||
            track.durationMs != 210000 + static_cast<int>(index) || track.artists.size() != 1 ||
            track.artists.front().name != EnrichmentArtistName(index))
            return false;

        if (index == kEnrichmentKnownIndex) {
            if (track.album != EnrichmentAlbumName(index) ||
                track.albumId != EnrichmentAlbumId(state, index) ||
                track.artists.front().id != EnrichmentArtistId(state, index))
                return false;
        } else if (!track.album.empty() || !track.albumId.empty() || !track.artists.front().id.empty()) {
            return false;
        }
    }
    return true;
}

bool SameEnrichmentOccurrences(const stream::ListPage& before, const stream::ListPage& after) {
    const auto* beforeRows = PagedRows(before);
    const auto* afterRows = PagedRows(after);
    return beforeRows && afterRows && beforeRows->size() == afterRows->size() &&
           std::equal(
               beforeRows->begin(), beforeRows->end(), afterRows->begin(),
               [](const stream::Track& left, const stream::Track& right) { return left.id == right.id; });
}

bool ValidEnrichmentFinal(const State& state, const stream::ListPage& page) {
    const auto* rows = PagedRows(page);
    if (!rows || rows->size() != kEnrichmentTrackCount || page.enrichmentPending) return false;

    for (size_t occurrence = 0; occurrence < kEnrichmentTrackCount; ++occurrence) {
        const size_t index = kEnrichmentIndexes[occurrence];
        const stream::Track& track = (*rows)[occurrence];
        if (track.id != EnrichmentTrackId(state, index) || track.title != EnrichmentTitle(index) ||
            track.album != EnrichmentAlbumName(index) || track.albumId != EnrichmentAlbumId(state, index) ||
            track.durationMs != 210000 + static_cast<int>(index) || track.artists.size() != 1 ||
            track.artists.front().id != EnrichmentArtistId(state, index) ||
            track.artists.front().name != EnrichmentArtistName(index))
            return false;
    }
    return true;
}

std::wstring RegionalAlbumCatalogId(const State& state) {
    return L"album.glasswave-regional." + state.enrichmentTag;
}

std::wstring RegionalAlbumLibraryId(const State& state) {
    return L"l.album.glasswave-regional." + state.enrichmentTag;
}

std::wstring RegionalUploadedSongCatalogId(const State& state) {
    return L"song.glasswave-regional." + state.enrichmentTag + L".uploaded";
}

std::wstring RegionalUploadedSongLibraryId(const State& state) {
    return L"i.song.glasswave-regional." + state.enrichmentTag + L".uploaded";
}

std::wstring RegionalLibraryOnlySongId(const State& state) {
    return L"i.song.glasswave-regional." + state.enrichmentTag + L".library-only";
}

std::wstring RegionalAlbumArtistCatalogId(const State& state) {
    return L"artist.glasswave-regional." + state.enrichmentTag + L".album";
}

std::wstring RegionalAlbumArtistLibraryId(const State& state) {
    return L"r.artist.glasswave-regional." + state.enrichmentTag + L".album";
}

std::wstring RegionalLibraryOnlyArtistId(const State& state) {
    return L"r.artist.glasswave-regional." + state.enrichmentTag + L".library-only";
}

std::wstring RegionalMissingArtistCatalogId(const State& state) {
    return L"artist.glasswave-regional." + state.enrichmentTag + L".missing";
}

std::wstring RegionalRetryArtistCatalogId(const State& state) {
    return L"artist.glasswave-ordered." + state.enrichmentTag + L".retry";
}

std::wstring RegionalDetailAlbumCatalogId(const State& state) {
    return L"album.glasswave-ordered." + state.enrichmentTag;
}

std::wstring RegionalDetailUploadedSongCatalogId(const State& state) {
    return L"song.glasswave-ordered." + state.enrichmentTag + L".uploaded";
}

std::wstring RegionalDetailUploadedSongLibraryId(const State& state) {
    return L"i.song.glasswave-ordered." + state.enrichmentTag + L".uploaded";
}

std::wstring RegionalDetailLibraryOnlySongId(const State& state) {
    return L"i.song.glasswave-ordered." + state.enrichmentTag + L".library-only";
}

std::wstring RegionalDetailArtistCatalogId(const State& state) {
    return L"artist.glasswave-ordered." + state.enrichmentTag + L".album";
}

std::wstring RegionalDetailLibraryOnlyArtistId(const State& state) {
    return L"r.artist.glasswave-ordered." + state.enrichmentTag + L".library-only";
}

std::wstring StartupPlaylistId(const State& state, wchar_t account) {
    return L"p.startup." + state.enrichmentTag + L"." + account;
}

stream::ListQuery StartupPlaylistQuery(const State& state, wchar_t account = L'a') {
    stream::ListQuery query;
    query.kind = stream::ListKind::PlaylistTracks;
    query.id = StartupPlaylistId(state, account);
    return query;
}

stream::ListQuery StartupFavoriteQuery() {
    stream::ListQuery query;
    query.kind = stream::ListKind::Favorites;
    return query;
}

std::wstring RegionalAlbumName(AppleMusicDisplayRegion region) {
    switch (region) {
    case AppleMusicDisplayRegion::Japan:
        return L"日本地域アルバム";
    case AppleMusicDisplayRegion::China:
        return L"中国地区专辑";
    case AppleMusicDisplayRegion::Account:
        return L"Regional Source Album";
    }
    return {};
}

std::wstring RegionalAlbumDescription(AppleMusicDisplayRegion region) {
    switch (region) {
    case AppleMusicDisplayRegion::Japan:
        return L"日本語のアルバム説明";
    case AppleMusicDisplayRegion::China:
        return L"中文专辑介绍";
    case AppleMusicDisplayRegion::Account:
        return L"Regional source album description";
    }
    return {};
}

std::wstring RegionalUploadedSongTitle(AppleMusicDisplayRegion region) {
    switch (region) {
    case AppleMusicDisplayRegion::Japan:
        return L"日本語のアップロード曲";
    case AppleMusicDisplayRegion::China:
        return L"中文上传歌曲";
    case AppleMusicDisplayRegion::Account:
        return L"Regional Source Uploaded Song";
    }
    return {};
}

std::wstring RegionalAlbumArtistName(AppleMusicDisplayRegion region) {
    switch (region) {
    case AppleMusicDisplayRegion::Japan:
        return L"日本語のアルバムアーティスト";
    case AppleMusicDisplayRegion::China:
        return L"中文专辑艺术家";
    case AppleMusicDisplayRegion::Account:
        return L"Regional Source Album Artist";
    }
    return {};
}

std::wstring RegionalBioArtistName(AppleMusicDisplayRegion region) {
    switch (region) {
    case AppleMusicDisplayRegion::Japan:
        return L"日本語のバイオアーティスト";
    case AppleMusicDisplayRegion::China:
        return L"中文简介艺术家";
    case AppleMusicDisplayRegion::Account:
        return L"Regional Source Album Artist";
    }
    return {};
}

std::wstring RegionalBio(AppleMusicDisplayRegion region) {
    switch (region) {
    case AppleMusicDisplayRegion::Japan:
        return L"日本語のアーティスト紹介";
    case AppleMusicDisplayRegion::China:
        return L"中文艺术家简介";
    case AppleMusicDisplayRegion::Account:
        return L"Regional source artist biography";
    }
    return {};
}

std::wstring RegionalAlbumProblem(const State& state, const stream::Album& album,
                                  const std::vector<stream::Track>& tracks, AppleMusicDisplayRegion region) {
    if (album.id != RegionalDetailAlbumCatalogId(state))
        return L"album_id(actual=" + album.id + L"; expected=" + RegionalDetailAlbumCatalogId(state) + L")";
    if (album.name != RegionalAlbumName(region))
        return L"album_name(actual=" + album.name + L"; expected=" + RegionalAlbumName(region) + L")";
    if (album.description != RegionalAlbumDescription(region))
        return L"album_description(actual=" + album.description + L"; expected=" +
               RegionalAlbumDescription(region) + L")";
    if (album.trackCount != 2) return L"album_track_count(actual=" + std::to_wstring(album.trackCount) + L")";
    if (album.year != 2021) return L"album_year(actual=" + std::to_wstring(album.year) + L")";
    if (album.artists.size() != 1)
        return L"album_artist_count(actual=" + std::to_wstring(album.artists.size()) + L")";
    if (album.artists.front().id != RegionalDetailArtistCatalogId(state))
        return L"album_artist_id(actual=" + album.artists.front().id + L"; expected=" +
               RegionalDetailArtistCatalogId(state) + L")";
    if (album.artists.front().name != RegionalAlbumArtistName(region))
        return L"album_artist_name(actual=" + album.artists.front().name + L"; expected=" +
               RegionalAlbumArtistName(region) + L")";
    if (tracks.size() != 2) return L"album_tracks(actual=" + std::to_wstring(tracks.size()) + L")";

    const auto uploaded = std::find_if(tracks.begin(), tracks.end(), [&state](const auto& track) {
        return track.id == RegionalDetailUploadedSongCatalogId(state);
    });
    const auto libraryOnly = std::find_if(tracks.begin(), tracks.end(), [&state](const auto& track) {
        return track.id == RegionalDetailLibraryOnlySongId(state);
    });
    if (uploaded == tracks.end()) return L"album_uploaded_track_missing";
    if (libraryOnly == tracks.end()) return L"album_library_only_track_missing";

    if (uploaded->title != RegionalUploadedSongTitle(region) ||
        uploaded->album != RegionalAlbumName(region) ||
        uploaded->albumId != RegionalDetailAlbumCatalogId(state) || uploaded->artists.size() != 1 ||
        uploaded->artists.front().id != RegionalDetailArtistCatalogId(state) ||
        uploaded->artists.front().name != RegionalAlbumArtistName(region) || uploaded->durationMs != 245678)
        return L"album_uploaded_track_mismatch(title=" + uploaded->title + L"; album=" + uploaded->album +
               L"; albumId=" + uploaded->albumId + L"; artist=" +
               (uploaded->artists.empty() ? std::wstring() : uploaded->artists.front().name) +
               L"; duration=" + std::to_wstring(uploaded->durationMs) + L")";

    // The library-only row has no catalog counterpart. Its title, artist and duration stay
    // source-owned while the shared album name may still receive the regional overlay.
    if (libraryOnly->title != L"Regional Source Library-only Song" ||
        libraryOnly->album != RegionalAlbumName(region) ||
        libraryOnly->albumId != RegionalDetailAlbumCatalogId(state) || libraryOnly->artists.size() != 1 ||
        libraryOnly->artists.front().id != RegionalDetailLibraryOnlyArtistId(state) ||
        libraryOnly->artists.front().name != L"Regional Source Library-only Artist" ||
        libraryOnly->durationMs != 198765)
        return L"album_library_only_track_mismatch(title=" + libraryOnly->title + L"; album=" +
               libraryOnly->album + L"; albumId=" + libraryOnly->albumId + L"; artist=" +
               (libraryOnly->artists.empty() ? std::wstring() : libraryOnly->artists.front().name) +
               L"; duration=" + std::to_wstring(libraryOnly->durationMs) + L")";

    const auto rawUploaded = Catalog::Get().FindSongRaw(RegionalDetailUploadedSongCatalogId(state));
    const auto rawLibraryOnly = Catalog::Get().FindSongRaw(RegionalDetailLibraryOnlySongId(state));
    if (!rawUploaded || rawUploaded->libraryId != RegionalDetailUploadedSongLibraryId(state) ||
        rawUploaded->catalogId != RegionalDetailUploadedSongCatalogId(state))
        return L"album_raw_uploaded_track_mismatch";
    if (!rawLibraryOnly || rawLibraryOnly->libraryId != RegionalDetailLibraryOnlySongId(state) ||
        rawLibraryOnly->catalogId.empty() == false)
        return L"album_raw_library_only_track_mismatch";
    return {};
}

std::wstring RegionalSongFirstProblem(const State& state, const std::vector<stream::Track>& tracks,
                                      AppleMusicDisplayRegion region) {
    if (tracks.size() != 2) return L"song_first_row_count";
    const auto uploaded = std::find_if(tracks.begin(), tracks.end(), [&state](const auto& track) {
        return track.id == RegionalDetailUploadedSongCatalogId(state);
    });
    const auto libraryOnly = std::find_if(tracks.begin(), tracks.end(), [&state](const auto& track) {
        return track.id == RegionalDetailLibraryOnlySongId(state);
    });
    if (uploaded == tracks.end()) return L"song_first_uploaded_missing";
    if (libraryOnly == tracks.end()) return L"song_first_library_only_missing";
    if (uploaded->title != RegionalUploadedSongTitle(region) ||
        uploaded->album != RegionalAlbumName(region) ||
        uploaded->albumId != RegionalDetailAlbumCatalogId(state) || uploaded->artists.size() != 1 ||
        uploaded->artists.front().id != RegionalDetailArtistCatalogId(state) ||
        uploaded->artists.front().name != RegionalAlbumArtistName(region) || uploaded->durationMs != 245678)
        return L"song_first_uploaded_mismatch";
    if (libraryOnly->title != L"Regional Source Library-only Song") return L"song_first_library_only_title";
    if (libraryOnly->album != RegionalAlbumName(region)) {
        const std::wstring sourceStorefront = DisplayMetadata::Get().SourceStorefront();
        const bool albumCached =
            DisplayMetadata::Get().HasAlbum(region, sourceStorefront, RegionalDetailAlbumCatalogId(state));
        return L"song_first_library_only_album(actual=" + libraryOnly->album + L"; expected=" +
               RegionalAlbumName(region) + L"; cached=" + (albumCached ? L"1" : L"0") + L"; source=" +
               sourceStorefront + L")";
    }
    if (libraryOnly->albumId != RegionalDetailAlbumCatalogId(state))
        return L"song_first_library_only_album_id";
    if (libraryOnly->artists.size() != 1) return L"song_first_library_only_artist_count";
    if (libraryOnly->artists.front().id != RegionalDetailLibraryOnlyArtistId(state))
        return L"song_first_library_only_artist_id";
    if (libraryOnly->artists.front().name != L"Regional Source Library-only Artist")
        return L"song_first_library_only_artist_name";
    if (libraryOnly->durationMs != 198765) return L"song_first_library_only_duration";
    const auto rawUploaded = Catalog::Get().FindSongRaw(RegionalDetailUploadedSongCatalogId(state));
    const auto rawLibraryOnly = Catalog::Get().FindSongRaw(RegionalDetailLibraryOnlySongId(state));
    if (!rawUploaded || rawUploaded->track.title != L"Regional Source Uploaded Song" ||
        rawUploaded->track.album != L"Regional Source Album" ||
        rawUploaded->libraryId != RegionalDetailUploadedSongLibraryId(state))
        return L"song_first_raw_uploaded_mismatch";
    if (!rawLibraryOnly || rawLibraryOnly->track.title != L"Regional Source Library-only Song" ||
        rawLibraryOnly->track.album != L"Regional Source Album")
        return L"song_first_raw_library_only_mismatch";
    return {};
}

bool ValidRegionalArtist(const State& state, const stream::Artist& artist, AppleMusicDisplayRegion region) {
    return artist.id == RegionalDetailArtistCatalogId(state) &&
           artist.name == RegionalBioArtistName(region) && artist.bio == RegionalBio(region) &&
           artist.albumCount == 3 && artist.trackCount == 7;
}

bool ValidRegionalMissingArtist(const State& state, const stream::Artist& artist) {
    return artist.id == RegionalMissingArtistCatalogId(state) &&
           artist.name == L"Regional Source Missing Artist" &&
           artist.bio == L"Regional missing source artist biography" && artist.albumCount == 4 &&
           artist.trackCount == 9;
}

std::wstring RegionalRetryArtistProblem(const State& state, const stream::Artist& artist,
                                        AppleMusicDisplayRegion region) {
    const bool japan = region == AppleMusicDisplayRegion::Japan;
    const std::wstring expectedName = japan ? L"日本語の再試行アーティスト" : L"Regional Source Retry Artist";
    const std::wstring expectedBio =
        japan ? L"日本語の再試行アーティスト紹介" : L"Regional source retry artist biography";
    if (artist.id != RegionalRetryArtistCatalogId(state))
        return L"retry_artist_id(actual=" + artist.id + L"; expected=" + RegionalRetryArtistCatalogId(state) +
               L")";
    if (artist.name != expectedName)
        return L"retry_artist_name(actual=" + artist.name + L"; expected=" + expectedName + L")";
    if (artist.bio != expectedBio)
        return L"retry_artist_bio(actual=" + artist.bio + L"; expected=" + expectedBio + L")";
    if (artist.albumCount != 2)
        return L"retry_artist_album_count(actual=" + std::to_wstring(artist.albumCount) + L")";
    if (artist.trackCount != 5)
        return L"retry_artist_track_count(actual=" + std::to_wstring(artist.trackCount) + L")";
    return {};
}

bool ValidJapanRegionalRequests(const RegionalFixtureCounters& counters) {
    return counters.sourceLibraryAlbumRequests == 0 && counters.sourceLibraryAlbumTrackRequests == 0 &&
           counters.sourceLibraryArtistRequests == 0 && counters.sourceCatalogArtistRequests == 1 &&
           counters.sourceRegionalAlbumRequests == 2 && counters.sourceRegionalArtistRequests == 1 &&
           counters.sourceRetryArtistRequests == 2 && counters.japanAlbumRequests == 1 &&
           counters.japanSongRequests == 1 && counters.japanArtistRequests == 4 &&
           counters.retryArtistRequests == 2 && counters.retryArtistFailures == 1 &&
           counters.retryArtistSuccesses == 1 && counters.chinaAlbumRequests == 0 &&
           counters.chinaSongRequests == 0 && counters.chinaArtistRequests == 0 &&
           counters.invalidRequests == 0;
}

bool ValidChinaRegionalRequests(const RegionalFixtureCounters& counters) {
    return counters.sourceLibraryAlbumRequests == 0 && counters.sourceLibraryAlbumTrackRequests == 0 &&
           counters.sourceLibraryArtistRequests == 0 && counters.sourceCatalogArtistRequests == 2 &&
           counters.sourceRegionalAlbumRequests == 4 && counters.sourceRegionalArtistRequests == 2 &&
           counters.sourceRetryArtistRequests == 2 && counters.japanAlbumRequests == 1 &&
           counters.japanSongRequests == 1 && counters.japanArtistRequests == 4 &&
           counters.retryArtistRequests == 2 && counters.retryArtistFailures == 1 &&
           counters.retryArtistSuccesses == 1 && counters.chinaAlbumRequests == 1 &&
           counters.chinaSongRequests == 1 && counters.chinaArtistRequests == 2 &&
           counters.invalidRequests == 0;
}

bool ValidAccountRegionalRequests(const RegionalFixtureCounters& counters) {
    return counters.sourceLibraryAlbumRequests == 0 && counters.sourceLibraryAlbumTrackRequests == 0 &&
           counters.sourceLibraryArtistRequests == 0 && counters.sourceCatalogArtistRequests == 2 &&
           counters.sourceRegionalAlbumRequests == 5 && counters.sourceRegionalArtistRequests == 3 &&
           counters.sourceRetryArtistRequests == 2 && counters.japanAlbumRequests == 1 &&
           counters.japanSongRequests == 1 && counters.japanArtistRequests == 4 &&
           counters.retryArtistRequests == 2 && counters.retryArtistFailures == 1 &&
           counters.retryArtistSuccesses == 1 && counters.chinaAlbumRequests == 1 &&
           counters.chinaSongRequests == 1 && counters.chinaArtistRequests == 2 &&
           counters.invalidRequests == 0;
}

bool SetProviderDisplayRegion(stream::Provider& provider, AppleMusicDisplayRegion region) {
    auto fields = provider.SettingFields();
    const auto found = std::find_if(fields.begin(), fields.end(), [](const auto& field) {
        return field.label == L"Apple Music metadata region";
    });
    if (found == fields.end() || !found->set) return false;
    found->set(static_cast<int>(region));
    return GetAppleMusicDisplayRegion() == region;
}

bool PagedCursor(const stream::ListCursor& cursor, size_t sourceOffset) {
    return cursor.offset == sourceOffset &&
           cursor.value.find(L"opaque=glasswave-paged-" + std::to_wstring(sourceOffset)) !=
               std::wstring::npos;
}

void AppendPagedTracks(const std::shared_ptr<State>& state, const std::vector<stream::Track>& rows) {
    state->pagingTracks.insert(state->pagingTracks.end(), rows.begin(), rows.end());
}

double ElapsedMs(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

Snapshot CaptureSnapshot(stream::Provider& provider) {
    Snapshot snapshot;
    for (const auto& item : provider.Playlists(stream::Shelf::Created))
        snapshot.created.push_back(PlaylistRow(item));
    for (const auto& item : provider.Playlists(stream::Shelf::Saved))
        snapshot.saved.push_back(PlaylistRow(item));
    for (const auto& item : provider.SavedAlbums())
        snapshot.albums.push_back(AlbumRow(item));
    for (const auto& item : provider.FollowedArtists())
        snapshot.artists.push_back(ArtistRow(item));
    if (const auto favorites = provider.CachedFavorites()) {
        for (const auto& track : *favorites)
            snapshot.favorites.push_back(track.id);
    } else {
        snapshot.favorites.push_back(L"<unloaded>");
    }
    return snapshot;
}

bool HasTrack(const std::vector<stream::Track>& tracks, std::wstring_view id) {
    return std::any_of(tracks.begin(), tracks.end(), [&](const auto& track) { return track.id == id; });
}

bool HasPlaylist(const std::vector<stream::Playlist>& rows, std::wstring_view id) {
    return std::any_of(rows.begin(), rows.end(), [&](const auto& playlist) { return playlist.id == id; });
}

bool HasPlaylistName(const std::vector<stream::Playlist>& rows, std::wstring_view id,
                     std::wstring_view name) {
    return std::any_of(rows.begin(), rows.end(),
                       [&](const auto& playlist) { return playlist.id == id && playlist.name == name; });
}

bool HasSavedPlaylistIdentity(stream::Provider& provider, std::wstring_view catalogId,
                              std::wstring_view libraryId) {
    const auto playlists = provider.Playlists(stream::Shelf::Saved);
    return std::any_of(playlists.begin(), playlists.end(), [&](const auto& playlist) {
        return playlist.id == catalogId || playlist.id == libraryId;
    });
}

bool HasArtist(const std::vector<stream::Artist>& rows, std::wstring_view id) {
    return std::any_of(rows.begin(), rows.end(), [&](const auto& artist) { return artist.id == id; });
}

bool HasAlbum(const std::vector<stream::Album>& rows, std::wstring_view id) {
    return std::any_of(rows.begin(), rows.end(), [&](const auto& album) { return album.id == id; });
}

bool HasSavedAlbumIdentity(stream::Provider& provider, std::wstring_view catalogId,
                           std::wstring_view libraryId) {
    const auto albums = provider.SavedAlbums();
    return std::any_of(albums.begin(), albums.end(),
                       [&](const auto& album) { return album.id == catalogId || album.id == libraryId; });
}

stream::Id SongsSection(stream::Provider& provider) {
    const auto sections = provider.Sections();
    const auto found = std::find_if(sections.begin(), sections.end(), [](const auto& item) {
        return item.kind == stream::SectionKind::Tracks;
    });
    return found == sections.end() ? stream::Id() : found->id;
}

bool CatalogSectionPage(const stream::ArtistSection& section, const stream::ListPage& page, bool firstPage) {
    const size_t total = section.id == L"top-songs" ? 4 : 3;
    if (page.more != firstPage || page.next.offset != (firstPage ? 2 : 0)) return false;
    switch (section.rowKind) {
    case stream::ArtistSectionRowKind::Tracks: {
        const auto* rows = std::get_if<std::vector<stream::Track>>(&page.rows);
        if (!rows || rows->size() != (firstPage ? 2 : total - 2)) return false;
        return std::all_of(rows->begin(), rows->end(),
                           [](const stream::Track& row) { return StartsWith(row.id, L"catalog.top."); });
    }
    case stream::ArtistSectionRowKind::Albums: {
        const auto* rows = std::get_if<std::vector<stream::Album>>(&page.rows);
        if (!rows || rows->size() != (firstPage ? 2 : total - 2)) return false;
        const std::wstring prefix = L"catalog." + section.id + L".";
        return std::all_of(rows->begin(), rows->end(),
                           [&prefix](const stream::Album& row) { return StartsWith(row.id, prefix); });
    }
    case stream::ArtistSectionRowKind::Playlists:
        if (section.id != L"featured-playlists") return false;
        {
            const auto* rows = std::get_if<std::vector<stream::Playlist>>(&page.rows);
            return rows && rows->size() == (firstPage ? 2 : total - 2) &&
                   std::all_of(rows->begin(), rows->end(), [](const stream::Playlist& row) {
                       return StartsWith(row.id, L"catalog.playlist.");
                   });
        }
    }
    return false;
}

bool CatalogSearchRowsMatch(const stream::ListKind kind, const stream::ListPage& page, size_t expectedRows,
                            bool expectedMore) {
    if (page.more != expectedMore) return false;
    switch (kind) {
    case stream::ListKind::CatalogSearch: {
        const auto* rows = std::get_if<std::vector<stream::Track>>(&page.rows);
        return rows && rows->size() == expectedRows;
    }
    case stream::ListKind::CatalogSearchArtists: {
        const auto* rows = std::get_if<std::vector<stream::Artist>>(&page.rows);
        return rows && rows->size() == expectedRows;
    }
    case stream::ListKind::CatalogSearchAlbums: {
        const auto* rows = std::get_if<std::vector<stream::Album>>(&page.rows);
        return rows && rows->size() == expectedRows;
    }
    case stream::ListKind::CatalogSearchPlaylists: {
        const auto* rows = std::get_if<std::vector<stream::Playlist>>(&page.rows);
        return rows && rows->size() == expectedRows;
    }
    case stream::ListKind::CatalogSearchTopResults: {
        const auto* rows = std::get_if<std::vector<stream::CatalogSearchItem>>(&page.rows);
        return rows && rows->size() == expectedRows;
    }
    default:
        return false;
    }
}

void ExpectCatalogFailure(const std::shared_ptr<State>& state, stream::Provider& provider,
                          stream::ListQuery query, std::wstring operation, std::function<void()> next) {
    provider.FetchListPage(std::move(query), {}, 2,
                           [state, operation = std::move(operation),
                            next = std::move(next)](stream::ListPage, std::wstring error) mutable {
                               if (!Live(state)) return;
                               if (error.empty()) return Fail(state, operation, L"unexpected_success");
                               AddOperation(state, std::move(operation), true);
                               next();
                           });
}

void RunCatalogRegressionSearch(const std::shared_ptr<State>& state);
void RunCatalogRegressionMixed(const std::shared_ptr<State>& state);
void RunCatalogRegressionSuggestions(const std::shared_ptr<State>& state);

void RunCatalogConcurrentEmpty(const std::shared_ptr<State>& state, stream::Provider* provider) {
    if (!provider) return Fail(state, L"catalog_artist_concurrent_404_coalesced", L"provider_unavailable");
    const std::vector<stream::ArtistSection> sections = {
        {L"featured-albums", L"", stream::ArtistSectionRowKind::Albums},
        {L"full-albums", L"", stream::ArtistSectionRowKind::Albums},
        {L"singles", L"", stream::ArtistSectionRowKind::Albums},
        {L"live-albums", L"", stream::ArtistSectionRowKind::Albums},
    };
    auto callbacks = std::make_shared<size_t>(0);
    auto failed = std::make_shared<bool>(false);
    for (const auto& section : sections) {
        provider->FetchListPage(
            stream::ArtistSectionQuery(kCatalogArtistConcurrentEmpty, section), {}, 2,
            [state, callbacks, failed](stream::ListPage page, std::wstring error) {
                if (!Live(state)) return;
                const size_t rows = std::visit([](const auto& values) { return values.size(); }, page.rows);
                if (!error.empty() || rows != 0 || page.more) *failed = true;
                if (++*callbacks != 4) return;
                if (*failed)
                    return Fail(state, L"catalog_artist_concurrent_404_coalesced",
                                L"concurrent_empty_section_failed");
                const auto mock = ReadMockTestingState();
                if (!mock)
                    return Fail(state, L"catalog_artist_concurrent_404_coalesced", L"mock_state_unavailable");
                const auto catalog = json::GetObject(*mock, L"catalog");
                if (!catalog)
                    return Fail(state, L"catalog_artist_concurrent_404_coalesced",
                                L"catalog_counters_missing");
                const auto roots = json::GetObject(*catalog, L"artistRoots");
                if (!roots || json::GetInt64(*roots, kCatalogArtistConcurrentEmpty, -1) != 1) {
                    return Fail(state, L"catalog_artist_concurrent_404_coalesced",
                                L"artist_root_verification_was_not_coalesced");
                }
                AddOperation(state, L"catalog_artist_concurrent_404_coalesced", true);
                RunCatalogRegressionSearch(state);
            });
    }
}

void RunCatalogErrorCases(const std::shared_ptr<State>& state, stream::Provider* provider) {
    if (!provider) return Fail(state, L"catalog_artist_unknown_404", L"provider_unavailable");
    stream::ListQuery unknown;
    unknown.kind = stream::ListKind::ArtistAlbums;
    unknown.id = kCatalogArtistUnknown;
    unknown.section = L"featured-albums";
    ExpectCatalogFailure(
        state, *provider, unknown, L"catalog_artist_unknown_404", [state, provider, unknown]() mutable {
            auto unauthorized = unknown;
            unauthorized.id = kCatalogArtistUnauthorized;
            ExpectCatalogFailure(
                state, *provider, unauthorized, L"catalog_artist_401",
                [state, provider, unauthorized]() mutable {
                    auto network = unauthorized;
                    network.id = kCatalogArtistNetworkError;
                    ExpectCatalogFailure(
                        state, *provider, network, L"catalog_artist_network_error", [state, provider]() {
                            stream::ListQuery continuation;
                            continuation.kind = stream::ListKind::ArtistAlbums;
                            continuation.id = kCatalogArtistContinuationError;
                            continuation.section = L"full-albums";
                            provider->FetchListPage(
                                continuation, {}, 2,
                                [state, provider, continuation](stream::ListPage page, std::wstring error) {
                                    if (!Live(state)) return;
                                    if (!error.empty() || !page.more || page.next.offset != 2) {
                                        return Fail(state, L"catalog_artist_continuation_404",
                                                    error.empty() ? L"continuation_not_published" : error);
                                    }
                                    provider->FetchListPage(
                                        continuation, page.next, 2,
                                        [state, provider](stream::ListPage, std::wstring error) {
                                            if (!Live(state)) return;
                                            if (error.empty())
                                                return Fail(state, L"catalog_artist_continuation_404",
                                                            L"continuation_404_was_treated_as_empty");
                                            AddOperation(state, L"catalog_artist_continuation_404", true);
                                            RunCatalogConcurrentEmpty(state, provider);
                                        });
                                });
                        });
                });
        });
}

void RunCatalogRegression(const std::shared_ptr<State>& state) {
    auto* provider = Provider(state);
    if (!provider) return;
    provider->Start();
    provider->RefreshLibrary([state](std::wstring error) {
        if (!Live(state)) return;
        if (!error.empty()) return Fail(state, L"catalog_refresh", error);
        auto* provider = Provider(state);
        if (!provider) return;

        const auto sections = provider->ArtistSections(kCatalogArtistAll);
        const std::vector<std::pair<std::wstring, stream::ArtistSectionRowKind>> expected = {
            {L"top-songs", stream::ArtistSectionRowKind::Tracks},
            {L"featured-albums", stream::ArtistSectionRowKind::Albums},
            {L"full-albums", stream::ArtistSectionRowKind::Albums},
            {L"singles", stream::ArtistSectionRowKind::Albums},
            {L"live-albums", stream::ArtistSectionRowKind::Albums},
            {L"appears-on-albums", stream::ArtistSectionRowKind::Albums},
            {L"compilation-albums", stream::ArtistSectionRowKind::Albums},
            {L"featured-playlists", stream::ArtistSectionRowKind::Playlists},
        };
        if (sections.size() != expected.size())
            return Fail(state, L"catalog_artist_sections", L"section_count_mismatch");
        for (size_t index = 0; index < expected.size(); ++index) {
            const auto query = stream::ArtistSectionQuery(kCatalogArtistAll, sections[index]);
            if (sections[index].id != expected[index].first ||
                sections[index].rowKind != expected[index].second || query.section != sections[index].id ||
                query.id != kCatalogArtistAll ||
                provider->ListPolicy(query) != stream::ListCachePolicy::Memory) {
                return Fail(state, L"catalog_artist_sections", L"section_descriptor_mismatch");
            }
            auto explicitMemory = query;
            explicitMemory.cache = stream::ListCachePolicy::Memory;
            auto explicitPersistent = query;
            explicitPersistent.cache = stream::ListCachePolicy::Persistent;
            if (provider->ListPolicy(explicitMemory) != stream::ListCachePolicy::Memory ||
                provider->ListPolicy(explicitPersistent) != stream::ListCachePolicy::Persistent) {
                return Fail(state, L"catalog_list_cache_opt_in", L"explicit_cache_policy_ignored");
            }
        }
        AddOperation(state, L"catalog_artist_sections", true);
        AddOperation(state, L"catalog_list_cache_opt_in", true);
        const std::vector<stream::ListKind> expectedKinds = {
            stream::ListKind::CatalogSearchTopResults, stream::ListKind::CatalogSearch,
            stream::ListKind::CatalogSearchArtists,    stream::ListKind::CatalogSearchAlbums,
            stream::ListKind::CatalogSearchPlaylists,
        };
        if (provider->CatalogSearchKinds() != expectedKinds || !provider->Can(stream::Capability::SearchSuggestions))
            return Fail(state, L"catalog_search_kinds", L"catalog_kind_advertisement_mismatch");
        AddOperation(state, L"catalog_search_kinds", true);

        auto cursors = std::make_shared<std::vector<stream::ListCursor>>(sections.size());
        auto index = std::make_shared<size_t>(0);
        auto loadFirst = std::make_shared<std::function<void()>>();
        *loadFirst = [state, provider, sections, cursors, index, loadFirst] {
            if (*index == sections.size()) {
                *index = 0;
                AddOperation(state, L"catalog_artist_section_pages", true);
                auto loadNext = std::make_shared<std::function<void()>>();
                *loadNext = [state, provider, sections, cursors, index, loadNext] {
                    if (*index == sections.size()) {
                        AddOperation(state, L"catalog_artist_section_continuations", true);
                        stream::ListQuery saturated =
                            stream::ArtistSectionQuery(kCatalogArtistAll, sections.front());
                        provider->FetchListPage(
                            saturated, {}, 100, [state, provider](stream::ListPage page, std::wstring error) {
                                if (!Live(state)) return;
                                const auto* rows = std::get_if<std::vector<stream::Track>>(&page.rows);
                                if (!error.empty() || !rows || rows->size() != 4 || page.more) {
                                    return Fail(state, L"catalog_artist_limit_forwarded",
                                                error.empty() ? L"limit_page_mismatch" : error);
                                }
                                const auto mock = ReadMockTestingState();
                                const auto catalog = mock ? json::GetObject(*mock, L"catalog") : std::nullopt;
                                const auto limits =
                                    catalog ? json::GetArray(*catalog, L"sectionLimits") : std::nullopt;
                                if (!limits || limits->Size() == 0) {
                                    return Fail(state, L"catalog_artist_limit_forwarded",
                                                L"section_limit_evidence_missing");
                                }
                                const auto last = limits->GetAt(limits->Size() - 1);
                                if (!last ||
                                    last.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number ||
                                    last.GetNumber() != 100) {
                                    return Fail(state, L"catalog_artist_limit_forwarded",
                                                L"caller_limit_was_not_forwarded");
                                }
                                AddOperation(state, L"catalog_artist_limit_forwarded", true);

                                auto empty = std::make_shared<std::vector<stream::ArtistSection>>();
                                empty->push_back(
                                    {L"featured-albums", L"", stream::ArtistSectionRowKind::Albums});
                                empty->push_back({L"live-albums", L"", stream::ArtistSectionRowKind::Albums});
                                empty->push_back(
                                    {L"compilation-albums", L"", stream::ArtistSectionRowKind::Albums});
                                empty->push_back(
                                    {L"featured-playlists", L"", stream::ArtistSectionRowKind::Playlists});
                                auto emptyIndex = std::make_shared<size_t>(0);
                                auto emptyNext = std::make_shared<std::function<void()>>();
                                *emptyNext = [state, provider, empty, emptyIndex, emptyNext] {
                                    if (!Live(state)) return;
                                    if (*emptyIndex == empty->size())
                                        return Fail(state, L"catalog_artist_empty_404",
                                                    L"empty_runner_unreachable");
                                    const auto section = (*empty)[*emptyIndex];
                                    provider->FetchListPage(
                                        stream::ArtistSectionQuery(kCatalogArtistEmpty, section), {}, 2,
                                        [state, empty, emptyIndex, emptyNext](stream::ListPage page,
                                                                              std::wstring error) {
                                            if (!Live(state)) return;
                                            const size_t rows = std::visit(
                                                [](const auto& values) { return values.size(); }, page.rows);
                                            if (!error.empty() || rows != 0 || page.more ||
                                                page.total.value_or(0) != 0) {
                                                return Fail(state, L"catalog_artist_empty_404",
                                                            error.empty() ? L"empty_view_failed" : error);
                                            }
                                            if (++*emptyIndex != empty->size()) return (*emptyNext)();
                                            AddOperation(state, L"catalog_artist_empty_404", true);
                                            if (auto* current = Provider(state))
                                                RunCatalogErrorCases(state, current);
                                        });
                                };
                                (*emptyNext)();
                            });
                        return;
                    }
                    const auto& section = sections[*index];
                    provider->FetchListPage(
                        stream::ArtistSectionQuery(kCatalogArtistAll, section), (*cursors)[*index], 2,
                        [state, sections, cursors, index, loadNext, section](stream::ListPage page,
                                                                             std::wstring error) {
                            if (!Live(state)) return;
                            if (!error.empty() || !CatalogSectionPage(section, page, false))
                                return Fail(state, L"catalog_artist_section_continuations",
                                            error.empty() ? L"continuation_page_mismatch" : error);
                            ++*index;
                            (*loadNext)();
                        });
                };
                (*loadNext)();
                return;
            }
            const auto& section = sections[*index];
            provider->FetchListPage(stream::ArtistSectionQuery(kCatalogArtistAll, section), {}, 2,
                                    [state, sections, cursors, index, loadFirst,
                                     section](stream::ListPage page, std::wstring error) {
                                        if (!Live(state)) return;
                                        if (!error.empty() || !CatalogSectionPage(section, page, true)) {
                                            return Fail(state, L"catalog_artist_section_pages",
                                                        error.empty() ? L"first_page_mismatch" : error);
                                        }
                                        (*cursors)[*index] = page.next;
                                        ++*index;
                                        (*loadFirst)();
                                    });
        };
        (*loadFirst)();
    });
}

void RunCatalogRegressionSearch(const std::shared_ptr<State>& state) {
    auto* provider = Provider(state);
    if (!provider) return;
    const std::vector<stream::ListKind> kinds = {
        stream::ListKind::CatalogSearch,
        stream::ListKind::CatalogSearchArtists,
        stream::ListKind::CatalogSearchAlbums,
        stream::ListKind::CatalogSearchPlaylists,
    };
    auto cursors = std::make_shared<std::vector<stream::ListCursor>>(kinds.size());
    auto index = std::make_shared<size_t>(0);
    auto first = std::make_shared<std::function<void()>>();
    *first = [state, provider, kinds, cursors, index, first] {
        if (*index == kinds.size()) {
            *index = 0;
            AddOperation(state, L"catalog_search_categories", true);
            auto next = std::make_shared<std::function<void()>>();
            *next = [state, provider, kinds, cursors, index, next] {
                if (*index == kinds.size()) {
                    AddOperation(state, L"catalog_search_continuations", true);
                    RunCatalogRegressionMixed(state);
                    return;
                }
                stream::ListQuery query;
                query.kind = kinds[*index];
                query.search = L"paged-search";
                provider->FetchListPage(
                    query, (*cursors)[*index], 100,
                    [state, kinds, index, next](stream::ListPage page, std::wstring error) {
                        if (!Live(state)) return;
                        if (!error.empty() || !CatalogSearchRowsMatch(kinds[*index], page, 1, false))
                            return Fail(state, L"catalog_search_continuations",
                                        error.empty() ? L"continuation_row_mismatch" : error);
                        ++*index;
                        (*next)();
                    });
            };
            (*next)();
            return;
        }
        stream::ListQuery query;
        query.kind = kinds[*index];
        query.search = L"paged-search";
        if (provider->ListPolicy(query) != stream::ListCachePolicy::None)
            return Fail(state, L"catalog_search_categories", L"default_cache_not_none");
        provider->FetchListPage(
            query, {}, 100, [state, kinds, cursors, index, first](stream::ListPage page, std::wstring error) {
                if (!Live(state)) return;
                if (!error.empty() || !CatalogSearchRowsMatch(kinds[*index], page, 1, true) ||
                    page.next.offset != 1) {
                    return Fail(state, L"catalog_search_categories",
                                error.empty() ? L"first_search_page_mismatch" : error);
                }
                (*cursors)[*index] = page.next;
                ++*index;
                (*first)();
            });
    };
    (*first)();
}

void RunCatalogRegressionMixed(const std::shared_ptr<State>& state) {
    auto* provider = Provider(state);
    if (!provider) return;
    stream::ListQuery terminal;
    terminal.kind = stream::ListKind::CatalogSearchTopResults;
    terminal.search = L"terminal-mixed";
    provider->FetchListPage(
        terminal, {}, 100, [state, provider, terminal](stream::ListPage page, std::wstring error) {
            if (!Live(state)) return;
            const auto* rows = std::get_if<std::vector<stream::CatalogSearchItem>>(&page.rows);
            bool valid = !error.empty() ? false
                                        : rows && rows->size() == 4 && !page.more && !page.total &&
                                              page.next.value.empty();
            if (valid) {
                const size_t order[] = {0, 1, 2, 3};
                std::set<std::wstring> keys;
                for (size_t index = 0; index < rows->size(); ++index) {
                    valid = valid && (*rows)[index].index() == order[index] &&
                            stream::CatalogSearchItemId((*rows)[index]) == L"991" &&
                            keys.insert(stream::CatalogSearchItemKey((*rows)[index])).second;
                }
            }
            if (!valid)
                return Fail(state, L"catalog_search_mixed_terminal",
                            error.empty() ? L"terminal_mixed_mismatch" : error);
            AddOperation(state, L"catalog_search_mixed_terminal", true);

            stream::ListQuery mixed = terminal;
            mixed.search = L"mixed-rank";
            provider->FetchListPage(
                mixed, {}, 100, [state, provider, mixed](stream::ListPage page, std::wstring error) {
                    if (!Live(state)) return;
                    const auto* rows = std::get_if<std::vector<stream::CatalogSearchItem>>(&page.rows);
                    const size_t order[] = {1, 0, 3, 2};
                    std::set<std::wstring> keys;
                    bool valid = !error.empty()
                                     ? false
                                     : rows && rows->size() == 4 && page.more && page.next.offset == 5;
                    if (valid) {
                        for (size_t index = 0; index < rows->size(); ++index) {
                            valid = valid && (*rows)[index].index() == order[index] &&
                                    stream::CatalogSearchItemId((*rows)[index]) == L"777" &&
                                    keys.insert(stream::CatalogSearchItemKey((*rows)[index])).second;
                        }
                    }
                    if (!valid)
                        return Fail(state, L"catalog_search_mixed_unknowns_continuation",
                                    error.empty() ? L"mixed_unknown_filter_mismatch" : error);
                    provider->FetchListPage(
                        mixed, page.next, 100,
                        [state, mixed](stream::ListPage continuation, std::wstring error) {
                            if (!Live(state)) return;
                            const auto* rows =
                                std::get_if<std::vector<stream::CatalogSearchItem>>(&continuation.rows);
                            if (!error.empty() || !rows || rows->size() != 2 || continuation.more ||
                                (*rows)[0].index() != 1 || (*rows)[1].index() != 0 ||
                                stream::CatalogSearchItemId((*rows)[0]) != L"778" ||
                                stream::CatalogSearchItemId((*rows)[1]) != L"778") {
                                return Fail(state, L"catalog_search_mixed_unknowns_continuation",
                                            error.empty() ? L"mixed_continuation_mismatch" : error);
                            }
                            AddOperation(state, L"catalog_search_mixed_unknowns_continuation", true);

                            stream::ListQuery missing = mixed;
                            missing.kind = stream::ListKind::CatalogSearchArtists;
                            missing.search = L"missing-category";
                            auto* current = Provider(state);
                            if (!current) return;
                            current->FetchListPage(
                                missing, {}, 100, [state](stream::ListPage page, std::wstring error) {
                                    if (!Live(state)) return;
                                    const auto* rows = std::get_if<std::vector<stream::Artist>>(&page.rows);
                                    if (!error.empty() || !rows || !rows->empty() || page.more ||
                                        page.total.value_or(1) != 0)
                                        return Fail(state, L"catalog_search_missing_category",
                                                    error.empty() ? L"missing_category_not_empty" : error);
                                    AddOperation(state, L"catalog_search_missing_category", true);
                                    RunCatalogRegressionSuggestions(state);
                                });
                        });
                });
        });
}

std::wstring SuggestionOrderKey(const stream::Provider::CatalogSearchSuggestionItem& item) {
    if (item.index() == 0) {
        const auto& term = std::get<stream::Provider::CatalogSearchSuggestionTerm>(item);
        return L"term(" + term.searchTerm + L"|" + term.displayTerm + L")";
    }
    const auto& result = std::get<stream::CatalogSearchItem>(item);
    switch (stream::CatalogSearchItemKindOf(result)) {
    case stream::CatalogSearchItemKind::Track:
        return L"track:" + stream::CatalogSearchItemId(result);
    case stream::CatalogSearchItemKind::Artist:
        return L"artist:" + stream::CatalogSearchItemId(result);
    case stream::CatalogSearchItemKind::Album:
        return L"album:" + stream::CatalogSearchItemId(result);
    case stream::CatalogSearchItemKind::Playlist:
        return L"playlist:" + stream::CatalogSearchItemId(result);
    }
    return L"unknown";
}

std::wstring JoinSuggestionOrder(const stream::Provider::CatalogSearchSuggestions& suggestions) {
    std::wstring order;
    for (const auto& item : suggestions.ordered) {
        if (!order.empty()) order += L',';
        order += SuggestionOrderKey(item);
    }
    return order;
}

void RunCatalogRegressionSuggestions(const std::shared_ptr<State>& state) {
    auto* provider = Provider(state);
    if (!provider) return;
    stream::Provider::CatalogSearchSuggestionQuery ordered;
    ordered.term = L"suggest-order";
    provider->FetchCatalogSearchSuggestions(
        std::move(ordered),
        [state, provider](stream::Provider::CatalogSearchSuggestions suggestions, std::wstring error) {
            if (!Live(state)) return;
            const bool expectedTermOuter[] = {true, false, true, false, false, false};
            const stream::CatalogSearchItemKind expectedResultKinds[] = {
                stream::CatalogSearchItemKind::Track,    stream::CatalogSearchItemKind::Artist,
                stream::CatalogSearchItemKind::Track,    stream::CatalogSearchItemKind::Album,
                stream::CatalogSearchItemKind::Playlist, stream::CatalogSearchItemKind::Track};
            const std::wstring expectedSearchTerms[] = {L"search-one", L"", L"search-two", L"", L"", L""};
            const std::wstring expectedDisplayTerms[] = {L"Display One", L"", L"Display Two", L"", L"", L""};
            const std::wstring expectedIds[] = {L"", L"777", L"", L"888", L"999", L"777"};
            bool valid = error.empty() && suggestions.ordered.size() == 6;
            if (valid) {
                for (size_t index = 0; index < suggestions.ordered.size(); ++index) {
                    const auto& item = suggestions.ordered[index];
                    if (item.index() == 0) {
                        const auto& term = std::get<stream::Provider::CatalogSearchSuggestionTerm>(item);
                        valid = valid && expectedTermOuter[index] &&
                                term.searchTerm == expectedSearchTerms[index] &&
                                term.displayTerm == expectedDisplayTerms[index];
                    } else {
                        const auto& result = std::get<stream::CatalogSearchItem>(item);
                        valid = valid && !expectedTermOuter[index] &&
                                stream::CatalogSearchItemKindOf(result) == expectedResultKinds[index] &&
                                stream::CatalogSearchItemId(result) == expectedIds[index];
                    }
                }
            }
            if (!valid)
                return Fail(state, L"catalog_suggestions_ordered",
                            error.empty()
                                ? L"suggestion_order_mismatch(actual=" + JoinSuggestionOrder(suggestions) +
                                      L";expected=term(search-one|Display One),artist:777,"
                                      L"term(search-two|Display Two),album:888,playlist:999,track:777)"
                                : error);
            AddOperation(state, L"catalog_suggestions_ordered", true);

            auto cancelledCalled = std::make_shared<std::atomic_bool>(false);
            stream::Provider::CatalogSearchSuggestionQuery cancelled;
            cancelled.term = L"suggest-canceled";
            cancelled.cancelled = std::make_shared<std::atomic_bool>(true);
            provider->FetchCatalogSearchSuggestions(
                std::move(cancelled),
                [cancelledCalled](stream::Provider::CatalogSearchSuggestions, std::wstring) {
                    cancelledCalled->store(true, std::memory_order_release);
                });
            PostAfter(state, 500, [state, cancelledCalled] {
                if (cancelledCalled->load(std::memory_order_acquire))
                    return Fail(state, L"catalog_suggestions_cancel",
                                L"cancelled_suggestion_callback_was_delivered");
                AddOperation(state, L"catalog_suggestions_cancel", true);
                auto* provider = Provider(state);
                if (!provider) return;
                if (!SetProviderDisplayRegion(*provider, AppleMusicDisplayRegion::Japan))
                    return Fail(state, L"catalog_suggestions_region_stale", L"could_not_select_region");
                auto regionCalled = std::make_shared<std::atomic_bool>(false);
                stream::Provider::CatalogSearchSuggestionQuery region;
                region.term = L"suggest-stale-region";
                provider->FetchCatalogSearchSuggestions(
                    std::move(region),
                    [regionCalled](stream::Provider::CatalogSearchSuggestions, std::wstring) {
                        regionCalled->store(true, std::memory_order_release);
                    });
                PostAfter(state, 150, [state] {
                    auto* provider = Provider(state);
                    if (provider) SetProviderDisplayRegion(*provider, AppleMusicDisplayRegion::China);
                });
                PostAfter(state, 1050, [state, regionCalled] {
                    if (regionCalled->load(std::memory_order_acquire))
                        return Fail(state, L"catalog_suggestions_region_stale",
                                    L"stale_region_suggestion_was_delivered");
                    AddOperation(state, L"catalog_suggestions_region_stale", true);
                    auto* provider = Provider(state);
                    if (!provider) return;
                    auto accountCalled = std::make_shared<std::atomic_bool>(false);
                    stream::Provider::CatalogSearchSuggestionQuery account;
                    account.term = L"suggest-stale-account";
                    provider->FetchCatalogSearchSuggestions(
                        std::move(account),
                        [accountCalled](stream::Provider::CatalogSearchSuggestions, std::wstring) {
                            accountCalled->store(true, std::memory_order_release);
                        });
                    PostAfter(state, 150, [state] {
                        if (auto* provider = Provider(state)) provider->SignOut();
                    });
                    PostAfter(state, 1050, [state, accountCalled] {
                        if (accountCalled->load(std::memory_order_acquire))
                            return Fail(state, L"catalog_suggestions_account_stale",
                                        L"stale_account_suggestion_was_delivered");
                        AddOperation(state, L"catalog_suggestions_account_stale", true);
                        Finish(state);
                    });
                });
            });
        });
}

void RunStartupSeed(const std::shared_ptr<State>& state) {
    auto* provider = Provider(state);
    if (!provider) return;
    provider->Start();
    provider->RefreshLibrary([state](std::wstring error) {
        if (!Live(state)) return;
        if (!error.empty()) return Fail(state, L"startup_seed_list", error);
        auto* provider = Provider(state);
        if (!provider) return;
        auto completed = std::make_shared<std::array<bool, 2>>();
        auto persisting = std::make_shared<bool>(false);
        auto finishIfComplete = [state, completed, persisting] {
            if (!Live(state) || !(*completed)[0] || !(*completed)[1]) return;
            if (*persisting || state->startupSessions.size() < 2) return;
            *persisting = true;
            auto saved = std::make_shared<std::array<bool, 3>>(std::array<bool, 3>{false, false, true});
            auto finished = std::make_shared<bool>(false);
            auto finishSaved = [state, saved, finished] {
                if (!Live(state) || !(*saved)[0] || !(*saved)[1] || *finished) return;
                *finished = true;
                if (!(*saved)[2])
                    return Fail(state, L"startup_seed_list", L"list_cache_persistence_failed");
                AddOperation(state, L"startup_seed_list", true);
                AddOperation(state, L"startup_seed_favorite", true);
                Finish(state);
            };
            PostAfter(state, 8000, [state, finished] {
                if (!Live(state) || *finished) return;
                *finished = true;
                Fail(state, L"startup_seed_list", L"list_cache_persistence_timeout");
            });
            state->startupSessions[0]->WaitForPersistentSave([saved, finishSaved](bool success) {
                (*saved)[0] = true;
                (*saved)[2] = (*saved)[2] && success;
                finishSaved();
            });
            state->startupSessions[1]->WaitForPersistentSave([saved, finishSaved](bool success) {
                (*saved)[1] = true;
                (*saved)[2] = (*saved)[2] && success;
                finishSaved();
            });
        };
        auto playlist = stream::ListSession::Open(
            *provider, StartupPlaylistQuery(*state),
            [state, completed, finishIfComplete](const stream::ListState& current) {
                if (!Live(state) || (*completed)[0]) return;
                if (!current.error.empty()) return Fail(state, L"startup_seed_list", current.error);
                const size_t rows =
                    std::visit([](const auto& values) { return values.size(); }, current.page.rows);
                if (!current.complete || rows == 0) return;
                (*completed)[0] = true;
                finishIfComplete();
            },
            stream::ListLoadMode::All);
        auto favorite = stream::ListSession::Open(
            *provider, StartupFavoriteQuery(),
            [state, completed, finishIfComplete](const stream::ListState& current) {
                if (!Live(state) || (*completed)[1]) return;
                if (!current.error.empty()) return Fail(state, L"startup_seed_favorite", current.error);
                const size_t rows =
                    std::visit([](const auto& values) { return values.size(); }, current.page.rows);
                if (!current.complete || rows == 0) return;
                (*completed)[1] = true;
                finishIfComplete();
            },
            stream::ListLoadMode::All);
        state->startupSessions.push_back(playlist);
        state->startupSessions.push_back(favorite);
        playlist->RequestNext();
        favorite->RequestNext();
    });
}

void RunStartupRestart(const std::shared_ptr<State>& state) {
    auto* provider = Provider(state);
    if (!provider) return;
    try {
        state->startup.sourceIdentity = BridgeSourceIdentity();
    } catch (const std::exception& exception) {
        return Fail(state, L"startup_origin_bound", Wide(exception.what()));
    }
    if (state->startup.sourceIdentity.rfind("external:http://127.0.0.1:", 0) != 0)
        return Fail(state, L"startup_origin_bound", L"mock_source_identity_was_not_external");
    AddOperation(state, L"startup_origin_bound", true);

    provider->Start();
    const std::wstring playlist = StartupPlaylistId(*state, L'a');
    const bool cachedSidebar = HasPlaylist(provider->Playlists(stream::Shelf::Created), playlist);
    if (state->scenario == "startup-cache-origin") {
        PostAfter(state, 180, [state, playlist, cachedSidebar] {
            auto* provider = Provider(state);
            const auto fixture = ReadStartupFixture();
            if (!provider || !fixture)
                return Fail(state, L"startup_origin_isolated", L"startup_fixture_unavailable");
            const bool authCompleted = json::GetBool(*fixture, L"authCompleted");
            const bool foreignPlaylist = HasPlaylist(provider->Playlists(stream::Shelf::Created), playlist);
            if (cachedSidebar || foreignPlaylist || authCompleted ||
                json::GetInt64(*fixture, L"listRequests", -1) != 0 ||
                json::GetInt64(*fixture, L"enrichmentRequests", -1) != 0) {
                return Fail(state, L"startup_origin_isolated",
                            L"foreign_origin_cache_or_early_list_work_was_visible");
            }
            auto attempts = std::make_shared<size_t>(0);
            auto poll = std::make_shared<std::function<void()>>();
            *poll = [state, provider, attempts, poll] {
                if (!Live(state)) return;
                if (!provider->AccountVerified()) {
                    if (++*attempts >= 80)
                        return Fail(state, L"startup_origin_isolated",
                                    L"account_verification_was_not_published");
                    PostAfter(state, 50, [poll] { (*poll)(); });
                    return;
                }
                auto leaked = std::make_shared<bool>(false);
                auto session = stream::ListSession::Open(
                    *provider, StartupPlaylistQuery(*state),
                    [state, leaked](const stream::ListState& current) {
                        if (!Live(state)) return;
                        if (current.fromCache) *leaked = true;
                    },
                    stream::ListLoadMode::OnDemand);
                auto favorite = stream::ListSession::Open(
                    *provider, StartupFavoriteQuery(),
                    [state](const stream::ListState&) {
                        if (!Live(state)) return;
                    },
                    stream::ListLoadMode::OnDemand);
                state->startupSessions.push_back(session);
                state->startupSessions.push_back(favorite);
                session->RequestNext();
                favorite->RequestNext();
                PostAfter(state, 100, [state, leaked, favorite] {
                    if (*leaked)
                        return Fail(state, L"startup_origin_isolated",
                                    L"foreign_origin_list_prefix_was_published");
                    state->startup.originIsolation = true;
                    AddOperation(state, L"startup_origin_isolated", true);

                    const auto fixture = ReadStartupFixture();
                    const auto current = favorite->State();
                    const size_t rows =
                        std::visit([](const auto& values) { return values.size(); }, current.page.rows);
                    const bool pending = fixture &&
                                         !json::GetBool(*fixture, L"playlistCollectionCompleted") &&
                                         !current.fromCache && rows == 0 && current.error.empty();
                    state->startup.favoriteColdPendingBeforeDiscovery = pending;
                    if (!pending)
                        return Fail(state, L"startup_favorite_cold_pending",
                                    L"cold_favorite_published_error_or_empty_before_discovery");
                    AddOperation(state, L"startup_favorite_cold_pending", true);

                    auto attempts = std::make_shared<size_t>(0);
                    auto wait = std::make_shared<std::function<void()>>();
                    *wait = [state, favorite, attempts, wait] {
                        if (!Live(state)) return;
                        const auto fixture = ReadStartupFixture();
                        if (fixture && json::GetBool(*fixture, L"playlistCollectionCompleted")) {
                            PostAfter(state, 150, [state, favorite] {
                                const auto fixture = ReadStartupFixture();
                                const auto current = favorite->State();
                                const size_t rows = std::visit(
                                    [](const auto& values) { return values.size(); }, current.page.rows);
                                const bool pageReady =
                                    fixture && current.error.empty() && rows > 0 &&
                                    json::GetInt64(*fixture, L"favoritePageRequests", 0) > 0 &&
                                    !json::GetBool(*fixture, L"albumCollectionCompleted") &&
                                    !json::GetBool(*fixture, L"artistCollectionCompleted") &&
                                    !json::GetBool(*fixture, L"drmCompleted");
                                state->startup.favoriteColdPageAfterDiscovery = pageReady;
                                if (!pageReady)
                                    return Fail(state, L"startup_favorite_after_discovery",
                                                L"cold_favorite_waited_for_unrelated_membership_or_drm");
                                AddOperation(state, L"startup_favorite_after_discovery", true);
                                Finish(state);
                            });
                            return;
                        }
                        if (++*attempts >= 80)
                            return Fail(state, L"startup_favorite_after_discovery",
                                        L"playlist_target_discovery_was_not_observed");
                        PostAfter(state, 50, [wait] { (*wait)(); });
                    };
                    (*wait)();
                });
            };
            (*poll)();
        });
        return;
    }

    auto cachedCaptured = std::make_shared<bool>(false);
    const auto query = StartupPlaylistQuery(*state);
    if (state->scenario == "startup-cache-same") {
        PostAfter(state, 8000, [state, provider, cachedSidebar, cachedCaptured] {
            if (!Live(state) || *cachedCaptured) return;
            auto& startup = state->startup;
            startup.cachedSidebarBeforeAuth = cachedSidebar;
            startup.restoringBeforeAuth = provider->SessionRestoring();
            startup.accountUnverifiedBeforeAuth = !provider->AccountVerified();
            if (const auto fixture = ReadStartupFixture()) {
                startup.listRequestsBeforeAuth =
                    static_cast<size_t>(json::GetInt64(*fixture, L"listRequests", -1));
                startup.enrichmentRequestsBeforeAuth =
                    static_cast<size_t>(json::GetInt64(*fixture, L"enrichmentRequests", -1));
            }
            Fail(state, L"startup_cached_before_auth", L"cache_prefix_callback_timed_out");
        });
    }
    auto session = stream::ListSession::Open(
        *provider, query,
        [state, provider, query, playlist, cachedSidebar, cachedCaptured](const stream::ListState& current) {
            if (!Live(state) || *cachedCaptured) return;
            if (!current.error.empty()) return Fail(state, L"startup_cached_before_auth", current.error);
            if (!current.fromCache) return;
            const size_t rows =
                std::visit([](const auto& values) { return values.size(); }, current.page.rows);
            const auto fixture = ReadStartupFixture();
            if (!fixture) return Fail(state, L"startup_cached_before_auth", L"startup_fixture_unavailable");
            auto& result = state->startup;
            result.cachedRows = rows;
            result.cachedSidebarBeforeAuth = cachedSidebar;
            result.cachedListBeforeAuth = current.fromCache && rows > 0;
            result.restoringBeforeAuth = provider->SessionRestoring();
            result.accountUnverifiedBeforeAuth = !provider->AccountVerified();
            result.listRequestsBeforeAuth =
                static_cast<size_t>(json::GetInt64(*fixture, L"listRequests", -1));
            result.enrichmentRequestsBeforeAuth =
                static_cast<size_t>(json::GetInt64(*fixture, L"enrichmentRequests", -1));
            const bool authCompleted = json::GetBool(*fixture, L"authCompleted");
            if (!result.cachedSidebarBeforeAuth || !result.cachedListBeforeAuth ||
                !result.restoringBeforeAuth || !result.accountUnverifiedBeforeAuth || authCompleted ||
                result.listRequestsBeforeAuth != 0 || result.enrichmentRequestsBeforeAuth != 0) {
                return Fail(state, L"startup_cached_before_auth", L"cache_or_network_timing_contract_failed");
            }
            *cachedCaptured = true;
            AddOperation(state, L"startup_cached_before_auth", true);

            if (state->scenario == "startup-cache-transient") {
                auto attempts = std::make_shared<size_t>(0);
                auto poll = std::make_shared<std::function<void()>>();
                *poll = [state, provider, playlist, attempts, poll] {
                    if (!Live(state)) return;
                    const auto fixture = ReadStartupFixture();
                    if (fixture && json::GetBool(*fixture, L"authCompleted") &&
                        !provider->AccountVerified()) {
                        const bool preserved =
                            provider->SessionRestoring() &&
                            HasPlaylist(provider->Playlists(stream::Shelf::Created), playlist);
                        state->startup.transientPreserved = preserved;
                        if (!preserved)
                            return Fail(state, L"startup_transient_preserves",
                                        L"cached_navigation_was_lost_after_transient_failure");
                        AddOperation(state, L"startup_transient_preserves", true);
                        Finish(state);
                        return;
                    }
                    if (++*attempts >= 80)
                        return Fail(state, L"startup_transient_preserves",
                                    L"transient_account_result_not_observed");
                    PostAfter(state, 50, [poll] { (*poll)(); });
                };
                (*poll)();
                return;
            }

            auto attempts = std::make_shared<size_t>(0);
            auto poll = std::make_shared<std::function<void()>>();
            *poll = [state, provider, query, playlist, attempts, poll] {
                if (!Live(state)) return;
                if (!provider->AccountVerified()) {
                    if (++*attempts >= 80)
                        return Fail(state, L"startup_auth_before_membership",
                                    L"account_verification_was_not_published");
                    PostAfter(state, 50, [poll] { (*poll)(); });
                    return;
                }
                const auto fixture = ReadStartupFixture();
                if (!fixture)
                    return Fail(state, L"startup_auth_before_membership", L"startup_fixture_unavailable");
                const bool membershipComplete = json::GetBool(*fixture, L"playlistCollectionCompleted") &&
                                                json::GetBool(*fixture, L"albumCollectionCompleted") &&
                                                json::GetBool(*fixture, L"artistCollectionCompleted") &&
                                                json::GetBool(*fixture, L"drmCompleted");
                if (!state->startup.favoriteCachedBeforeAuth &&
                    (state->scenario == "startup-cache-same" || state->scenario == "startup-cache-b" ||
                     state->scenario == "startup-cache-transient")) {
                    return Fail(state, L"startup_cached_before_auth",
                                L"cached_favorite_binding_was_not_visible");
                }
                state->startup.authPublishedBeforeMembership = !membershipComplete;
                if (!state->startup.authPublishedBeforeMembership)
                    return Fail(state, L"startup_auth_before_membership",
                                L"auth_waited_for_global_membership_or_drm");
                AddOperation(state, L"startup_auth_before_membership", true);

                if (state->scenario == "startup-cache-b") {
                    const bool revoked = !HasPlaylist(provider->Playlists(stream::Shelf::Created), playlist);
                    state->startup.differentAccountRevokedA = revoked;
                    if (!revoked)
                        return Fail(state, L"startup_b_revokes_a_at_auth",
                                    L"account_b_still_exposed_account_a_navigation");
                    AddOperation(state, L"startup_b_revokes_a_at_auth", true);
                    auto favorite = stream::ListSession::Open(
                        *provider, StartupFavoriteQuery(),
                        [state](const stream::ListState&) {
                            if (!Live(state)) return;
                        },
                        stream::ListLoadMode::OnDemand);
                    state->startupSessions.push_back(favorite);
                    favorite->RequestNext();
                    PostAfter(state, 100, [state, favorite] {
                        const auto fixture = ReadStartupFixture();
                        const auto current = favorite->State();
                        const size_t rows =
                            std::visit([](const auto& values) { return values.size(); }, current.page.rows);
                        const bool clearedBinding =
                            fixture && !json::GetBool(*fixture, L"playlistCollectionCompleted") &&
                            !current.fromCache && rows == 0 && current.error.empty();
                        state->startup.favoriteBindingClearedOnAccountChange = clearedBinding;
                        if (!clearedBinding)
                            return Fail(state, L"startup_favorite_binding_cleared",
                                        L"account_change_published_cached_favorite_target");
                        AddOperation(state, L"startup_favorite_binding_cleared", true);

                        auto attempts = std::make_shared<size_t>(0);
                        auto wait = std::make_shared<std::function<void()>>();
                        *wait = [state, favorite, attempts, wait] {
                            if (!Live(state)) return;
                            const auto fixture = ReadStartupFixture();
                            if (fixture && json::GetBool(*fixture, L"playlistCollectionCompleted")) {
                                PostAfter(state, 150, [state, favorite] {
                                    const auto final = favorite->State();
                                    const auto fixture = ReadStartupFixture();
                                    const size_t rows = std::visit(
                                        [](const auto& values) { return values.size(); }, final.page.rows);
                                    const bool empty =
                                        fixture && final.error.empty() && !final.loading && rows == 0 &&
                                        !json::GetBool(*fixture, L"albumCollectionCompleted") &&
                                        !json::GetBool(*fixture, L"artistCollectionCompleted") &&
                                        !json::GetBool(*fixture, L"drmCompleted");
                                    state->startup.favoriteNoFavoriteEmptyAfterDiscovery = empty;
                                    if (!empty)
                                        return Fail(state, L"startup_favorite_no_favorite_empty",
                                                    L"confirmed_absence_was_not_published_as_empty");
                                    AddOperation(state, L"startup_favorite_no_favorite_empty", true);
                                    Finish(state);
                                });
                                return;
                            }
                            if (++*attempts >= 80)
                                return Fail(state, L"startup_favorite_no_favorite_empty",
                                            L"playlist_target_discovery_was_not_observed");
                            PostAfter(state, 50, [wait] { (*wait)(); });
                        };
                        (*wait)();
                    });
                    return;
                }
                if (state->scenario == "startup-cache-signed-out") {
                    const bool cleared =
                        !provider->SignedIn() && provider->Playlists(stream::Shelf::Created).empty();
                    state->startup.signedOutCleared = cleared;
                    if (!cleared)
                        return Fail(state, L"startup_signedout_clears",
                                    L"signed_out_startup_retained_private_navigation");
                    AddOperation(state, L"startup_signedout_clears", true);
                    Finish(state);
                    return;
                }

                auto session = state->startupSessions.back();
                auto favorite = state->startupSessions[state->startupSessions.size() - 2];
                session->Refresh();
                favorite->Refresh();
                PostAfter(state, 120, [state, provider, query, playlist, session, favorite] {
                    const auto fixture = ReadStartupFixture();
                    if (!fixture)
                        return Fail(state, L"startup_playlist_page_after_auth",
                                    L"startup_fixture_unavailable");
                    const auto current = session->State();
                    const size_t rows =
                        std::visit([](const auto& values) { return values.size(); }, current.page.rows);
                    const bool earlyPage =
                        json::GetInt64(*fixture, L"playlistPageAfterAuthBeforeMembership", 0) > 0;
                    const bool favoriteEarlyPage =
                        json::GetInt64(*fixture, L"favoritePageAfterAuthBeforeMembership", 0) > 0;
                    state->startup.playlistPageAfterAuthBeforeMembership = earlyPage;
                    state->startup.favoriteWarmPageAfterAuth = favoriteEarlyPage;
                    state->startup.sameQueryIdentity =
                        query.kind == stream::ListKind::PlaylistTracks && query.id == playlist;
                    state->startup.sameAccountPrefixPreserved = rows > 0;
                    if (!earlyPage || !favoriteEarlyPage)
                        return Fail(state, L"startup_playlist_page_after_auth",
                                    L"known_playlist_page_waited_for_global_membership_or_drm");
                    AddOperation(state, L"startup_playlist_page_after_auth", true);
                    AddOperation(state, L"startup_favorite_warm_page_after_auth", true);
                    if (!state->startup.sameQueryIdentity || !state->startup.sameAccountPrefixPreserved ||
                        !HasPlaylist(provider->Playlists(stream::Shelf::Created), playlist)) {
                        return Fail(state, L"startup_same_identity_preserved",
                                    L"same_account_restart_reset_route_or_prefix");
                    }
                    AddOperation(state, L"startup_same_identity_preserved", true);
                    Finish(state);
                });
            };
            (*poll)();
        },
        stream::ListLoadMode::All);
    auto favorite = stream::ListSession::Open(
        *provider, StartupFavoriteQuery(),
        [state](const stream::ListState& current) {
            if (!Live(state) || !current.fromCache) return;
            const size_t rows =
                std::visit([](const auto& values) { return values.size(); }, current.page.rows);
            if (rows > 0) state->startup.favoriteCachedBeforeAuth = true;
        },
        stream::ListLoadMode::All);
    state->startupSessions.push_back(session);
    state->startupSessions.push_back(favorite);
    session->RequestNext();
    favorite->RequestNext();
}

void RunStartupRegression(const std::shared_ptr<State>& state) {
    if (state->scenario == "startup-cache-seed")
        RunStartupSeed(state);
    else
        RunStartupRestart(state);
}

void Continue(const std::shared_ptr<State>& state, unsigned step);

void ControlMock(const wchar_t* path) {
    abort_callback_impl abort;
    Request(path, abort, L"POST", "{}");
}

bool PlaylistAuthorityIsFalse(stream::Provider& provider) {
    const auto catalogState = provider.PlaylistSavedState(kMutationPlaylistCatalog);
    const auto libraryState = provider.PlaylistSavedState(kMutationPlaylistLibrary);
    const auto detail = provider.FindPlaylist(kMutationPlaylistCatalog);
    return catalogState.has_value() && !*catalogState && libraryState.has_value() && !*libraryState &&
           detail && !detail->saved &&
           !HasPlaylist(provider.Playlists(stream::Shelf::Saved), kMutationPlaylistCatalog) &&
           !HasPlaylist(provider.Playlists(stream::Shelf::Saved), kMutationPlaylistLibrary);
}

bool PlaylistAuthorityCatalogIsTrue(stream::Provider& provider) {
    const auto catalogState = provider.PlaylistSavedState(kMutationPlaylistCatalog);
    const auto detail = provider.FindPlaylist(kMutationPlaylistCatalog);
    return catalogState.has_value() && *catalogState && detail && detail->saved &&
           HasPlaylist(provider.Playlists(stream::Shelf::Saved), kMutationPlaylistCatalog);
}

bool PlaylistAuthorityIsTrue(stream::Provider& provider) {
    const auto catalogState = provider.PlaylistSavedState(kMutationPlaylistCatalog);
    const auto libraryState = provider.PlaylistSavedState(kMutationPlaylistLibrary);
    const auto detail = provider.FindPlaylist(kMutationPlaylistCatalog);
    const auto saved = provider.Playlists(stream::Shelf::Saved);
    return catalogState.has_value() && *catalogState && libraryState.has_value() && *libraryState && detail &&
           detail->saved &&
           (HasPlaylist(saved, kMutationPlaylistCatalog) || HasPlaylist(saved, kMutationPlaylistLibrary));
}

std::wstring PlaylistAuthorityTrueFailure(stream::Provider& provider) {
    const auto catalogState = provider.PlaylistSavedState(kMutationPlaylistCatalog);
    const auto libraryState = provider.PlaylistSavedState(kMutationPlaylistLibrary);
    const auto detail = provider.FindPlaylist(kMutationPlaylistCatalog);
    const auto stateText = [](const std::optional<bool>& value) {
        return std::wstring(value ? (*value ? L"true" : L"false") : L"unknown");
    };
    return std::wstring(L"catalog=") + stateText(catalogState) + L";library=" + stateText(libraryState) +
           L";detail=" + std::wstring(detail ? (detail->saved ? L"true" : L"false") : L"missing") +
           L";shelf=" +
           std::wstring(
               HasPlaylist(provider.Playlists(stream::Shelf::Saved), kMutationPlaylistCatalog) ||
                       HasPlaylist(provider.Playlists(stream::Shelf::Saved), kMutationPlaylistLibrary)
                   ? L"present"
                   : L"absent");
}

void VerifyPlaylistAuthorityPresent(const std::shared_ptr<State>& state, std::wstring error);

void VerifyPlaylistAuthorityAbsent(const std::shared_ptr<State>& state, std::wstring error,
                                   unsigned retries) {
    if (!Live(state)) return;
    if (!error.empty()) return Fail(state, L"playlist_saved_authority_barrier", error);
    const auto counters = ReadPlaylistAuthorityFixtureCounters();
    if (!counters || !counters->staleServed)
        return Fail(state, L"playlist_saved_authority_barrier", L"stale_snapshot_not_served");
    auto* provider = Provider(state);
    if (!provider || !PlaylistAuthorityIsFalse(*provider))
        return Fail(state, L"playlist_saved_authority_barrier",
                    L"stale_present_snapshot_overrode_remove_ack");
    if (!counters->absentServed) {
        if (retries == 0)
            return Fail(state, L"playlist_saved_authority_barrier", L"matching_absent_snapshot_not_served");
        provider->RefreshLibrary([state, retries](std::wstring refreshError) {
            VerifyPlaylistAuthorityAbsent(state, std::move(refreshError), retries - 1);
        });
        return;
    }
    try {
        ControlMock(L"/api/v1/glasswave/testing/set-playlist-external-present");
    } catch (const std::exception& exception) {
        return Fail(state, L"playlist_saved_authority_barrier", Wide(exception.what()));
    }
    provider->RefreshLibrary([state](std::wstring refreshError) {
        VerifyPlaylistAuthorityPresent(state, std::move(refreshError));
    });
}

void VerifyPlaylistAuthorityPresent(const std::shared_ptr<State>& state, std::wstring error) {
    if (!Live(state)) return;
    if (!error.empty()) return Fail(state, L"playlist_saved_authority_barrier", error);
    const auto counters = ReadPlaylistAuthorityFixtureCounters();
    auto* provider = Provider(state);
    if (!counters || !counters->externalPresent || !provider || !PlaylistAuthorityIsTrue(*provider)) {
        return Fail(state, L"playlist_saved_authority_barrier",
                    provider ? std::wstring(L"external_readd_snapshot_was_not_accepted(") +
                                   PlaylistAuthorityTrueFailure(*provider) + L")"
                             : L"external_readd_provider_missing");
    }
    AddOperation(state, L"playlist_saved_authority_barrier", true);
    Continue(state, 53);
}

void MaybeContinueAccountSwitch(const std::shared_ptr<State>& state) {
    if (Live(state) && state->accountSwitchCompleted && state->accountOldRefreshCompleted)
        Continue(state, 55);
}

void Continue(const std::shared_ptr<State>& state, unsigned step) {
    auto* provider = Provider(state);
    if (!provider) return;
    if (step == 0) {
        if (!SetProviderDisplayRegion(*provider, AppleMusicDisplayRegion::Account))
            return Fail(state, L"fixture_display_region", L"fixture_display_region_failed");
        state->fixtureDisplayRegion = GetAppleMusicDisplayRegion();
        provider->Start();
        provider->RefreshLibrary([state](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty()) return Fail(state, L"refresh", error);
            AddOperation(state, L"refresh", true);
            Continue(state, 1);
        });
    } else if (step == 1) {
        provider->FetchFavorites([state](std::vector<stream::Track>, std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty()) return Fail(state, L"fetch_favorites_initial", L"provider_read_failed");
            AddOperation(state, L"fetch_favorites_initial", true);
            Continue(state, 2);
        });
    } else if (step == 2) {
        const stream::Id section = SongsSection(*provider);
        if (section.empty()) return Fail(state, L"fetch_library_songs", L"library_songs_section_missing");
        provider->FetchSectionTracks(
            section, L"", [state](std::vector<stream::Track> tracks, std::wstring error) {
                if (!Live(state)) return;
                if (!error.empty() || !HasTrack(tracks, kSongCatalog))
                    return Fail(state, L"fetch_library_songs", L"library_songs_read_failed");
                AddOperation(state, L"fetch_library_songs", true);
                Continue(state, 3);
            });
    } else if (step == 3) {
        provider->SetFavorite(kSongCatalog, true, [state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() || !apple->IsFavorite(kSongCatalog))
                return Fail(state, L"favorite_add", L"favorite_add_failed");
            AddOperation(state, L"favorite_add", true);
            Continue(state, 4);
        });
    } else if (step == 4) {
        provider->SetFavorite(kSongCatalog, false, [state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() || apple->IsFavorite(kSongCatalog))
                return Fail(state, L"favorite_remove", L"favorite_remove_failed");
            AddOperation(state, L"favorite_remove", true);
            Continue(state, 5);
        });
    } else if (step == 5) {
        provider->FetchFavorites([state](std::vector<stream::Track> tracks, std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty() || HasTrack(tracks, kSongCatalog))
                return Fail(state, L"fetch_favorites_final", L"favorite_final_state_failed");
            AddOperation(state, L"fetch_favorites_final", true);
            Continue(state, 6);
        });
    } else if (step == 6) {
        provider->CreatePlaylist(kScratchName, [state](stream::Id id, std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() || id.empty() || !HasPlaylist(apple->Playlists(stream::Shelf::Created), id))
                return Fail(state, L"playlist_create", L"playlist_create_failed");
            state->scratch = id;
            AddOperation(state, L"playlist_create", true);
            Continue(state, 7);
        });
    } else if (step == 7) {
        state->before_rename = CaptureSnapshot(*provider);
        provider->RenamePlaylist(state->scratch, kFailRename, [state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            // Unrelated lazy reads (favorites/artist enrichment) may finish while
            // the failure is in flight. The rename must preserve playlist state.
            const auto after = CaptureSnapshot(*apple);
            if (error.empty() || after.created != state->before_rename.created ||
                after.saved != state->before_rename.saved)
                return Fail(state, L"rename_rollback", L"rename_rollback_failed");
            AddOperation(state, L"rename_rollback", true, L"injected_failure_observed");
            Continue(state, 18);
        });
    } else if (step == 8) {
        provider->AddToPlaylist(state->scratch, {kSongCatalog}, [state](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty()) return Fail(state, L"playlist_add", L"playlist_add_failed");
            AddOperation(state, L"playlist_add", true);
            Continue(state, 9);
        });
    } else if (step == 9 || step == 11) {
        const bool added = step == 9;
        provider->FetchPlaylist(
            state->scratch,
            [state, added](stream::Playlist, std::vector<stream::Track> tracks, std::wstring error) {
                if (!Live(state)) return;
                const bool present = HasTrack(tracks, kSongCatalog);
                if (!error.empty() || present != added) {
                    return Fail(state, added ? L"playlist_fetch_added" : L"playlist_fetch_removed",
                                L"playlist_readback_failed");
                }
                AddOperation(state, added ? L"playlist_fetch_added" : L"playlist_fetch_removed", true);
                Continue(state, added ? 10 : 12);
            });
    } else if (step == 10) {
        provider->RemoveFromPlaylist(state->scratch, {kSongCatalog}, [state](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty()) return Fail(state, L"playlist_remove", L"playlist_remove_failed");
            AddOperation(state, L"playlist_remove", true);
            Continue(state, 11);
        });
    } else if (step == 12) {
        provider->DeletePlaylist(state->scratch, [state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() || HasPlaylist(apple->Playlists(stream::Shelf::Created), state->scratch))
                return Fail(state, L"playlist_delete", L"playlist_delete_failed");
            AddOperation(state, L"playlist_delete", true);
            Continue(state, 13);
        });
    } else if (step == 13 || step == 14) {
        const bool save = step == 13;
        provider->SetAlbumSaved(kAlbumCatalog, save, [state, save](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            // The mutation callback acknowledges the server response and publishes the
            // confirmed saved state. Library identity reconciliation is asynchronous and
            // is checked after the explicit final refresh below.
            if (!error.empty() || apple->IsAlbumSaved(kAlbumCatalog) != save)
                return Fail(state, save ? L"album_save" : L"album_unsave", L"album_ack_state_failed");
            AddOperation(state, save ? L"album_save" : L"album_unsave", true);
            Continue(state, save ? 14 : 15);
        });
    } else if (step == 15 || step == 16) {
        const bool save = step == 15;
        provider->SetPlaylistSaved(kPlaylistCatalog, save, [state, save](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() ||
                HasSavedPlaylistIdentity(*apple, kPlaylistCatalog, kPlaylistLibrary) != save)
                return Fail(state, save ? L"playlist_save" : L"playlist_unsave",
                            L"playlist_library_mutation_failed");
            AddOperation(state, save ? L"playlist_save" : L"playlist_unsave", true);
            Continue(state, save ? 16 : 17);
        });
    } else if (step == 17) {
        provider->RefreshLibrary([state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() || apple->IsFavorite(kSongCatalog) ||
                HasSavedAlbumIdentity(*apple, kAlbumCatalog, kAlbumLibrary) ||
                HasSavedPlaylistIdentity(*apple, kPlaylistCatalog, kPlaylistLibrary))
                return Fail(state, L"final_refresh", L"final_state_failed");
            AddOperation(state, L"final_refresh", true);
            Continue(state, 19);
        });
    } else if (step == 18) {
        provider->RenamePlaylist(state->scratch, kRenamedName, [state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() ||
                !HasPlaylistName(apple->Playlists(stream::Shelf::Created), state->scratch, kRenamedName)) {
                return Fail(state, L"playlist_rename", L"playlist_rename_failed");
            }
            AddOperation(state, L"playlist_rename", true);
            Continue(state, 8);
        });
    } else if (step == 19) {
        state->performanceStarted = std::chrono::steady_clock::now();
        auto done = [state](stream::Playlist info, std::vector<stream::Track> tracks, std::wstring error) {
            if (!Live(state)) return;
            auto& performance = state->performance;
            size_t uniqueTracks = 0;
            const bool valid = error.empty() && ValidPerformanceTracks(info, tracks, uniqueTracks);
            ++performance.coalescedCallbacks;
            if (performance.coalescedCallbacks == 1) {
                performance.firstRows = tracks.size();
                performance.firstValid = valid;
                return;
            }
            if (performance.coalescedCallbacks == 2) {
                performance.secondRows = tracks.size();
                performance.secondValid = valid;
                performance.uniqueTracks = uniqueTracks;
                performance.coalescedMs = ElapsedMs(state->performanceStarted);
                if (!performance.firstValid || !performance.secondValid)
                    return Fail(state, L"performance_playlist_coalescing",
                                L"performance_playlist_read_failed");
                Continue(state, 20);
                return;
            }
            Fail(state, L"performance_playlist_coalescing", L"performance_callback_overrun");
        };
        provider->FetchPlaylist(kPlaylistPerformance, done);
        provider->FetchPlaylist(kPlaylistPerformance, done);
    } else if (step == 20) {
        const auto started = std::chrono::steady_clock::now();
        provider->FetchPlaylist(
            kPlaylistPerformance,
            [state, started](stream::Playlist info, std::vector<stream::Track> tracks, std::wstring error) {
                if (!Live(state)) return;
                size_t uniqueTracks = 0;
                auto& performance = state->performance;
                performance.cachedRows = tracks.size();
                performance.cachedValid = error.empty() && ValidPerformanceTracks(info, tracks, uniqueTracks);
                performance.uniqueTracks = uniqueTracks;
                performance.cachedMs = ElapsedMs(started);
                if (!performance.cachedValid)
                    return Fail(state, L"performance_playlist_cache", L"performance_cached_read_failed");
                Continue(state, 21);
            });
    } else if (step == 21) {
        provider->RefreshLibrary([state](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty()) return Fail(state, L"performance_refresh", L"performance_refresh_failed");
            Continue(state, 22);
        });
    } else if (step == 22) {
        const auto started = std::chrono::steady_clock::now();
        provider->FetchPlaylist(kPlaylistPerformance, [state, started](stream::Playlist info,
                                                                       std::vector<stream::Track> tracks,
                                                                       std::wstring error) {
            if (!Live(state)) return;
            size_t uniqueTracks = 0;
            auto& performance = state->performance;
            performance.fallbackRows = tracks.size();
            performance.fallbackValid = error.empty() && ValidPerformanceTracks(info, tracks, uniqueTracks);
            performance.uniqueTracks = uniqueTracks;
            performance.fallbackMs = ElapsedMs(started);
            if (!performance.fallbackValid)
                return Fail(state, L"performance_playlist_fallback", L"performance_fallback_read_failed");
            Continue(state, 23);
        });
    } else if (step == 23) {
        provider->FetchPlaylist(
            kPlaylistPerformanceCatalog,
            [state](stream::Playlist info, std::vector<stream::Track> tracks, std::wstring error) {
                if (!Live(state)) return;
                auto& performance = state->performance;
                performance.catalogRows = tracks.size();
                performance.catalogValid = error.empty() && ValidCatalogPerformanceTracks(info, tracks);
                if (!performance.catalogValid)
                    return Fail(state, L"catalog_performance_playlist", L"catalog_performance_read_failed");
                Continue(state, 24);
            });
    } else if (step == 24) {
        // Exercise the provider boundary directly. Generic ListSession behavior is covered
        // separately with a fake provider so this fixture cannot mask adapter paging defects.
        state->pagingQuery.kind = stream::ListKind::PlaylistTracks;
        state->pagingQuery.id = kPlaylistPagedPerformance;
        provider->FetchListPage(
            state->pagingQuery, {}, 100, [state](stream::ListPage page, std::wstring error) {
                if (!Live(state)) return;
                const auto* rows = PagedRows(page);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                const auto counters = ReadPagingFixtureCounters();
                const bool paused =
                    counters && counters->primaryRequests == 1 && counters->pageRequests == 0 &&
                    counters->zeroOffsetRequests == 0 && counters->exactNestedIncludes == 1 &&
                    counters->invalidPrimaryRequests == 0 && counters->invalidPageRequests == 0;
                if (!error.empty() || !rows || rows->size() != 100 || !page.more ||
                    !PagedCursor(page.next, 100) || !paused) {
                    return Fail(state, L"fetch_list_page_first_pause",
                                error.empty() ? L"fetch_list_page_first_page_failed"
                                              : L"fetch_list_page_first_page_error");
                }
                state->paging.firstRows = rows->size();
                state->paging.firstMore = page.more;
                state->paging.firstOffset = page.next.offset == 100;
                state->paging.firstOpaque = PagedCursor(page.next, 100);
                state->paging.pausedAtFirstPage = paused;
                state->pagingFirstCursor = page.next;
                state->pagingCurrentCursor = page.next;
                AppendPagedTracks(state, *rows);
                AddOperation(state, L"fetch_list_page_first_pause", true);
                Continue(state, 25);
            });
    } else if (step == 25) {
        provider->FetchListPage(state->pagingQuery, state->pagingFirstCursor, 100,
                                [state](stream::ListPage page, std::wstring error) {
                                    if (!Live(state)) return;
                                    const auto* rows = PagedRows(page);
                                    if (!error.empty() || !rows || rows->size() != 100 || !page.more ||
                                        !PagedCursor(page.next, 200)) {
                                        return Fail(state, L"fetch_list_page_next",
                                                    error.empty() ? L"fetch_list_page_second_page_failed"
                                                                  : L"fetch_list_page_second_page_error");
                                    }
                                    state->paging.secondRows = rows->size();
                                    state->paging.explicitNextOffset = true;
                                    state->pagingCurrentCursor = page.next;
                                    AppendPagedTracks(state, *rows);
                                    AddOperation(state, L"fetch_list_page_next", true);
                                    Continue(state, 26);
                                });
    } else if (step == 26) {
        // This is cursor-integrity validation, not one-shot consumption: the source offset
        // no longer matches the encoded offset while the opaque continuation stays intact.
        stream::ListCursor mismatched = state->pagingFirstCursor;
        ++mismatched.offset;
        const auto before = ReadPagingFixtureCounters();
        provider->FetchListPage(state->pagingQuery, mismatched, 100,
                                [state, before](stream::ListPage, std::wstring error) {
                                    if (!Live(state)) return;
                                    const auto after = ReadPagingFixtureCounters();
                                    const bool rejected = !error.empty() && before && after &&
                                                          before->pageRequests == after->pageRequests;
                                    if (!rejected)
                                        return Fail(state, L"fetch_list_page_cursor_mismatch_rejected",
                                                    L"cursor_offset_mismatch_accepted");
                                    state->paging.mismatchedCursorRejected = true;
                                    AddOperation(state, L"fetch_list_page_cursor_mismatch_rejected", true);
                                    Continue(state, 27);
                                });
    } else if (step == 27) {
        stream::ListQuery crossQuery = state->pagingQuery;
        crossQuery.search = L"cross-query";
        const auto before = ReadPagingFixtureCounters();
        provider->FetchListPage(crossQuery, state->pagingFirstCursor, 100,
                                [state, before](stream::ListPage, std::wstring error) {
                                    if (!Live(state)) return;
                                    const auto after = ReadPagingFixtureCounters();
                                    const bool rejected = !error.empty() && before && after &&
                                                          before->pageRequests == after->pageRequests;
                                    if (!rejected)
                                        return Fail(state, L"fetch_list_page_cross_query_rejected",
                                                    L"cross_query_cursor_accepted");
                                    state->paging.crossQueryRejected = true;
                                    AddOperation(state, L"fetch_list_page_cross_query_rejected", true);
                                    Continue(state, 28);
                                });
    } else if (step == 28) {
        provider->FetchListPage(state->pagingQuery, state->pagingCurrentCursor, 100,
                                [state](stream::ListPage page, std::wstring error) {
                                    if (!Live(state)) return;
                                    const auto* rows = PagedRows(page);
                                    if (!error.empty() || !rows || rows->size() != 100 || !page.more ||
                                        !PagedCursor(page.next, 300)) {
                                        return Fail(state, L"fetch_list_page_third",
                                                    error.empty() ? L"fetch_list_page_third_page_failed"
                                                                  : L"fetch_list_page_third_page_error");
                                    }
                                    state->paging.thirdRows = rows->size();
                                    state->pagingCurrentCursor = page.next;
                                    AppendPagedTracks(state, *rows);
                                    Continue(state, 29);
                                });
    } else if (step == 29) {
        provider->FetchListPage(
            state->pagingQuery, state->pagingCurrentCursor, 100,
            [state](stream::ListPage page, std::wstring error) {
                if (!Live(state)) return;
                const auto* rows = PagedRows(page);
                if (!error.empty() || !rows || rows->size() != 59 || page.more || !page.next.value.empty()) {
                    return Fail(state, L"fetch_list_page_exhaust",
                                error.empty() ? L"fetch_list_page_fourth_page_failed"
                                              : L"fetch_list_page_fourth_page_error");
                }
                auto& paging = state->paging;
                paging.fourthRows = rows->size();
                AppendPagedTracks(state, *rows);
                size_t uniqueTracks = 0;
                paging.creditsValid = ValidPagedTracks(state->pagingTracks, uniqueTracks);
                paging.uniqueTracks = uniqueTracks;
                paging.totalRows = state->pagingTracks.size();
                const auto counters = ReadPagingFixtureCounters();
                const bool offsets = counters && counters->offsets.size() == 3 &&
                                     counters->offsets[0] == 100 && counters->offsets[1] == 200 &&
                                     counters->offsets[2] == 300;
                paging.exhausted = paging.totalRows == kPerformanceTrackCount && paging.creditsValid &&
                                   counters && counters->primaryRequests == 1 &&
                                   counters->pageRequests == 3 && counters->zeroOffsetRequests == 0 &&
                                   counters->exactNestedIncludes == 4 && counters->opaqueQueryRetained == 3 &&
                                   counters->invalidPrimaryRequests == 0 &&
                                   counters->invalidPageRequests == 0 &&
                                   counters->pageRows == kPerformanceTrackCount && offsets;
                if (!paging.exhausted)
                    return Fail(state, L"fetch_list_page_exhaust", L"fetch_list_page_fixture_mismatch");
                AddOperation(state, L"fetch_list_page_exhaust", true);
                Continue(state, 30);
            });
    } else if (step == 30) {
        const stream::Id id = L"song.glasswave-restore-merge";
        Song rich;
        rich.track.id = id;
        rich.track.title = L"Rich restore title";
        rich.track.album = L"Rich restore album";
        rich.track.albumId = L"l.album.glasswave-restore";
        rich.track.artists = {{L"r.artist.glasswave-restore", L"Rich restore artist"}};
        rich.track.durationMs = 321000;
        rich.coverUrl = L"https://example.invalid/rich-restore-cover";
        rich.catalogId = id;
        rich.genreNames = {L"Restore genre"};
        rich.composerNames = {L"Restore composer"};
        Catalog::Get().Put(rich);

        stream::ListPage stored;
        stream::Track partial;
        partial.id = id;
        stored.rows = std::vector<stream::Track>{partial};
        provider->RestoreListRecords(stored);
        const auto restored = Catalog::Get().FindSong(id);
        state->paging.restorePreserved =
            restored && restored->track.title == rich.track.title &&
            restored->track.album == rich.track.album && restored->track.albumId == rich.track.albumId &&
            restored->coverUrl == rich.coverUrl && restored->genreNames == rich.genreNames &&
            restored->composerNames == rich.composerNames;
        if (!state->paging.restorePreserved)
            return Fail(state, L"restore_list_records_merge", L"partial_restore_overwrote_metadata");
        AddOperation(state, L"restore_list_records_merge", true);
        Continue(state, 31);
    } else if (step == 31) {
        // The nonce-scoped fixture starts outside the persistent catalog. Its five
        // occurrences include one fully known row and one duplicate of a row needing IDs.
        auto& enrichment = state->enrichment;
        enrichment.freshCatalogIds = FreshEnrichmentCatalogIds(*state);
        if (!enrichment.freshCatalogIds)
            return Fail(state, L"fetch_list_page_enrichment_baseline",
                        L"enrichment_fixture_was_already_catalogued");

        state->enrichmentQuery.kind = stream::ListKind::PlaylistTracks;
        state->enrichmentQuery.id = EnrichmentPlaylistId(*state);
        provider->FetchListPage(
            state->enrichmentQuery, {}, kEnrichmentPageLimit,
            [state](stream::ListPage page, std::wstring error) {
                if (!Live(state)) return;
                const auto counters = ReadEnrichmentFixtureCounters();
                auto& enrichment = state->enrichment;
                const auto* rows = PagedRows(page);
                enrichment.baselineRows = rows ? rows->size() : 0;
                if (rows) {
                    std::set<stream::Id> unique;
                    for (const auto& row : *rows)
                        unique.insert(row.id);
                    enrichment.uniqueTracks = unique.size();
                }
                enrichment.baselineUsable = error.empty() && ValidEnrichmentBaseline(*state, page);
                enrichment.baselinePending = page.enrichmentPending;
                enrichment.noHydrationBeforeBaseline =
                    counters && counters->tag == state->enrichmentTag && counters->trackRequests == 1 &&
                    counters->trackRows == kEnrichmentTrackCount && counters->invalidTrackRequests == 0 &&
                    counters->hydrationRequests == 0 && counters->hydrationRows == 0 &&
                    counters->hydrationIds.empty();
                if (!enrichment.baselineUsable || !enrichment.baselinePending ||
                    !enrichment.noHydrationBeforeBaseline) {
                    return Fail(state, L"fetch_list_page_enrichment_baseline",
                                error.empty() ? L"enrichment_baseline_failed" : L"enrichment_baseline_error");
                }
                state->enrichmentBaseline = std::move(page);
                AddOperation(state, L"fetch_list_page_enrichment_baseline", true);
                Continue(state, 32);
            });
    } else if (step == 32) {
        auto& enrichment = state->enrichment;
        auto cancelledQuery = state->enrichmentQuery;
        cancelledQuery.cancelled = std::make_shared<std::atomic_bool>(true);
        const auto before = ReadEnrichmentFixtureCounters();
        const bool cancelledAccepted = provider->EnrichListPage(
            cancelledQuery, state->enrichmentBaseline,
            [state](stream::ListPage, std::wstring) { ++state->enrichment.cancellationCallbacks; });
        const auto after = ReadEnrichmentFixtureCounters();
        enrichment.cancellationRejected = !cancelledAccepted;
        enrichment.cancellationNoRequests =
            before && after && before->trackRequests == after->trackRequests &&
            before->hydrationRequests == after->hydrationRequests &&
            before->hydrationRows == after->hydrationRows &&
            before->invalidHydrationRequests == after->invalidHydrationRequests;
        if (!enrichment.cancellationRejected || !enrichment.cancellationNoRequests ||
            enrichment.cancellationCallbacks != 0) {
            return Fail(state, L"enrich_list_page_cancel_before_start",
                        L"canceled_enrichment_was_not_rejected_without_work");
        }
        AddOperation(state, L"enrich_list_page_cancel_before_start", true);

        enrichment.callbackCount = 0;
        state->enrichmentCallReturned = false;
        state->enrichmentStarted = std::chrono::steady_clock::now();
        auto done = [state](stream::ListPage page, std::wstring error) {
            auto& enrichment = state->enrichment;
            ++enrichment.callbackCount;
            const bool notInline = state->enrichmentCallReturned;
            if (!Live(state)) return;
            enrichment.callbackMs = ElapsedMs(state->enrichmentStarted);
            const auto* rows = PagedRows(page);
            enrichment.enrichedRows = rows ? rows->size() : 0;
            enrichment.callbackNotInline = notInline;
            enrichment.occurrenceIdentityPreserved =
                error.empty() && SameEnrichmentOccurrences(state->enrichmentBaseline, page);
            enrichment.relationsValid = error.empty() && ValidEnrichmentFinal(*state, page);
            enrichment.pendingCleared = error.empty() && !page.enrichmentPending;

            const auto counters = ReadEnrichmentFixtureCounters();
            enrichment.hydratedUniqueTracks = counters ? counters->hydrationIds.size() : 0;
            std::vector<stream::Id> expectedHydrationIds;
            for (size_t occurrence = 0; occurrence < kEnrichmentTrackCount; ++occurrence) {
                const size_t index = kEnrichmentIndexes[occurrence];
                if (index == kEnrichmentKnownIndex) continue;
                const auto id = EnrichmentLibraryId(*state, index);
                if (std::find(expectedHydrationIds.begin(), expectedHydrationIds.end(), id) ==
                    expectedHydrationIds.end())
                    expectedHydrationIds.push_back(id);
            }
            enrichment.knownRecordSkipped =
                counters && counters->tag == state->enrichmentTag && counters->trackRequests == 1 &&
                counters->invalidTrackRequests == 0 && counters->hydrationRequests == 1 &&
                counters->hydrationBatchSizes == std::vector<size_t>{kEnrichmentMissingUniqueCount} &&
                counters->hydrationIds == expectedHydrationIds &&
                counters->hydrationRows == kEnrichmentMissingUniqueCount &&
                counters->invalidHydrationRequests == 0 && counters->knownIdRequests == 0 &&
                counters->hydrationDuplicateIds == 0 && counters->maxHydrationConcurrency == 1 &&
                counters->activeHydrationRequests == 0 && counters->hydrationDelayMs > 0;

            if (enrichment.callbackCount != 1 || !enrichment.callbackNotInline ||
                !enrichment.relationsValid || !enrichment.occurrenceIdentityPreserved ||
                !enrichment.pendingCleared || !enrichment.knownRecordSkipped) {
                return Fail(state, L"enrich_list_page_relations",
                            error.empty() ? L"enrichment_result_failed" : L"enrichment_result_error");
            }

            state->enrichmentFinal = std::move(page);
            AddOperation(state, L"enrich_list_page_relations", true);
            Continue(state, 33);
        };
        const bool accepted =
            provider->EnrichListPage(state->enrichmentQuery, state->enrichmentBaseline, done);
        state->enrichmentCallReturned = true;
        enrichment.explicitAccepted = accepted;
        if (!accepted) {
            if (Live(state)) Fail(state, L"enrich_list_page_relations", L"enrichment_was_not_accepted");
            return;
        }
    } else if (step == 33) {
        auto& enrichment = state->enrichment;
        const auto before = ReadEnrichmentFixtureCounters();
        const bool accepted = provider->EnrichListPage(
            state->enrichmentQuery, state->enrichmentFinal,
            [state](stream::ListPage, std::wstring) { ++state->enrichment.fullyKnownCallbacks; });
        const auto after = ReadEnrichmentFixtureCounters();
        enrichment.fullyKnownPageSkipped = !accepted && enrichment.fullyKnownCallbacks == 0 && before &&
                                           after && before->hydrationRequests == after->hydrationRequests &&
                                           before->hydrationRows == after->hydrationRows;
        if (!enrichment.fullyKnownPageSkipped)
            return Fail(state, L"enrich_list_page_fully_known_skipped",
                        L"fully_known_page_requested_hydration");
        AddOperation(state, L"enrich_list_page_fully_known_skipped", true);
        Continue(state, 34);
    } else if (step == 34) {
        auto& display = DisplayMetadata::Get();
        display.Clear();
        SetAppleMusicDisplayRegion(AppleMusicDisplayRegion::Japan);

        Song sourceSong;
        sourceSong.track.id = RegionalUploadedSongCatalogId(*state);
        sourceSong.track.title = L"Regional Source Uploaded Song";
        sourceSong.track.album = L"Regional Source Album";
        sourceSong.track.albumId = RegionalAlbumLibraryId(*state);
        sourceSong.track.artists = {{RegionalAlbumArtistLibraryId(*state), L"Regional Source Album Artist"}};
        sourceSong.track.durationMs = 245678;
        sourceSong.catalogId = RegionalUploadedSongCatalogId(*state);
        sourceSong.libraryId = RegionalUploadedSongLibraryId(*state);
        sourceSong.composerName = L"Regional source composer";
        Catalog::Get().Put(sourceSong);

        stream::Album sourceAlbum;
        sourceAlbum.id = RegionalAlbumLibraryId(*state);
        sourceAlbum.name = L"Regional Source Album";
        sourceAlbum.description = L"Regional source album description";
        sourceAlbum.artists = sourceSong.track.artists;
        sourceAlbum.trackCount = 2;
        sourceAlbum.year = 2021;
        Catalog::Get().Put(sourceAlbum);

        stream::Artist sourceArtist;
        sourceArtist.id = RegionalAlbumArtistLibraryId(*state);
        sourceArtist.name = L"Regional Source Album Artist";
        sourceArtist.bio = L"Regional source artist biography";
        sourceArtist.albumCount = 3;
        sourceArtist.trackCount = 7;
        Catalog::Get().Put(sourceArtist);

        SongDisplayNames japanSong;
        japanSong.title = RegionalUploadedSongTitle(AppleMusicDisplayRegion::Japan);
        japanSong.album = RegionalAlbumName(AppleMusicDisplayRegion::Japan);
        japanSong.artists = {
            {RegionalAlbumArtistLibraryId(*state), RegionalAlbumArtistName(AppleMusicDisplayRegion::Japan)}};
        japanSong.composerName = L"日本語の作曲者";
        display.PutSongFor(AppleMusicDisplayRegion::Japan, L"US", sourceSong.track.id, japanSong);
        display.PutAlbumFor(AppleMusicDisplayRegion::Japan, L"US", sourceAlbum.id,
                            {RegionalAlbumName(AppleMusicDisplayRegion::Japan),
                             RegionalAlbumDescription(AppleMusicDisplayRegion::Japan), japanSong.artists});
        display.PutArtistFor(AppleMusicDisplayRegion::Japan, L"US", sourceArtist.id,
                             {RegionalBioArtistName(AppleMusicDisplayRegion::Japan),
                              RegionalBio(AppleMusicDisplayRegion::Japan)});

        SetAppleMusicDisplayRegion(AppleMusicDisplayRegion::China);
        display.PutSongFor(AppleMusicDisplayRegion::Japan, L"US", sourceSong.track.id,
                           {L"STALE SONG", L"STALE ALBUM", {}, L"STALE COMPOSER"});
        display.PutAlbumFor(AppleMusicDisplayRegion::Japan, L"US", sourceAlbum.id,
                            {L"STALE ALBUM", L"STALE DESCRIPTION", {}});
        display.PutArtistFor(AppleMusicDisplayRegion::Japan, L"US", sourceArtist.id,
                             {L"STALE ARTIST", L"STALE BIO"});

        SongDisplayNames chinaSong;
        chinaSong.title = RegionalUploadedSongTitle(AppleMusicDisplayRegion::China);
        chinaSong.album = RegionalAlbumName(AppleMusicDisplayRegion::China);
        chinaSong.artists = {
            {RegionalAlbumArtistLibraryId(*state), RegionalAlbumArtistName(AppleMusicDisplayRegion::China)}};
        chinaSong.composerName = L"中文作曲者";
        display.PutSongFor(AppleMusicDisplayRegion::China, L"US", sourceSong.track.id, chinaSong);
        display.PutAlbumFor(AppleMusicDisplayRegion::China, L"US", sourceAlbum.id,
                            {RegionalAlbumName(AppleMusicDisplayRegion::China),
                             RegionalAlbumDescription(AppleMusicDisplayRegion::China), chinaSong.artists});
        display.PutArtistFor(AppleMusicDisplayRegion::China, L"US", sourceArtist.id,
                             {RegionalBioArtistName(AppleMusicDisplayRegion::China),
                              RegionalBio(AppleMusicDisplayRegion::China)});

        Song china = sourceSong;
        stream::Album chinaAlbum = sourceAlbum;
        stream::Artist chinaArtist = sourceArtist;
        display.Apply(china, L"US");
        display.Apply(chinaAlbum, L"US");
        display.Apply(chinaArtist, L"US");
        const bool staleWritesIsolated =
            china.track.title == chinaSong.title &&
            chinaAlbum.description == RegionalAlbumDescription(AppleMusicDisplayRegion::China) &&
            chinaArtist.bio == RegionalBio(AppleMusicDisplayRegion::China);

        Song foreign = sourceSong;
        display.Apply(foreign, L"CA");
        const bool sourceStorefrontIsolated = foreign.track.title == sourceSong.track.title;

        SetAppleMusicDisplayRegion(AppleMusicDisplayRegion::Account);
        Song account = sourceSong;
        stream::Album accountAlbum = sourceAlbum;
        stream::Artist accountArtist = sourceArtist;
        display.Apply(account, L"US");
        display.Apply(accountAlbum, L"US");
        display.Apply(accountArtist, L"US");
        const bool accountRestored = account.track.title == sourceSong.track.title &&
                                     accountAlbum.description == sourceAlbum.description &&
                                     accountArtist.bio == sourceArtist.bio;

        SetAppleMusicDisplayRegion(AppleMusicDisplayRegion::Japan);
        Song japan = sourceSong;
        stream::Album japanAlbum = sourceAlbum;
        stream::Artist japanArtist = sourceArtist;
        display.Apply(japan, L"US");
        display.Apply(japanAlbum, L"US");
        display.Apply(japanArtist, L"US");
        const bool japanRetained =
            japan.track.title == japanSong.title && japan.composerName == japanSong.composerName &&
            japan.track.durationMs == sourceSong.track.durationMs &&
            japan.libraryId == sourceSong.libraryId &&
            japanAlbum.description == RegionalAlbumDescription(AppleMusicDisplayRegion::Japan) &&
            japanArtist.bio == RegionalBio(AppleMusicDisplayRegion::Japan);
        Song unknown;
        unknown.track.id = sourceSong.track.id + L".unknown";
        unknown.track.title = L"Unknown source";
        display.Apply(unknown, L"US");
        const bool directCacheContract = staleWritesIsolated && accountRestored && japanRetained &&
                                         unknown.track.title == L"Unknown source";
        state->regional.directCacheContract = directCacheContract;
        state->regional.staleWritesIsolated = staleWritesIsolated;
        state->regional.sourceStorefrontIsolated = sourceStorefrontIsolated;
        if (!directCacheContract)
            return Fail(state, L"display_metadata_overlay", L"display_names_contract_failed");
        AddOperation(state, L"display_metadata_overlay", true);
        if (!staleWritesIsolated || !sourceStorefrontIsolated)
            return Fail(state, L"display_metadata_region_guard", L"display_region_guard_failed");
        AddOperation(state, L"display_metadata_region_guard", true);

        const std::wstring priorSourceStorefront = DisplayMetadata::Get().SourceStorefront();
        struct RestoreSourceStorefront {
            std::wstring value;
            ~RestoreSourceStorefront() { DisplayMetadata::Get().SetSourceStorefront(std::move(value)); }
        } restoreSourceStorefront{priorSourceStorefront};
        stream::ListQuery cacheQuery;
        cacheQuery.kind = stream::ListKind::SavedAlbums;
        DisplayMetadata::Get().SetSourceStorefront(L"US");
        SetAppleMusicDisplayRegion(AppleMusicDisplayRegion::Account);
        const std::string accountContext = provider->ListCacheContext(cacheQuery);
        SetAppleMusicDisplayRegion(AppleMusicDisplayRegion::Japan);
        const std::string japanUsContext = provider->ListCacheContext(cacheQuery);
        const bool japanPersistent = provider->ListPolicy(cacheQuery) == stream::ListCachePolicy::Persistent;
        SetAppleMusicDisplayRegion(AppleMusicDisplayRegion::China);
        const std::string chinaUsContext = provider->ListCacheContext(cacheQuery);
        const bool chinaPersistent = provider->ListPolicy(cacheQuery) == stream::ListCachePolicy::Persistent;
        SetAppleMusicDisplayRegion(AppleMusicDisplayRegion::Japan);
        DisplayMetadata::Get().SetSourceStorefront(L"CA");
        const std::string japanCaContext = provider->ListCacheContext(cacheQuery);
        DisplayMetadata::Get().SetSourceStorefront(L"US");
        const bool regionalListPersistent = japanPersistent && chinaPersistent;
        const bool regionalCacheContextsSeparated =
            !accountContext.empty() && !japanUsContext.empty() && !chinaUsContext.empty() &&
            !japanCaContext.empty() && accountContext != japanUsContext && accountContext != chinaUsContext &&
            japanUsContext != chinaUsContext && japanUsContext != japanCaContext;
        state->regional.regionalListPersistent = regionalListPersistent;
        state->regional.regionalCacheContextsSeparated = regionalCacheContextsSeparated;
        if (!regionalListPersistent || !regionalCacheContextsSeparated)
            return Fail(state, L"provider_regional_cache_identity",
                        L"regional_cache_identity_contract_failed");
        AddOperation(state, L"provider_regional_cache_identity", true);

        stream::ListPage localizedPage;
        std::vector<stream::Track> localizedRows{sourceSong.track};
        display.Apply(localizedRows.front(), L"US");
        localizedPage.rows = std::move(localizedRows);
        localizedPage.album = sourceAlbum;
        localizedPage.artist = sourceArtist;
        display.Apply(*localizedPage.album, L"US");
        display.Apply(*localizedPage.artist, L"US");
        provider->RestoreListRecords(localizedPage);
        const auto rawSong = Catalog::Get().FindSongRaw(sourceSong.track.id);
        const auto rawAlbum = Catalog::Get().FindAlbumRaw(sourceAlbum.id);
        const auto rawArtist = Catalog::Get().FindArtistRaw(sourceArtist.id);
        const bool mergePreserved =
            rawSong && rawSong->track.title == sourceSong.track.title &&
            rawSong->track.album == sourceSong.track.album && rawAlbum &&
            rawAlbum->name == sourceAlbum.name && rawAlbum->description == sourceAlbum.description &&
            rawArtist && rawArtist->name == sourceArtist.name && rawArtist->bio == sourceArtist.bio;
        if (!mergePreserved)
            return Fail(state, L"restore_list_page_preserves_canonical",
                        L"localized_restore_overwrote_canonical_metadata");

        if (!SetProviderDisplayRegion(*provider, AppleMusicDisplayRegion::Account))
            return Fail(state, L"restore_list_page_preserves_canonical", L"account_region_switch_failed");
        provider->RefreshLibrary([state, sourceSong, sourceAlbum, sourceArtist](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty())
                return Fail(state, L"restore_list_page_preserves_canonical", L"account_refresh_failed");
            auto* current = Provider(state);
            if (!current) return;
            const stream::Id section = SongsSection(*current);
            if (section.empty())
                return Fail(state, L"restore_list_page_preserves_canonical",
                            L"library_songs_section_missing");
            current->FetchSectionTracks(
                section, L"",
                [state, sourceSong, sourceAlbum, sourceArtist](std::vector<stream::Track> rows,
                                                               std::wstring songsError) {
                    if (!Live(state)) return;
                    const auto restoredSong = Catalog::Get().FindSongRaw(sourceSong.track.id);
                    const auto restoredAlbum = Catalog::Get().FindAlbumRaw(sourceAlbum.id);
                    const auto restoredArtist = Catalog::Get().FindArtistRaw(sourceArtist.id);
                    const auto displayedSong = Catalog::Get().FindSong(sourceSong.track.id);
                    const auto displayedAlbum = Catalog::Get().FindAlbum(sourceAlbum.id);
                    const auto displayedArtist = Catalog::Get().FindArtist(sourceArtist.id);
                    const bool rowsValid = HasTrack(rows, RegionalUploadedSongCatalogId(*state)) &&
                                           HasTrack(rows, RegionalLibraryOnlySongId(*state));
                    const bool ok = songsError.empty() && rowsValid && restoredSong &&
                                    restoredSong->track.title == sourceSong.track.title && restoredAlbum &&
                                    restoredAlbum->name == sourceAlbum.name &&
                                    restoredAlbum->description == sourceAlbum.description && restoredArtist &&
                                    restoredArtist->name == sourceArtist.name &&
                                    restoredArtist->bio == sourceArtist.bio && displayedSong &&
                                    displayedSong->track.title == sourceSong.track.title && displayedAlbum &&
                                    displayedAlbum->name == sourceAlbum.name && displayedArtist &&
                                    displayedArtist->name == sourceArtist.name;
                    state->regional.restoreCanonicalRecords = ok;
                    if (!ok)
                        return Fail(state, L"restore_list_page_preserves_canonical",
                                    songsError.empty() ? L"account_toggle_retained_localized_raw_metadata"
                                                       : L"account_library_songs_failed");
                    AddOperation(state, L"restore_list_page_preserves_canonical", true);
                    Continue(state, 35);
                });
        });
    } else if (step == 35) {
        if (!SetProviderDisplayRegion(*provider, AppleMusicDisplayRegion::Japan))
            return Fail(state, L"provider_display_region_japan", L"japan_region_setting_missing");
        AddOperation(state, L"provider_display_region_japan", true);
        provider->RefreshLibrary([state](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty())
                return Fail(state, L"provider_regional_japan_song_first", L"japan_refresh_failed");
            DisplayMetadata::Get().Clear();
            Continue(state, 36);
        });
    } else if (step == 36) {
        provider->FetchAlbumsTracks(
            {RegionalDetailAlbumCatalogId(*state)},
            [state](std::vector<stream::Track> tracks, std::wstring error) {
                if (!Live(state)) return;
                if (!error.empty()) {
                    return Fail(state, L"provider_regional_japan_song_first", L"song_first_fetch_failed");
                }
                const auto problem = RegionalSongFirstProblem(*state, tracks, AppleMusicDisplayRegion::Japan);
                if (!problem.empty()) {
                    return Fail(state, L"provider_regional_japan_song_first", problem);
                }
                AddOperation(state, L"provider_regional_japan_song_first", true);
                Continue(state, 37);
            });
    } else if (step == 37) {
        provider->FetchAlbum(RegionalDetailAlbumCatalogId(*state), [state](stream::Album album,
                                                                           std::vector<stream::Track> tracks,
                                                                           std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty()) {
                return Fail(state, L"provider_regional_japan_album", L"album_detail_fetch_failed");
            }
            const auto problem = RegionalAlbumProblem(*state, album, tracks, AppleMusicDisplayRegion::Japan);
            if (!problem.empty()) {
                return Fail(state, L"provider_regional_japan_album", problem);
            }
            state->regional.japanProviderFetch = true;
            state->regional.albumDescriptionOverlay = true;
            state->regional.identityDurationPreserved = true;
            AddOperation(state, L"provider_regional_japan_album", true);
            Continue(state, 38);
        });
    } else if (step == 38) {
        provider->FetchArtist(RegionalDetailArtistCatalogId(*state), [state](stream::Artist artist,
                                                                             std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty() || !ValidRegionalArtist(*state, artist, AppleMusicDisplayRegion::Japan)) {
                return Fail(state, L"provider_regional_japan_artist",
                            error.empty() ? L"artist_detail_resolution_failed"
                                          : L"artist_detail_fetch_failed");
            }
            auto* current = Provider(state);
            if (!current) return;
            current->FetchArtistBio(RegionalDetailArtistCatalogId(*state), [state](std::wstring bio,
                                                                                   std::wstring bioError) {
                if (!Live(state)) return;
                if (!bioError.empty() || bio != RegionalBio(AppleMusicDisplayRegion::Japan)) {
                    return Fail(state, L"provider_regional_japan_artist",
                                bioError.empty() ? L"artist_bio_overlay_failed" : L"artist_bio_fetch_failed");
                }
                state->regional.artistBioOverlay = true;
                AddOperation(state, L"provider_regional_japan_artist", true);
                Continue(state, 39);
            });
        });
    } else if (step == 39) {
        provider->FetchArtist(
            RegionalRetryArtistCatalogId(*state), [state](stream::Artist artist, std::wstring error) {
                if (!Live(state)) return;
                if (!error.empty()) {
                    return Fail(state, L"provider_regional_japan_retry", L"transient_failure_returned_error");
                }
                const auto fallbackProblem =
                    RegionalRetryArtistProblem(*state, artist, AppleMusicDisplayRegion::Account);
                if (!fallbackProblem.empty()) {
                    return Fail(state, L"provider_regional_japan_retry", fallbackProblem);
                }
                auto* current = Provider(state);
                if (!current) return;
                current->FetchArtist(RegionalRetryArtistCatalogId(*state), [state](stream::Artist retry,
                                                                                   std::wstring retryError) {
                    if (!Live(state)) return;
                    if (!retryError.empty()) {
                        return Fail(state, L"provider_regional_japan_retry", L"retry_fetch_failed");
                    }
                    const auto retryProblem =
                        RegionalRetryArtistProblem(*state, retry, AppleMusicDisplayRegion::Japan);
                    if (!retryProblem.empty()) {
                        return Fail(state, L"provider_regional_japan_retry", retryProblem);
                    }
                    state->regional.transientRetryRecovered = true;
                    AddOperation(state, L"provider_regional_japan_retry", true);
                    auto* retried = Provider(state);
                    if (!retried) return;
                    retried->FetchArtist(
                        RegionalMissingArtistCatalogId(*state),
                        [state](stream::Artist missing, std::wstring missingError) {
                            if (!Live(state)) return;
                            if (!missingError.empty() || !ValidRegionalMissingArtist(*state, missing)) {
                                return Fail(state, L"provider_regional_japan_missing_artist",
                                            missingError.empty() ? L"missing_counterpart_not_preserved"
                                                                 : L"missing_counterpart_fetch_failed");
                            }
                            const auto counters = ReadRegionalFixtureCounters();
                            if (!counters || !ValidJapanRegionalRequests(*counters))
                                return Fail(state, L"provider_regional_japan_requests",
                                            L"japan_request_isolation_failed");
                            state->regional.missingCounterpartFallback = true;
                            state->regional.sourceUsRequestsOnly = true;
                            AddOperation(state, L"provider_regional_japan_missing_artist", true);
                            AddOperation(state, L"provider_regional_japan_requests", true);
                            Continue(state, 40);
                        });
                });
            });
    } else if (step == 40) {
        if (!SetProviderDisplayRegion(*provider, AppleMusicDisplayRegion::China))
            return Fail(state, L"provider_display_region_china", L"china_region_setting_missing");
        AddOperation(state, L"provider_display_region_china", true);
        provider->RefreshLibrary([state](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty())
                return Fail(state, L"provider_regional_china_song_first", L"china_refresh_failed");
            DisplayMetadata::Get().Clear();
            Continue(state, 41);
        });
    } else if (step == 41) {
        provider->FetchAlbumsTracks(
            {RegionalDetailAlbumCatalogId(*state)},
            [state](std::vector<stream::Track> tracks, std::wstring error) {
                if (!Live(state)) return;
                if (!error.empty()) {
                    return Fail(state, L"provider_regional_china_song_first",
                                L"china_song_first_fetch_failed");
                }
                const auto problem = RegionalSongFirstProblem(*state, tracks, AppleMusicDisplayRegion::China);
                if (!problem.empty()) {
                    return Fail(state, L"provider_regional_china_song_first", problem);
                }
                AddOperation(state, L"provider_regional_china_song_first", true);
                Continue(state, 42);
            });
    } else if (step == 42) {
        provider->FetchAlbum(RegionalDetailAlbumCatalogId(*state), [state](stream::Album album,
                                                                           std::vector<stream::Track> tracks,
                                                                           std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty()) {
                return Fail(state, L"provider_regional_china_album", L"china_album_detail_fetch_failed");
            }
            const auto problem = RegionalAlbumProblem(*state, album, tracks, AppleMusicDisplayRegion::China);
            if (!problem.empty()) {
                return Fail(state, L"provider_regional_china_album", problem);
            }
            state->regional.chinaProviderFetch = true;
            AddOperation(state, L"provider_regional_china_album", true);
            Continue(state, 43);
        });
    } else if (step == 43) {
        provider->FetchArtist(
            RegionalDetailArtistCatalogId(*state), [state](stream::Artist artist, std::wstring error) {
                if (!Live(state)) return;
                if (!error.empty() || !ValidRegionalArtist(*state, artist, AppleMusicDisplayRegion::China)) {
                    return Fail(state, L"provider_regional_china_artist",
                                error.empty() ? L"china_artist_detail_resolution_failed"
                                              : L"china_artist_detail_fetch_failed");
                }
                auto* current = Provider(state);
                if (!current) return;
                current->FetchArtistBio(
                    RegionalDetailArtistCatalogId(*state), [state](std::wstring bio, std::wstring bioError) {
                        if (!Live(state)) return;
                        if (!bioError.empty() || bio != RegionalBio(AppleMusicDisplayRegion::China)) {
                            return Fail(state, L"provider_regional_china_artist",
                                        bioError.empty() ? L"china_artist_bio_overlay_failed"
                                                         : L"china_artist_bio_fetch_failed");
                        }
                        AddOperation(state, L"provider_regional_china_artist", true);
                        Continue(state, 44);
                    });
            });
    } else if (step == 44) {
        provider->FetchArtist(RegionalMissingArtistCatalogId(*state), [state](stream::Artist artist,
                                                                              std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty() || !ValidRegionalMissingArtist(*state, artist)) {
                return Fail(state, L"provider_regional_china_missing_artist",
                            error.empty() ? L"china_missing_counterpart_not_preserved"
                                          : L"china_missing_counterpart_fetch_failed");
            }
            const auto counters = ReadRegionalFixtureCounters();
            if (!counters || !ValidChinaRegionalRequests(*counters))
                return Fail(state, L"provider_regional_china_requests", L"china_request_isolation_failed");
            AddOperation(state, L"provider_regional_china_missing_artist", true);
            AddOperation(state, L"provider_regional_china_requests", true);
            Continue(state, 45);
        });
    } else if (step == 45) {
        if (!SetProviderDisplayRegion(*provider, AppleMusicDisplayRegion::Account))
            return Fail(state, L"provider_display_region_account", L"account_region_setting_missing");
        AddOperation(state, L"provider_display_region_account", true);
        provider->RefreshLibrary([state](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty())
                return Fail(state, L"provider_regional_account_album", L"account_refresh_failed");
            Continue(state, 46);
        });
    } else if (step == 46) {
        provider->FetchAlbum(
            RegionalDetailAlbumCatalogId(*state),
            [state](stream::Album album, std::vector<stream::Track> tracks, std::wstring error) {
                if (!Live(state)) return;
                if (!error.empty()) {
                    return Fail(state, L"provider_regional_account_album", L"account_album_fetch_failed");
                }
                const auto problem =
                    RegionalAlbumProblem(*state, album, tracks, AppleMusicDisplayRegion::Account);
                if (!problem.empty()) {
                    return Fail(state, L"provider_regional_account_album", problem);
                }
                AddOperation(state, L"provider_regional_account_album", true);
                Continue(state, 47);
            });
    } else if (step == 47) {
        provider->FetchArtist(RegionalDetailArtistCatalogId(*state), [state](stream::Artist artist,
                                                                             std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty() || !ValidRegionalArtist(*state, artist, AppleMusicDisplayRegion::Account)) {
                return Fail(state, L"provider_regional_account_artist",
                            error.empty() ? L"account_artist_restore_failed"
                                          : L"account_artist_fetch_failed");
            }
            auto* current = Provider(state);
            if (!current) return;
            current->FetchArtistBio(
                RegionalDetailArtistCatalogId(*state), [state](std::wstring bio, std::wstring bioError) {
                    if (!Live(state)) return;
                    if (!bioError.empty() || bio != RegionalBio(AppleMusicDisplayRegion::Account)) {
                        return Fail(state, L"provider_regional_account_artist",
                                    bioError.empty() ? L"account_artist_bio_restore_failed"
                                                     : L"account_artist_bio_fetch_failed");
                    }
                    state->regional.accountProviderFetch = true;
                    AddOperation(state, L"provider_regional_account_artist", true);
                    Continue(state, 48);
                });
        });
    } else if (step == 48) {
        const auto counters = ReadRegionalFixtureCounters();
        if (!counters || !ValidAccountRegionalRequests(*counters))
            return Fail(state, L"provider_regional_account_requests", L"account_request_isolation_failed");
        AddOperation(state, L"provider_regional_account_requests", true);
        if (state->scenario == "mutation-reconciliation")
            Continue(state, 49);
        else
            Finish(state);
    } else if (step == 49) {
        // The fixture holds this refresh before commit and acknowledges the remove
        // only after both the old album and artist reads have started.
        try {
            ControlMock(L"/api/v1/glasswave/testing/arm-mutation-stale-reads");
        } catch (const std::exception& exception) {
            return Fail(state, L"album_remove_ack_reconcile", Wide(exception.what()));
        }
        provider->RefreshLibrary([state](std::wstring error) {
            if (!Live(state)) return;
            state->mutationOldRefreshError = std::move(error);
            state->mutationOldRefreshCompleted.store(true, std::memory_order_release);
        });
        provider->SetAlbumSaved(kMutationAlbumLibrary, false, [state](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty()) return Fail(state, L"album_remove_ack_reconcile", error);
            state->mutationWaitFuture = std::async(std::launch::async, [state] {
                // The fixture releases its pre-mutation snapshot at ACK and
                // delays it long enough to exercise a late stale commit.
                std::this_thread::sleep_for(std::chrono::milliseconds(900));
                fb2k::inMainThread([state] {
                    if (!Live(state)) return;
                    auto* apple = Provider(state);
                    if (!apple) return;
                    if (!state->mutationOldRefreshCompleted.load(std::memory_order_acquire))
                        return Fail(state, L"album_remove_ack_reconcile", L"old_refresh_did_not_complete");
                    if (HasSavedAlbumIdentity(*apple, kMutationAlbumCatalog, kMutationAlbumLibrary))
                        return Fail(state, L"album_remove_ack_reconcile",
                                    L"confirmed_remove_was_resurrected");
                    const auto alias = Catalog::Get().FindAlbumCatalogId(kMutationAlbumLibrary);
                    if (alias != kMutationAlbumCatalog)
                        return Fail(state, L"album_remove_ack_reconcile",
                                    L"catalog_alias_mismatch(actual=" + alias + L"; expected=" +
                                        kMutationAlbumCatalog + L")");
                    if (!Catalog::Get().FindAlbumRaw(kMutationAlbumLibrary))
                        return Fail(state, L"album_remove_ack_reconcile", L"library_metadata_was_removed");
                    AddOperation(state, L"album_remove_ack_reconcile", true,
                                 L"refresh_failure_and_old_snapshot_do_not_rollback_ack");
                    Continue(state, 61);
                });
            });
        });
    } else if (step == 61) {
        provider->FetchAlbumsTracks(
            {kMutationAlbumLibrary}, [state](std::vector<stream::Track> tracks, std::wstring error) {
                if (!Live(state)) return;
                const auto counters = ReadAlbumResolverFixtureCounters();
                if (!error.empty() || !HasTrack(tracks, kMutationCatalogSong) || !counters ||
                    counters->catalogDetailReads == 0 || counters->libraryDetailReads != 0 ||
                    counters->libraryTrackReads != 0) {
                    return Fail(state, L"album_catalog_resolver",
                                error.empty() ? L"stale_library_album_used_wrong_endpoint" : error);
                }
                AddOperation(state, L"album_catalog_resolver", true);
                Continue(state, 50);
            });
    } else if (step == 50) {
        // The page still carries its original l.* id after removal. Re-adding it
        // must use the retained catalog identity.
        provider->SetAlbumSaved(kMutationAlbumLibrary, true, [state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() ||
                !HasSavedAlbumIdentity(*apple, kMutationAlbumCatalog, kMutationAlbumLibrary) ||
                Catalog::Get().FindAlbumCatalogId(kMutationAlbumLibrary) != kMutationAlbumCatalog ||
                !Catalog::Get().FindAlbumRaw(kMutationAlbumLibrary)) {
                return Fail(state, L"album_readd_catalog_identity",
                            error.empty() ? L"readd_identity_failed" : error);
            }
            AddOperation(state, L"album_readd_catalog_identity", true);
            Continue(state, 51);
        });
    } else if (step == 51) {
        provider->SetPlaylistSaved(kMutationPlaylistCatalog, true, [state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() || !PlaylistAuthorityCatalogIsTrue(*apple))
                return Fail(state, L"playlist_save_ack_reconcile",
                            error.empty() ? L"playlist_save_ack_failed" : error);
            AddOperation(state, L"playlist_save_ack_reconcile", true,
                         L"refresh_failure_does_not_rollback_ack");
            // Let the post-save refresh finish before arm the post-remove stale snapshot.
            apple->RefreshLibrary([state](std::wstring) {
                if (Live(state)) Continue(state, 52);
            });
        });
    } else if (step == 52) {
        try {
            ControlMock(L"/api/v1/glasswave/testing/arm-playlist-remove-stale-snapshot");
        } catch (const std::exception& exception) {
            return Fail(state, L"playlist_remove_ack_reconcile", Wide(exception.what()));
        }
        provider->SetPlaylistSaved(kMutationPlaylistLibrary, false, [state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() || !PlaylistAuthorityIsFalse(*apple))
                return Fail(state, L"playlist_remove_ack_reconcile",
                            error.empty() ? L"playlist_remove_ack_failed" : error);
            AddOperation(state, L"playlist_remove_ack_reconcile", true);
            Continue(state, 62);
        });
    } else if (step == 62) {
        state->playlistAuthorityWaitFuture = std::async(std::launch::async, [state] {
            for (size_t attempt = 0; attempt < 100 && Live(state); ++attempt) {
                const auto counters = ReadPlaylistAuthorityFixtureCounters();
                if (counters && counters->staleServed) {
                    fb2k::inMainThread([state] {
                        if (!Live(state)) return;
                        auto* provider = Provider(state);
                        if (!provider) return;
                        if (!PlaylistAuthorityIsFalse(*provider))
                            return Fail(state, L"playlist_saved_authority_barrier",
                                        L"stale_present_snapshot_overrode_remove_ack");
                        provider->RefreshLibrary([state](std::wstring error) {
                            VerifyPlaylistAuthorityAbsent(state, std::move(error), 1);
                        });
                    });
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            fb2k::inMainThread([state] {
                if (Live(state))
                    Fail(state, L"playlist_saved_authority_barrier", L"stale_present_snapshot_not_observed");
            });
        });
    } else if (step == 53) {
        state->rapidMutationCallbacks = 0;
        state->rapidMutationFailed = false;
        const auto complete = [state](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty()) state->rapidMutationFailed = true;
            ++state->rapidMutationCallbacks;
            if (state->rapidMutationCallbacks != 2) return;

            auto* apple = Provider(state);
            if (!apple) return;
            if (state->rapidMutationFailed ||
                !HasSavedAlbumIdentity(*apple, kMutationAlbumCatalog, kMutationAlbumLibrary)) {
                return Fail(state, L"rapid_album_remove_add", L"rapid_mutation_final_state_failed");
            }
            AddOperation(state, L"rapid_album_remove_add", true);
            Continue(state, 54);
        };
        provider->SetAlbumSaved(kMutationAlbumLibrary, false, complete);
        provider->SetAlbumSaved(kMutationAlbumLibrary, true, complete);
    } else if (step == 54) {
        try {
            ControlMock(L"/api/v1/glasswave/testing/arm-account-switch");
        } catch (const std::exception& exception) {
            return Fail(state, L"account_switch_stale_epoch", Wide(exception.what()));
        }
        state->accountSwitchRequested = true;
        provider->RefreshLibrary([state](std::wstring error) {
            if (!Live(state)) return;
            state->accountOldRefreshCompleted = true;
            state->accountOldRefreshError = std::move(error);
            MaybeContinueAccountSwitch(state);
        });
        state->accountSwitchFuture = std::async(std::launch::async, [state] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            try {
                ControlMock(L"/api/v1/glasswave/testing/switch-account");
                fb2k::inMainThread([state] {
                    if (!Live(state)) return;
                    state->accountSwitchCompleted = true;
                    MaybeContinueAccountSwitch(state);
                });
            } catch (const std::exception& exception) {
                const std::wstring error = Wide(exception.what());
                fb2k::inMainThread([state, error] {
                    if (Live(state)) Fail(state, L"account_switch_stale_epoch", error);
                });
            }
        });
    } else if (step == 55) {
        provider->RefreshLibrary([state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            if (!error.empty() || apple->AccountName() != L"Provider Contract Mock" ||
                HasSavedAlbumIdentity(*apple, kMutationAlbumCatalog, kMutationAlbumLibrary)) {
                return Fail(state, L"account_switch_stale_epoch",
                            error.empty() ? L"account_stale_state_visible" : error);
            }
            AddOperation(state, L"account_switch_stale_epoch", true);
            Continue(state, 56);
        });
    } else if (step == 56) {
        provider->SetFollowed(kSourcePageArtistLibrary, true, [state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            const auto counters = ReadArtistFixtureCounters();
            if (!error.empty() || !HasArtist(apple->FollowedArtists(), kSourcePageArtistLibrary) ||
                !Catalog::Get().FindArtistRaw(kSourcePageArtistLibrary) || !counters ||
                counters->originalLibraryReads != 0 || counters->sourcePageCanonicalReads == 0 ||
                counters->writes.empty() ||
                counters->writes.back() !=
                    std::pair<std::wstring, stream::Id>{L"POST", kSourcePageArtistCatalog}) {
                return Fail(state, L"artist_favorite_add",
                            error.empty() ? L"artist_alias_add_failed" : error);
            }
            AddOperation(state, L"artist_favorite_add", true);
            Continue(state, 57);
        });
    } else if (step == 57) {
        provider->SetFollowed(kSourcePageArtistLibrary, false, [state](std::wstring error) {
            if (!Live(state)) return;
            auto* apple = Provider(state);
            if (!apple) return;
            const auto counters = ReadArtistFixtureCounters();
            if (!error.empty() || HasArtist(apple->FollowedArtists(), kSourcePageArtistLibrary) ||
                !Catalog::Get().FindArtistRaw(kSourcePageArtistLibrary) || !counters ||
                counters->originalLibraryReads != 0 || counters->writes.size() < 2 ||
                counters->writes.back() !=
                    std::pair<std::wstring, stream::Id>{L"DELETE", kSourcePageArtistCatalog}) {
                return Fail(state, L"artist_favorite_remove",
                            error.empty() ? L"artist_alias_remove_failed" : error);
            }
            AddOperation(state, L"artist_favorite_remove", true);
            Continue(state, 58);
        });
    } else if (step == 58) {
        try {
            ControlMock(L"/api/v1/glasswave/testing/arm-artist-preference-stale-read");
        } catch (const std::exception& exception) {
            return Fail(state, L"artist_stale_read_ack_wins", Wide(exception.what()));
        }
        provider->RefreshLibrary([](std::wstring) {});
        state->artistPreferenceWaitFuture = std::async(std::launch::async, [state] {
            for (size_t attempt = 0; attempt < 100 && Live(state); ++attempt) {
                const auto counters = ReadArtistFixtureCounters();
                if (counters && counters->staleReadHeld) {
                    fb2k::inMainThread([state] {
                        if (!Live(state)) return;
                        auto* apple = Provider(state);
                        if (!apple) return;
                        apple->SetFollowed(kStaleArtistLibrary, true, [state](std::wstring error) {
                            if (!Live(state)) return;
                            auto* current = Provider(state);
                            if (!current) return;
                            if (!error.empty() ||
                                !HasArtist(current->FollowedArtists(), kStaleArtistLibrary) ||
                                !Catalog::Get().FindArtistRaw(kStaleArtistLibrary)) {
                                return Fail(state, L"artist_stale_read_ack_wins",
                                            error.empty() ? L"artist_stale_ack_failed" : error);
                            }
                            try {
                                ControlMock(L"/api/v1/glasswave/testing/"
                                            L"release-artist-preference-stale-read");
                            } catch (const std::exception& exception) {
                                return Fail(state, L"artist_stale_read_ack_wins", Wide(exception.what()));
                            }
                            state->artistPreferenceFinalizeFuture = std::async(std::launch::async, [state] {
                                std::this_thread::sleep_for(std::chrono::milliseconds(900));
                                fb2k::inMainThread([state] {
                                    if (!Live(state)) return;
                                    auto* current = Provider(state);
                                    if (!current) return;
                                    const auto counters = ReadArtistFixtureCounters();
                                    if (!counters || !counters->staleReadCompleted ||
                                        !HasArtist(current->FollowedArtists(), kStaleArtistLibrary) ||
                                        !Catalog::Get().FindArtistRaw(kStaleArtistLibrary)) {
                                        return Fail(state, L"artist_stale_read_ack_wins",
                                                    L"stale_inFavorites_read_rolled_back_ack");
                                    }
                                    current->SetFollowed(
                                        kStaleArtistLibrary, false, [state](std::wstring cleanupError) {
                                            if (!Live(state)) return;
                                            auto* provider = Provider(state);
                                            if (!provider) return;
                                            const auto finalCounters = ReadArtistFixtureCounters();
                                            if (!cleanupError.empty() ||
                                                HasArtist(provider->FollowedArtists(), kStaleArtistLibrary) ||
                                                !Catalog::Get().FindArtistRaw(kStaleArtistLibrary) ||
                                                !finalCounters || finalCounters->originalLibraryReads != 0 ||
                                                finalCounters->writes.empty() ||
                                                finalCounters->writes.back() !=
                                                    std::pair<std::wstring, stream::Id>{
                                                        L"DELETE", kStaleArtistCatalog}) {
                                                return Fail(state, L"artist_stale_read_ack_wins",
                                                            cleanupError.empty()
                                                                ? L"artist_stale_cleanup_failed"
                                                                : cleanupError);
                                            }
                                            AddOperation(state, L"artist_stale_read_ack_wins", true);
                                            Continue(state, 59);
                                        });
                                });
                            });
                        });
                    });
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            fb2k::inMainThread([state] {
                if (Live(state))
                    Fail(state, L"artist_stale_read_ack_wins", L"artist_stale_refresh_did_not_start");
            });
        });
    } else if (step == 59) {
        state->rapidArtistCallbacks = 0;
        state->rapidArtistFailed = false;
        const auto complete = [state](std::wstring error) {
            if (!Live(state)) return;
            if (!error.empty()) state->rapidArtistFailed = true;
            ++state->rapidArtistCallbacks;
            if (state->rapidArtistCallbacks != 2) return;
            auto* apple = Provider(state);
            if (!apple) return;
            const auto counters = ReadArtistFixtureCounters();
            if (state->rapidArtistFailed || HasArtist(apple->FollowedArtists(), kSourcePageArtistLibrary) ||
                !Catalog::Get().FindArtistRaw(kSourcePageArtistLibrary) || !counters ||
                counters->originalLibraryReads != 0 || counters->writes.size() < 2 ||
                counters->writes[counters->writes.size() - 2] !=
                    std::pair<std::wstring, stream::Id>{L"POST", kSourcePageArtistCatalog} ||
                counters->writes.back() !=
                    std::pair<std::wstring, stream::Id>{L"DELETE", kSourcePageArtistCatalog}) {
                return Fail(state, L"artist_rapid_last_intent", L"rapid_artist_order_or_final_state_failed");
            }
            AddOperation(state, L"artist_rapid_last_intent", true);
            Continue(state, 60);
        };
        provider->SetFollowed(kSourcePageArtistLibrary, true, complete);
        provider->SetFollowed(kSourcePageArtistLibrary, false, complete);
    } else if (step == 60) {
        Finish(state);
    } else {
        Finish(state, L"invalid_probe_step");
    }
}

class ProviderProbeLifecycle : public initquit {
  public:
    void on_init() override {
        const auto paths = ProbePaths();
        if (!paths) return;
        std::error_code ec;
        if (!fs::is_regular_file(paths->request, ec) || ec) return;
        if (!MoveFileExW(paths->request.c_str(), paths->claimed.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return;
        const auto request = ReadRequest(paths->claimed);
        DeleteFileW(paths->claimed.c_str());
        DeleteFileW(paths->result.c_str());

        auto state = std::make_shared<State>(paths->result);
        state->displayRegionToRestore = GetAppleMusicDisplayRegion();
        if (!request || !VerifyMockBridge(request->nonce)) {
            Finish(state, request ? L"mock_bridge_required" : L"invalid_request");
            return;
        }
        state->enrichmentTag = Wide(request->nonce.substr(0, kEnrichmentTagLength));
        state->scenario = request->scenario;
        m_state = state;
        fb2k::inMainThread([state] {
            if (state->scenario == "catalog-regression")
                RunCatalogRegression(state);
            else if (state->scenario == "catalog-suggestions")
                RunCatalogRegressionSuggestions(state);
            else if (state->scenario.rfind("startup-cache-", 0) == 0)
                RunStartupRegression(state);
            else
                Continue(state, 0);
        });
    }

    void on_quit() override {
        if (m_state) {
            m_state->stop = true;
            if (!RestoreDisplayRegion(*m_state) && m_state->error.empty())
                m_state->error = L"display_region_restore_failed";
            if (!m_state->done && m_state->error.empty())
                m_state->error = L"probe_stopped_before_completion";
            m_state->done = true;
            WriteResult(*m_state);
        }
        m_state.reset();
    }

  private:
    std::shared_ptr<State> m_state;
};

FB2K_SERVICE_FACTORY(ProviderProbeLifecycle);

} // namespace
} // namespace glasswave::applemusic
