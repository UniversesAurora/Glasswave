#define GLASSWAVE_CACHE_STANDALONE
#define GLASSWAVE_LISTS_STANDALONE
#define GLASSWAVE_LISTS_TESTING

#include "../src/foo_ui_glasswave/cache/lists.cpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using glasswave::cache::Store;
using glasswave::cache::lists::Identity;
using glasswave::cache::lists::Record;
using glasswave::stream::CatalogSearchItem;
using glasswave::stream::ListCursor;
using glasswave::stream::ListPage;
using glasswave::stream::Track;

std::atomic_size_t checkCount = 0;

class TestFailure final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

void Check(bool condition, std::string_view message) {
    ++checkCount;
    if (!condition) throw TestFailure(std::string(message));
}

fs::path Normalized(const fs::path& path) {
    std::error_code error;
    const auto absolute = fs::absolute(path, error);
    if (error) throw TestFailure("Unable to make a test path absolute.");
    const auto canonical = fs::weakly_canonical(absolute, error);
    if (error) throw TestFailure("Unable to resolve a test path.");
    return canonical.lexically_normal();
}

bool IsWithin(const fs::path& root, const fs::path& path) {
    const auto relative = Normalized(path).lexically_relative(Normalized(root));
    if (relative.empty()) return false;
    const auto first = relative.begin();
    return first != relative.end() && *first != fs::path(L"..");
}

fs::path ValidateRoot(const fs::path& argument) {
    const auto root = Normalized(argument);
    const auto parent = Normalized(fs::path(L".local/tests"));
    const auto name = root.filename().wstring();
    Check(root.parent_path() == parent, "List-cache test root must be a direct child of .local/tests");
    Check(name.starts_with(L"list-cache-"), "List-cache test root has an unsafe prefix");
    return root;
}

void SafeRemove(const fs::path& root) {
    const auto parent = Normalized(fs::path(L".local/tests"));
    Check(IsWithin(parent, root), "Refusing to remove a path outside .local/tests");
    if (!fs::exists(root)) return;
    std::error_code error;
    fs::remove_all(root, error);
    if (error) throw TestFailure("Unable to remove the list-cache test root.");
}

struct Adapter {
    std::mutex mutex;
    Store* store = nullptr;
    std::map<std::string, uint64_t> revisions;
};

Adapter& TestAdapter() {
    static Adapter adapter;
    return adapter;
}

template <typename Function> void ExpectFailure(Function&& function, std::string_view message) {
    bool failed = false;
    try {
        function();
    } catch (...) {
        failed = true;
    }
    Check(failed, message);
}

Track MakeTrack(std::wstring id, std::wstring title) {
    Track track;
    track.id = std::move(id);
    track.title = std::move(title);
    track.artists.push_back({L"artist-1", L"Artist"});
    track.durationMs = 245'000;
    track.popularity = 42;
    return track;
}

Identity MakeIdentity(uint64_t revision = 0) {
    return {.provider = "test", .logical = "tracks/v1/lang=en/search=same", .accountRevision = revision};
}

