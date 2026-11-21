#include "pch.h"
#include "stream/list_session.h"
#include "core/text.h"
#include "stream/provider.h"
#include "ui/page.h"
#include "ui/remote_pages.h"

#include <SDK/initquit.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace glasswave::stream::list_session_probe {
namespace {

namespace fs = std::filesystem;
namespace json = winrt::Windows::Data::Json;
using Clock = std::chrono::steady_clock;
using JsonArray = json::JsonArray;
using JsonObject = json::JsonObject;
using JsonValue = json::JsonValue;

constexpr std::string_view kProbe = "list-session";
constexpr std::string_view kProvider = "list-session-test";
// Disk-cache manifests have a separate backend regression; this probe stays memory-only.
constexpr size_t kMaxMarkerBytes = 4096;
constexpr auto kProbeBudget = std::chrono::seconds(55);
constexpr auto kCaseBudget = std::chrono::seconds(5);
constexpr auto kMainCallTimeout = std::chrono::seconds(10);

struct Paths {
    fs::path request;
    fs::path claimed;
    fs::path result;
};

struct Marker {
    bool valid = false;
    std::string runId;
};

struct CaseResult {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct RequestRecord {
    std::wstring query;
    ListCursor cursor;
    size_t limit = 0;
};

struct PendingRequest {
    ListPageDone done;
    std::shared_ptr<std::atomic_bool> cancelled;
};

struct PendingEnrichment {
    ListPage primary;
    ListPageDone done;
    std::shared_ptr<std::atomic_bool> cancelled;
};

struct Checks {
    bool callbackDeferred = false;
    uint64_t onDemandCallsBeforeNext = 0;
    std::vector<uint64_t> onDemandCounts;
    std::vector<uint64_t> allProgress;
    uint64_t duplicateRows = 0;
    uint64_t duplicateIdOccurrences = 0;
    uint64_t coalescedCalls = 0;
    uint64_t isolatedCallbackDelta = 0;
    uint64_t cacheContextRequestCount = 0;
    bool sameContextCoalesced = false;
    bool changedContextIsolated = false;
    bool capturedContextReused = false;
    bool lateContextFetchRejected = false;
    bool lateContextPrimaryRejected = false;
    bool lateContextEnrichmentRejected = false;
    bool lateContextDataIsolated = false;
    uint64_t autoDrainCallsBeforeCancel = 0;
    uint64_t autoDrainCallsAfterLate = 0;
    uint64_t refreshedCount = 0;
    bool oldGenerationIgnored = false;
    bool refreshTokenSignalled = false;
    uint64_t repeatedCursorCalls = 0;
    uint64_t noProgressCalls = 0;
    bool noProgressBounded = false;
    bool cancelTokenSignalled = false;
    uint64_t errorPrefixCount = 0;
    uint64_t retryCursorOffset = 0;
    bool retrySameCursor = false;
    uint64_t finalRetryCount = 0;
    uint64_t earlyNextRequests = 0;
    std::vector<uint64_t> earlyNextCounts;
    uint64_t cancelledNextRequestsBeforeB = 0;
    uint64_t cancelledNextRequestsAfterB = 0;
    uint64_t cancelledNextCount = 0;
    uint64_t metadataRejectCount = 0;
    bool metadataRetrySameCursor = false;
    bool metadataRetryRepeatedCursorError = false;
    uint64_t metadataRetryCount = 0;
    bool metadataFinalComplete = false;
    uint64_t remoteInitialCount = 0;
    std::vector<uint64_t> remoteFilterCounts;
    uint64_t remoteOnDemandCount = 0;
    bool remoteSortedPages = false;
    uint64_t remoteTileRows = 0;
    uint64_t remoteTileGroupCount = 0;
    bool remoteTilePresentationKept = false;
    bool remoteDestroyCancelled = false;
    uint64_t ordinaryMetadataReadyRows = 0;
    bool ordinaryUpdatedRowsAbsent = false;
    bool initialPrimaryBeforeEnrichment = false;
    uint64_t initialMetadataReadyRows = 0;
    uint64_t initialEnrichedVersionDelta = 0;
    uint64_t initialDataVersionDelta = 0;
    bool initialPatchTransportStable = false;
    bool initialUpdatedRange = false;
    uint64_t nextPageRequestsWhilePending = 0;
    uint64_t nextPageRowsBeforeEnrichment = 0;
    uint64_t nextPageMetadataReadyBefore = 0;
    bool nextPageTransportStable = false;
    std::vector<uint64_t> outOfOrderReadyRows;
    bool outOfOrderPatchesApplied = false;
    bool outOfOrderTransportStable = false;
    bool outOfOrderConcurrencyBounded = false;
    bool metadataCancellationShared = false;
    bool unchangedMetadataAdvanced = false;
    bool unchangedDataVersionStable = false;
    bool unchangedUpdatedRowsAbsent = false;
    bool declinedEnrichmentReleased = false;
    bool declinedEnrichmentNoCallback = false;
    bool declinedEnrichmentDataVersionStable = false;
    bool refreshOldPatchRejected = false;
    bool refreshNewPatchApplied = false;
    uint64_t refreshPatchRequests = 0;
    bool refreshOldCancellation = false;
    bool cancelPatchRejected = false;
    bool cancelBaseRetained = false;
    bool invalidCountRejected = false;
    bool invalidIdentityRejected = false;
    uint64_t invalidMetadataReadyRows = 0;
    bool invalidBaseRetained = false;
    uint64_t duplicateMetadataOccurrences = 0;
    bool duplicateMetadataPatchedByOccurrence = false;
    bool failurePrimaryRetained = false;
    bool failureMetadataReady = false;
    bool failureErrorAbsent = false;
    bool failureDataVersionStable = false;
    bool failureUpdatedRowsAbsent = false;
    bool accountPatchRejected = false;
    bool accountBaseRetained = false;
    bool restoreInitialDeferred = false;
    bool restoreInitialEmptyNoError = false;
    bool restoreNextPreserved = false;
    bool softRetirePrefixRetained = false;
    bool softRetireOldNetworkRejected = false;
    bool softRetireNewNetworkApplied = false;
    bool remotePendingGated = false;
    bool remoteMetadataReveal = false;
    bool remoteNextPendingGated = false;
    bool remoteSelectionFocusKept = false;
    bool remoteSortedGroupedSearchUpdated = false;
    uint64_t remoteMetadataRows = 0;
    uint64_t remoteMetadataGroupCount = 0;
    uint64_t remoteMetadataFilterCount = 0;
};

struct ProbeResult {
    std::string runId;
    std::string error;
    double elapsedMs = 0;
    std::vector<CaseResult> cases;
    Checks checks;
    uint64_t providerRequests = 0;
    uint64_t enrichmentRequests = 0;
};

struct ViewSnapshot {
    uint64_t version = 0;
    uint64_t dataVersion = 0;
    size_t rowCount = 0;
    size_t appendOffset = 0;
};

using core::Utf8;
using core::Wide;

double ElapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

JsonValue Number(double value) {
    return JsonValue::CreateNumberValue(value);
}

JsonValue Number(uint64_t value) {
    return JsonValue::CreateNumberValue(static_cast<double>(value));
}

bool SameName(const fs::path& left, const fs::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::optional<fs::path> FinalPath(const fs::path& path, bool directory) {
    const DWORD flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;

    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (!length || length >= buffer.size()) return std::nullopt;

    std::wstring text(buffer.data(), length);
    if (text.rfind(L"\\\\?\\UNC\\", 0) == 0)
        text = L"\\\\" + text.substr(8);
    else if (text.rfind(L"\\\\?\\", 0) == 0)
        text.erase(0, 4);
    if (directory) {
        const size_t rootLength = fs::path(text).root_path().native().size();
        while (text.size() > rootLength && (text.back() == L'\\' || text.back() == L'/'))
            text.pop_back();
    }
    return fs::path(text).lexically_normal();
}

std::optional<fs::path> NativeProfileDirectory() {
    pfc::string8 native;
    try {
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return FinalPath(fs::path(pfc::stringcvt::string_wide_from_utf8(native.get_ptr()).get_ptr()), true);
}

std::optional<Paths> ProbePaths() {
    const auto profile = NativeProfileDirectory();
    if (!profile || !SameName(profile->filename(), fs::path(L"profile")) ||
        !SameName(profile->parent_path().filename(), fs::path(L"fb2k-dev")) ||
        !SameName(profile->parent_path().parent_path().filename(), fs::path(L".local")))
        return std::nullopt;

    const fs::path directory = *profile / L"glasswave" / L"list-session-probe";
    return Paths{
        directory / L"request.json",
        directory / L"request.claimed.json",
        directory / L"result.json",
    };
}

std::optional<std::string> ReadBounded(const fs::path& path, size_t limit) {
    std::error_code error;
    const uintmax_t size = fs::file_size(path, error);
    if (error || size == 0 || size > limit) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes(static_cast<size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) return std::nullopt;
    return bytes;
}

bool ValidRunId(std::string_view value) {
    if (value.size() < 8 || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    });
}

Marker ReadMarker(std::string_view bytes) {
    Marker marker;
    try {
        const JsonObject object = JsonObject::Parse(winrt::to_hstring(std::string(bytes)));
        const auto probe = object.GetNamedString(L"probe");
        marker.runId = winrt::to_string(object.GetNamedString(L"runId"));
        marker.valid = wcscmp(probe.c_str(), L"list-session") == 0 && ValidRunId(marker.runId);
    } catch (...) {
    }
    return marker;
}

struct MainCall;

struct RunControl {
    std::mutex mutex;
    std::shared_ptr<MainCall> activeCall;
    std::atomic_bool stopping = false;
};

struct MainCall {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic_bool cancelled = false;
    bool done = false;
    bool value = false;
};

bool OnMain(RunControl& control, abort_callback& abort, std::function<bool()> action) {
    auto call = std::make_shared<MainCall>();
    {
        std::lock_guard lock(control.mutex);
        if (control.stopping.load(std::memory_order_acquire) || abort.is_aborting() || control.activeCall)
            return false;
        control.activeCall = call;
    }
    try {
        fb2k::inMainThread([call, action = std::move(action)]() mutable {
            bool value = false;
            if (!call->cancelled.load(std::memory_order_acquire)) {
                try {
                    value = action();
                } catch (...) {
                }
            }
            {
                std::lock_guard lock(call->mutex);
                call->value = value;
                call->done = true;
            }
            call->changed.notify_all();
        });
    } catch (...) {
        call->cancelled.store(true, std::memory_order_release);
        std::lock_guard lock(control.mutex);
        if (control.activeCall == call) control.activeCall.reset();
        return false;
    }

    std::unique_lock lock(call->mutex);
    const bool finished = call->changed.wait_until(lock, Clock::now() + kMainCallTimeout, [&] {
        return call->done || control.stopping.load(std::memory_order_acquire) || abort.is_aborting();
    });
    {
        std::lock_guard controlLock(control.mutex);
        if (control.activeCall == call) control.activeCall.reset();
    }
    const bool valid = finished && call->done && call->value &&
                       !control.stopping.load(std::memory_order_acquire) && !abort.is_aborting();
    if (!valid) call->cancelled.store(true, std::memory_order_release);
    return valid;
}

bool Barrier(RunControl& control, abort_callback& abort) {
    return OnMain(control, abort, [] { return true; });
}

bool WaitPolling(std::unique_lock<std::mutex>& lock, std::condition_variable& changed, abort_callback& abort,
                 Clock::time_point deadline, const std::function<bool()>& predicate) {
    while (!abort.is_aborting()) {
        if (predicate()) return true;
        const auto now = Clock::now();
        if (now >= deadline) return false;
        changed.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(100)));
    }
    return false;
}

class FakeProvider final : public Provider {
  public:
    void Start() override {}
    std::wstring_view Key() const override {
        static constexpr wchar_t key[] = L"list-session-test";
        return key;
    }
    std::wstring Name() const override { return L"List session probe"; }
    Capabilities Offers() const override { return {}; }
    bool SignedIn() const override { return false; }
    bool SessionRestoring() const override { return m_sessionRestoring.load(std::memory_order_acquire); }
    bool AccountVerified() const override { return m_accountVerified.load(std::memory_order_acquire); }
    std::wstring AccountName() const override { return {}; }
    void SignIn(HWND) override {}
    void SignOut() override {}
    void RefreshLibrary(Done) override {}

    Id TrackId(const metadb_handle_ptr&) const override { return {}; }
    metadb_handle_ptr TrackItem(const Id& id) const override {
        if (id.empty()) return {};
        const std::string path = "list-session://" + Utf8(id);
        return metadb::get()->handle_create(path.c_str(), 0);
    }
    std::optional<Track> FindTrack(const Id&) const override { return std::nullopt; }
    metadb_handle_ptr ArtItem(ArtKind, const Id&) const override { return {}; }
    bool ReadFavorite(const Id&) const override { return false; }
    std::vector<Playlist> Playlists(Shelf) const override { return {}; }
    std::optional<Shelf> ShelfOf(const Id&) const override { return std::nullopt; }
    std::vector<Artist> FollowedArtists() const override { return {}; }
    std::vector<Album> SavedAlbums() const override { return {}; }
    std::optional<Playlist> FindPlaylist(const Id&) const override { return std::nullopt; }
    std::optional<Album> FindAlbum(const Id&) const override { return std::nullopt; }

    void FetchArtist(const Id&, ArtistDone done) override {
        if (done) done({}, L"Unavailable.");
    }
    void FetchArtistBio(const Id&, TextDone done) override {
        if (done) done({}, L"Unavailable.");
    }

    void FetchListPage(const ListQuery& query, const ListCursor& cursor, size_t limit,
                       ListPageDone done) override {
        {
            std::lock_guard lock(m_mutex);
            m_requests.push_back({query.search, cursor, limit});
            m_pending.push_back({std::move(done), query.cancelled});
        }
        m_changed.notify_all();
    }

    bool EnrichListPage(const ListQuery& query, const ListPage& page, ListPageDone done) override {
        if (!page.enrichmentPending || !done) return false;
        {
            std::lock_guard lock(m_mutex);
            ++m_enrichmentRequests;
            if (query.search == L"enrich-declined") return false;
            m_enrichments.push_back({page, std::move(done), query.cancelled});
        }
        m_changed.notify_all();
        return true;
    }

    ListCachePolicy ListPolicy(const ListQuery&) const override { return ListCachePolicy::Memory; }
    std::string ListCacheContext(const ListQuery&) const override {
        std::lock_guard lock(m_mutex);
        return m_cacheContext;
    }

    void WriteFavorite(const Id&, bool, Done) override {}
    void WriteFollowed(const Id&, bool, Done) override {}
    void WriteAlbumSaved(const Id&, bool, Done) override {}
    void WritePlaylistSaved(const Id&, bool, Done) override {}
    void AddToPlaylist(const Id&, std::vector<Id>, Done) override {}
    void RemoveFromPlaylist(const Id&, std::vector<Id>, Done) override {}
    void CreatePlaylist(std::wstring, CreatedDone done) override {
        if (done) done({}, L"Unavailable.");
    }
    void RenamePlaylist(const Id&, std::wstring, Done) override {}
    void DeletePlaylist(const Id&, Done) override {}
    void ReorderPlaylists(std::vector<Id>, Done) override {}

    bool WaitRequests(size_t count, abort_callback& abort, Clock::time_point deadline) {
        std::unique_lock lock(m_mutex);
        return WaitPolling(lock, m_changed, abort, deadline, [&] { return m_requests.size() >= count; });
    }

    bool WaitPending(size_t count, abort_callback& abort, Clock::time_point deadline) {
        std::unique_lock lock(m_mutex);
        return WaitPolling(lock, m_changed, abort, deadline, [&] { return m_pending.size() >= count; });
    }

    bool WaitEnrichments(size_t count, abort_callback& abort, Clock::time_point deadline) {
        std::unique_lock lock(m_mutex);
        return WaitPolling(lock, m_changed, abort, deadline, [&] { return m_enrichments.size() >= count; });
    }

    size_t PendingCount() const {
        std::lock_guard lock(m_mutex);
        return m_pending.size();
    }

    size_t EnrichmentPendingCount() const {
        std::lock_guard lock(m_mutex);
        return m_enrichments.size();
    }

    uint64_t EnrichmentRequests() const {
        std::lock_guard lock(m_mutex);
        return m_enrichmentRequests;
    }

    bool FrontCancellationSignalled() const {
        std::lock_guard lock(m_mutex);
        return !m_pending.empty() && m_pending.front().cancelled &&
               m_pending.front().cancelled->load(std::memory_order_acquire);
    }

    bool FrontEnrichmentCancellationSignalled() const {
        std::lock_guard lock(m_mutex);
        return !m_enrichments.empty() && m_enrichments.front().cancelled &&
               m_enrichments.front().cancelled->load(std::memory_order_acquire);
    }

    bool FirstTwoEnrichmentTokensShared() const {
        std::lock_guard lock(m_mutex);
        return m_enrichments.size() >= 2 && m_enrichments[0].cancelled && m_enrichments[1].cancelled &&
               m_enrichments[0].cancelled.get() == m_enrichments[1].cancelled.get();
    }

    std::vector<RequestRecord> Requests() const {
        std::lock_guard lock(m_mutex);
        return m_requests;
    }

    bool DeliverNext(ListPage page, std::wstring error) {
        PendingRequest request;
        {
            std::lock_guard lock(m_mutex);
            if (m_pending.empty()) return false;
            request = std::move(m_pending.front());
            m_pending.pop_front();
        }
        if (request.done) request.done(std::move(page), std::move(error));
        return true;
    }

    bool DeliverEnrichment(std::wstring_view firstTrackId, ListPage patch, std::wstring error = {}) {
        PendingEnrichment enrichment;
        {
            std::lock_guard lock(m_mutex);
            const auto found =
                std::find_if(m_enrichments.begin(), m_enrichments.end(), [&](const auto& item) {
                    const auto* rows = std::get_if<std::vector<Track>>(&item.primary.rows);
                    return rows && !rows->empty() && rows->front().id == firstTrackId;
                });
            if (found == m_enrichments.end()) return false;
            enrichment = std::move(*found);
            m_enrichments.erase(found);
        }
        if (enrichment.done) enrichment.done(std::move(patch), std::move(error));
        return true;
    }