Record MakePageRecord(std::vector<Track> tracks, std::size_t ordinal, bool more, std::wstring nextValue = {},
                      std::size_t nextOffset = 0, std::optional<std::size_t> total = std::nullopt,
                      bool enrichmentPending = false) {
    ListPage page;
    page.rows = std::move(tracks);
    page.next = {std::move(nextValue), nextOffset};
    page.more = more;
    page.total = total;
    page.revision = L"revision-1";
    page.enrichmentPending = enrichmentPending;
    return {.page = std::move(page), .pages = ordinal, .validatedAt = 1'760'000'000};
}

std::optional<Record> LoadAndWait(Identity identity) {
    auto promise = std::make_shared<std::promise<std::optional<Record>>>();
    auto future = promise->get_future();
    glasswave::cache::lists::Load(std::move(identity), [promise](std::optional<Record> value) {
        promise->set_value(std::move(value));
    });
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw TestFailure("List-cache load callback timed out.");
    return future.get();
}

std::future<std::optional<Record>> StartLoadWithManager(glasswave::cache::lists::Manager& manager,
                                                        Identity identity) {
    auto promise = std::make_shared<std::promise<std::optional<Record>>>();
    auto future = promise->get_future();
    manager.Load(std::move(identity),
                 [promise](std::optional<Record> value) { promise->set_value(std::move(value)); });
    return future;
}

std::optional<Record> WaitForLoad(std::future<std::optional<Record>> future) {
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw TestFailure("List-cache load callback timed out.");
    return future.get();
}

std::optional<Record> LoadWithManagerAndWait(glasswave::cache::lists::Manager& manager, Identity identity) {
    return WaitForLoad(StartLoadWithManager(manager, std::move(identity)));
}

std::vector<Track> Rows(const Record& record) {
    return std::get<std::vector<Track>>(record.page.rows);
}

void TestSerializerRoundTrip() {
    ListPage page;
    page.rows = std::vector<Track>{MakeTrack(L"17", L"Repeated"), MakeTrack(L"2", L"Middle"),
                                   MakeTrack(L"17", L"Repeated again")};
    page.next = {L"opaque-next", 37};
    page.more = true;
    page.total = 9;
    page.revision = L"rev-9";
    page.enrichmentPending = true;
    const auto body = glasswave::cache::lists::detail::SerializePageForTest(1, page);
    const auto parsed = glasswave::cache::lists::detail::ParsePageForTest(body, 1);
    const auto rows = std::get<std::vector<Track>>(parsed.rows);
    Check(rows.size() == 3, "Serializer changed the row count");
    Check(rows[0].id == L"17" && rows[1].id == L"2" && rows[2].id == L"17",
          "Serializer reordered or deduplicated track rows");
    Check(rows[0].title == L"Repeated" && rows[2].title == L"Repeated again",
          "Serializer did not preserve duplicate tracks independently");
    Check(parsed.next.value == L"opaque-next" && parsed.next.offset == 37,
          "Serializer did not preserve the opaque cursor");
    Check(parsed.more && parsed.total == 9 && parsed.revision == L"rev-9",
          "Serializer did not preserve page headers");
    Check(parsed.enrichmentPending, "Serializer did not preserve the enrichment marker");

    auto missingMarker = body;
    const std::string marker = "\"enrichmentPending\":true,";
    const auto markerAt = missingMarker.find(marker);
    if (markerAt == std::string::npos)
        throw TestFailure("Serializer output did not contain the enrichment marker");
    missingMarker.erase(markerAt, marker.size());
    ExpectFailure([&] { (void)glasswave::cache::lists::detail::ParsePageForTest(missingMarker, 1); },
                  "A current-format page without the enrichment marker was accepted");

    // Whatever the current version is, a page naming another one is refused.
    auto oldSchema = body;
    constexpr std::string_view kSchemaPrefix = "glasswave-list-page/";
    const auto schemaAt = oldSchema.find(kSchemaPrefix);
    Check(schemaAt != std::string::npos, "Serializer output did not contain the page schema");
    const auto versionAt = schemaAt + kSchemaPrefix.size();
    const auto versionEnd = oldSchema.find('"', versionAt);
    Check(versionEnd != std::string::npos, "Serializer output did not end the page schema");
    oldSchema.replace(versionAt, versionEnd - versionAt, "legacy");
    ExpectFailure([&] { (void)glasswave::cache::lists::detail::ParsePageForTest(oldSchema, 1); },
                  "A page using a legacy schema was accepted");
}

void TestCatalogSearchMixedRowsRoundTrip() {
    Check(glasswave::stream::IsCatalogSearch(glasswave::stream::ListKind::CatalogSearch) &&
              glasswave::stream::IsCatalogSearch(glasswave::stream::ListKind::CatalogSearchArtists) &&
              glasswave::stream::IsCatalogSearch(glasswave::stream::ListKind::CatalogSearchAlbums) &&
              glasswave::stream::IsCatalogSearch(glasswave::stream::ListKind::CatalogSearchPlaylists) &&
              glasswave::stream::IsCatalogSearch(glasswave::stream::ListKind::CatalogSearchTopResults) &&
              !glasswave::stream::IsCatalogSearch(glasswave::stream::ListKind::ArtistPlaylists),
          "Catalog search categories were not classified independently from artist lists");

    const CatalogSearchItem track = MakeTrack(L"shared", L"Track result");
    const CatalogSearchItem artist = glasswave::stream::Artist{.id = L"shared", .name = L"Artist result"};
    const CatalogSearchItem album = glasswave::stream::Album{.id = L"shared", .name = L"Album result"};
    const CatalogSearchItem playlist =
        glasswave::stream::Playlist{.id = L"shared", .name = L"Playlist result"};

    ListPage first;
    first.rows = std::vector<CatalogSearchItem>{track, artist};
    first.next = {L"search-next", 2};
    first.more = true;
    first.total = 4;
    first.revision = L"search-rank";
    const auto serialized = glasswave::cache::lists::detail::SerializePageForTest(1, first);
    const auto parsed = glasswave::cache::lists::detail::ParsePageForTest(serialized, 1);
    const auto* parsedRows = std::get_if<std::vector<CatalogSearchItem>>(&parsed.rows);
    Check(parsedRows && parsedRows->size() == 2 &&
              glasswave::stream::CatalogSearchItemKey((*parsedRows)[0]) == L"track:shared" &&
              glasswave::stream::CatalogSearchItemKey((*parsedRows)[1]) == L"artist:shared",
          "The mixed catalog-search codec changed server rank or type-qualified identity");

    Identity identity{.provider = "catalog-search", .logical = "search/v1/topResults", .accountRevision = 0};
    glasswave::cache::lists::SavePage(identity, {.page = first, .pages = 1, .validatedAt = 1'760'000'000}, 1);
    ListPage second;
    second.rows = std::vector<CatalogSearchItem>{album, playlist};
    second.total = 4;
    second.revision = L"search-rank";
    glasswave::cache::lists::SavePage(identity, {.page = second, .pages = 2, .validatedAt = 1'760'000'000},
                                      1);
    const auto loaded = LoadAndWait(identity);
    const auto* loadedRows =
        loaded ? std::get_if<std::vector<CatalogSearchItem>>(&loaded->page.rows) : nullptr;
    Check(loadedRows && loadedRows->size() == 4 &&
              glasswave::stream::CatalogSearchItemKey((*loadedRows)[0]) == L"track:shared" &&
              glasswave::stream::CatalogSearchItemKey((*loadedRows)[1]) == L"artist:shared" &&
              glasswave::stream::CatalogSearchItemKey((*loadedRows)[2]) == L"album:shared" &&
              glasswave::stream::CatalogSearchItemKey((*loadedRows)[3]) == L"playlist:shared",
          "Persisted top results did not preserve mixed server order");
}

void TestArtistSectionQueriesStayIndependent() {
    const glasswave::stream::ArtistSection tracks{.id = L"top-songs",
                                                  .label = L"Hot songs",
                                                  .rowKind = glasswave::stream::ArtistSectionRowKind::Tracks};
    const glasswave::stream::ArtistSection albums{.id = L"live-albums",
                                                  .label = L"Live albums",
                                                  .rowKind = glasswave::stream::ArtistSectionRowKind::Albums};
    const glasswave::stream::ArtistSection playlists{.id = L"featured-playlists",
                                                     .label = L"Featured playlists",
                                                     .rowKind =
                                                         glasswave::stream::ArtistSectionRowKind::Playlists};
    const auto trackQuery = glasswave::stream::ArtistSectionQuery(L"artist-1", tracks);
    const auto albumQuery = glasswave::stream::ArtistSectionQuery(L"artist-1", albums);
    const auto playlistQuery = glasswave::stream::ArtistSectionQuery(L"artist-1", playlists);
    Check(trackQuery.kind == glasswave::stream::ListKind::ArtistTopTracks &&
              albumQuery.kind == glasswave::stream::ListKind::ArtistAlbums &&
              playlistQuery.kind == glasswave::stream::ListKind::ArtistPlaylists,
          "Artist section row kinds did not map to their independent list kinds");
    Check(trackQuery != albumQuery && albumQuery != playlistQuery &&
              trackQuery.section != albumQuery.section && albumQuery.section != playlistQuery.section,
          "Artist sections lost independent query or cache identities");
}

void TestMalformedInputs() {
    auto record = MakePageRecord({MakeTrack(L"1", L"One")}, 1, true, L"next", 20, 2);
    const auto valid = glasswave::cache::lists::detail::SerializePageForTest(1, record.page);
    const auto comma = valid.find(',');
    Check(comma != std::string::npos, "Serializer output did not contain fields");
    const std::string duplicate =
        valid.substr(0, comma) + ",\"schema\":\"glasswave-list-page/1\"" + valid.substr(comma);
    ExpectFailure([&] { (void)glasswave::cache::lists::detail::ParsePageForTest(duplicate, 1); },
                  "Duplicate JSON keys were accepted");

    auto typeMismatch = valid;
    const auto moreAt = typeMismatch.find("\"more\":true");
    Check(moreAt != std::string::npos, "Serializer output did not contain the completion flag");
    typeMismatch.replace(moreAt, std::string("\"more\":true").size(), "\"more\":\"yes\"");
    ExpectFailure([&] { (void)glasswave::cache::lists::detail::ParsePageForTest(typeMismatch, 1); },
                  "A wrong JSON value type was accepted");

    const std::string manifest =
        R"({"schema":"glasswave-list-manifest/3","logical":"tracks/v1/lang=en/search=same","generation":"run:1","generationInput":"1","saveTicket":"1","validatedAt":1760000000,"complete":false,"rowKind":"track","rowCount":1,"nextValue":"same","nextOffset":20,"revision":"r","pages":[{"ordinal":1,"rowCount":1,"more":true,"enrichmentPending":false,"nextValue":"same","nextOffset":20,"revision":"r","chunk":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}]})";
    const auto parsedManifest =
        glasswave::cache::lists::detail::ParseManifestForTest(manifest, "tracks/v1/lang=en/search=same");
    auto duplicateCursor = MakePageRecord({MakeTrack(L"2", L"Two")}, 2, true, L"same", 40);
    ExpectFailure(
        [&] {
            glasswave::cache::lists::detail::ValidateContinuityForTest(parsedManifest, 2,
                                                                       duplicateCursor.page);
        },
        "A duplicate continuation cursor was accepted");
    auto gap = MakePageRecord({MakeTrack(L"2", L"Two")}, 3, false);
    ExpectFailure(
        [&] { glasswave::cache::lists::detail::ValidateContinuityForTest(parsedManifest, 3, gap.page); },
        "A noncontiguous page ordinal was accepted");

    auto tooManyRows = manifest;
    const auto rowCountAt = tooManyRows.find("\"rowCount\":1");
    Check(rowCountAt != std::string::npos, "Manifest fixture did not contain a row count");
    tooManyRows.replace(rowCountAt, std::string("\"rowCount\":1").size(), "\"rowCount\":200001");
    ExpectFailure(
        [&] {
            (void)glasswave::cache::lists::detail::ParseManifestForTest(tooManyRows,
                                                                        "tracks/v1/lang=en/search=same");
        },
        "A manifest row count beyond the bound was accepted");

    ListPage changedSchema;
    changedSchema.rows = std::vector<glasswave::stream::Playlist>{{.id = L"playlist", .name = L"List"}};
    changedSchema.more = false;
    ExpectFailure(
        [&] { glasswave::cache::lists::detail::ValidateContinuityForTest(parsedManifest, 2, changedSchema); },
        "A page with a different row schema was accepted");
}

void TestManifestRoundTrip() {
    const std::string body =
        R"({"schema":"glasswave-list-manifest/3","logical":"tracks/v1/lang=en/search=same","generation":"abcdef:7","generationInput":"7","saveTicket":"11","validatedAt":1760000000,"complete":true,"rowKind":"track","rowCount":2,"nextValue":"","nextOffset":0,"total":2,"revision":"rev","pages":[{"ordinal":1,"rowCount":2,"more":false,"enrichmentPending":false,"nextValue":"","nextOffset":0,"revision":"rev","total":2,"chunk":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}]})";
    const auto parsed =
        glasswave::cache::lists::detail::ParseManifestForTest(body, "tracks/v1/lang=en/search=same");
    const auto encoded = glasswave::cache::lists::detail::SerializeManifestForTest(parsed);
    const auto reparsed =
        glasswave::cache::lists::detail::ParseManifestForTest(encoded, "tracks/v1/lang=en/search=same");
    Check(reparsed.pages.size() == 1 && reparsed.pages[0].rowCount == 2,
          "Manifest serialization changed page references");
    Check(reparsed.complete && reparsed.total == 2 && reparsed.generationInput == 7,
          "Manifest serialization changed aggregate headers");
    Check(reparsed.pages[0].chunk == "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" &&
              !reparsed.pages[0].enrichmentPending,
          "Manifest serialization changed the immutable chunk or enrichment marker");
}

void TestSnapshotSurvivesStoreReopen(std::unique_ptr<Store>& store) {
    Identity identity{.provider = "persist", .logical = "tracks/v1/lang=en", .accountRevision = 0};
    const auto accountScope = store->Scope(identity.provider);
    const auto guardScope = store->Scope("lists:" + identity.provider);
    const auto first = MakePageRecord({MakeTrack(L"1", L"Persisted"), MakeTrack(L"1", L"Duplicate")}, 1, true,
                                      L"persisted-next", 2, 3);
    const auto second = MakePageRecord({MakeTrack(L"2", L"Tail")}, 2, false, {}, 0, 3);
    const auto alwaysCurrent = [] { return true; };
    Check(glasswave::cache::lists::SavePageDocument(*store, identity, accountScope, guardScope, 1, "abcdef:1",
                                                    1, first, alwaysCurrent),
          "The first page was not persisted directly");
    Check(glasswave::cache::lists::SavePageDocument(*store, identity, accountScope, guardScope, 1, "abcdef:1",
                                                    2, second, alwaysCurrent),
          "The second page was not persisted directly");

    const auto cacheRoot = store->Root();
    TestAdapter().store = nullptr;
    store.reset();
    store = std::make_unique<Store>(cacheRoot);
    TestAdapter().store = store.get();
    const auto loaded = glasswave::cache::lists::AssembleRecord(*store, identity, accountScope, guardScope);
    Check(loaded && loaded->pages == 2, "A persisted manifest did not survive Store reopen");
    const auto rows = Rows(*loaded);
    Check(rows.size() == 3 && rows[0].id == L"1" && rows[1].id == L"1" && rows[2].id == L"2",
          "A reopened snapshot changed duplicate-row order");
}

void TestCurrentFormatOnlyRejectsLegacyManifest() {
    Identity identity{
        .provider = "legacy", .logical = "tracks/v1/lang=en/search=legacy", .accountRevision = 0};
    const auto accountScope = TestAdapter().store->Scope(identity.provider);
    const auto guardScope = TestAdapter().store->Scope("lists:" + identity.provider);
    const std::string manifest =
        R"({"schema":"glasswave-list-manifest/2","logical":"tracks/v1/lang=en/search=legacy","generation":"legacy:1","generationInput":"1","saveTicket":"1","validatedAt":1760000000,"complete":false,"rowKind":"track","rowCount":1,"nextValue":"legacy-next","nextOffset":20,"total":2,"revision":"revision-1","pages":[{"ordinal":1,"rowCount":1,"more":true,"nextValue":"legacy-next","nextOffset":20,"revision":"revision-1","total":2,"hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}]})";
    Check(glasswave::cache::lists::WriteSnapshot(
              *TestAdapter().store, glasswave::cache::lists::ManifestKey(identity, accountScope, guardScope),
              manifest, glasswave::cache::lists::kManifestContentType, 1'760'000'000, "schema=2"),
          "Could not create a legacy manifest fixture");

    auto loaded = LoadAndWait(identity);
    Check(!loaded.has_value(), "A legacy manifest was accepted instead of rejected");

    const std::string generation = "current:1";
    const auto second = MakePageRecord({MakeTrack(L"tail", L"Tail")}, 2, false, {}, 0, 2);
    const auto alwaysCurrent = [] { return true; };
    const auto first = MakePageRecord({MakeTrack(L"current", L"Current row")}, 1, true, L"cursor", 20, 2);
    Check(glasswave::cache::lists::SavePageDocument(*TestAdapter().store, identity, accountScope, guardScope,
                                                    1, generation, 1, first, alwaysCurrent),
          "A current snapshot could not replace the rejected legacy manifest");
    Check(glasswave::cache::lists::SavePageDocument(*TestAdapter().store, identity, accountScope, guardScope,
                                                    1, generation, 2, second, alwaysCurrent),
          "A current snapshot could not continue after replacing the legacy manifest");
    loaded = LoadAndWait(identity);
    const auto rows = loaded ? Rows(*loaded) : std::vector<Track>{};
    Check(loaded && loaded->pages == 2 && rows.size() == 2 && rows[0].id == L"current" &&
              rows[1].id == L"tail",
          "The current snapshot after legacy rejection changed row order or page assembly");
}

void TestIncrementalSaveAndGenerationSwitch() {
    const auto identity = MakeIdentity();
    const auto baselineEntries = TestAdapter().store->CurrentUsage(glasswave::cache::Kind::Data).entries;
    glasswave::cache::lists::SavePage(
        identity,
        MakePageRecord({MakeTrack(L"1", L"First"), MakeTrack(L"2", L"Second")}, 1, true, L"cursor-1", 2, 3),
        5);
    auto loaded = LoadAndWait(identity);
    Check(loaded && Rows(*loaded).size() == 2 && loaded->pages == 1 && loaded->page.more &&
              loaded->page.next.value == L"cursor-1",
          "A contiguous partial prefix did not load with its trailing cursor");
    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"2", L"Second duplicate")}, 2, false, {}, 0, 3), 5);
    loaded = LoadAndWait(identity);
    Check(loaded.has_value(), "Incrementally saved snapshot did not load");
    auto rows = Rows(*loaded);
    Check(rows.size() == 3 && rows[0].id == L"1" && rows[1].id == L"2" && rows[2].id == L"2",
          "Loaded prefix did not preserve page order and duplicate identities");
    Check(loaded->pages == 2 && !loaded->page.more && loaded->page.total == 3,
          "Loaded prefix did not report its page and trailing headers");
    Check(TestAdapter().store->CurrentUsage(glasswave::cache::Kind::Data).entries == baselineEntries + 3,
          "Incremental pages did not use one chunk per page plus one manifest");

    const auto beforeRetry = TestAdapter().store->CurrentUsage(glasswave::cache::Kind::Data);
    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"2", L"Second duplicate")}, 2, false, {}, 0, 3), 5);
    loaded = LoadAndWait(identity);
    Check(loaded.has_value(), "An unchanged page retry made the snapshot unreadable");
    const auto afterRetry = TestAdapter().store->CurrentUsage(glasswave::cache::Kind::Data);
    Check(afterRetry.entries == beforeRetry.entries,
          "An unchanged page retry rewrote an immutable page chunk");

    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"new", L"New generation")}, 1, false, {}, 0, 1), 6);
    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"3", L"Stale tail")}, 2, false, {}, 0, 3), 5);
    loaded = LoadAndWait(identity);
    Check(loaded.has_value(), "New generation snapshot did not load");
    rows = Rows(*loaded);
    Check(rows.size() == 1 && rows[0].id == L"new",
          "An older generation was mixed into the current snapshot");
    Check(TestAdapter().store->CurrentUsage(glasswave::cache::Kind::Data).entries == baselineEntries + 2,
          "The superseded generation retained an unreachable page chunk");
}

// A later run (another generation) extends a partly read snapshot it loaded, page for page;
// without the loaded base, or with a base that was replaced, the page is refused.
void TestContinueLoadedSnapshot() {
    Identity identity{.provider = "continue-loaded", .logical = "tracks/v1/lang=en", .accountRevision = 0};
    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"1", L"First"), MakeTrack(L"2", L"Second")}, 1, true, L"cursor-1", 2, 4),
        10);
    auto loaded = LoadAndWait(identity);
    Check(loaded && loaded->pages == 1 && loaded->page.more && !loaded->base.empty(),
          "A partial snapshot did not load with its generation as base");
    const std::string base = loaded->base;

    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"3", L"Third")}, 2, true, L"cursor-2", 3, 4), 11);
    loaded = LoadAndWait(identity);
    Check(loaded && loaded->pages == 1, "A page of another generation was appended without the loaded base");

    auto stale = MakePageRecord({MakeTrack(L"3", L"Third")}, 2, true, L"cursor-2", 3, 4);
    stale.base = base + "-replaced";
    glasswave::cache::lists::SavePage(identity, std::move(stale), 11);
    loaded = LoadAndWait(identity);
    Check(loaded && loaded->pages == 1, "A page continuing a replaced snapshot was appended");

    auto second = MakePageRecord({MakeTrack(L"3", L"Third")}, 2, true, L"cursor-2", 3, 4, true);
    second.base = base;
    glasswave::cache::lists::SavePage(identity, std::move(second), 11);
    auto third = MakePageRecord({MakeTrack(L"4", L"Fourth")}, 3, false, {}, 0, 4);
    third.base = base;
    glasswave::cache::lists::SavePage(identity, std::move(third), 11);
    loaded = LoadAndWait(identity);
    Check(loaded && loaded->pages == 3 && !loaded->page.more && Rows(*loaded).size() == 4 &&
              Rows(*loaded)[2].id == L"3" && Rows(*loaded)[3].id == L"4" && loaded->base == base,
          "Pages continuing the loaded snapshot were not appended to it");
    Check(loaded->metadataReadyRows == 2, "An appended page lost its pending description");

    auto described = MakePageRecord({MakeTrack(L"3", L"Third described")}, 2, true, L"cursor-2", 3, 4);
    described.base = base;
    glasswave::cache::lists::UpdatePage(identity, std::move(described), 11);
    loaded = LoadAndWait(identity);
    Check(loaded && loaded->metadataReadyRows == 4 && Rows(*loaded)[2].title == L"Third described",
          "A description of an appended page was not written into the loaded snapshot");

    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"new", L"Fresh read")}, 1, false, {}, 0, 1), 12);
    loaded = LoadAndWait(identity);
    Check(loaded && loaded->pages == 1 && Rows(*loaded)[0].id == L"new" && loaded->base != base,
          "A fresh read did not replace the continued snapshot");
}