    bool DeliverFirstEnrichment(ListPage patch, std::wstring error = {}) {
        PendingEnrichment enrichment;
        {
            std::lock_guard lock(m_mutex);
            if (m_enrichments.empty()) return false;
            enrichment = std::move(m_enrichments.front());
            m_enrichments.pop_front();
        }
        if (enrichment.done) enrichment.done(std::move(patch), std::move(error));
        return true;
    }

    void NotifyAccountChange() { NotifyAccountChanged(); }

    void SetRestoreState(bool restoring, bool verified) {
        m_sessionRestoring.store(restoring, std::memory_order_release);
        m_accountVerified.store(verified, std::memory_order_release);
    }
    void NotifyAccountObservedChange() { NotifyAccountObserved(); }

    void SetCacheContext(std::string context) {
        std::lock_guard lock(m_mutex);
        m_cacheContext = std::move(context);
    }

    void ClearPending() {
        std::lock_guard lock(m_mutex);
        m_pending.clear();
        m_enrichments.clear();
    }

  private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<RequestRecord> m_requests;
    std::deque<PendingRequest> m_pending;
    std::deque<PendingEnrichment> m_enrichments;
    uint64_t m_enrichmentRequests = 0;
    std::string m_cacheContext;
    std::atomic_bool m_sessionRestoring = false;
    std::atomic_bool m_accountVerified = false;
};

class Observer {
  public:
    void Publish(const ListState& state, bool opening) {
        {
            std::lock_guard lock(m_mutex);
            if (opening) m_callbackDuringOpen = true;
            ++m_count;
            m_states.push_back(state);
        }
        m_changed.notify_all();
    }

    bool Wait(abort_callback& abort, Clock::time_point deadline,
              const std::function<bool(const ListState&)>& predicate, ListState* result = nullptr) {
        std::unique_lock lock(m_mutex);
        if (!WaitPolling(lock, m_changed, abort, deadline,
                         [&] { return std::any_of(m_states.begin(), m_states.end(), predicate); }))
            return false;
        const auto found = std::find_if(m_states.begin(), m_states.end(), predicate);
        if (found == m_states.end()) return false;
        if (result) *result = *found;
        return true;
    }

    bool WaitAfterVersion(uint64_t afterVersion, abort_callback& abort, Clock::time_point deadline,
                          const std::function<bool(const ListState&)>& predicate,
                          ListState* result = nullptr) {
        std::unique_lock lock(m_mutex);
        const auto matches = [&](const ListState& state) {
            return state.version > afterVersion && predicate(state);
        };
        if (!WaitPolling(lock, m_changed, abort, deadline,
                         [&] { return std::any_of(m_states.begin(), m_states.end(), matches); }))
            return false;
        const auto found = std::find_if(m_states.begin(), m_states.end(), matches);
        if (found == m_states.end()) return false;
        if (result) *result = *found;
        return true;
    }

    bool Any(std::function<bool(const ListState&)> predicate) const {
        std::lock_guard lock(m_mutex);
        return std::any_of(m_states.begin(), m_states.end(), std::move(predicate));
    }

    uint64_t Count() const {
        std::lock_guard lock(m_mutex);
        return m_count;
    }

    ListState Latest() const {
        std::lock_guard lock(m_mutex);
        return m_states.empty() ? ListState{} : m_states.back();
    }

    bool CallbackDuringOpen() const {
        std::lock_guard lock(m_mutex);
        return m_callbackDuringOpen;
    }

  private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<ListState> m_states;
    uint64_t m_count = 0;
    bool m_callbackDuringOpen = false;
};

class FakePageHost final : public ui::PageHost {
  public:
    const media::LibraryModel& Library() const override {
        static const media::LibraryModel empty;
        return empty;
    }
    const ui::View& CurrentView() const override { return m_view; }
    void Open(const ui::View& view) override { m_view = view; }
    void Replace(const ui::View& view) override { m_view = view; }
    void Reload() override {}
    void Search(const std::wstring&) override {}
    void Refresh() override {}
    void SetPageName(const std::wstring& name) override { m_view.name = name; }
    void ShowSource(std::unique_ptr<media::ListSource> source) override { m_source = std::move(source); }
    media::ListSource* Source() const override { return m_source.get(); }
    void ListChanged() override {
        {
            std::lock_guard lock(m_mutex);
            ++m_listChanges;
        }
        m_changed.notify_all();
    }
    void ChromeChanged() override {}
    media::ViewStyle SavedStyle(media::ViewStyle fallback) const override { return fallback; }
    HWND Owner() const override { return nullptr; }
    std::weak_ptr<int> Lifetime() const override { return m_lifetime; }

    void SetView(ui::View view) { m_view = std::move(view); }
    uint64_t ListChanges() const {
        std::lock_guard lock(m_mutex);
        return m_listChanges;
    }
    bool WaitListChanged(uint64_t after, abort_callback& abort, Clock::time_point deadline) {
        std::unique_lock lock(m_mutex);
        return WaitPolling(lock, m_changed, abort, deadline, [&] { return m_listChanges > after; });
    }

  private:
    ui::View m_view;
    std::unique_ptr<media::ListSource> m_source;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    uint64_t m_listChanges = 0;
};

struct UiFixture {
    FakePageHost host;
    std::unique_ptr<ui::Page> page;
    metadb_handle_ptr selected;
};

struct ProbeContext {
    std::unique_ptr<FakeProvider> provider;
    std::vector<std::shared_ptr<ListSession>> sessions;
    std::vector<std::shared_ptr<UiFixture>> uiFixtures;

    size_t Open(ListQuery query, const std::shared_ptr<Observer>& observer,
                const std::shared_ptr<std::atomic_bool>& opening, ListLoadMode mode) {
        auto changed = [observer, opening](const ListState& state) {
            observer->Publish(state, opening->load(std::memory_order_acquire));
        };
        opening->store(true, std::memory_order_release);
        try {
            auto session = ListSession::Open(*provider, std::move(query), std::move(changed), mode);
            opening->store(false, std::memory_order_release);
            sessions.push_back(std::move(session));
            return sessions.size() - 1;
        } catch (...) {
            opening->store(false, std::memory_order_release);
            throw;
        }
    }

    ListSession* Get(size_t id) { return id < sessions.size() ? sessions[id].get() : nullptr; }

    void Release(size_t id) {
        if (id >= sessions.size() || !sessions[id]) return;
        sessions[id]->Cancel();
        sessions[id].reset();
    }

    void ReleaseAll() {
        for (auto& session : sessions) {
            if (!session) continue;
            session->Cancel();
            session.reset();
        }
        sessions.clear();
        for (const auto& fixture : uiFixtures) {
            if (!fixture) continue;
            fixture->page.reset();
            fixture->host.ShowSource(nullptr);
        }
        uiFixtures.clear();
        if (provider) provider->ClearPending();
    }
};

std::optional<size_t> OpenOne(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                              abort_callback& abort, ListQuery query,
                              const std::shared_ptr<Observer>& observer,
                              const std::shared_ptr<std::atomic_bool>& opening, ListLoadMode mode) {
    size_t id = SIZE_MAX;
    if (!OnMain(control, abort, [&] {
            id = context->Open(std::move(query), observer, opening, mode);
            return id != SIZE_MAX;
        }))
        return std::nullopt;
    return id;
}

std::optional<size_t> OpenOneAndRequestNext(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                                            abort_callback& abort, ListQuery query,
                                            const std::shared_ptr<Observer>& observer,
                                            const std::shared_ptr<std::atomic_bool>& opening,
                                            ListLoadMode mode) {
    size_t id = SIZE_MAX;
    if (!OnMain(control, abort, [&] {
            id = context->Open(std::move(query), observer, opening, mode);
            auto* session = context->Get(id);
            if (!session) return false;
            session->RequestNext();
            return true;
        }))
        return std::nullopt;
    return id;
}

bool OpenPair(const std::shared_ptr<ProbeContext>& context, RunControl& control, abort_callback& abort,
              ListQuery query, const std::shared_ptr<Observer>& first,
              const std::shared_ptr<Observer>& second, const std::shared_ptr<std::atomic_bool>& opening,
              ListLoadMode mode, size_t* firstId, size_t* secondId) {
    return OnMain(control, abort, [&] {
        *firstId = context->Open(query, first, opening, mode);
        *secondId = context->Open(std::move(query), second, opening, mode);
        return true;
    });
}

bool RequestNext(const std::shared_ptr<ProbeContext>& context, size_t id, RunControl& control,
                 abort_callback& abort) {
    return OnMain(control, abort, [context, id] {
        auto* session = context->Get(id);
        if (!session) return false;
        session->RequestNext();
        return true;
    });
}

bool Refresh(const std::shared_ptr<ProbeContext>& context, size_t id, RunControl& control,
             abort_callback& abort) {
    return OnMain(control, abort, [context, id] {
        auto* session = context->Get(id);
        if (!session) return false;
        session->Refresh();
        return true;
    });
}

bool Cancel(const std::shared_ptr<ProbeContext>& context, size_t id, RunControl& control,
            abort_callback& abort) {
    return OnMain(control, abort, [context, id] {
        auto* session = context->Get(id);
        if (!session) return false;
        session->Cancel();
        return true;
    });
}

bool GetState(const std::shared_ptr<ProbeContext>& context, size_t id, RunControl& control,
              abort_callback& abort, ListState* state) {
    return OnMain(control, abort, [context, id, state] {
        auto* session = context->Get(id);
        if (!session) return false;
        *state = session->State();
        return true;
    });
}

bool GetViewSnapshot(const std::shared_ptr<ProbeContext>& context, size_t id, RunControl& control,
                     abort_callback& abort, ViewSnapshot* snapshot) {
    return OnMain(control, abort, [context, id, snapshot] {
        auto* session = context->Get(id);
        if (!session) return false;
        const ListState& view = session->View();
        snapshot->version = view.version;
        snapshot->dataVersion = view.dataVersion;
        snapshot->rowCount = std::visit([](const auto& rows) { return rows.size(); }, view.page.rows);
        snapshot->appendOffset = view.appendOffset;
        return true;
    });
}

bool Deliver(const std::shared_ptr<ProbeContext>& context, RunControl& control, abort_callback& abort,
             ListPage page, std::wstring error = {}) {
    return OnMain(control, abort, [context, page = std::move(page), error = std::move(error)]() mutable {
        return context->provider && context->provider->DeliverNext(std::move(page), std::move(error));
    });
}

bool DeliverEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                       abort_callback& abort, std::wstring firstTrackId, ListPage patch,
                       std::wstring error = {}) {
    return OnMain(control, abort,
                  [context, firstTrackId = std::move(firstTrackId), patch = std::move(patch),
                   error = std::move(error)]() mutable {
                      return context->provider && context->provider->DeliverEnrichment(
                                                      firstTrackId, std::move(patch), std::move(error));
                  });
}

bool DeliverFirstEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                            abort_callback& abort, ListPage patch, std::wstring error = {}) {
    return OnMain(control, abort, [context, patch = std::move(patch), error = std::move(error)]() mutable {
        return context->provider &&
               context->provider->DeliverFirstEnrichment(std::move(patch), std::move(error));
    });
}

class CaseCleanup {
  public:
    CaseCleanup(std::shared_ptr<ProbeContext> context, RunControl& control, abort_callback& abort)
        : m_context(std::move(context)), m_control(control), m_abort(abort) {}

    ~CaseCleanup() {
        if (m_abort.is_aborting() || m_control.stopping.load(std::memory_order_acquire)) return;
        OnMain(m_control, m_abort, [context = m_context, ids = m_ids] {
            for (size_t id : ids)
                context->Release(id);
            if (context->provider) context->provider->ClearPending();
            return true;
        });
    }

    void Keep(size_t id) { m_ids.push_back(id); }

  private:
    std::shared_ptr<ProbeContext> m_context;
    RunControl& m_control;
    abort_callback& m_abort;
    std::vector<size_t> m_ids;
};

class UiCleanup {
  public:
    UiCleanup(std::shared_ptr<UiFixture> fixture, RunControl& control, abort_callback& abort)
        : m_fixture(std::move(fixture)), m_control(control), m_abort(abort) {}

    ~UiCleanup() {
        if (m_abort.is_aborting() || m_control.stopping.load(std::memory_order_acquire)) return;
        OnMain(m_control, m_abort, [fixture = m_fixture] {
            fixture->page.reset();
            fixture->host.ShowSource(nullptr);
            return true;
        });
    }

  private:
    std::shared_ptr<UiFixture> m_fixture;
    RunControl& m_control;
    abort_callback& m_abort;
};

bool StartPlaylistPage(const std::shared_ptr<ProbeContext>& context,
                       const std::shared_ptr<UiFixture>& fixture, const stream::Id& id, RunControl& control,
                       abort_callback& abort) {
    return OnMain(control, abort, [context, fixture, id] {
        auto& provider = *context->provider;
        if (std::find(context->uiFixtures.begin(), context->uiFixtures.end(), fixture) ==
            context->uiFixtures.end())
            context->uiFixtures.push_back(fixture);
        const ui::View view = ui::PlaylistView(provider, id, L"Probe playlist", std::nullopt);
        fixture->host.SetView(view);
        fixture->page = ui::MakeRemotePage(fixture->host, provider, view);
        if (!fixture->page) return false;
        auto source = fixture->page->Start(false);
        if (!source) return false;
        fixture->host.ShowSource(std::move(source));
        return true;
    });
}

bool PageSourceAction(const std::shared_ptr<UiFixture>& fixture, RunControl& control, abort_callback& abort,
                      std::function<void(media::ListSource&)> action) {
    return OnMain(control, abort, [fixture, action = std::move(action)]() mutable {
        auto* source = fixture->host.Source();
        if (!source || !fixture->page) return false;
        action(*source);
        fixture->page->OnPresentationChanged();
        return true;
    });
}

bool DeliverUiPage(const std::shared_ptr<ProbeContext>& context, const std::shared_ptr<UiFixture>& fixture,
                   RunControl& control, abort_callback& abort, ListPage page, Clock::time_point deadline) {
    const uint64_t before = fixture->host.ListChanges();
    return Deliver(context, control, abort, std::move(page)) &&
           fixture->host.WaitListChanged(before, abort, deadline);
}

bool DeliverUiEnrichment(const std::shared_ptr<ProbeContext>& context,
                         const std::shared_ptr<UiFixture>& fixture, RunControl& control,
                         abort_callback& abort, std::wstring firstTrackId, ListPage patch,
                         Clock::time_point deadline, std::wstring error = {}) {
    const uint64_t before = fixture->host.ListChanges();
    return DeliverEnrichment(context, control, abort, std::move(firstTrackId), std::move(patch),
                             std::move(error)) &&
           fixture->host.WaitListChanged(before, abort, deadline);
}

bool UiSourceCount(const std::shared_ptr<UiFixture>& fixture, RunControl& control, abort_callback& abort,
                   size_t* count) {
    return OnMain(control, abort, [fixture, count] {
        auto* source = fixture->host.Source();
        if (!source) return false;
        *count = source->Count();
        return true;
    });
}

bool UiSourceSorted(const std::shared_ptr<UiFixture>& fixture, RunControl& control, abort_callback& abort,
                    bool* sorted) {
    return OnMain(control, abort, [fixture, sorted] {
        auto* source = fixture->host.Source();
        if (!source) return false;
        *sorted = true;
        for (size_t row = 1; row < source->Count(); ++row)
            if (source->Cell(row, 2) < source->Cell(row - 1, 2)) {
                *sorted = false;
                break;
            }
        return true;
    });
}

bool UiMetadataPresentation(const std::shared_ptr<UiFixture>& fixture, RunControl& control,
                            abort_callback& abort, bool* filtered, bool* sorted, bool* focusKept,
                            uint64_t* groupCount, uint64_t* albumACount) {
    return OnMain(control, abort, [fixture, filtered, sorted, focusKept, groupCount, albumACount] {
        auto* source = fixture->host.Source();
        if (!source) return false;
        *filtered = source->HasFilter();
        *sorted = true;
        for (size_t row = 1; row < source->Count(); ++row)
            if (source->Cell(row, 2) < source->Cell(row - 1, 2)) {
                *sorted = false;
                break;
            }
        const size_t selected = source->FindRow(fixture->selected);
        *focusKept = selected != SIZE_MAX && source->IsSelected(selected) &&
                     source->Focus() < source->Count() && source->Item(source->Focus()) == fixture->selected;
        *groupCount = 0;
        *albumACount = 0;
        const auto* groups = source->Groups();
        if (!groups) return false;
        for (const auto& group : *groups) {
            ++*groupCount;
            if (group.header == L"Album A") *albumACount = group.count;
        }
        return true;
    });
}

size_t RowCount(const ListState& state) {
    return std::visit([](const auto& rows) { return rows.size(); }, state.page.rows);
}

ListQuery Query(std::wstring search) {
    ListQuery query;
    query.kind = ListKind::PlaylistTracks;
    query.id = L"probe-list";
    query.search = std::move(search);
    return query;
}

Track TrackRow(size_t index) {
    Track track;
    track.id = L"track-" + std::to_wstring(index);
    track.title = L"Probe track " + std::to_wstring(index);
    track.album = L"Probe album";
    track.albumId = L"probe-album";
    track.artists = {{L"probe-artist", L"Probe artist"}};
    track.durationMs = 180000;
    return track;
}

Track NamedTrack(std::wstring id, std::wstring title) {
    Track track = TrackRow(0);
    track.id = std::move(id);
    track.title = std::move(title);
    return track;
}

std::vector<Track> Tracks(size_t first, size_t count) {
    std::vector<Track> rows;
    rows.reserve(count);
    for (size_t i = 0; i < count; ++i)
        rows.push_back(TrackRow(first + i));
    return rows;
}

ListPage TrackPage(std::vector<Track> rows, bool more, size_t nextOffset,
                   std::wstring revision = L"list-session-r1", std::optional<size_t> total = std::nullopt,
                   bool enrichmentPending = false) {
    ListPage page;
    page.rows = std::move(rows);
    page.more = more;
    page.next.offset = nextOffset;
    page.revision = std::move(revision);
    page.total = total;
    page.enrichmentPending = enrichmentPending;
    return page;
}