void TestDelayedPageUpdatePreservesLaterPagesAndContinuation() {
    Identity identity{.provider = "patch-continuation", .logical = "tracks/v1/lang=en", .accountRevision = 0};
    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"1", L"Base")}, 1, true, L"cursor-1", 20, 3, true), 7);
    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"2", L"Later")}, 2, true, L"cursor-2", 40, 3), 7);

    auto loaded = LoadAndWait(identity);
    Check(loaded && loaded->pages == 2 && loaded->page.enrichmentPending,
          "A pending earlier page was not preserved after a later page was saved");

    const auto beforeUpdate = TestAdapter().store->CurrentUsage(glasswave::cache::Kind::Data).entries;
    glasswave::cache::lists::UpdatePage(
        identity, MakePageRecord({MakeTrack(L"1", L"Enhanced")}, 1, true, L"cursor-1", 20, 3), 7);
    loaded = LoadAndWait(identity);
    auto rows = loaded ? Rows(*loaded) : std::vector<Track>{};
    Check(rows.size() == 2 && rows[0].id == L"1" && rows[0].title == L"Enhanced" && rows[1].id == L"2" &&
              rows[1].title == L"Later",
          "A delayed page update reordered or changed a later page");
    Check(loaded && loaded->pages == 2 && !loaded->page.enrichmentPending,
          "Clearing the last pending page did not update the aggregate marker");
    Check(loaded->page.next.value == L"cursor-2" && loaded->page.next.offset == 40 &&
              loaded->page.total == 3 && loaded->page.revision == L"revision-1",
          "A delayed page update changed aggregate page headers");
    Check(TestAdapter().store->CurrentUsage(glasswave::cache::Kind::Data).entries == beforeUpdate,
          "A page replacement retained an unreachable old variant");

    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"3", L"Tail")}, 3, false, {}, 0, 3, true), 7);
    loaded = LoadAndWait(identity);
    rows = loaded ? Rows(*loaded) : std::vector<Track>{};
    Check(rows.size() == 3 && rows[0].title == L"Enhanced" && rows[1].title == L"Later" && rows[2].id == L"3",
          "An ordinary page save could not continue after a page replacement");
    Check(loaded && loaded->pages == 3 && !loaded->page.more && loaded->page.enrichmentPending,
          "Continuation after replacement did not preserve the aggregate marker or completion");
}