ListPage PatchTracks(const ListPage& primary, const std::function<void(Track&, size_t)>& patch) {
    ListPage page = primary;
    page.enrichmentPending = false;
    auto& rows = std::get<std::vector<Track>>(page.rows);
    for (size_t index = 0; index < rows.size(); ++index)
        patch(rows[index], index);
    return page;
}

bool UpdatedRows(const ListState& state, size_t offset, size_t count) {
    return state.updatedRows && state.updatedRows->offset == offset && state.updatedRows->count == count;
}

bool NoUpdatedRows(const ListState& state) {
    return !state.updatedRows.has_value();
}

bool SameTrackIds(const ListState& state, const std::vector<std::wstring>& expected) {
    const auto* rows = std::get_if<std::vector<Track>>(&state.page.rows);
    if (!rows || rows->size() != expected.size()) return false;
    return std::equal(rows->begin(), rows->end(), expected.begin(),
                      [](const Track& track, const std::wstring& id) { return track.id == id; });
}

std::wstring FirstTrackId(const ListPage& page) {
    const auto* rows = std::get_if<std::vector<Track>>(&page.rows);
    return rows && !rows->empty() ? rows->front().id : std::wstring();
}

std::wstring PaddedIndex(size_t value) {
    wchar_t text[16]{};
    swprintf_s(text, L"%05zu", value);
    return text;
}

ListPage UiFilterPage(size_t pageIndex) {
    constexpr std::array<size_t, 6> starts{0, 100, 150, 200, 250, 300};
    constexpr std::array<size_t, 6> sizes{100, 50, 50, 50, 50, 50};
    std::vector<Track> rows;
    rows.reserve(sizes[pageIndex]);
    for (size_t i = 0; i < sizes[pageIndex]; ++i) {
        const size_t global = starts[pageIndex] + i;
        const bool match =
            (pageIndex == 0 && i % 10 == 0) || (pageIndex == 1 && i < 15) || (pageIndex == 2 && i < 10);
        Track track;
        track.id = L"ui-filter-" + std::to_wstring(global);
        track.title = std::wstring(match ? L"Match " : L"Track ") + PaddedIndex(9999 - global);
        track.album = L"Album " + std::to_wstring(global % 3);
        track.albumId = L"album-" + std::to_wstring(global % 3);
        track.artists = {{L"ui-artist", L"UI artist"}};
        track.durationMs = 120000;
        rows.push_back(std::move(track));
    }
    return TrackPage(std::move(rows), pageIndex + 1 < sizes.size(), starts[pageIndex] + sizes[pageIndex]);
}

ListPage UiTilePage(size_t pageIndex) {
    const size_t count = pageIndex == 0 ? 100 : 20;
    std::vector<Track> rows;
    rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const bool albumA = pageIndex == 0 ? i < 10 : true;
        Track track;
        track.id = L"ui-tile-" + std::to_wstring(pageIndex) + L"-" + std::to_wstring(i);
        track.title = L"Tile track " + std::to_wstring(pageIndex) + L"-" + PaddedIndex(i);
        track.album = albumA ? L"Album A" : L"Album B";
        track.albumId = albumA ? L"album-a" : L"album-b";
        track.artists = {{L"ui-artist", L"UI artist"}};
        track.durationMs = 120000;
        rows.push_back(std::move(track));
    }
    return TrackPage(std::move(rows), pageIndex == 0, pageIndex == 0 ? 100 : 120);
}

ListPage UiEnrichedPage(size_t pageIndex, bool patched) {
    constexpr std::array<size_t, 3> starts{0, 100, 120};
    constexpr std::array<size_t, 3> sizes{100, 20, 10};
    std::vector<Track> rows;
    rows.reserve(sizes[pageIndex]);
    for (size_t i = 0; i < sizes[pageIndex]; ++i) {
        const size_t global = starts[pageIndex] + i;
        Track track;
        track.id = L"ui-enrich-" + std::to_wstring(global);
        const std::wstring prefix =
            patched ? (pageIndex == 2 ? L"Alpha metadata " : L"Beta metadata ") : L"metadata baseline ";
        track.title = prefix + PaddedIndex(global);
        const bool albumA = pageIndex == 2 || (pageIndex == 0 && i < 50);
        track.album = albumA ? L"Album A" : L"Album B";
        track.albumId = albumA ? L"album-a" : L"album-b";
        track.artists = {{L"ui-artist", L"UI artist"}};
        track.durationMs = 120000;
        rows.push_back(std::move(track));
    }
    return TrackPage(std::move(rows), pageIndex + 1 < sizes.size(), starts[pageIndex] + sizes[pageIndex],
                     L"ui-enrich-r1", 130, true);
}

std::shared_ptr<Observer> NewObserver() {
    return std::make_shared<Observer>();
}

std::shared_ptr<std::atomic_bool> NewOpening() {
    return std::make_shared<std::atomic_bool>(false);
}

void AddCase(ProbeResult& result, std::string name, bool ok, std::string detail = {}) {
    result.cases.push_back({std::move(name), ok, std::move(detail)});
}

Clock::time_point CaseDeadline(Clock::time_point globalDeadline) {
    return std::min(globalDeadline, Clock::now() + kCaseBudget);
}

void CaseOnDemand(const std::shared_ptr<ProbeContext>& context, RunControl& control, abort_callback& abort,
                  Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"on-demand"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "on_demand_deferred_single_page", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    if (!provider.WaitPending(1, abort, deadline)) {
        AddCase(result, "on_demand_deferred_single_page", false, "initial_fetch_missing");
        return;
    }

    ListState state;
    if (!Deliver(context, control, abort, TrackPage(Tracks(0, 100), true, 100)) ||
        !observer->Wait(
            abort, deadline, [](const ListState& value) { return !value.loading && RowCount(value) == 100; },
            &state)) {
        AddCase(result, "on_demand_deferred_single_page", false, "first_page_not_published");
        return;
    }
    const size_t firstAppendOffset = state.appendOffset;
    result.checks.ordinaryMetadataReadyRows = state.metadataReadyRows;
    result.checks.ordinaryUpdatedRowsAbsent = NoUpdatedRows(state);
    ViewSnapshot firstView;
    if (!GetViewSnapshot(context, *id, control, abort, &firstView) || firstAppendOffset != 0 ||
        firstView.version != state.version || firstView.dataVersion != state.dataVersion ||
        firstView.rowCount != RowCount(state) || firstView.appendOffset != firstAppendOffset) {
        AddCase(result, "on_demand_deferred_single_page", false, "first_view_mismatch");
        return;
    }
    const size_t callsBeforeNext = provider.Requests().size() - base;
    if (callsBeforeNext != 1 || provider.PendingCount() != 0) {
        AddCase(result, "on_demand_deferred_single_page", false, "on_demand_auto_drained");
        return;
    }

    if (!RequestNext(context, *id, control, abort) || !provider.WaitRequests(base + 2, abort, deadline)) {
        AddCase(result, "on_demand_deferred_single_page", false, "request_next_missing");
        return;
    }
    const auto requests = provider.Requests();
    if (requests.size() < base + 2 || requests[base + 1].cursor.offset != 100 ||
        !Deliver(context, control, abort, TrackPage(Tracks(100, 50), false, 150)) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) { return value.complete && !value.loading && RowCount(value) == 150; },
            &state)) {
        AddCase(result, "on_demand_deferred_single_page", false, "second_page_not_published");
        return;
    }
    ViewSnapshot secondView;
    if (!GetViewSnapshot(context, *id, control, abort, &secondView) || state.appendOffset != 100 ||
        secondView.version != state.version || secondView.dataVersion != state.dataVersion ||
        secondView.rowCount != RowCount(state) || secondView.appendOffset != state.appendOffset) {
        AddCase(result, "on_demand_deferred_single_page", false, "second_view_mismatch");
        return;
    }

    result.checks.callbackDeferred = !observer->CallbackDuringOpen();
    result.checks.onDemandCallsBeforeNext = callsBeforeNext;
    result.checks.onDemandCounts = {100, static_cast<uint64_t>(RowCount(state))};
    const bool pass = result.checks.callbackDeferred && result.checks.ordinaryMetadataReadyRows == 100 &&
                      result.checks.ordinaryUpdatedRowsAbsent;
    AddCase(result, "on_demand_deferred_single_page", pass,
            pass ? std::string() : "ordinary_page_was_not_immediately_metadata_ready");
}

void CaseAll(const std::shared_ptr<ProbeContext>& context, RunControl& control, abort_callback& abort,
             Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"all-mode"), observer, opening, ListLoadMode::All);
    if (!id) {
        AddCase(result, "all_mode_page_progress", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    const std::array<size_t, 3> sizes{100, 100, 50};
    size_t total = 0;
    for (size_t pageIndex = 0; pageIndex < sizes.size(); ++pageIndex) {
        if (!provider.WaitPending(1, abort, deadline)) {
            AddCase(result, "all_mode_page_progress", false, "auto_drain_fetch_missing");
            return;
        }
        const auto requests = provider.Requests();
        if (requests.size() <= base + pageIndex || requests[base + pageIndex].cursor.offset != total) {
            AddCase(result, "all_mode_page_progress", false, "unexpected_progress_cursor");
            return;
        }
        const size_t next = total + sizes[pageIndex];
        const bool more = pageIndex + 1 < sizes.size();
        if (!Deliver(context, control, abort, TrackPage(Tracks(total, sizes[pageIndex]), more, next))) {
            AddCase(result, "all_mode_page_progress", false, "controlled_page_missing");
            return;
        }
        total = next;
        ListState state;
        if (!observer->Wait(
                abort, deadline,
                [total, pageIndex](const ListState& value) {
                    return !value.loading && RowCount(value) == total && value.pages == pageIndex + 1;
                },
                &state)) {
            AddCase(result, "all_mode_page_progress", false, "page_progress_not_published");
            return;
        }
        result.checks.allProgress.push_back(RowCount(state));
    }
    const bool pass = provider.PendingCount() == 0 && provider.Requests().size() == base + 3;
    AddCase(result, "all_mode_page_progress", pass, pass ? std::string() : "all_mode_did_not_settle");
}

void CaseDuplicates(const std::shared_ptr<ProbeContext>& context, RunControl& control, abort_callback& abort,
                    Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"duplicates"), observer, opening, ListLoadMode::All);
    if (!id) {
        AddCase(result, "duplicate_rows_retained", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);

    auto first = Tracks(0, 100);
    first[0].id = L"duplicate-row";
    first[5] = first[0];
    if (!provider.WaitRequests(base + 1, abort, deadline) || provider.Requests()[base].cursor.offset != 0 ||
        !Deliver(context, control, abort, TrackPage(std::move(first), true, 100))) {
        AddCase(result, "duplicate_rows_retained", false, "first_page_missing");
        return;
    }
    auto secondRows = Tracks(100, 100);
    secondRows[20].id = L"duplicate-row";
    secondRows[20].title = L"Probe track 0";
    if (!provider.WaitRequests(base + 2, abort, deadline) ||
        provider.Requests()[base + 1].cursor.offset != 100 ||
        !Deliver(context, control, abort, TrackPage(std::move(secondRows), true, 200))) {
        AddCase(result, "duplicate_rows_retained", false, "second_page_missing");
        return;
    }
    if (!provider.WaitRequests(base + 3, abort, deadline) ||
        provider.Requests()[base + 2].cursor.offset != 200 ||
        !Deliver(context, control, abort, TrackPage(Tracks(200, 50), false, 250))) {
        AddCase(result, "duplicate_rows_retained", false, "third_page_missing");
        return;
    }

    ListState state;
    if (!observer->Wait(
            abort, deadline,
            [](const ListState& value) { return value.complete && !value.loading && RowCount(value) == 250; },
            &state)) {
        AddCase(result, "duplicate_rows_retained", false, "final_state_missing");
        return;
    }
    uint64_t duplicateCount = 0;
    const auto& rows = std::get<std::vector<Track>>(state.page.rows);
    for (const auto& row : rows)
        if (row.id == L"duplicate-row") ++duplicateCount;
    result.checks.duplicateRows = RowCount(state);
    result.checks.duplicateIdOccurrences = duplicateCount;
    const bool pass =
        result.checks.duplicateRows == 250 && duplicateCount == 3 && provider.Requests().size() == base + 3;
    AddCase(result, "duplicate_rows_retained", pass, pass ? std::string() : "duplicate_rows_were_collapsed");
}

void CaseTwoLeases(const std::shared_ptr<ProbeContext>& context, RunControl& control, abort_callback& abort,
                   Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto firstObserver = NewObserver();
    const auto secondObserver = NewObserver();
    const auto opening = NewOpening();
    size_t first = SIZE_MAX, second = SIZE_MAX;
    if (!OpenPair(context, control, abort, Query(L"two-leases"), firstObserver, secondObserver, opening,
                  ListLoadMode::OnDemand, &first, &second)) {
        AddCase(result, "two_leases_coalesce_isolated_cancel", false, "open_pair_failed");
        return;
    }
    cleanup.Keep(first);
    cleanup.Keep(second);
    if (!provider.WaitRequests(base + 1, abort, deadline)) {
        AddCase(result, "two_leases_coalesce_isolated_cancel", false, "coalesced_request_missing");
        return;
    }
    if (provider.Requests().size() != base + 1 ||
        !Deliver(context, control, abort, TrackPage(Tracks(0, 100), true, 100))) {
        AddCase(result, "two_leases_coalesce_isolated_cancel", false, "leases_did_not_coalesce");
        return;
    }
    if (!firstObserver->Wait(
            abort, deadline,
            [](const ListState& value) { return !value.loading && RowCount(value) == 100; }) ||
        !secondObserver->Wait(abort, deadline, [](const ListState& value) {
            return !value.loading && RowCount(value) == 100;
        })) {
        AddCase(result, "two_leases_coalesce_isolated_cancel", false, "lease_callback_missing");
        return;
    }
    const uint64_t firstCallbacks = firstObserver->Count();
    if (!Cancel(context, first, control, abort) || !RequestNext(context, second, control, abort) ||
        !provider.WaitRequests(base + 2, abort, deadline) ||
        !Deliver(context, control, abort, TrackPage(Tracks(100, 50), false, 150))) {
        AddCase(result, "two_leases_coalesce_isolated_cancel", false, "remaining_lease_did_not_continue");
        return;
    }
    if (!secondObserver->Wait(abort, deadline, [](const ListState& value) {
            return value.complete && !value.loading && RowCount(value) == 150;
        })) {
        AddCase(result, "two_leases_coalesce_isolated_cancel", false, "remaining_lease_terminal_missing");
        return;
    }
    Barrier(control, abort);
    result.checks.coalescedCalls = provider.Requests().size() - base;
    result.checks.isolatedCallbackDelta = firstObserver->Count() - firstCallbacks;
    const bool pass = result.checks.coalescedCalls == 2 && result.checks.isolatedCallbackDelta == 0;
    AddCase(result, "two_leases_coalesce_isolated_cancel", pass,
            pass ? std::string() : "cancel_leaked_to_remaining_lease");
}

void CaseCacheContext(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                      abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();

    // The embedded NUL makes accidental string concatenation observable as a distinct context.
    const std::string contextA("jp:US\0account-a", 15);
    const std::string contextB("jp:US;account-b");
    provider.SetCacheContext(contextA);

    const auto first = OpenOne(context, control, abort, Query(L"cache-context"), NewObserver(), NewOpening(),
                               ListLoadMode::OnDemand);
    if (!first) {
        AddCase(result, "cache_context_identity", false, "first_open_failed");
        return;
    }
    cleanup.Keep(*first);
    if (!provider.WaitRequests(base + 1, abort, deadline)) {
        AddCase(result, "cache_context_identity", false, "first_request_missing");
        return;
    }

    const auto same = OpenOne(context, control, abort, Query(L"cache-context"), NewObserver(), NewOpening(),
                              ListLoadMode::OnDemand);
    if (!same) {
        AddCase(result, "cache_context_identity", false, "same_context_open_failed");
        return;
    }
    cleanup.Keep(*same);
    if (!Barrier(control, abort)) {
        AddCase(result, "cache_context_identity", false, "same_context_barrier_failed");
        return;
    }
    result.checks.sameContextCoalesced = provider.Requests().size() == base + 1;

    provider.SetCacheContext(contextB);
    const auto changed = OpenOne(context, control, abort, Query(L"cache-context"), NewObserver(),
                                 NewOpening(), ListLoadMode::OnDemand);
    if (!changed || !provider.WaitRequests(base + 2, abort, deadline)) {
        AddCase(result, "cache_context_identity", false, "changed_context_did_not_isolate");
        return;
    }
    cleanup.Keep(*changed);
    result.checks.changedContextIsolated = provider.Requests().size() == base + 2;

    provider.SetCacheContext(contextA);
    const auto originalAgain = OpenOne(context, control, abort, Query(L"cache-context"), NewObserver(),
                                       NewOpening(), ListLoadMode::OnDemand);
    if (!originalAgain) {
        AddCase(result, "cache_context_identity", false, "original_context_reopen_failed");
        return;
    }
    cleanup.Keep(*originalAgain);
    if (!Barrier(control, abort)) {
        AddCase(result, "cache_context_identity", false, "captured_context_barrier_failed");
        return;
    }
    result.checks.capturedContextReused = provider.Requests().size() == base + 2;
    result.checks.cacheContextRequestCount = provider.Requests().size() - base;
    const bool pass = result.checks.sameContextCoalesced && result.checks.changedContextIsolated &&
                      result.checks.capturedContextReused && result.checks.cacheContextRequestCount == 2 &&
                      provider.PendingCount() == 2;
    AddCase(result, "cache_context_identity", pass,
            pass ? std::string() : "context_was_not_captured_in_read_identity");
}