void TestWarmSnapshotRetainsMetadataFrontier() {
    Identity identity{
        .provider = "metadata-frontier", .logical = "tracks/v1/lang=en", .accountRevision = 0};
    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"ready-1", L"Ready 1"), MakeTrack(L"ready-2", L"Ready 2")},
                                 1, true, L"cursor-1", 2, 4),
        3);
    glasswave::cache::lists::SavePage(
        identity,
        MakePageRecord({MakeTrack(L"pending-1", L"Pending 1"), MakeTrack(L"pending-2", L"Pending 2")}, 2,
                       false, {}, 0, 4, true),
        3);
    auto loaded = LoadAndWait(identity);
    Check(loaded && Rows(*loaded).size() == 4 && loaded->page.enrichmentPending &&
              loaded->metadataReadyRows == 2,
          "Warm load did not retain the enriched prefix before an unfinished page");

    glasswave::cache::lists::UpdatePage(
        identity,
        MakePageRecord({MakeTrack(L"pending-1", L"Enriched 1"), MakeTrack(L"pending-2", L"Enriched 2")}, 2,
                       false, {}, 0, 4, false),
        3);
    loaded = LoadAndWait(identity);
    Check(loaded && !loaded->page.enrichmentPending && loaded->metadataReadyRows == 4,
          "A completed warm snapshot did not expose all persisted metadata");

    const Identity unavailable{
        .provider = "metadata-frontier-pending",
        .logical = "tracks/v1/lang=en",
        .accountRevision = 0,
    };
    glasswave::cache::lists::SavePage(
        unavailable,
        MakePageRecord({MakeTrack(L"pending", L"Pending")}, 1, true, L"cursor-1", 1, 2, true), 1);
    glasswave::cache::lists::SavePage(
        unavailable, MakePageRecord({MakeTrack(L"ready", L"Ready")}, 2, false, {}, 0, 2), 1);
    loaded = LoadAndWait(unavailable);
    Check(loaded && Rows(*loaded).size() == 2 && loaded->metadataReadyRows == 0,
          "An unfinished first page incorrectly marked a later classification prefix ready");
}

void TestOverlappingUpdatesRemainCoherent() {
    auto manager = std::make_shared<glasswave::cache::lists::Manager>();
    Identity identity{.provider = "patch-overlap", .logical = "tracks/v1/lang=en", .accountRevision = 0};
    manager->SavePage(identity, MakePageRecord({MakeTrack(L"1", L"Base")}, 1, true, L"cursor-1", 20, 2, true),
                      3);
    manager->SavePage(identity, MakePageRecord({MakeTrack(L"2", L"Tail")}, 2, false, {}, 0, 2), 3);

    manager->UpdatePage(identity,
                        MakePageRecord({MakeTrack(L"1", L"Metadata A")}, 1, true, L"cursor-1", 20, 2), 3);
    auto first = StartLoadWithManager(*manager, identity);
    manager->UpdatePage(identity,
                        MakePageRecord({MakeTrack(L"1", L"Metadata B")}, 1, true, L"cursor-1", 20, 2), 3);
    auto second = StartLoadWithManager(*manager, identity);

    const auto firstSnapshot = WaitForLoad(std::move(first));
    const auto secondSnapshot = WaitForLoad(std::move(second));
    const auto firstRows = firstSnapshot ? Rows(*firstSnapshot) : std::vector<Track>{};
    const auto secondRows = secondSnapshot ? Rows(*secondSnapshot) : std::vector<Track>{};
    Check(firstSnapshot && firstSnapshot->pages == 2 && firstRows.size() == 2 &&
              firstRows[0].title == L"Metadata A" && firstRows[1].title == L"Tail",
          "The first overlapping update produced a mixed or partial snapshot");
    Check(secondSnapshot && secondSnapshot->pages == 2 && secondRows.size() == 2 &&
              secondRows[0].title == L"Metadata B" && secondRows[1].title == L"Tail",
          "Overlapping page updates did not publish in ticket order");
    manager->Shutdown();
}

void TestPageUpdateValidation() {
    Identity identity{.provider = "patch-validation", .logical = "tracks/v1/lang=en", .accountRevision = 0};
    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"stable", L"Stable")}, 1, true, L"cursor", 20, 2), 4);
    glasswave::cache::lists::SavePage(identity,
                                      MakePageRecord({MakeTrack(L"tail", L"Tail")}, 2, false, {}, 0, 2), 4);

    const auto unchanged = [&] {
        const auto loaded = LoadAndWait(identity);
        const auto rows = loaded ? Rows(*loaded) : std::vector<Track>{};
        Check(loaded && loaded->pages == 2 && rows.size() == 2 && rows[0].id == L"stable" &&
                  rows[0].title == L"Stable" && rows[1].id == L"tail",
              "An invalid page update changed the cached snapshot");
    };

    auto wrongIdentity = MakePageRecord({MakeTrack(L"other", L"Wrong")}, 1, true, L"cursor", 20, 2);
    glasswave::cache::lists::UpdatePage(identity, wrongIdentity, 4);
    unchanged();

    auto wrongCount = MakePageRecord({MakeTrack(L"stable", L"Stable"), MakeTrack(L"extra", L"Extra")}, 1,
                                     true, L"cursor", 20, 2);
    glasswave::cache::lists::UpdatePage(identity, wrongCount, 4);
    unchanged();

    Record wrongKind{.page = {}, .pages = 1, .validatedAt = 1'760'000'000};
    wrongKind.page.rows = std::vector<glasswave::stream::Album>{{.id = L"stable", .name = L"Wrong kind"}};
    wrongKind.page.next = {L"cursor", 20};
    wrongKind.page.more = true;
    wrongKind.page.total = 2;
    wrongKind.page.revision = L"revision-1";
    glasswave::cache::lists::UpdatePage(identity, wrongKind, 4);
    unchanged();

    auto wrongHeader = MakePageRecord({MakeTrack(L"stable", L"Changed")}, 1, true, L"other-cursor", 20, 2);
    glasswave::cache::lists::UpdatePage(identity, wrongHeader, 4);
    unchanged();

    auto wrongTotal = MakePageRecord({MakeTrack(L"stable", L"Changed")}, 1, true, L"cursor", 20, 3);
    glasswave::cache::lists::UpdatePage(identity, wrongTotal, 4);
    unchanged();

    auto pendingRegression =
        MakePageRecord({MakeTrack(L"stable", L"Changed")}, 1, true, L"cursor", 20, 2, true);
    glasswave::cache::lists::UpdatePage(identity, pendingRegression, 4);
    unchanged();

    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"current", L"Current generation")}, 1, false, {}, 0, 1), 5);
    glasswave::cache::lists::UpdatePage(
        identity, MakePageRecord({MakeTrack(L"stable", L"Stale update")}, 1, true, L"cursor", 20, 2), 4);
    const auto current = LoadAndWait(identity);
    const auto currentRows = current ? Rows(*current) : std::vector<Track>{};
    Check(current && currentRows.size() == 1 && currentRows[0].id == L"current",
          "An outdated generation updated the current snapshot");
}

void TestAccountInvalidationDropsPendingUpdate() {
    const std::string provider = "patch-account";
    const Identity old{.provider = provider, .logical = "tracks/v1/lang=en", .accountRevision = 0};
    glasswave::cache::lists::SavePage(
        old, MakePageRecord({MakeTrack(L"old", L"Old account")}, 1, false, {}, 0, 1, true), 2);
    glasswave::cache::lists::UpdatePage(
        old, MakePageRecord({MakeTrack(L"old", L"Late update")}, 1, false, {}, 0, 1), 2);
    glasswave::cache::RotateAccountScope(provider);
    const Identity current{.provider = provider,
                           .logical = old.logical,
                           .accountRevision = glasswave::cache::AccountRevision(provider)};
    Check(!LoadAndWait(current).has_value(), "A pending update published into a new account scope");
}

void TestOrdinaryLoadMissesComplete() {
    Identity identity{.provider = "miss", .logical = "tracks/v1/lang=en", .accountRevision = 0};
    auto promise = std::make_shared<std::promise<std::optional<Record>>>();
    auto future = promise->get_future();
    glasswave::cache::lists::Load(
        identity, [promise](std::optional<Record> value) { promise->set_value(std::move(value)); });
    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"saved", L"After load")}, 1, false, {}, 0, 1), 1);
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw TestFailure("An ordinary list-cache miss hung while a save was pending.");
    Check(!future.get().has_value(), "An ordinary list-cache miss did not return empty");
    Check(LoadAndWait(identity).has_value(), "The save after an ordinary miss did not complete");

    Identity corrupt{.provider = "corrupt", .logical = "tracks/v1/lang=en", .accountRevision = 0};
    const auto accountScope = TestAdapter().store->Scope(corrupt.provider);
    const auto guardScope = TestAdapter().store->Scope("lists:" + corrupt.provider);
    Check(glasswave::cache::lists::WriteSnapshot(
              *TestAdapter().store, glasswave::cache::lists::ManifestKey(corrupt, accountScope, guardScope),
              "{", glasswave::cache::lists::kManifestContentType, 1, "schema=3"),
          "Could not create the corrupt-manifest fixture");
    Check(!LoadAndWait(corrupt).has_value(), "A corrupt list-cache manifest did not return empty");
}

void TestInvalidationIsolation() {
    const auto identity = MakeIdentity();
    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"before", L"Before revoke")}, 1, false, {}, 0, 1), 10);
    Check(LoadAndWait(identity).has_value(), "Pre-invalidation snapshot did not load");

    const auto realScope = TestAdapter().store->Scope(identity.provider);
    const auto guardScope = TestAdapter().store->Scope("lists:" + identity.provider);
    glasswave::cache::lists::InvalidateProvider(identity.provider);
    Check(!LoadAndWait(identity).has_value(), "A revoked list snapshot remained visible");
    Check(TestAdapter().store->Scope(identity.provider) == realScope,
          "List invalidation rotated the shared provider account scope");
    Check(TestAdapter().store->Scope("lists:" + identity.provider) != guardScope,
          "List invalidation did not persist a provider-local guard token");

    glasswave::cache::lists::SavePage(
        identity, MakePageRecord({MakeTrack(L"after", L"After revoke")}, 1, false, {}, 0, 1), 1);
    const auto loaded = LoadAndWait(identity);
    Check(loaded && Rows(*loaded)[0].id == L"after",
          "A fresh snapshot could not be written after list-only invalidation");
}

void TestAccountRevisionDropsOldTask() {
    Identity old = MakeIdentity(0);
    glasswave::cache::lists::SavePage(
        old, MakePageRecord({MakeTrack(L"old", L"Old account")}, 1, false, {}, 0, 1), 1);
    glasswave::cache::RotateAccountScope(old.provider);
    const auto current = MakeIdentity(glasswave::cache::AccountRevision(old.provider));
    Check(glasswave::cache::AccountRevision(old.provider) > old.accountRevision,
          "The account revision did not advance");
    Check(!LoadAndWait(current).has_value(), "An old-account task published into the new scope");
}

void TestSessionEpochPreventsRestartCollision() {
    Identity identity{.provider = "restart", .logical = "tracks/v1/lang=en", .accountRevision = 0};
    auto first = std::make_shared<glasswave::cache::lists::Manager>();
    first->SavePage(identity, MakePageRecord({MakeTrack(L"old", L"First session")}, 1, false, {}, 0, 1), 9);
    Check(LoadWithManagerAndWait(*first, identity).has_value(),
          "The first list-cache session did not persist its snapshot");
    first->Shutdown();

    auto second = std::make_shared<glasswave::cache::lists::Manager>();
    second->SavePage(identity, MakePageRecord({MakeTrack(L"new", L"Second session")}, 1, false, {}, 0, 1), 9);
    const auto loaded = LoadWithManagerAndWait(*second, identity);
    second->Shutdown();
    Check(loaded && Rows(*loaded).size() == 1 && Rows(*loaded)[0].id == L"new",
          "A restarted session reused a colliding generation page key");
}