void CaseCacheContextLateData(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                              abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const std::string contextA("cn:US;account-a");
    const std::string contextB("jp:US;account-b");
    provider.SetCacheContext(contextA);

    const auto fetchObserver = NewObserver();
    const auto fetchId = OpenOne(context, control, abort, Query(L"cache-context-late-fetch"), fetchObserver,
                                 NewOpening(), ListLoadMode::OnDemand);
    if (!fetchId || !provider.WaitRequests(base + 1, abort, deadline) ||
        !Deliver(context, control, abort, TrackPage(Tracks(10, 1), true, 11)) ||
        !fetchObserver->Wait(abort, deadline,
                             [](const ListState& state) { return !state.loading && RowCount(state) == 1; })) {
        AddCase(result, "cache_context_rejects_late_data", false, "fetch_guard_setup_failed");
        return;
    }
    cleanup.Keep(*fetchId);

    const auto oldObserver = NewObserver();
    const auto oldId = OpenOne(context, control, abort, Query(L"cache-context-late"), oldObserver,
                               NewOpening(), ListLoadMode::OnDemand);
    if (!oldId || !provider.WaitRequests(base + 2, abort, deadline)) {
        AddCase(result, "cache_context_rejects_late_data", false, "old_open_failed");
        return;
    }
    cleanup.Keep(*oldId);

    const ListPage primary = TrackPage(Tracks(0, 2), true, 2, L"cache-context-late-r1", 3, true);
    ListState oldBefore;
    if (!Deliver(context, control, abort, primary) ||
        !oldObserver->Wait(
            abort, deadline,
            [](const ListState& state) {
                return !state.loading && RowCount(state) == 2 && state.metadataReadyRows == 0;
            },
            &oldBefore) ||
        !provider.WaitEnrichments(1, abort, deadline) || !RequestNext(context, *oldId, control, abort) ||
        !provider.WaitRequests(base + 3, abort, deadline)) {
        AddCase(result, "cache_context_rejects_late_data", false, "old_page_setup_failed");
        return;
    }

    provider.SetCacheContext(contextB);
    const auto newObserver = NewObserver();
    const auto newId = OpenOne(context, control, abort, Query(L"cache-context-late"), newObserver,
                               NewOpening(), ListLoadMode::OnDemand);
    if (!newId || !provider.WaitRequests(base + 4, abort, deadline)) {
        AddCase(result, "cache_context_rejects_late_data", false, "new_context_open_failed");
        return;
    }
    cleanup.Keep(*newId);
    if (!RequestNext(context, *fetchId, control, abort) || !Barrier(control, abort)) {
        AddCase(result, "cache_context_rejects_late_data", false, "late_fetch_guard_failed");
        return;
    }
    const bool fetchRejected = provider.Requests().size() == base + 4 && provider.PendingCount() == 2;

    // The old continuation is first in the controlled queue. Its callback must not append a row
    // under the captured identity after the provider has moved to context B.
    if (!Deliver(context, control, abort, TrackPage(Tracks(2, 1), true, 3, L"cache-context-late-r1", 3)) ||
        !Barrier(control, abort)) {
        AddCase(result, "cache_context_rejects_late_data", false, "late_primary_delivery_failed");
        return;
    }
    ListState oldAfterPrimary;
    const bool primaryRejected = GetState(context, *oldId, control, abort, &oldAfterPrimary) &&
                                 RowCount(oldAfterPrimary) == RowCount(oldBefore) &&
                                 oldAfterPrimary.dataVersion == oldBefore.dataVersion;

    ListPage oldPatch =
        PatchTracks(primary, [](Track& track, size_t) { track.title = L"Old context enrichment"; });
    if (!DeliverFirstEnrichment(context, control, abort, std::move(oldPatch)) || !Barrier(control, abort)) {
        AddCase(result, "cache_context_rejects_late_data", false, "late_enrichment_delivery_failed");
        return;
    }
    ListState oldAfterEnrichment;
    const bool enrichmentRejected =
        GetState(context, *oldId, control, abort, &oldAfterEnrichment) &&
        RowCount(oldAfterEnrichment) == RowCount(oldBefore) &&
        oldAfterEnrichment.metadataReadyRows == oldBefore.metadataReadyRows &&
        std::get<std::vector<Track>>(oldAfterEnrichment.page.rows)[0].title != L"Old context enrichment";

    std::vector<Track> newRows = Tracks(99, 1);
    newRows[0].id = L"new-context-row";
    newRows[0].title = L"New context row";
    if (!Deliver(context, control, abort, TrackPage(std::move(newRows), false, 1)) ||
        !newObserver->Wait(abort, deadline, [](const ListState& state) {
            return state.complete && !state.loading && RowCount(state) == 1;
        })) {
        AddCase(result, "cache_context_rejects_late_data", false, "new_context_page_failed");
        return;
    }
    ListState newState;
    const bool dataIsolated = GetState(context, *newId, control, abort, &newState) &&
                              RowCount(newState) == 1 &&
                              std::get<std::vector<Track>>(newState.page.rows)[0].id == L"new-context-row" &&
                              provider.Requests().size() == base + 4 && provider.PendingCount() == 0;
    result.checks.lateContextFetchRejected = fetchRejected;
    result.checks.lateContextPrimaryRejected = primaryRejected;
    result.checks.lateContextEnrichmentRejected = enrichmentRejected;
    result.checks.lateContextDataIsolated = dataIsolated;
    const bool pass = fetchRejected && primaryRejected && enrichmentRejected && dataIsolated;
    AddCase(result, "cache_context_rejects_late_data", pass,
            pass ? std::string() : "old_context_callback_leaked_into_new_read");
}

void CaseCancelAll(const std::shared_ptr<ProbeContext>& context, RunControl& control, abort_callback& abort,
                   Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto firstObserver = NewObserver();
    const auto secondObserver = NewObserver();
    const auto opening = NewOpening();
    size_t first = SIZE_MAX, second = SIZE_MAX;
    if (!OpenPair(context, control, abort, Query(L"cancel-all"), firstObserver, secondObserver, opening,
                  ListLoadMode::All, &first, &second)) {
        AddCase(result, "last_lease_cancel_stops_auto_drain", false, "open_pair_failed");
        return;
    }
    cleanup.Keep(first);
    cleanup.Keep(second);
    if (!provider.WaitPending(1, abort, deadline) ||
        !Deliver(context, control, abort, TrackPage(Tracks(0, 100), true, 100))) {
        AddCase(result, "last_lease_cancel_stops_auto_drain", false, "first_page_failed");
        return;
    }
    if (!firstObserver->Wait(
            abort, deadline,
            [](const ListState& value) { return !value.loading && RowCount(value) == 100; }) ||
        !secondObserver->Wait(
            abort, deadline,
            [](const ListState& value) { return !value.loading && RowCount(value) == 100; }) ||
        !provider.WaitRequests(base + 2, abort, deadline)) {
        AddCase(result, "last_lease_cancel_stops_auto_drain", false, "auto_drain_missing");
        return;
    }
    const uint64_t firstBefore = firstObserver->Count();
    const uint64_t secondBefore = secondObserver->Count();
    if (!Cancel(context, first, control, abort) || !Cancel(context, second, control, abort)) {
        AddCase(result, "last_lease_cancel_stops_auto_drain", false, "cancel_failed");
        return;
    }
    result.checks.cancelTokenSignalled = provider.FrontCancellationSignalled();
    result.checks.autoDrainCallsBeforeCancel = provider.Requests().size() - base;
    // Deliver into the engine after cancellation to exercise its generation guard.
    if (!Deliver(context, control, abort, TrackPage(Tracks(100, 50), false, 150))) {
        AddCase(result, "last_lease_cancel_stops_auto_drain", false, "late_callback_missing");
        return;
    }
    Barrier(control, abort);
    result.checks.autoDrainCallsAfterLate = provider.Requests().size() - base;
    const bool pass = result.checks.cancelTokenSignalled && firstObserver->Count() == firstBefore &&
                      secondObserver->Count() == secondBefore && provider.PendingCount() == 0 &&
                      result.checks.autoDrainCallsBeforeCancel == 2 &&
                      result.checks.autoDrainCallsAfterLate == 2;
    AddCase(result, "last_lease_cancel_stops_auto_drain", pass,
            pass ? std::string() : "late_callback_or_drain_escaped_cancellation");
}

void CaseRefreshGeneration(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                           abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id = OpenOne(context, control, abort, Query(L"refresh-generation"), observer, opening,
                            ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "refresh_rejects_old_generation", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    if (!provider.WaitPending(1, abort, deadline) || !Refresh(context, *id, control, abort) ||
        !provider.WaitRequests(base + 2, abort, deadline)) {
        AddCase(result, "refresh_rejects_old_generation", false, "refresh_request_missing");
        return;
    }
    result.checks.refreshTokenSignalled = provider.FrontCancellationSignalled();

    if (!Deliver(context, control, abort, TrackPage({TrackRow(9000)}, false, 1)) ||
        !Barrier(control, abort)) {
        AddCase(result, "refresh_rejects_old_generation", false, "old_callback_delivery_failed");
        return;
    }
    const bool oldIgnored = !observer->Any([](const ListState& value) { return RowCount(value) == 1; });
    if (!Deliver(context, control, abort, TrackPage({TrackRow(1), TrackRow(2)}, false, 2, L"r2"))) {
        AddCase(result, "refresh_rejects_old_generation", false, "new_callback_delivery_failed");
        return;
    }
    ListState state;
    if (!observer->Wait(
            abort, deadline,
            [](const ListState& value) { return value.complete && !value.loading && RowCount(value) == 2; },
            &state)) {
        AddCase(result, "refresh_rejects_old_generation", false, "refreshed_state_missing");
        return;
    }
    ViewSnapshot refreshedView;
    if (state.appendOffset != 0 || !GetViewSnapshot(context, *id, control, abort, &refreshedView) ||
        refreshedView.version != state.version || refreshedView.dataVersion != state.dataVersion ||
        refreshedView.rowCount != RowCount(state) || refreshedView.appendOffset != 0) {
        AddCase(result, "refresh_rejects_old_generation", false, "refreshed_view_mismatch");
        return;
    }
    result.checks.refreshedCount = RowCount(state);
    result.checks.oldGenerationIgnored = oldIgnored;
    const bool pass = result.checks.refreshTokenSignalled && oldIgnored && result.checks.refreshedCount == 2;
    AddCase(result, "refresh_rejects_old_generation", pass,
            pass ? std::string() : "old_generation_was_applied");
}

void CaseRepeatedCursor(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                        abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id = OpenOne(context, control, abort, Query(L"repeated-cursor"), observer, opening,
                            ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "repeated_cursor_rejected", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    ListState state;
    if (!provider.WaitPending(1, abort, deadline) ||
        !Deliver(context, control, abort, TrackPage({}, true, 0)) ||
        !observer->Wait(
            abort, deadline, [](const ListState& value) { return !value.error.empty(); }, &state)) {
        AddCase(result, "repeated_cursor_rejected", false, "repeated_cursor_not_rejected");
        return;
    }
    result.checks.repeatedCursorCalls = provider.Requests().size() - base;
    const bool pass = result.checks.repeatedCursorCalls == 1 && RowCount(state) == 0;
    AddCase(result, "repeated_cursor_rejected", pass, pass ? std::string() : "repeated_cursor_was_accepted");
}

void CaseNoProgress(const std::shared_ptr<ProbeContext>& context, RunControl& control, abort_callback& abort,
                    Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"empty-pages"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "empty_page_progress_bounded", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    for (size_t page = 1; page <= 9; ++page) {
        if (!provider.WaitPending(1, abort, deadline) ||
            !Deliver(context, control, abort, TrackPage({}, true, page))) {
            AddCase(result, "empty_page_progress_bounded", false, "empty_page_delivery_failed");
            return;
        }
        if (page < 9 && !provider.WaitRequests(base + page + 1, abort, deadline)) {
            AddCase(result, "empty_page_progress_bounded", false, "empty_page_auto_drain_missing");
            return;
        }
    }
    ListState state;
    if (!observer->Wait(
            abort, deadline, [](const ListState& value) { return !value.error.empty(); }, &state)) {
        AddCase(result, "empty_page_progress_bounded", false, "empty_page_limit_not_reported");
        return;
    }
    result.checks.noProgressCalls = provider.Requests().size() - base;
    result.checks.noProgressBounded = result.checks.noProgressCalls == 9 && provider.PendingCount() == 0;
    AddCase(result, "empty_page_progress_bounded", result.checks.noProgressBounded,
            result.checks.noProgressBounded ? std::string() : "empty_page_drain_was_unbounded");
}

void CaseErrorRetry(const std::shared_ptr<ProbeContext>& context, RunControl& control, abort_callback& abort,
                    Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"error-retry"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "error_prefix_retry_same_cursor", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    ListState state;
    if (!provider.WaitPending(1, abort, deadline) ||
        !Deliver(context, control, abort, TrackPage(Tracks(0, 100), true, 100)) ||
        !observer->Wait(
            abort, deadline, [](const ListState& value) { return !value.loading && RowCount(value) == 100; },
            &state)) {
        AddCase(result, "error_prefix_retry_same_cursor", false, "valid_prefix_missing");
        return;
    }
    if (!RequestNext(context, *id, control, abort) || !provider.WaitRequests(base + 2, abort, deadline) ||
        !Deliver(context, control, abort, {}, L"controlled page error") ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return !value.loading && !value.error.empty() && RowCount(value) == 100;
            },
            &state)) {
        AddCase(result, "error_prefix_retry_same_cursor", false, "error_did_not_retain_prefix");
        return;
    }
    result.checks.errorPrefixCount = RowCount(state);
    const auto requests = provider.Requests();
    if (requests.size() < base + 2) {
        AddCase(result, "error_prefix_retry_same_cursor", false, "error_cursor_missing");
        return;
    }
    const size_t errorCursor = requests[base + 1].cursor.offset;
    if (!RequestNext(context, *id, control, abort) || !provider.WaitRequests(base + 3, abort, deadline)) {
        AddCase(result, "error_prefix_retry_same_cursor", false, "retry_request_missing");
        return;
    }
    const auto retryRequests = provider.Requests();
    result.checks.retryCursorOffset = retryRequests[base + 2].cursor.offset;
    result.checks.retrySameCursor = errorCursor == result.checks.retryCursorOffset;
    if (!result.checks.retrySameCursor ||
        !Deliver(context, control, abort, TrackPage(Tracks(100, 50), false, 150)) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return value.complete && !value.loading && value.error.empty() && RowCount(value) == 150;
            },
            &state)) {
        AddCase(result, "error_prefix_retry_same_cursor", false, "retry_did_not_complete");
        return;
    }
    result.checks.finalRetryCount = RowCount(state);
    const bool pass = result.checks.errorPrefixCount == 100 && result.checks.retrySameCursor &&
                      result.checks.finalRetryCount == 150;
    AddCase(result, "error_prefix_retry_same_cursor", pass,
            pass ? std::string() : "retry_cursor_or_final_prefix_changed");
}

void CaseEarlyNext(const std::shared_ptr<ProbeContext>& context, RunControl& control, abort_callback& abort,
                   Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id = OpenOneAndRequestNext(context, control, abort, Query(L"early-next"), observer, opening,
                                          ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "early_next_before_deferred_start", false, "open_and_next_failed");
        return;
    }
    cleanup.Keep(*id);
    if (!provider.WaitRequests(base + 1, abort, deadline) || provider.Requests().size() != base + 1 ||
        !Deliver(context, control, abort, TrackPage(Tracks(0, 100), true, 100))) {
        AddCase(result, "early_next_before_deferred_start", false, "initial_page_failed");
        return;
    }
    ListState state;
    if (!observer->Wait(
            abort, deadline, [](const ListState& value) { return !value.loading && RowCount(value) == 100; },
            &state) ||
        !provider.WaitRequests(base + 2, abort, deadline)) {
        AddCase(result, "early_next_before_deferred_start", false, "deferred_next_demand_lost");
        return;
    }
    const auto requests = provider.Requests();
    if (requests.size() != base + 2 || requests[base + 1].cursor.offset != 100 ||
        !Deliver(context, control, abort, TrackPage(Tracks(100, 50), false, 150)) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) { return value.complete && !value.loading && RowCount(value) == 150; },
            &state)) {
        AddCase(result, "early_next_before_deferred_start", false, "second_page_failed");
        return;
    }
    result.checks.earlyNextRequests = provider.Requests().size() - base;
    result.checks.earlyNextCounts = {100, RowCount(state)};
    const bool pass = result.checks.earlyNextRequests == 2 && provider.PendingCount() == 0;
    AddCase(result, "early_next_before_deferred_start", pass,
            pass ? std::string() : "early_next_request_count_changed");
}

void CaseCancelledDemand(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                         abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto firstObserver = NewObserver();
    const auto secondObserver = NewObserver();
    const auto opening = NewOpening();
    size_t first = SIZE_MAX, second = SIZE_MAX;
    if (!OpenPair(context, control, abort, Query(L"cancelled-demand"), firstObserver, secondObserver, opening,
                  ListLoadMode::OnDemand, &first, &second)) {
        AddCase(result, "cancelled_lease_demand_is_isolated", false, "open_pair_failed");
        return;
    }
    cleanup.Keep(first);
    cleanup.Keep(second);
    if (!provider.WaitRequests(base + 1, abort, deadline) || !RequestNext(context, first, control, abort) ||
        !Cancel(context, first, control, abort)) {
        AddCase(result, "cancelled_lease_demand_is_isolated", false, "setup_failed");
        return;
    }
    const uint64_t cancelledCallbacks = firstObserver->Count();
    if (!Deliver(context, control, abort, TrackPage(Tracks(0, 100), true, 100)) ||
        !secondObserver->Wait(abort, deadline, [](const ListState& value) {
            return !value.loading && RowCount(value) == 100;
        })) {
        AddCase(result, "cancelled_lease_demand_is_isolated", false, "first_page_failed");
        return;
    }
    Barrier(control, abort);
    result.checks.cancelledNextRequestsBeforeB = provider.Requests().size() - base;
    if (result.checks.cancelledNextRequestsBeforeB != 1 || provider.PendingCount() != 0 ||
        firstObserver->Count() != cancelledCallbacks) {
        AddCase(result, "cancelled_lease_demand_is_isolated", false, "cancelled_demand_leaked_to_b");
        return;
    }
    if (!RequestNext(context, second, control, abort) || !provider.WaitRequests(base + 2, abort, deadline)) {
        AddCase(result, "cancelled_lease_demand_is_isolated", false, "explicit_b_next_missing");
        return;
    }
    const auto requests = provider.Requests();
    ListState state;
    if (requests.size() != base + 2 || requests[base + 1].cursor.offset != 100 ||
        !Deliver(context, control, abort, TrackPage(Tracks(100, 50), false, 150)) ||
        !secondObserver->Wait(
            abort, deadline,
            [](const ListState& value) { return value.complete && !value.loading && RowCount(value) == 150; },
            &state)) {
        AddCase(result, "cancelled_lease_demand_is_isolated", false, "b_continuation_failed");
        return;
    }
    result.checks.cancelledNextRequestsAfterB = provider.Requests().size() - base;
    result.checks.cancelledNextCount = RowCount(state);
    const bool pass =
        result.checks.cancelledNextRequestsAfterB == 2 && result.checks.cancelledNextCount == 150;
    AddCase(result, "cancelled_lease_demand_is_isolated", pass,
            pass ? std::string() : "explicit_b_next_did_not_continue");
}