void TestSoftRetirementPreservesSnapshotAndRestartsGeneration() {
    const std::string provider = "soft-retire";
    const Identity identity{
        .provider = provider, .logical = "tracks/v1/lang=en/context=jp", .accountRevision = 0};
    auto manager = std::make_shared<glasswave::cache::lists::Manager>();
    manager->SavePage(identity, MakePageRecord({MakeTrack(L"cached", L"Cached")}, 1, false, {}, 0, 1), 10);
    auto loaded = LoadWithManagerAndWait(*manager, identity);
    Check(loaded && Rows(*loaded)[0].title == L"Cached",
          "The initial soft-retirement snapshot was not persisted");

    const auto accountBefore = TestAdapter().store->Scope(provider);
    const auto guardBefore = TestAdapter().store->Scope("lists:" + provider);
    manager->RetireProvider(provider);
    Check(TestAdapter().store->Scope(provider) == accountBefore,
          "Soft retirement rotated the shared account scope");
    Check(TestAdapter().store->Scope("lists:" + provider) == guardBefore,
          "Soft retirement rotated the provider-local list guard");
    loaded = LoadWithManagerAndWait(*manager, identity);
    Check(loaded && Rows(*loaded)[0].title == L"Cached",
          "Soft retirement made the last-good persisted snapshot unreadable");

    manager->SavePage(identity, MakePageRecord({MakeTrack(L"fresh", L"Fresh")}, 1, false, {}, 0, 1), 1);
    loaded = LoadWithManagerAndWait(*manager, identity);
    Check(loaded && Rows(*loaded)[0].id == L"fresh",
          "Soft retirement did not reset generation ownership for a restarted reader");
    manager->Shutdown();
}

void TestSoftRetirementRejectsOldWriter() {
    const std::string provider = "soft-race";
    const Identity identity{
        .provider = provider, .logical = "tracks/v1/lang=en/context=jp", .accountRevision = 0};
    auto manager = std::make_shared<glasswave::cache::lists::Manager>();
    manager->SavePage(identity, MakePageRecord({MakeTrack(L"base", L"Base")}, 1, false, {}, 0, 1), 10);
    Check(LoadWithManagerAndWait(*manager, identity).has_value(),
          "The soft-retirement race baseline was not persisted");

    manager->SavePage(identity, MakePageRecord({MakeTrack(L"stale", L"Stale")}, 1, false, {}, 0, 1), 11);
    manager->RetireProvider(provider);
    manager->SavePage(identity, MakePageRecord({MakeTrack(L"current", L"Current")}, 1, false, {}, 0, 1), 1);

    const auto loaded = LoadWithManagerAndWait(*manager, identity);
    const auto rows = loaded ? Rows(*loaded) : std::vector<Track>{};
    Check(rows.size() == 1 && rows[0].id == L"current",
          "A writer from the retired publisher epoch replaced the newer publication");
    manager->Shutdown();
}

void TestSoftRetirementLinearizesManifestPublication() {
    const std::string provider = "soft-commit-race";
    const Identity identity{
        .provider = provider, .logical = "tracks/v1/lang=en/context=jp", .accountRevision = 0};
    auto manager = std::make_shared<glasswave::cache::lists::Manager>();
    manager->SavePage(identity, MakePageRecord({MakeTrack(L"base", L"Base")}, 1, false, {}, 0, 1), 10);
    Check(LoadWithManagerAndWait(*manager, identity).has_value(),
          "The manifest-publication race baseline was not persisted");

    struct HookReset {
        ~HookReset() {
            glasswave::cache::lists::detail::SetManifestPauseForTest({});
            glasswave::cache::lists::detail::SetManifestCommitPauseForTest({});
            glasswave::cache::lists::detail::SetManifestCompleteForTest({});
        }
    } reset;
    auto oldEntered = std::make_shared<std::promise<void>>();
    auto oldRelease = std::make_shared<std::promise<void>>();
    auto oldComplete = std::make_shared<std::promise<void>>();
    auto newEntered = std::make_shared<std::promise<void>>();
    auto newRelease = std::make_shared<std::promise<void>>();
    auto newComplete = std::make_shared<std::promise<void>>();
    auto oldEnteredFuture = oldEntered->get_future();
    auto oldCompleteFuture = oldComplete->get_future();
    auto newEnteredFuture = newEntered->get_future();
    auto newCompleteFuture = newComplete->get_future();
    struct ReleaseOnExit {
        std::shared_ptr<std::promise<void>> release;
        bool pending = true;
        ~ReleaseOnExit() {
            if (!pending) return;
            try {
                release->set_value();
            } catch (...) {
            }
        }
    } oldReleaseOnExit{oldRelease}, newReleaseOnExit{newRelease};
    glasswave::cache::lists::detail::SetManifestPauseForTest([oldEntered, oldRelease] {
        oldEntered->set_value();
        oldRelease->get_future().wait();
    });

    manager->SavePage(identity,
                      MakePageRecord({MakeTrack(L"stale", L"Stale late writer")}, 1, false, {}, 0, 1), 11);
    if (oldEnteredFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw TestFailure("The stale writer did not reach the pre-publication pause.");

    glasswave::cache::lists::detail::SetManifestCommitPauseForTest([newEntered, newRelease] {
        newEntered->set_value();
        newRelease->get_future().wait();
    });
    manager->RetireProvider(provider);
    manager->SavePage(identity,
                      MakePageRecord({MakeTrack(L"current", L"Current publication")}, 1, false, {}, 0, 1), 1);
    if (newEnteredFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw TestFailure("The current writer did not reach the pre-commit pause.");

    glasswave::cache::lists::detail::SetManifestCompleteForTest([oldComplete] { oldComplete->set_value(); });
    oldRelease->set_value();
    oldReleaseOnExit.pending = false;
    if (oldCompleteFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw TestFailure("The retired writer did not finish after the publication check.");

    glasswave::cache::lists::detail::SetManifestCompleteForTest([newComplete] { newComplete->set_value(); });
    newRelease->set_value();
    newReleaseOnExit.pending = false;
    if (newCompleteFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw TestFailure("The current writer did not finish after the retired writer.");

    auto loaded = LoadWithManagerAndWait(*manager, identity);
    const auto rows = loaded ? Rows(*loaded) : std::vector<Track>{};
    Check(rows.size() == 1 && rows[0].id == L"current",
          "The retired writer invalidated the current writer's manifest ticket");
    manager->Shutdown();
}

void TestSoftRetirementDoesNotCancelPendingHardRotation() {
    const std::string provider = "hard-then-soft";
    const Identity identity{
        .provider = provider, .logical = "tracks/v1/lang=en/context=jp", .accountRevision = 0};
    auto manager = std::make_shared<glasswave::cache::lists::Manager>();
    manager->SavePage(identity, MakePageRecord({MakeTrack(L"old", L"Old account data")}, 1, false, {}, 0, 1),
                      4);
    Check(LoadWithManagerAndWait(*manager, identity).has_value(),
          "The hard-rotation fixture was not persisted");

    const auto guardBefore = TestAdapter().store->Scope("lists:" + provider);
    manager->InvalidateProvider(provider);
    manager->RetireProvider(provider);
    manager->Shutdown();
    Check(TestAdapter().store->Scope("lists:" + provider) != guardBefore,
          "Soft retirement cancelled a pending hard list-guard rotation");

    auto restarted = std::make_shared<glasswave::cache::lists::Manager>();
    Check(!LoadWithManagerAndWait(*restarted, identity).has_value(),
          "A hard-retired snapshot became visible again after soft retirement");
    restarted->Shutdown();
}

void TestContextRevisitReusesOnlyItsSnapshot() {
    const std::string provider = "context-revisit";
    const Identity japan{
        .provider = provider, .logical = "tracks/v1/lang=en/context=jp", .accountRevision = 0};
    const Identity china{
        .provider = provider, .logical = "tracks/v1/lang=en/context=cn", .accountRevision = 0};
    auto manager = std::make_shared<glasswave::cache::lists::Manager>();
    manager->SavePage(japan, MakePageRecord({MakeTrack(L"jp", L"Japan")}, 1, false, {}, 0, 1), 3);
    manager->SavePage(china, MakePageRecord({MakeTrack(L"cn", L"China")}, 1, false, {}, 0, 1), 3);
    const auto firstJapan = LoadWithManagerAndWait(*manager, japan);
    const auto firstChina = LoadWithManagerAndWait(*manager, china);
    Check(firstJapan && Rows(*firstJapan)[0].id == L"jp" && firstChina && Rows(*firstChina)[0].id == L"cn",
          "Context-scoped snapshots were mixed before retirement");

    manager->RetireProvider(provider);
    const auto revisitJapan = LoadWithManagerAndWait(*manager, japan);
    const auto revisitChina = LoadWithManagerAndWait(*manager, china);
    const auto japanAgain = LoadWithManagerAndWait(*manager, japan);
    Check(revisitJapan && Rows(*revisitJapan)[0].id == L"jp" && revisitChina &&
              Rows(*revisitChina)[0].id == L"cn" && japanAgain && Rows(*japanAgain)[0].id == L"jp",
          "JP -> CN -> JP context revisits crossed cache prefixes");
    manager->Shutdown();
}

void TestShutdownDrainsGuardRotation() {
    const std::string provider = "shutdown";
    const auto sharedScope = TestAdapter().store->Scope(provider);
    const auto guardScope = TestAdapter().store->Scope("lists:" + provider);
    auto manager = std::make_shared<glasswave::cache::lists::Manager>();
    manager->InvalidateProvider(provider);
    manager->Shutdown();
    Check(TestAdapter().store->Scope(provider) == sharedScope,
          "Shutdown guard persistence rotated the shared provider scope");
    Check(TestAdapter().store->Scope("lists:" + provider) != guardScope,
          "Shutdown did not persist a queued list-only guard rotation");
}

void TestGenerationStateBound() {
    auto manager = std::make_shared<glasswave::cache::lists::Manager>();
    manager->FillGenerationStatesForTest(3'000);
    Check(manager->GenerationStateCountForTest() == glasswave::cache::lists::kMaxGenerationStates,
          "Per-query generation state did not enforce its LRU bound");
    manager->Shutdown();
}

} // namespace

namespace glasswave::cache {

Store& GetStore() {
    Store* store = TestAdapter().store;
    if (!store) throw std::runtime_error("The list-cache test store is unavailable.");
    return *store;
}

std::string AccountScope(const std::string& provider) {
    auto& adapter = TestAdapter();
    std::lock_guard lock(adapter.mutex);
    return adapter.store->Scope(provider);
}

void RotateAccountScope(const std::string& provider) {
    auto& adapter = TestAdapter();
    std::lock_guard lock(adapter.mutex);
    adapter.store->RotateScope(provider);
    ++adapter.revisions[provider];
}

uint64_t AccountRevision(const std::string& provider) {
    auto& adapter = TestAdapter();
    std::lock_guard lock(adapter.mutex);
    return adapter.revisions[provider];
}

} // namespace glasswave::cache

int wmain(int argc, wchar_t** argv) {
    fs::path root;
    try {
        if (argc != 2) throw TestFailure("Expected one test root argument.");
        root = ValidateRoot(fs::path(argv[1]));
        if (fs::exists(root)) throw TestFailure("The test root already exists.");
        fs::create_directories(root);
        auto store = std::make_unique<Store>(root / L"cache");
        TestAdapter().store = store.get();

        TestSerializerRoundTrip();
        TestCatalogSearchMixedRowsRoundTrip();
        TestArtistSectionQueriesStayIndependent();
        TestMalformedInputs();
        TestManifestRoundTrip();
        TestSnapshotSurvivesStoreReopen(store);
        TestCurrentFormatOnlyRejectsLegacyManifest();
        TestIncrementalSaveAndGenerationSwitch();
        TestContinueLoadedSnapshot();
        TestDelayedPageUpdatePreservesLaterPagesAndContinuation();
        TestWarmSnapshotRetainsMetadataFrontier();
        TestOverlappingUpdatesRemainCoherent();
        TestPageUpdateValidation();
        TestAccountInvalidationDropsPendingUpdate();
        TestOrdinaryLoadMissesComplete();
        TestInvalidationIsolation();
        TestAccountRevisionDropsOldTask();
        TestSessionEpochPreventsRestartCollision();
        TestSoftRetirementPreservesSnapshotAndRestartsGeneration();
        TestSoftRetirementRejectsOldWriter();
        TestSoftRetirementLinearizesManifestPublication();
        TestSoftRetirementDoesNotCancelPendingHardRotation();
        TestContextRevisitReusesOnlyItsSnapshot();
        TestShutdownDrainsGuardRotation();
        TestGenerationStateBound();

        glasswave::cache::lists::Shutdown();
        const auto lateCallback = std::make_shared<std::atomic_bool>(false);
        glasswave::cache::lists::Load(MakeIdentity(glasswave::cache::AccountRevision("test")),
                                      [lateCallback](std::optional<Record>) { lateCallback->store(true); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Check(!lateCallback->load(), "A load callback was delivered after shutdown");
        TestAdapter().store = nullptr;
        store.reset();
        SafeRemove(root);
        std::cout << "list cache tests passed: " << checkCount << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        glasswave::cache::lists::Shutdown();
        TestAdapter().store = nullptr;
        try {
            SafeRemove(root);
        } catch (...) {
        }
        std::cerr << "list cache tests failed: " << error.what() << "\n";
        return 1;
    }
}