void CaseMetadataAtomic(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                        abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id = OpenOne(context, control, abort, Query(L"metadata-atomic"), observer, opening,
                            ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "metadata_total_cursor_atomicity", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    ListState state;
    if (!provider.WaitRequests(base + 1, abort, deadline) ||
        !Deliver(context, control, abort, TrackPage(Tracks(0, 100), true, 100, L"metadata-r1", 200)) ||
        !observer->Wait(
            abort, deadline, [](const ListState& value) { return !value.loading && RowCount(value) == 100; },
            &state) ||
        !RequestNext(context, *id, control, abort) || !provider.WaitRequests(base + 2, abort, deadline)) {
        AddCase(result, "metadata_total_cursor_atomicity", false, "first_page_or_error_request_failed");
        return;
    }
    if (provider.Requests()[base + 1].cursor.offset != 100 ||
        !Deliver(context, control, abort, TrackPage(Tracks(100, 100), true, 200, L"metadata-r1", 201)) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return !value.loading && !value.error.empty() && RowCount(value) == 100;
            },
            &state)) {
        AddCase(result, "metadata_total_cursor_atomicity", false, "total_mismatch_not_rejected");
        return;
    }
    const uint64_t rejectedVersion = state.version;
    result.checks.metadataRejectCount = RowCount(state);
    if (!RequestNext(context, *id, control, abort) || !provider.WaitRequests(base + 3, abort, deadline)) {
        AddCase(result, "metadata_total_cursor_atomicity", false, "metadata_retry_request_missing");
        return;
    }
    const auto retryRequests = provider.Requests();
    result.checks.metadataRetrySameCursor = retryRequests[base + 2].cursor.offset == 100;
    if (!result.checks.metadataRetrySameCursor ||
        !Deliver(context, control, abort, TrackPage(Tracks(100, 100), true, 200, L"metadata-r1", 200)) ||
        !observer->WaitAfterVersion(
            rejectedVersion, abort, deadline,
            [](const ListState& value) {
                return !value.loading && (RowCount(value) == 200 || !value.error.empty());
            },
            &state)) {
        AddCase(result, "metadata_total_cursor_atomicity", false, "metadata_retry_failed");
        return;
    }
    const uint64_t retriedVersion = state.version;
    result.checks.metadataRetryRepeatedCursorError = !state.error.empty();
    result.checks.metadataRetryCount = RowCount(state);
    if (result.checks.metadataRetryRepeatedCursorError || !RequestNext(context, *id, control, abort) ||
        !provider.WaitRequests(base + 4, abort, deadline) ||
        provider.Requests()[base + 3].cursor.offset != 200 ||
        !Deliver(context, control, abort, TrackPage({}, false, 200, L"metadata-r1", 200)) ||
        !observer->WaitAfterVersion(
            retriedVersion, abort, deadline,
            [](const ListState& value) {
                return value.complete && !value.loading && value.error.empty() && RowCount(value) == 200;
            },
            &state)) {
        AddCase(result, "metadata_total_cursor_atomicity", false, "metadata_final_page_failed");
        return;
    }
    result.checks.metadataFinalComplete = state.complete;
    const bool pass = result.checks.metadataRejectCount == 100 && result.checks.metadataRetrySameCursor &&
                      !result.checks.metadataRetryRepeatedCursorError &&
                      result.checks.metadataRetryCount == 200 && result.checks.metadataFinalComplete &&
                      provider.Requests().size() == base + 4;
    AddCase(result, "metadata_total_cursor_atomicity", pass,
            pass ? std::string() : "metadata_or_cursor_was_committed_early");
}

void CaseInitialEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                           abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"enrich-initial"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "initial_publish_before_enrichment", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);

    const ListPage primary = TrackPage(Tracks(0, 2), true, 2, L"enrich-initial-r1", 2, true);
    if (!provider.WaitPending(1, abort, deadline) || !Deliver(context, control, abort, primary) ||
        !observer->Wait(abort, deadline,
                        [](const ListState& value) {
                            return !value.loading && RowCount(value) == 2 && value.metadataReadyRows == 0;
                        }) ||
        !provider.WaitEnrichments(1, abort, deadline)) {
        AddCase(result, "initial_publish_before_enrichment", false, "primary_was_not_published_first");
        return;
    }

    ListState before;
    const bool primaryVisible = observer->Wait(
        abort, deadline,
        [](const ListState& value) {
            return !value.loading && RowCount(value) == 2 && value.metadataReadyRows == 0;
        },
        &before);
    const uint64_t initialPrimaryVersion = before.version;
    const uint64_t initialPrimaryDataVersion = before.dataVersion;
    const bool enrichmentPending = provider.EnrichmentPendingCount() == 1;
    ListPage patch = PatchTracks(primary, [](Track& track, size_t index) {
        track.title = L"Initial enriched " + std::to_wstring(index);
        track.album = L"Enriched album";
    });
    if (!primaryVisible || !enrichmentPending ||
        !DeliverFirstEnrichment(context, control, abort, std::move(patch)) ||
        !observer->WaitAfterVersion(
            before.version, abort, deadline,
            [](const ListState& value) {
                return value.metadataReadyRows == 2 && value.dataVersion > 0 && value.updatedRows &&
                       value.updatedRows->offset == 0 && value.updatedRows->count == 2;
            },
            &before)) {
        AddCase(result, "initial_publish_before_enrichment", false, "enrichment_did_not_complete");
        return;
    }

    const auto& rows = std::get<std::vector<Track>>(before.page.rows);
    result.checks.initialPrimaryBeforeEnrichment = enrichmentPending;
    result.checks.initialMetadataReadyRows = 0;
    result.checks.initialEnrichedVersionDelta = before.version - initialPrimaryVersion;
    result.checks.initialDataVersionDelta = before.dataVersion - initialPrimaryDataVersion;
    result.checks.initialPatchTransportStable = before.appendOffset == 0 && before.page.next.offset == 2 &&
                                                before.page.more && before.pages == 1 && !before.complete;
    result.checks.initialUpdatedRange =
        UpdatedRows(before, 0, 2) && rows.size() == 2 && rows[0].title == L"Initial enriched 0" &&
        rows[1].title == L"Initial enriched 1" && !before.page.enrichmentPending;

    const bool pass = result.checks.initialPrimaryBeforeEnrichment &&
                      result.checks.initialPatchTransportStable && result.checks.initialUpdatedRange &&
                      provider.Requests().size() == base + 1;
    AddCase(result, "initial_publish_before_enrichment", pass,
            pass ? std::string() : "primary_was_blocked_or_patch_changed_transport");
}

void CaseNextPageWhileEnrichmentPending(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                                        abort_callback& abort, Clock::time_point deadline,
                                        ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"enrich-next"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "next_page_continues_while_enrichment_pending", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);

    const ListPage first = TrackPage(Tracks(0, 2), true, 2, L"enrich-next-r1", 4, true);
    const ListPage second = TrackPage(Tracks(2, 2), false, 4, L"enrich-next-r1", 4, true);
    if (!provider.WaitPending(1, abort, deadline) || !Deliver(context, control, abort, first) ||
        !observer->Wait(abort, deadline,
                        [](const ListState& value) {
                            return !value.loading && RowCount(value) == 2 && value.metadataReadyRows == 0;
                        }) ||
        !provider.WaitEnrichments(1, abort, deadline) || !RequestNext(context, *id, control, abort) ||
        !provider.WaitRequests(base + 2, abort, deadline) || !Deliver(context, control, abort, second)) {
        AddCase(result, "next_page_continues_while_enrichment_pending", false, "second_page_was_blocked");
        return;
    }

    ListState pending;
    if (!observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return !value.loading && RowCount(value) == 4 && value.metadataReadyRows == 0 &&
                       value.pages == 2 && value.complete;
            },
            &pending) ||
        !provider.WaitEnrichments(2, abort, deadline)) {
        AddCase(result, "next_page_continues_while_enrichment_pending", false,
                "second_page_did_not_publish_before_metadata");
        return;
    }
    result.checks.nextPageRequestsWhilePending = provider.Requests().size() - base;
    result.checks.nextPageRowsBeforeEnrichment = RowCount(pending);
    result.checks.nextPageMetadataReadyBefore = pending.metadataReadyRows;
    result.checks.nextPageTransportStable =
        pending.page.next.offset == 4 && pending.pages == 2 && pending.complete &&
        SameTrackIds(pending, {L"track-0", L"track-1", L"track-2", L"track-3"});

    ListPage secondPatch = PatchTracks(
        second, [](Track& track, size_t index) { track.title = L"Next enriched " + std::to_wstring(index); });
    ListPage firstPatch = PatchTracks(
        first, [](Track& track, size_t index) { track.title = L"First enriched " + std::to_wstring(index); });
    if (!DeliverEnrichment(context, control, abort, L"track-2", std::move(secondPatch)) ||
        !DeliverEnrichment(context, control, abort, L"track-0", std::move(firstPatch)) ||
        !observer->Wait(abort, deadline,
                        [](const ListState& value) { return value.metadataReadyRows == 4; })) {
        AddCase(result, "next_page_continues_while_enrichment_pending", false, "metadata_drain_failed");
        return;
    }

    const bool pass = result.checks.nextPageRequestsWhilePending == 2 &&
                      result.checks.nextPageRowsBeforeEnrichment == 4 &&
                      result.checks.nextPageMetadataReadyBefore == 0 && result.checks.nextPageTransportStable;
    AddCase(result, "next_page_continues_while_enrichment_pending", pass,
            pass ? std::string() : "pending_enrichment_blocked_or_advanced_primary");
}

void CaseOutOfOrderEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                              abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const uint64_t enrichBase = provider.EnrichmentRequests();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"enrich-order"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "out_of_order_page_patches", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);

    auto firstRows = Tracks(0, 2);
    firstRows[0].id = L"page-a-0";
    firstRows[1].id = L"page-a-1";
    auto secondRows = Tracks(2, 2);
    secondRows[0].id = L"page-b-0";
    secondRows[1].id = L"page-b-1";
    auto thirdRows = Tracks(4, 2);
    thirdRows[0].id = L"page-c-0";
    thirdRows[1].id = L"page-c-1";
    const ListPage first = TrackPage(std::move(firstRows), true, 2, L"enrich-order-r1", 6, true);
    const ListPage second = TrackPage(std::move(secondRows), true, 4, L"enrich-order-r1", 6, true);
    const ListPage third = TrackPage(std::move(thirdRows), false, 6, L"enrich-order-r1", 6, true);

    if (!provider.WaitPending(1, abort, deadline) || !Deliver(context, control, abort, first) ||
        !provider.WaitEnrichments(1, abort, deadline) || !RequestNext(context, *id, control, abort) ||
        !provider.WaitRequests(base + 2, abort, deadline) || !Deliver(context, control, abort, second) ||
        !provider.WaitEnrichments(2, abort, deadline) || !RequestNext(context, *id, control, abort) ||
        !provider.WaitRequests(base + 3, abort, deadline) || !Deliver(context, control, abort, third)) {
        AddCase(result, "out_of_order_page_patches", false, "setup_failed");
        return;
    }

    ListState state;
    if (!observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return !value.loading && RowCount(value) == 6 && value.metadataReadyRows == 0 &&
                       value.pages == 3 && value.complete;
            },
            &state)) {
        AddCase(result, "out_of_order_page_patches", false, "primary_pages_missing");
        return;
    }
    result.checks.outOfOrderConcurrencyBounded =
        provider.EnrichmentPendingCount() == 2 && provider.EnrichmentRequests() == enrichBase + 2;
    result.checks.metadataCancellationShared = provider.FirstTwoEnrichmentTokensShared();
    const uint64_t primaryVersion = state.version;
    const uint64_t primaryDataVersion = state.dataVersion;
    ListPage secondPatch = PatchTracks(second, [](Track& track, size_t index) {
        track.title = L"Second page enriched " + std::to_wstring(index);
    });
    if (!DeliverEnrichment(context, control, abort, L"page-b-0", std::move(secondPatch)) ||
        !observer->WaitAfterVersion(
            primaryVersion, abort, deadline,
            [](const ListState& value) {
                return RowCount(value) == 6 && value.metadataReadyRows == 0 && value.updatedRows &&
                       value.updatedRows->offset == 2 && value.updatedRows->count == 2;
            },
            &state)) {
        AddCase(result, "out_of_order_page_patches", false, "later_page_patch_failed");
        return;
    }
    result.checks.outOfOrderReadyRows.push_back(state.metadataReadyRows);
    const bool secondPatchWorked =
        std::get<std::vector<Track>>(state.page.rows)[2].title == L"Second page enriched 0" &&
        std::get<std::vector<Track>>(state.page.rows)[3].title == L"Second page enriched 1";

    if (!provider.WaitEnrichments(2, abort, deadline) || provider.EnrichmentRequests() != enrichBase + 3) {
        AddCase(result, "out_of_order_page_patches", false, "concurrency_slot_not_released");
        return;
    }
    const uint64_t secondPatchVersion = state.version;
    ListPage firstPatch = PatchTracks(first, [](Track& track, size_t index) {
        track.title = L"First page enriched " + std::to_wstring(index);
    });
    if (!DeliverEnrichment(context, control, abort, L"page-a-0", std::move(firstPatch)) ||
        !observer->WaitAfterVersion(
            secondPatchVersion, abort, deadline,
            [](const ListState& value) {
                return value.metadataReadyRows == 4 && value.updatedRows && value.updatedRows->offset == 0 &&
                       value.updatedRows->count == 2;
            },
            &state)) {
        AddCase(result, "out_of_order_page_patches", false, "earlier_page_patch_failed");
        return;
    }
    result.checks.outOfOrderReadyRows.push_back(state.metadataReadyRows);
    const bool firstPatchWorked =
        std::get<std::vector<Track>>(state.page.rows)[0].title == L"First page enriched 0" &&
        std::get<std::vector<Track>>(state.page.rows)[1].title == L"First page enriched 1";

    const uint64_t firstPatchVersion = state.version;
    ListPage thirdPatch = PatchTracks(third, [](Track& track, size_t index) {
        track.title = L"Third page enriched " + std::to_wstring(index);
    });
    if (!DeliverEnrichment(context, control, abort, L"page-c-0", std::move(thirdPatch)) ||
        !observer->WaitAfterVersion(
            firstPatchVersion, abort, deadline,
            [](const ListState& value) {
                return value.metadataReadyRows == 6 && value.updatedRows && value.updatedRows->offset == 4 &&
                       value.updatedRows->count == 2;
            },
            &state)) {
        AddCase(result, "out_of_order_page_patches", false, "third_page_patch_failed");
        return;
    }
    result.checks.outOfOrderReadyRows.push_back(state.metadataReadyRows);
    const bool thirdPatchWorked =
        std::get<std::vector<Track>>(state.page.rows)[4].title == L"Third page enriched 0" &&
        std::get<std::vector<Track>>(state.page.rows)[5].title == L"Third page enriched 1";
    result.checks.outOfOrderPatchesApplied = secondPatchWorked && firstPatchWorked && thirdPatchWorked;
    result.checks.outOfOrderTransportStable =
        state.dataVersion == primaryDataVersion + 3 && state.appendOffset == 0 &&
        state.page.next.offset == 6 && state.pages == 3 && state.complete && !state.page.enrichmentPending &&
        SameTrackIds(state, {L"page-a-0", L"page-a-1", L"page-b-0", L"page-b-1", L"page-c-0", L"page-c-1"});
    const bool pass = result.checks.outOfOrderReadyRows == std::vector<uint64_t>{0, 4, 6} &&
                      result.checks.outOfOrderConcurrencyBounded &&
                      result.checks.metadataCancellationShared && result.checks.outOfOrderPatchesApplied &&
                      result.checks.outOfOrderTransportStable && provider.Requests().size() == base + 3;
    AddCase(result, "out_of_order_page_patches", pass,
            pass ? std::string() : "out_of_order_patch_lost_rows_or_transport");
}

void CaseUnchangedEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                             abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id = OpenOne(context, control, abort, Query(L"enrich-unchanged"), observer, opening,
                            ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "unchanged_metadata_advances_frontier", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    const ListPage primary = TrackPage(Tracks(0, 2), false, 2, L"enrich-unchanged-r1", 2, true);
    ListState before;
    if (!provider.WaitPending(1, abort, deadline) || !Deliver(context, control, abort, primary) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return !value.loading && value.metadataReadyRows == 0 && RowCount(value) == 2;
            },
            &before) ||
        !provider.WaitEnrichments(1, abort, deadline) ||
        !DeliverFirstEnrichment(context, control, abort, PatchTracks(primary, [](Track&, size_t) {})) ||
        !observer->WaitAfterVersion(
            before.version, abort, deadline,
            [](const ListState& value) { return value.metadataReadyRows == 2; }, &before)) {
        AddCase(result, "unchanged_metadata_advances_frontier", false, "completion_missing");
        return;
    }
    result.checks.unchangedMetadataAdvanced = before.metadataReadyRows == 2 && !before.page.enrichmentPending;
    result.checks.unchangedDataVersionStable = before.dataVersion == 1;
    result.checks.unchangedUpdatedRowsAbsent = NoUpdatedRows(before);
    const bool pass = result.checks.unchangedMetadataAdvanced && result.checks.unchangedDataVersionStable &&
                      result.checks.unchangedUpdatedRowsAbsent && provider.Requests().size() == base + 1;
    AddCase(result, "unchanged_metadata_advances_frontier", pass,
            pass ? std::string() : "unchanged_metadata_was_not_completed");
}

void CaseDeclinedEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                            abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const uint64_t enrichBase = provider.EnrichmentRequests();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id = OpenOne(context, control, abort, Query(L"enrich-declined"), observer, opening,
                            ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "declined_enrichment_releases_frontier", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    ListState state;
    if (!provider.WaitPending(1, abort, deadline) ||
        !Deliver(context, control, abort,
                 TrackPage(Tracks(0, 2), false, 2, L"enrich-declined-r1", 2, true)) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return !value.loading && value.metadataReadyRows == 2 && value.error.empty();
            },
            &state)) {
        AddCase(result, "declined_enrichment_releases_frontier", false, "decline_was_not_released");
        return;
    }
    result.checks.declinedEnrichmentReleased = state.metadataReadyRows == 2 &&
                                               !state.page.enrichmentPending && state.complete &&
                                               provider.EnrichmentPendingCount() == 0;
    result.checks.declinedEnrichmentNoCallback =
        provider.EnrichmentRequests() == enrichBase + 1 && provider.EnrichmentPendingCount() == 0;
    result.checks.declinedEnrichmentDataVersionStable = state.dataVersion == 1 && NoUpdatedRows(state);
    const bool pass =
        result.checks.declinedEnrichmentReleased && result.checks.declinedEnrichmentNoCallback &&
        result.checks.declinedEnrichmentDataVersionStable && provider.Requests().size() == base + 1;
    AddCase(result, "declined_enrichment_releases_frontier", pass,
            pass ? std::string() : "decline_left_pending_or_mutated_data");
}

void CaseRefreshEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                           abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"enrich-refresh"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "refresh_rejects_old_metadata_patch", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    auto oldRows = Tracks(0, 2);
    oldRows[0].id = L"refresh-0";
    oldRows[1].id = L"refresh-1";
    for (auto& row : oldRows)
        row.title = L"Old baseline";
    const ListPage oldPrimary = TrackPage(oldRows, false, 2, L"enrich-refresh-r1", 2, true);
    if (!provider.WaitPending(1, abort, deadline) || !Deliver(context, control, abort, oldPrimary) ||
        !observer->Wait(abort, deadline,
                        [](const ListState& value) {
                            return !value.loading && value.metadataReadyRows == 0 && RowCount(value) == 2;
                        }) ||
        !provider.WaitEnrichments(1, abort, deadline) || !Refresh(context, *id, control, abort) ||
        !provider.WaitRequests(base + 2, abort, deadline)) {
        AddCase(result, "refresh_rejects_old_metadata_patch", false, "refresh_setup_failed");
        return;
    }
    result.checks.refreshOldCancellation = provider.FrontEnrichmentCancellationSignalled();

    auto newRows = oldRows;
    for (auto& row : newRows)
        row.title = L"New baseline";
    const ListPage newPrimary = TrackPage(newRows, false, 2, L"enrich-refresh-r1", 2, true);
    if (!Deliver(context, control, abort, newPrimary) || !provider.WaitEnrichments(2, abort, deadline)) {
        AddCase(result, "refresh_rejects_old_metadata_patch", false, "new_primary_failed");
        return;
    }
    ListPage oldPatch = PatchTracks(oldPrimary, [](Track& track, size_t) { track.title = L"Old enriched"; });
    if (!DeliverEnrichment(context, control, abort, L"refresh-0", std::move(oldPatch)) ||
        !Barrier(control, abort)) {
        AddCase(result, "refresh_rejects_old_metadata_patch", false, "old_patch_delivery_failed");
        return;
    }
    ListState rejected;
    if (!GetState(context, *id, control, abort, &rejected) ||
        std::get<std::vector<Track>>(rejected.page.rows)[0].title != L"New baseline") {
        AddCase(result, "refresh_rejects_old_metadata_patch", false, "old_patch_was_applied");
        return;
    }
    result.checks.refreshOldPatchRejected = true;

    ListPage newPatch = PatchTracks(newPrimary, [](Track& track, size_t) { track.title = L"New enriched"; });
    if (!DeliverEnrichment(context, control, abort, L"refresh-0", std::move(newPatch)) ||
        !observer->Wait(abort, deadline, [](const ListState& value) {
            return value.metadataReadyRows == 2 && value.updatedRows &&
                   std::get<std::vector<Track>>(value.page.rows)[0].title == L"New enriched";
        })) {
        AddCase(result, "refresh_rejects_old_metadata_patch", false, "new_patch_failed");
        return;
    }
    result.checks.refreshPatchRequests = provider.Requests().size() - base;
    result.checks.refreshNewPatchApplied = true;
    const bool pass = result.checks.refreshOldCancellation && result.checks.refreshOldPatchRejected &&
                      result.checks.refreshNewPatchApplied && result.checks.refreshPatchRequests == 2;
    AddCase(result, "refresh_rejects_old_metadata_patch", pass,
            pass ? std::string() : "refresh_generation_selection_failed");
}

void CaseCancelEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                          abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"enrich-cancel"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "cancel_rejects_old_metadata_patch", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    const ListPage primary = TrackPage(Tracks(0, 2), false, 2, L"enrich-cancel-r1", 2, true);
    ListState before;
    if (!provider.WaitPending(1, abort, deadline) || !Deliver(context, control, abort, primary) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return !value.loading && value.metadataReadyRows == 0 && RowCount(value) == 2;
            },
            &before) ||
        !provider.WaitEnrichments(1, abort, deadline) || !Cancel(context, *id, control, abort)) {
        AddCase(result, "cancel_rejects_old_metadata_patch", false, "cancel_setup_failed");
        return;
    }
    const uint64_t callbacks = observer->Count();
    const bool token = provider.FrontEnrichmentCancellationSignalled();
    ListPage patch = PatchTracks(primary, [](Track& track, size_t) { track.title = L"Cancelled enriched"; });
    if (!DeliverFirstEnrichment(context, control, abort, std::move(patch)) || !Barrier(control, abort)) {
        AddCase(result, "cancel_rejects_old_metadata_patch", false, "late_patch_delivery_failed");
        return;
    }
    const ListState retained = observer->Latest();
    result.checks.cancelPatchRejected =
        token && observer->Count() == callbacks && RowCount(retained) == 2 &&
        std::get<std::vector<Track>>(retained.page.rows)[0].title != L"Cancelled enriched";
    result.checks.cancelBaseRetained = before.complete && !before.loading &&
                                       retained.dataVersion == before.dataVersion &&
                                       retained.metadataReadyRows == 0 && RowCount(retained) == 2;
    const bool pass = result.checks.cancelPatchRejected && result.checks.cancelBaseRetained;
    const bool exact = pass && provider.Requests().size() == base + 1;
    AddCase(result, "cancel_rejects_old_metadata_patch", exact,
            exact ? std::string() : "cancelled_patch_changed_or_fetched_the_primary");
}

void CaseInvalidEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                           abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"enrich-invalid"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "invalid_patch_shape_and_identity_rejected", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    auto firstRows = Tracks(0, 2);
    firstRows[0].id = L"invalid-0";
    firstRows[1].id = L"invalid-1";
    const ListPage first = TrackPage(std::move(firstRows), true, 2, L"enrich-invalid-r1", 4, true);
    if (!provider.WaitPending(1, abort, deadline) || !Deliver(context, control, abort, first) ||
        !provider.WaitEnrichments(1, abort, deadline)) {
        AddCase(result, "invalid_patch_shape_and_identity_rejected", false, "first_page_failed");
        return;
    }
    ListPage badShape = PatchTracks(first, [](Track& track, size_t) { track.title = L"Invalid shape"; });
    std::get<std::vector<Track>>(badShape.rows).pop_back();
    ListState state;
    if (!DeliverFirstEnrichment(context, control, abort, std::move(badShape)) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return value.metadataReadyRows == 2 &&
                       std::get<std::vector<Track>>(value.page.rows)[0].title == L"Probe track 0";
            },
            &state)) {
        AddCase(result, "invalid_patch_shape_and_identity_rejected", false,
                "invalid_shape_changed_or_blocked_primary");
        return;
    }
    result.checks.invalidCountRejected =
        RowCount(state) == 2 && NoUpdatedRows(state) && state.dataVersion == 1;

    auto secondRows = Tracks(2, 2);
    secondRows[0].id = L"invalid-2";
    secondRows[1].id = L"invalid-3";
    const ListPage second = TrackPage(std::move(secondRows), false, 4, L"enrich-invalid-r1", 4, true);
    if (!RequestNext(context, *id, control, abort) || !provider.WaitRequests(base + 2, abort, deadline) ||
        !Deliver(context, control, abort, second) || !provider.WaitEnrichments(1, abort, deadline)) {
        AddCase(result, "invalid_patch_shape_and_identity_rejected", false, "second_page_failed");
        return;
    }
    ListPage badIdentity = PatchTracks(second, [](Track& track, size_t index) {
        track.title = L"Invalid identity";
        if (index == 1) track.id = L"wrong-id";
    });
    if (!DeliverFirstEnrichment(context, control, abort, std::move(badIdentity)) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return value.metadataReadyRows == 4 && RowCount(value) == 4 &&
                       std::get<std::vector<Track>>(value.page.rows)[2].title == L"Probe track 2";
            },
            &state)) {
        AddCase(result, "invalid_patch_shape_and_identity_rejected", false,
                "invalid_identity_changed_or_blocked_primary");
        return;
    }
    result.checks.invalidIdentityRejected =
        SameTrackIds(state, {L"invalid-0", L"invalid-1", L"invalid-2", L"invalid-3"}) &&
        NoUpdatedRows(state) && state.dataVersion == 2;
    result.checks.invalidMetadataReadyRows = state.metadataReadyRows;
    result.checks.invalidBaseRetained =
        RowCount(state) == 4 && !state.loading && state.complete && !state.page.enrichmentPending;
    const bool pass = result.checks.invalidCountRejected && result.checks.invalidIdentityRejected &&
                      result.checks.invalidMetadataReadyRows == 4 && result.checks.invalidBaseRetained &&
                      provider.Requests().size() == base + 2;
    AddCase(result, "invalid_patch_shape_and_identity_rejected", pass,
            pass ? std::string() : "invalid_patch_escaped_validation");
}

void CaseDuplicateEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                             abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id = OpenOne(context, control, abort, Query(L"enrich-duplicates"), observer, opening,
                            ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "duplicate_metadata_occurrences_preserved", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    std::vector<Track> rows = Tracks(0, 3);
    rows[0].id = L"duplicate-metadata";
    rows[0].title = L"Duplicate A";
    rows[2].id = L"duplicate-metadata";
    rows[2].title = L"Duplicate B";
    const ListPage primary = TrackPage(std::move(rows), false, 3, L"enrich-duplicates-r1", 3, true);
    ListState state;
    if (!provider.WaitPending(1, abort, deadline) || !Deliver(context, control, abort, primary) ||
        !provider.WaitEnrichments(1, abort, deadline)) {
        AddCase(result, "duplicate_metadata_occurrences_preserved", false, "setup_failed");
        return;
    }
    ListPage patch = PatchTracks(primary, [](Track& track, size_t index) {
        if (index == 0) track.title = L"Duplicate A enriched";
        if (index == 2) track.title = L"Duplicate B enriched";
    });
    if (!DeliverFirstEnrichment(context, control, abort, std::move(patch)) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return value.metadataReadyRows == 3 && value.updatedRows && value.updatedRows->offset == 0 &&
                       value.updatedRows->count == 3;
            },
            &state)) {
        AddCase(result, "duplicate_metadata_occurrences_preserved", false, "patch_missing");
        return;
    }
    const auto& tracks = std::get<std::vector<Track>>(state.page.rows);
    result.checks.duplicateMetadataOccurrences = static_cast<uint64_t>(std::count_if(
        tracks.begin(), tracks.end(), [](const Track& track) { return track.id == L"duplicate-metadata"; }));
    result.checks.duplicateMetadataPatchedByOccurrence =
        tracks[0].title == L"Duplicate A enriched" && tracks[2].title == L"Duplicate B enriched";
    const bool pass = result.checks.duplicateMetadataOccurrences == 2 &&
                      result.checks.duplicateMetadataPatchedByOccurrence && RowCount(state) == 3 &&
                      provider.Requests().size() == base + 1;
    AddCase(result, "duplicate_metadata_occurrences_preserved", pass,
            pass ? std::string() : "duplicate_occurrences_collapsed");
}

void CaseFailureEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                           abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"enrich-failure"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "metadata_failure_does_not_hide_primary", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    const ListPage primary = TrackPage(Tracks(0, 2), false, 2, L"enrich-failure-r1", 2, true);
    ListState before;
    if (!provider.WaitPending(1, abort, deadline) || !Deliver(context, control, abort, primary) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return !value.loading && value.metadataReadyRows == 0 && RowCount(value) == 2;
            },
            &before) ||
        !provider.WaitEnrichments(1, abort, deadline) ||
        !DeliverFirstEnrichment(context, control, abort, {}, L"controlled metadata failure") ||
        !observer->Wait(
            abort, deadline, [](const ListState& value) { return value.metadataReadyRows == 2; }, &before)) {
        AddCase(result, "metadata_failure_does_not_hide_primary", false, "failure_did_not_complete");
        return;
    }
    result.checks.failurePrimaryRetained =
        RowCount(before) == 2 && !before.loading && before.complete && before.appendOffset == 0;
    result.checks.failureMetadataReady = before.metadataReadyRows == 2 && !before.page.enrichmentPending;
    result.checks.failureErrorAbsent = before.error.empty();
    result.checks.failureDataVersionStable = before.dataVersion == 1;
    result.checks.failureUpdatedRowsAbsent = NoUpdatedRows(before);
    const bool pass = result.checks.failurePrimaryRetained && result.checks.failureMetadataReady &&
                      result.checks.failureErrorAbsent && result.checks.failureDataVersionStable &&
                      result.checks.failureUpdatedRowsAbsent && provider.Requests().size() == base + 1;
    AddCase(result, "metadata_failure_does_not_hide_primary", pass,
            pass ? std::string() : "failure_hid_or_mutated_primary");
}

void CaseAccountEnrichment(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                           abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id =
        OpenOne(context, control, abort, Query(L"enrich-account"), observer, opening, ListLoadMode::OnDemand);
    if (!id) {
        AddCase(result, "account_generation_rejects_old_metadata_patch", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);
    const ListPage primary = TrackPage(Tracks(0, 2), false, 2, L"enrich-account-r1", 2, true);
    ListState before;
    if (!provider.WaitPending(1, abort, deadline) || !Deliver(context, control, abort, primary) ||
        !observer->Wait(
            abort, deadline,
            [](const ListState& value) {
                return !value.loading && value.metadataReadyRows == 0 && RowCount(value) == 2;
            },
            &before) ||
        !provider.WaitEnrichments(1, abort, deadline) || !OnMain(control, abort, [context] {
            context->provider->NotifyAccountChange();
            return true;
        })) {
        AddCase(result, "account_generation_rejects_old_metadata_patch", false,
                "account_invalidation_failed");
        return;
    }
    const bool token = provider.FrontEnrichmentCancellationSignalled();
    ListPage patch =
        PatchTracks(primary, [](Track& track, size_t) { track.title = L"Old account enriched"; });
    if (!DeliverFirstEnrichment(context, control, abort, std::move(patch)) || !Barrier(control, abort)) {
        AddCase(result, "account_generation_rejects_old_metadata_patch", false,
                "late_account_patch_delivery_failed");
        return;
    }
    ListState retained;
    result.checks.accountPatchRejected =
        token && GetState(context, *id, control, abort, &retained) && RowCount(retained) == 2 &&
        std::get<std::vector<Track>>(retained.page.rows)[0].title != L"Old account enriched";
    result.checks.accountBaseRetained = RowCount(retained) == 2 && retained.metadataReadyRows == 0 &&
                                        retained.dataVersion == before.dataVersion;
    const bool pass = result.checks.accountPatchRejected && result.checks.accountBaseRetained &&
                      provider.Requests().size() == base + 1;
    AddCase(result, "account_generation_rejects_old_metadata_patch", pass,
            pass ? std::string() : "account_generation_leaked_old_patch");
}

void CaseRestoreGateAndSoftRetirement(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                                      abort_callback& abort, Clock::time_point deadline,
                                      ProbeResult& result) {
    CaseCleanup cleanup(context, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    const auto setObserved = [&](bool restoring, bool verified) {
        return OnMain(control, abort, [&] {
            provider.SetRestoreState(restoring, verified);
            provider.NotifyAccountObservedChange();
            return true;
        });
    };
    if (!setObserved(true, false)) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "restore_state_setup_failed");
        return;
    }

    const auto observer = NewObserver();
    const auto opening = NewOpening();
    const auto id = OpenOne(context, control, abort, Query(L"restore-soft-retire"), observer, opening,
                            ListLoadMode::OnDemand);
    if (!id || !Barrier(control, abort)) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "open_failed");
        return;
    }
    cleanup.Keep(*id);

    ListState initial;
    result.checks.restoreInitialDeferred = provider.Requests().size() == base;
    result.checks.restoreInitialEmptyNoError = GetState(context, *id, control, abort, &initial) &&
                                               RowCount(initial) == 0 && !initial.loading &&
                                               !initial.refreshing && initial.error.empty();
    if (!result.checks.restoreInitialDeferred || !result.checks.restoreInitialEmptyNoError) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "restore_phase_started_network");
        return;
    }
    if (!RequestNext(context, *id, control, abort)) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "restore_next_setup_failed");
        return;
    }

    ListPage retainedPage =
        TrackPage({NamedTrack(L"restore-retained", L"Retained prefix")}, true, 1, L"restore-r1", 2);
    if (!setObserved(false, true) || !provider.WaitPending(1, abort, deadline) ||
        !Deliver(context, control, abort, std::move(retainedPage)) ||
        !observer->Wait(abort, deadline,
                        [](const ListState& state) {
                            // Preserved Next immediately starts the continuation request. The
                            // stable first-prefix state is therefore visibly loading.
                            return state.loading && SameTrackIds(state, {L"restore-retained"});
                        }) ||
        !provider.WaitPending(1, abort, deadline)) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "initial_verified_page_failed");
        return;
    }
    result.checks.restoreNextPreserved = true;
    if (!Deliver(context, control, abort,
                 TrackPage({NamedTrack(L"restore-resumed", L"Restored continuation")}, false, 2,
                           L"restore-r1", 2)) ||
        !observer->Wait(abort, deadline, [](const ListState& state) {
            return !state.loading && SameTrackIds(state, {L"restore-retained", L"restore-resumed"});
        })) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "restored_continuation_failed");
        return;
    }

    const auto requestsAfterRetained = provider.Requests().size();
    if (!setObserved(true, false) || !Barrier(control, abort)) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "unverified_retirement_failed");
        return;
    }
    ListState retained;
    result.checks.softRetirePrefixRetained =
        provider.Requests().size() == requestsAfterRetained &&
        GetState(context, *id, control, abort, &retained) && !retained.loading && !retained.refreshing &&
        SameTrackIds(retained, {L"restore-retained", L"restore-resumed"});
    if (!result.checks.softRetirePrefixRetained) {
        AddCase(result, "restore_gate_and_soft_retirement", false,
                "unverified_retirement_blanked_or_fetched");
        return;
    }

    if (!setObserved(false, true) || !provider.WaitPending(1, abort, deadline)) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "verified_refresh_missing");
        return;
    }
    if (!setObserved(true, false) || !Barrier(control, abort)) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "second_unverified_retirement_failed");
        return;
    }
    const bool oldCancelled = provider.FrontCancellationSignalled();
    const auto requestsBeforeReverify = provider.Requests().size();
    if (!setObserved(false, true) || !provider.WaitPending(2, abort, deadline)) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "reverified_refresh_missing");
        return;
    }

    if (!Deliver(context, control, abort,
                 TrackPage({NamedTrack(L"restore-stale", L"Stale response")}, false, 0, L"restore-r2", 1)) ||
        !Barrier(control, abort)) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "stale_response_delivery_failed");
        return;
    }
    ListState afterStale;
    result.checks.softRetireOldNetworkRejected =
        oldCancelled && GetState(context, *id, control, abort, &afterStale) &&
        SameTrackIds(afterStale, {L"restore-retained", L"restore-resumed"});
    if (!result.checks.softRetireOldNetworkRejected) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "retired_network_published");
        return;
    }

    if (!Deliver(
            context, control, abort,
            TrackPage({NamedTrack(L"restore-current", L"Current response")}, false, 0, L"restore-r3", 1)) ||
        !observer->Wait(abort, deadline, [](const ListState& state) {
            return !state.loading && SameTrackIds(state, {L"restore-current"});
        })) {
        AddCase(result, "restore_gate_and_soft_retirement", false, "current_response_missing");
        return;
    }
    result.checks.softRetireNewNetworkApplied = provider.Requests().size() == requestsBeforeReverify + 1;
    const bool pass = result.checks.softRetirePrefixRetained && result.checks.softRetireOldNetworkRejected &&
                      result.checks.softRetireNewNetworkApplied && result.checks.restoreNextPreserved &&
                      provider.Requests().size() == base + 4 && provider.PendingCount() == 0;
    AddCase(result, "restore_gate_and_soft_retirement", pass,
            pass ? std::string() : "restore_gate_or_generation_handoff_failed");
}

void CaseRemotePageFilterSort(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                              abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    const auto fixture = std::make_shared<UiFixture>();
    UiCleanup cleanup(fixture, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    if (!StartPlaylistPage(context, fixture, L"ui-filter-sort", control, abort) ||
        !provider.WaitPending(1, abort, deadline) ||
        !DeliverUiPage(context, fixture, control, abort, UiFilterPage(0), deadline)) {
        AddCase(result, "remote_page_filter_sort", false, "initial_page_failed");
        return;
    }
    size_t count = 0;
    if (!UiSourceCount(fixture, control, abort, &count) || count != 100 || provider.PendingCount() != 0 ||
        provider.Requests().size() != base + 1) {
        AddCase(result, "remote_page_filter_sort", false, "default_did_not_stop_after_first_page");
        return;
    }
    result.checks.remoteInitialCount = count;

    if (!PageSourceAction(fixture, control, abort,
                          [](media::ListSource& source) { source.SetFilter(L"match"); }) ||
        !UiSourceCount(fixture, control, abort, &count)) {
        AddCase(result, "remote_page_filter_sort", false, "filter_action_failed");
        return;
    }
    result.checks.remoteFilterCounts.push_back(count);
    for (size_t page = 1; page <= 2; ++page) {
        if (!provider.WaitPending(1, abort, deadline) ||
            !DeliverUiPage(context, fixture, control, abort, UiFilterPage(page), deadline) ||
            !UiSourceCount(fixture, control, abort, &count)) {
            AddCase(result, "remote_page_filter_sort", false, "filtered_page_failed");
            return;
        }
        result.checks.remoteFilterCounts.push_back(count);
    }
    if (result.checks.remoteFilterCounts != std::vector<uint64_t>{10, 25, 35} ||
        !provider.WaitPending(1, abort, deadline)) {
        AddCase(result, "remote_page_filter_sort", false, "filter_progress_mismatch");
        return;
    }

    if (!PageSourceAction(fixture, control, abort, [](media::ListSource& source) { source.SetFilter({}); }) ||
        !DeliverUiPage(context, fixture, control, abort, UiFilterPage(3), deadline) ||
        !UiSourceCount(fixture, control, abort, &count)) {
        AddCase(result, "remote_page_filter_sort", false, "mid_drain_clear_failed");
        return;
    }
    result.checks.remoteOnDemandCount = count;
    if (count != 250 || provider.PendingCount() != 0 || provider.Requests().size() != base + 4) {
        AddCase(result, "remote_page_filter_sort", false, "cleared_filter_did_not_stop_on_demand");
        return;
    }

    if (!PageSourceAction(fixture, control, abort,
                          [](media::ListSource& source) { source.Sort(2, false); }) ||
        !provider.WaitPending(1, abort, deadline) ||
        !DeliverUiPage(context, fixture, control, abort, UiFilterPage(4), deadline)) {
        AddCase(result, "remote_page_filter_sort", false, "sorted_page_five_failed");
        return;
    }
    bool firstSorted = false;
    if (!UiSourceSorted(fixture, control, abort, &firstSorted) || !firstSorted ||
        !provider.WaitPending(1, abort, deadline) ||
        !DeliverUiPage(context, fixture, control, abort, UiFilterPage(5), deadline)) {
        AddCase(result, "remote_page_filter_sort", false, "sort_did_not_drain_progressively");
        return;
    }
    bool finalSorted = false;
    if (!UiSourceSorted(fixture, control, abort, &finalSorted) ||
        !UiSourceCount(fixture, control, abort, &count)) {
        AddCase(result, "remote_page_filter_sort", false, "final_sorted_source_unavailable");
        return;
    }
    result.checks.remoteSortedPages = firstSorted && finalSorted && count == 350 &&
                                      provider.PendingCount() == 0 && provider.Requests().size() == base + 6;
    AddCase(result, "remote_page_filter_sort", result.checks.remoteSortedPages,
            result.checks.remoteSortedPages ? std::string() : "filter_sort_presentation_failed");
}

void CaseRemotePageTiles(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                         abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    const auto fixture = std::make_shared<UiFixture>();
    UiCleanup cleanup(fixture, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    if (!StartPlaylistPage(context, fixture, L"ui-tiles", control, abort) ||
        !PageSourceAction(fixture, control, abort,
                          [](media::ListSource& source) { source.SetViewStyle(media::ViewStyle::Tiles); }) ||
        !provider.WaitPending(1, abort, deadline) ||
        !DeliverUiPage(context, fixture, control, abort, UiTilePage(0), deadline) ||
        !provider.WaitPending(1, abort, deadline)) {
        AddCase(result, "remote_page_tiles_selection", false, "tile_first_page_failed");
        return;
    }
    const bool selected = OnMain(control, abort, [fixture] {
        auto* source = fixture->host.Source();
        if (!source || source->Count() < 51 || source->GetViewStyle() != media::ViewStyle::Tiles)
            return false;
        source->SelectOnly(50);
        source->SetFocus(50);
        fixture->selected = source->Item(50);
        return fixture->selected.is_valid();
    });
    if (!selected || !DeliverUiPage(context, fixture, control, abort, UiTilePage(1), deadline)) {
        AddCase(result, "remote_page_tiles_selection", false, "tile_selection_setup_failed");
        return;
    }
    uint64_t groupCount = 0;
    const bool presentationKept = OnMain(control, abort, [fixture, &groupCount] {
        auto* source = fixture->host.Source();
        if (!source) return false;
        const size_t row = source->FindRow(fixture->selected);
        if (row == SIZE_MAX || !source->IsSelected(row) || source->Focus() >= source->Count() ||
            source->Item(source->Focus()) != fixture->selected)
            return false;
        const auto* groups = source->Groups();
        if (!groups) return false;
        for (const auto& group : *groups)
            if (group.header == L"Album A") groupCount = group.count;
        return true;
    });
    size_t count = 0;
    if (!UiSourceCount(fixture, control, abort, &count)) {
        AddCase(result, "remote_page_tiles_selection", false, "tile_source_missing");
        return;
    }
    result.checks.remoteTileRows = count;
    result.checks.remoteTileGroupCount = groupCount;
    result.checks.remoteTilePresentationKept = presentationKept;
    const bool pass = count == 120 && groupCount == 30 && presentationKept && provider.PendingCount() == 0 &&
                      provider.Requests().size() == base + 2;
    AddCase(result, "remote_page_tiles_selection", pass,
            pass ? std::string() : "tile_group_or_presentation_lost");
}

void CaseRemotePageDestroy(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                           abort_callback& abort, Clock::time_point deadline, ProbeResult& result) {
    const auto fixture = std::make_shared<UiFixture>();
    UiCleanup cleanup(fixture, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    if (!StartPlaylistPage(context, fixture, L"ui-destroy", control, abort) ||
        !provider.WaitPending(1, abort, deadline)) {
        AddCase(result, "remote_page_destroy_cancels", false, "page_start_failed");
        return;
    }
    const uint64_t changedBefore = fixture->host.ListChanges();
    if (!OnMain(control, abort, [fixture] {
            fixture->page.reset();
            return true;
        })) {
        AddCase(result, "remote_page_destroy_cancels", false, "page_destroy_failed");
        return;
    }
    const bool tokenSignalled = provider.FrontCancellationSignalled();
    if (!Deliver(context, control, abort, UiFilterPage(0)) || !Barrier(control, abort)) {
        AddCase(result, "remote_page_destroy_cancels", false, "late_page_delivery_failed");
        return;
    }
    result.checks.remoteDestroyCancelled = tokenSignalled && fixture->host.ListChanges() == changedBefore &&
                                           provider.PendingCount() == 0 &&
                                           provider.Requests().size() == base + 1;
    AddCase(result, "remote_page_destroy_cancels", result.checks.remoteDestroyCancelled,
            result.checks.remoteDestroyCancelled ? std::string() : "destroyed_page_session_survived");
}

void CaseRemotePageEnrichmentPresentation(const std::shared_ptr<ProbeContext>& context, RunControl& control,
                                          abort_callback& abort, Clock::time_point deadline,
                                          ProbeResult& result) {
    const auto fixture = std::make_shared<UiFixture>();
    UiCleanup cleanup(fixture, control, abort);
    auto& provider = *context->provider;
    const size_t base = provider.Requests().size();
    if (!StartPlaylistPage(context, fixture, L"ui-enrich-presentation", control, abort) ||
        !provider.WaitPending(1, abort, deadline)) {
        AddCase(result, "remote_page_enrichment_presentation", false, "page_start_failed");
        return;
    }

    const ListPage firstPrimary = UiEnrichedPage(0, false);
    if (!DeliverUiPage(context, fixture, control, abort, firstPrimary, deadline) ||
        !provider.WaitEnrichments(1, abort, deadline) ||
        !DeliverUiEnrichment(context, fixture, control, abort, L"ui-enrich-0", UiEnrichedPage(0, true),
                             deadline)) {
        AddCase(result, "remote_page_enrichment_presentation", false, "first_metadata_failed");
        return;
    }
    size_t count = 0;
    if (!UiSourceCount(fixture, control, abort, &count) || count != 100) {
        AddCase(result, "remote_page_enrichment_presentation", false, "first_metadata_not_revealed");
        return;
    }

    const bool selected = OnMain(control, abort, [fixture] {
        auto* source = fixture->host.Source();
        if (!source || source->Count() < 51) return false;
        source->SelectOnly(50);
        source->SetFocus(50);
        fixture->selected = source->Item(50);
        return fixture->selected.is_valid();
    });
    if (!selected ||
        !PageSourceAction(fixture, control, abort,
                          [](media::ListSource& source) {
                              source.SetViewStyle(media::ViewStyle::Tiles);
                              source.SetFilter(L"metadata");
                              source.Sort(2, false);
                          }) ||
        !provider.WaitPending(1, abort, deadline)) {
        AddCase(result, "remote_page_enrichment_presentation", false, "presentation_setup_failed");
        return;
    }

    const ListPage secondPrimary = UiEnrichedPage(1, false);
    const ListPage thirdPrimary = UiEnrichedPage(2, false);
    if (!DeliverUiPage(context, fixture, control, abort, secondPrimary, deadline) ||
        !provider.WaitPending(1, abort, deadline) ||
        !DeliverUiPage(context, fixture, control, abort, thirdPrimary, deadline) ||
        !provider.WaitEnrichments(2, abort, deadline) || !UiSourceCount(fixture, control, abort, &count)) {
        AddCase(result, "remote_page_enrichment_presentation", false, "pending_page_drain_failed");
        return;
    }
    result.checks.remoteNextPendingGated = count == 100;

    if (!DeliverEnrichment(context, control, abort, L"ui-enrich-120", UiEnrichedPage(2, true)) ||
        !Barrier(control, abort) ||
        !DeliverUiEnrichment(context, fixture, control, abort, L"ui-enrich-100", UiEnrichedPage(1, true),
                             deadline) ||
        !UiSourceCount(fixture, control, abort, &count)) {
        AddCase(result, "remote_page_enrichment_presentation", false, "out_of_order_ui_patch_failed");
        return;
    }

    bool filtered = false, sorted = false, focusKept = false;
    uint64_t groupCount = 0, albumACount = 0;
    const bool presentation = UiMetadataPresentation(fixture, control, abort, &filtered, &sorted, &focusKept,
                                                     &groupCount, &albumACount);
    result.checks.remotePendingGated = result.checks.remoteNextPendingGated;
    result.checks.remoteMetadataReveal = count == 130;
    result.checks.remoteSelectionFocusKept = focusKept;
    result.checks.remoteSortedGroupedSearchUpdated =
        filtered && sorted && groupCount == 2 && albumACount == 60;
    result.checks.remoteMetadataRows = count;
    result.checks.remoteMetadataGroupCount = groupCount;
    result.checks.remoteMetadataFilterCount = count;
    const bool pass = presentation && result.checks.remotePendingGated &&
                      result.checks.remoteMetadataReveal && result.checks.remoteSelectionFocusKept &&
                      result.checks.remoteSortedGroupedSearchUpdated && provider.PendingCount() == 0 &&
                      provider.EnrichmentPendingCount() == 0 && provider.Requests().size() == base + 3;
    AddCase(result, "remote_page_enrichment_presentation", pass,
            pass ? std::string() : "metadata_ready_ui_gate_patch_or_presentation_failed");
}

bool WriteAll(HANDLE file, std::string_view bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD count =
            static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

JsonObject ResultJson(const ProbeResult& result, bool ok) {
    JsonObject object;
    object.Insert(L"probe", JsonValue::CreateStringValue(Wide(kProbe)));
    object.Insert(L"provider", JsonValue::CreateStringValue(Wide(kProvider)));
    object.Insert(L"runId", JsonValue::CreateStringValue(Wide(result.runId)));
    object.Insert(L"ok", JsonValue::CreateBooleanValue(ok));
    object.Insert(L"error", result.error.empty() ? JsonValue::CreateNullValue()
                                                 : JsonValue::CreateStringValue(Wide(result.error)));
    object.Insert(L"elapsedMs", Number(result.elapsedMs));
    object.Insert(L"cachePolicy", JsonValue::CreateStringValue(L"Memory"));
    object.Insert(L"diskCacheExercised", JsonValue::CreateBooleanValue(false));
    object.Insert(L"providerRequests", Number(result.providerRequests));
    object.Insert(L"enrichmentRequests", Number(result.enrichmentRequests));

    JsonArray cases;
    JsonArray failures;
    for (const auto& item : result.cases) {
        JsonObject row;
        row.Insert(L"name", JsonValue::CreateStringValue(Wide(item.name)));
        row.Insert(L"ok", JsonValue::CreateBooleanValue(item.ok));
        row.Insert(L"detail", item.detail.empty() ? JsonValue::CreateNullValue()
                                                  : JsonValue::CreateStringValue(Wide(item.detail)));
        cases.Append(row);
        if (!item.ok) failures.Append(JsonValue::CreateStringValue(Wide(item.name + ": " + item.detail)));
    }
    object.Insert(L"cases", cases);
    object.Insert(L"caseCount", Number(static_cast<uint64_t>(result.cases.size())));
    object.Insert(L"failureCodes", failures);

    JsonObject checks;
    checks.Insert(L"callbackDeferred", JsonValue::CreateBooleanValue(result.checks.callbackDeferred));
    checks.Insert(L"onDemandCallsBeforeNext", Number(result.checks.onDemandCallsBeforeNext));
    JsonArray onDemandCounts;
    for (uint64_t value : result.checks.onDemandCounts)
        onDemandCounts.Append(Number(value));
    checks.Insert(L"onDemandCounts", onDemandCounts);
    JsonArray allProgress;
    for (uint64_t value : result.checks.allProgress)
        allProgress.Append(Number(value));
    checks.Insert(L"allProgress", allProgress);
    checks.Insert(L"duplicateRows", Number(result.checks.duplicateRows));
    checks.Insert(L"duplicateIdOccurrences", Number(result.checks.duplicateIdOccurrences));
    checks.Insert(L"coalescedCalls", Number(result.checks.coalescedCalls));
    checks.Insert(L"isolatedCallbackDelta", Number(result.checks.isolatedCallbackDelta));
    checks.Insert(L"cacheContextRequestCount", Number(result.checks.cacheContextRequestCount));
    checks.Insert(L"sameContextCoalesced", JsonValue::CreateBooleanValue(result.checks.sameContextCoalesced));
    checks.Insert(L"changedContextIsolated",
                  JsonValue::CreateBooleanValue(result.checks.changedContextIsolated));
    checks.Insert(L"capturedContextReused",
                  JsonValue::CreateBooleanValue(result.checks.capturedContextReused));
    checks.Insert(L"lateContextFetchRejected",
                  JsonValue::CreateBooleanValue(result.checks.lateContextFetchRejected));
    checks.Insert(L"lateContextPrimaryRejected",
                  JsonValue::CreateBooleanValue(result.checks.lateContextPrimaryRejected));
    checks.Insert(L"lateContextEnrichmentRejected",
                  JsonValue::CreateBooleanValue(result.checks.lateContextEnrichmentRejected));
    checks.Insert(L"lateContextDataIsolated",
                  JsonValue::CreateBooleanValue(result.checks.lateContextDataIsolated));
    checks.Insert(L"autoDrainCallsBeforeCancel", Number(result.checks.autoDrainCallsBeforeCancel));
    checks.Insert(L"autoDrainCallsAfterLate", Number(result.checks.autoDrainCallsAfterLate));
    checks.Insert(L"refreshedCount", Number(result.checks.refreshedCount));
    checks.Insert(L"oldGenerationIgnored", JsonValue::CreateBooleanValue(result.checks.oldGenerationIgnored));
    checks.Insert(L"refreshTokenSignalled",
                  JsonValue::CreateBooleanValue(result.checks.refreshTokenSignalled));
    checks.Insert(L"repeatedCursorCalls", Number(result.checks.repeatedCursorCalls));
    checks.Insert(L"noProgressCalls", Number(result.checks.noProgressCalls));
    checks.Insert(L"noProgressBounded", JsonValue::CreateBooleanValue(result.checks.noProgressBounded));
    checks.Insert(L"cancelTokenSignalled", JsonValue::CreateBooleanValue(result.checks.cancelTokenSignalled));
    checks.Insert(L"errorPrefixCount", Number(result.checks.errorPrefixCount));
    checks.Insert(L"retryCursorOffset", Number(result.checks.retryCursorOffset));
    checks.Insert(L"retrySameCursor", JsonValue::CreateBooleanValue(result.checks.retrySameCursor));
    checks.Insert(L"finalRetryCount", Number(result.checks.finalRetryCount));
    checks.Insert(L"earlyNextRequests", Number(result.checks.earlyNextRequests));
    JsonArray earlyNextCounts;
    for (uint64_t value : result.checks.earlyNextCounts)
        earlyNextCounts.Append(Number(value));
    checks.Insert(L"earlyNextCounts", earlyNextCounts);
    checks.Insert(L"cancelledNextRequestsBeforeB", Number(result.checks.cancelledNextRequestsBeforeB));
    checks.Insert(L"cancelledNextRequestsAfterB", Number(result.checks.cancelledNextRequestsAfterB));
    checks.Insert(L"cancelledNextCount", Number(result.checks.cancelledNextCount));
    checks.Insert(L"metadataRejectCount", Number(result.checks.metadataRejectCount));
    checks.Insert(L"metadataRetrySameCursor",
                  JsonValue::CreateBooleanValue(result.checks.metadataRetrySameCursor));
    checks.Insert(L"metadataRetryRepeatedCursorError",
                  JsonValue::CreateBooleanValue(result.checks.metadataRetryRepeatedCursorError));
    checks.Insert(L"metadataRetryCount", Number(result.checks.metadataRetryCount));
    checks.Insert(L"metadataFinalComplete",
                  JsonValue::CreateBooleanValue(result.checks.metadataFinalComplete));
    checks.Insert(L"remoteInitialCount", Number(result.checks.remoteInitialCount));
    JsonArray remoteFilterCounts;
    for (uint64_t value : result.checks.remoteFilterCounts)
        remoteFilterCounts.Append(Number(value));
    checks.Insert(L"remoteFilterCounts", remoteFilterCounts);
    checks.Insert(L"remoteOnDemandCount", Number(result.checks.remoteOnDemandCount));
    checks.Insert(L"remoteSortedPages", JsonValue::CreateBooleanValue(result.checks.remoteSortedPages));
    checks.Insert(L"remoteTileRows", Number(result.checks.remoteTileRows));
    checks.Insert(L"remoteTileGroupCount", Number(result.checks.remoteTileGroupCount));
    checks.Insert(L"remoteTilePresentationKept",
                  JsonValue::CreateBooleanValue(result.checks.remoteTilePresentationKept));
    checks.Insert(L"remoteDestroyCancelled",
                  JsonValue::CreateBooleanValue(result.checks.remoteDestroyCancelled));
    checks.Insert(L"ordinaryMetadataReadyRows", Number(result.checks.ordinaryMetadataReadyRows));
    checks.Insert(L"ordinaryUpdatedRowsAbsent",
                  JsonValue::CreateBooleanValue(result.checks.ordinaryUpdatedRowsAbsent));
    checks.Insert(L"initialPrimaryBeforeEnrichment",
                  JsonValue::CreateBooleanValue(result.checks.initialPrimaryBeforeEnrichment));
    checks.Insert(L"initialMetadataReadyRows", Number(result.checks.initialMetadataReadyRows));
    checks.Insert(L"initialEnrichedVersionDelta", Number(result.checks.initialEnrichedVersionDelta));
    checks.Insert(L"initialDataVersionDelta", Number(result.checks.initialDataVersionDelta));
    checks.Insert(L"initialPatchTransportStable",
                  JsonValue::CreateBooleanValue(result.checks.initialPatchTransportStable));
    checks.Insert(L"initialUpdatedRange", JsonValue::CreateBooleanValue(result.checks.initialUpdatedRange));
    checks.Insert(L"nextPageRequestsWhilePending", Number(result.checks.nextPageRequestsWhilePending));
    checks.Insert(L"nextPageRowsBeforeEnrichment", Number(result.checks.nextPageRowsBeforeEnrichment));
    checks.Insert(L"nextPageMetadataReadyBefore", Number(result.checks.nextPageMetadataReadyBefore));
    checks.Insert(L"nextPageTransportStable",
                  JsonValue::CreateBooleanValue(result.checks.nextPageTransportStable));
    JsonArray outOfOrderReadyRows;
    for (uint64_t value : result.checks.outOfOrderReadyRows)
        outOfOrderReadyRows.Append(Number(value));
    checks.Insert(L"outOfOrderReadyRows", outOfOrderReadyRows);
    checks.Insert(L"outOfOrderPatchesApplied",
                  JsonValue::CreateBooleanValue(result.checks.outOfOrderPatchesApplied));
    checks.Insert(L"outOfOrderTransportStable",
                  JsonValue::CreateBooleanValue(result.checks.outOfOrderTransportStable));
    checks.Insert(L"outOfOrderConcurrencyBounded",
                  JsonValue::CreateBooleanValue(result.checks.outOfOrderConcurrencyBounded));
    checks.Insert(L"metadataCancellationShared",
                  JsonValue::CreateBooleanValue(result.checks.metadataCancellationShared));
    checks.Insert(L"unchangedMetadataAdvanced",
                  JsonValue::CreateBooleanValue(result.checks.unchangedMetadataAdvanced));
    checks.Insert(L"unchangedDataVersionStable",
                  JsonValue::CreateBooleanValue(result.checks.unchangedDataVersionStable));
    checks.Insert(L"unchangedUpdatedRowsAbsent",
                  JsonValue::CreateBooleanValue(result.checks.unchangedUpdatedRowsAbsent));
    checks.Insert(L"declinedEnrichmentReleased",
                  JsonValue::CreateBooleanValue(result.checks.declinedEnrichmentReleased));
    checks.Insert(L"declinedEnrichmentNoCallback",
                  JsonValue::CreateBooleanValue(result.checks.declinedEnrichmentNoCallback));
    checks.Insert(L"declinedEnrichmentDataVersionStable",
                  JsonValue::CreateBooleanValue(result.checks.declinedEnrichmentDataVersionStable));
    checks.Insert(L"refreshOldPatchRejected",
                  JsonValue::CreateBooleanValue(result.checks.refreshOldPatchRejected));
    checks.Insert(L"refreshNewPatchApplied",
                  JsonValue::CreateBooleanValue(result.checks.refreshNewPatchApplied));
    checks.Insert(L"refreshPatchRequests", Number(result.checks.refreshPatchRequests));
    checks.Insert(L"refreshOldCancellation",
                  JsonValue::CreateBooleanValue(result.checks.refreshOldCancellation));
    checks.Insert(L"cancelPatchRejected", JsonValue::CreateBooleanValue(result.checks.cancelPatchRejected));
    checks.Insert(L"cancelBaseRetained", JsonValue::CreateBooleanValue(result.checks.cancelBaseRetained));
    checks.Insert(L"invalidCountRejected", JsonValue::CreateBooleanValue(result.checks.invalidCountRejected));
    checks.Insert(L"invalidIdentityRejected",
                  JsonValue::CreateBooleanValue(result.checks.invalidIdentityRejected));
    checks.Insert(L"invalidMetadataReadyRows", Number(result.checks.invalidMetadataReadyRows));
    checks.Insert(L"invalidBaseRetained", JsonValue::CreateBooleanValue(result.checks.invalidBaseRetained));
    checks.Insert(L"duplicateMetadataOccurrences", Number(result.checks.duplicateMetadataOccurrences));
    checks.Insert(L"duplicateMetadataPatchedByOccurrence",
                  JsonValue::CreateBooleanValue(result.checks.duplicateMetadataPatchedByOccurrence));
    checks.Insert(L"failurePrimaryRetained",
                  JsonValue::CreateBooleanValue(result.checks.failurePrimaryRetained));
    checks.Insert(L"failureMetadataReady", JsonValue::CreateBooleanValue(result.checks.failureMetadataReady));
    checks.Insert(L"failureErrorAbsent", JsonValue::CreateBooleanValue(result.checks.failureErrorAbsent));
    checks.Insert(L"failureDataVersionStable",
                  JsonValue::CreateBooleanValue(result.checks.failureDataVersionStable));
    checks.Insert(L"failureUpdatedRowsAbsent",
                  JsonValue::CreateBooleanValue(result.checks.failureUpdatedRowsAbsent));
    checks.Insert(L"accountPatchRejected", JsonValue::CreateBooleanValue(result.checks.accountPatchRejected));
    checks.Insert(L"accountBaseRetained", JsonValue::CreateBooleanValue(result.checks.accountBaseRetained));
    checks.Insert(L"restoreInitialDeferred",
                  JsonValue::CreateBooleanValue(result.checks.restoreInitialDeferred));
    checks.Insert(L"restoreInitialEmptyNoError",
                  JsonValue::CreateBooleanValue(result.checks.restoreInitialEmptyNoError));
    checks.Insert(L"restoreNextPreserved", JsonValue::CreateBooleanValue(result.checks.restoreNextPreserved));
    checks.Insert(L"softRetirePrefixRetained",
                  JsonValue::CreateBooleanValue(result.checks.softRetirePrefixRetained));
    checks.Insert(L"softRetireOldNetworkRejected",
                  JsonValue::CreateBooleanValue(result.checks.softRetireOldNetworkRejected));
    checks.Insert(L"softRetireNewNetworkApplied",
                  JsonValue::CreateBooleanValue(result.checks.softRetireNewNetworkApplied));
    checks.Insert(L"remotePendingGated", JsonValue::CreateBooleanValue(result.checks.remotePendingGated));
    checks.Insert(L"remoteMetadataReveal", JsonValue::CreateBooleanValue(result.checks.remoteMetadataReveal));
    checks.Insert(L"remoteNextPendingGated",
                  JsonValue::CreateBooleanValue(result.checks.remoteNextPendingGated));
    checks.Insert(L"remoteSelectionFocusKept",
                  JsonValue::CreateBooleanValue(result.checks.remoteSelectionFocusKept));
    checks.Insert(L"remoteSortedGroupedSearchUpdated",
                  JsonValue::CreateBooleanValue(result.checks.remoteSortedGroupedSearchUpdated));
    checks.Insert(L"remoteMetadataRows", Number(result.checks.remoteMetadataRows));
    checks.Insert(L"remoteMetadataGroupCount", Number(result.checks.remoteMetadataGroupCount));
    checks.Insert(L"remoteMetadataFilterCount", Number(result.checks.remoteMetadataFilterCount));
    object.Insert(L"checks", checks);
    return object;
}

bool WriteResult(const fs::path& resultPath, const ProbeResult& result, bool ok) {
    const fs::path temporary = resultPath.wstring() + L".tmp";
    const std::string bytes = winrt::to_string(ResultJson(result, ok).Stringify());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool written = WriteAll(file, bytes) && FlushFileBuffers(file);
    CloseHandle(file);
    if (!written || !MoveFileExW(temporary.c_str(), resultPath.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

void RunCases(const Paths& paths, const std::shared_ptr<ProbeContext>& context, RunControl& control,
              abort_callback& abort, ProbeResult& result, Clock::time_point globalDeadline) {
    const auto requestBytes = ReadBounded(paths.claimed, kMaxMarkerBytes);
    DeleteFileW(paths.claimed.c_str());
    if (!requestBytes) {
        result.error = "request_read_failed";
        return;
    }
    const Marker marker = ReadMarker(*requestBytes);
    if (!marker.valid) {
        result.error = "invalid_probe_marker";
        return;
    }
    result.runId = marker.runId;

    if (!OnMain(control, abort, [context] {
            context->provider = std::make_unique<FakeProvider>();
            return context->provider != nullptr;
        })) {
        result.error = "provider_init_failed";
        return;
    }

    CaseOnDemand(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseAll(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseDuplicates(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseTwoLeases(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseCacheContext(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseCacheContextLateData(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseCancelAll(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseRefreshGeneration(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseRepeatedCursor(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseNoProgress(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseErrorRetry(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseEarlyNext(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseCancelledDemand(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseMetadataAtomic(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseRemotePageFilterSort(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseRemotePageTiles(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseRemotePageDestroy(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseInitialEnrichment(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseNextPageWhileEnrichmentPending(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseOutOfOrderEnrichment(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseUnchangedEnrichment(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseDeclinedEnrichment(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseRefreshEnrichment(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseCancelEnrichment(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseInvalidEnrichment(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseDuplicateEnrichment(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseFailureEnrichment(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseAccountEnrichment(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseRestoreGateAndSoftRetirement(context, control, abort, CaseDeadline(globalDeadline), result);
    CaseRemotePageEnrichmentPresentation(context, control, abort, CaseDeadline(globalDeadline), result);
    result.providerRequests = context->provider ? context->provider->Requests().size() : 0;
    result.enrichmentRequests = context->provider ? context->provider->EnrichmentRequests() : 0;
}

void RunProbe(const Paths& paths, const std::shared_ptr<ProbeContext>& context, RunControl& control,
              abort_callback& abort) {
    SetThreadDescription(GetCurrentThread(), L"Glasswave list session probe");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    const auto started = Clock::now();
    const auto deadline = started + kProbeBudget;
    ProbeResult result;
    try {
        RunCases(paths, context, control, abort, result, deadline);
    } catch (const exception_aborted&) {
        result.error = "cancelled";
    } catch (...) {
        result.error = "probe_exception";
    }

    if (!abort.is_aborting() && !control.stopping.load(std::memory_order_acquire)) {
        if (!OnMain(control, abort, [context] {
                context->ReleaseAll();
                return true;
            })) {
            result.error = result.error.empty() ? "cleanup_failed" : result.error;
        }
    }
    result.elapsedMs = ElapsedMs(started);
    const bool casesPass =
        !result.cases.empty() &&
        std::all_of(result.cases.begin(), result.cases.end(), [](const CaseResult& item) { return item.ok; });
    const bool ok = result.error.empty() && casesPass && result.cases.size() == 30;
    WriteResult(paths.result, result, ok);
    winrt::uninit_apartment();
}

class ListSessionProbeLifecycle : public initquit {
  public:
    ~ListSessionProbeLifecycle() { Stop(); }

    void on_init() override {
        const auto paths = ProbePaths();
        if (!paths) return;
        std::error_code error;
        if (!fs::is_regular_file(paths->request, error) || error) return;

        DeleteFileW(paths->result.c_str());
        if (!MoveFileExW(paths->request.c_str(), paths->claimed.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return;

        m_abort = std::make_unique<abort_callback_impl>();
        m_control = std::make_shared<RunControl>();
        m_context = std::make_shared<ProbeContext>();
        auto* abort = m_abort.get();
        auto control = m_control;
        auto context = m_context;
        m_worker = std::thread(
            [paths = *paths, context, control, abort] { RunProbe(paths, context, *control, *abort); });
    }

    void on_quit() override { Stop(); }

  private:
    void Stop() noexcept {
        std::shared_ptr<MainCall> activeCall;
        if (m_control) {
            {
                std::lock_guard lock(m_control->mutex);
                m_control->stopping.store(true, std::memory_order_release);
                activeCall = m_control->activeCall;
            }
            if (activeCall) activeCall->changed.notify_all();
        }
        if (m_abort) m_abort->abort();
        if (m_worker.joinable()) m_worker.join();
        if (m_context && core_api::is_main_thread()) m_context->ReleaseAll();
        m_control.reset();
        m_abort.reset();
    }

    std::shared_ptr<RunControl> m_control;
    std::shared_ptr<ProbeContext> m_context;
    std::unique_ptr<abort_callback_impl> m_abort;
    std::thread m_worker;
};

FB2K_SERVICE_FACTORY(ListSessionProbeLifecycle);

} // namespace
} // namespace glasswave::stream::list_session_probe
