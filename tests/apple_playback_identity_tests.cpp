#include "applemusic/playback_identity.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace glasswave::applemusic;

int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
}

void TestCatalogSongIdValidation() {
    Expect(IsCatalogSongId(L"1237064886"), "numeric catalog id accepted");
    Expect(IsCatalogSongId(L"1865440142"), "numeric reporting id accepted");
    Expect(!IsCatalogSongId(L""), "empty id rejected");
    Expect(!IsCatalogSongId(L"i.GlasswaveStaleFixture"), "library song id rejected");
    Expect(!IsCatalogSongId(L"l.album123"), "library album id rejected");
    Expect(!IsCatalogSongId(L"glasswave-test"), "non-numeric fixture id rejected");
    Expect(!IsCatalogSongId(L".."), "dot traversal rejected");
    Expect(!IsCatalogSongId(std::wstring(257, L'1')), "overlong id rejected");
}

void TestPreferredCatalogAssetId() {
    Expect(PreferredCatalogAssetId(L"1237064886", L"1237064886", L"1865440142") == L"1865440142",
           "prefers valid reportingId over catalogId");
    Expect(PreferredCatalogAssetId(L"glasswave-test", L"1237064886", L"") == L"1237064886",
           "falls back to catalogId when reportingId is empty");
    Expect(PreferredCatalogAssetId(L"1237064886", L"1237064886", L"i.badReporting") == L"1237064886",
           "ignores non-catalog reportingId");
    Expect(PreferredCatalogAssetId(L"i.1YBNNKGTqJaz4dR", L"", L"") == L"i.1YBNNKGTqJaz4dR",
           "falls back to library trackId for uploaded songs");
}

LibrarySongIdentity BaseLibraryItem() {
    LibrarySongIdentity item;
    item.id = L"i.LibrarySong1";
    item.type = L"library-songs";
    item.playParamsPresent = true;
    item.isLibrary = true;
    item.reporting = true;
    item.playParamsId = L"i.LibrarySong1";
    item.playParamsKind = L"song";
    item.playParamsCatalogId = L"1237064886";
    item.playParamsReportingId = L"1865440142";
    return item;
}

void TestSelectLibraryReplacementId() {
    // First-time remap.
    {
        const auto item = BaseLibraryItem();
        const auto resolved = SelectLibraryReplacementId(item, L"i.LibrarySong1", {L"1237064886"});
        Expect(resolved.has_value() && *resolved == L"1865440142",
               "first-time library reportingId remap succeeds");
    }
    // Second future remap: catalogId is still 1237064886, old reportingId 1865440142 now 404s,
    // Apple now reports 2999999999.
    {
        auto item = BaseLibraryItem();
        item.playParamsReportingId = L"2999999999";
        const auto resolved =
            SelectLibraryReplacementId(item, L"i.LibrarySong1", {L"1237064886", L"1865440142"});
        Expect(resolved.has_value() && *resolved == L"2999999999",
               "subsequent reportingId remap succeeds even with unchanged original catalogId");
    }
    // Embedded catalog relationship fallback when reportingId is absent.
    {
        auto item = BaseLibraryItem();
        item.reporting = false;
        item.playParamsReportingId.clear();
        item.embeddedCatalogId = L"2999999999";
        const auto resolved = SelectLibraryReplacementId(item, L"i.LibrarySong1", {L"1237064886"});
        Expect(resolved.has_value() && *resolved == L"2999999999",
               "relationships.catalog id used when playParams.reportingId is unavailable");
    }
    // Apple directly updated playParams.catalogId to the new catalog id.
    {
        auto item = BaseLibraryItem();
        item.reporting = false;
        item.playParamsReportingId.clear();
        item.playParamsCatalogId = L"2999999999";
        const auto resolved = SelectLibraryReplacementId(item, L"i.LibrarySong1", {L"1237064886"});
        Expect(resolved.has_value() && *resolved == L"2999999999",
               "updated playParams.catalogId accepted when different from failed id");
    }
    // Strict negative cases.
    {
        auto item = BaseLibraryItem();
        item.reporting = false;
        const auto resolved = SelectLibraryReplacementId(item, L"i.LibrarySong1", {L"1237064886"});
        Expect(!resolved.has_value(),
               "reporting=false with stale catalogId and no embedded catalog rejects reportingId");
    }
    {
        auto item = BaseLibraryItem();
        item.playParamsReportingId = L"i.NotACatalogId";
        const auto resolved = SelectLibraryReplacementId(item, L"i.LibrarySong1", {L"1237064886"});
        Expect(!resolved.has_value(), "library id inside reportingId is rejected");
    }
    {
        auto item = BaseLibraryItem();
        const auto resolved = SelectLibraryReplacementId(item, L"i.WrongLibraryId", {L"1237064886"});
        Expect(!resolved.has_value(), "mismatched library id is rejected");
    }
    {
        auto item = BaseLibraryItem();
        item.playParamsKind = L"musicVideo";
        const auto resolved = SelectLibraryReplacementId(item, L"i.LibrarySong1", {L"1237064886"});
        Expect(!resolved.has_value(), "non-song playParams.kind is rejected");
    }
}

void TestRankCatalogCandidates() {
    MatchHints hints;
    hints.title = L"Colors of Happiness";
    hints.album = L"Reset";
    hints.contentRating = L"clean";
    hints.durationMs = 258333;

    std::vector<CatalogSongCandidate> candidates = {
        {L"1237064886", L"", true, L"Colors of Happiness", L"Reset", L"clean", 258333}, // excluded
        {L"1000000001", L"", false, L"Colors of Happiness", L"Reset", L"clean", 258333}, // unplayable
        {L"1000000002", L"", true, L"Colors of Happiness", L"Best Of", L"explicit", 258000},
        {L"1865440142", L"", true, L"Colors of Happiness", L"Reset", L"clean", 258300},
        {L"1000000003", L"1865440142", true, L"Colors of Happiness", L"Reset", L"clean", 258300}, // duplicate effective
    };

    const auto ranked = RankCatalogCandidates(candidates, hints, {L"1237064886"});
    Expect(ranked.size() == 3, "excluded and duplicate effective ids filtered");
    Expect(ranked[0] == L"1865440142", "playable matching album/rating/duration ranks first");
    Expect(ranked[1] == L"1000000002", "other playable candidate ranks ahead of unplayable");
    Expect(ranked[2] == L"1000000001", "unplayable candidate ranks last");
}

void TestRecoverAfterNotFound() {
    // Case 1: Tier 1 library mapping succeeds (no catalog discovery calls made).
    {
        SongIdentityHint hint;
        hint.trackId = L"1237064886";
        hint.catalogId = L"1237064886";
        hint.libraryId = L"i.LibrarySong1";
        hint.isrc = L"JP1234567890";

        int eqCalls = 0;
        int isrcCalls = 0;
        RecoveryCallbacks cb;
        cb.tryOpen = [](const std::wstring& id) { return id == L"1865440142"; };
        cb.fromLibrary = [](std::wstring_view, const std::set<std::wstring>&) -> std::optional<std::wstring> {
            return L"1865440142";
        };
        cb.fromEquivalents = [&](std::wstring_view, const std::set<std::wstring>&) {
            ++eqCalls;
            return std::vector<std::wstring>{};
        };
        cb.fromIsrc = [&](std::wstring_view, const std::set<std::wstring>&) {
            ++isrcCalls;
            return std::vector<std::wstring>{};
        };

        const auto resolved = RecoverAfterNotFound(hint, L"1237064886", true, cb);
        Expect(resolved.has_value() && *resolved == L"1865440142", "tier 1 recovers replacement id");
        Expect(eqCalls == 0 && isrcCalls == 0, "tier 1 short-circuits without calling tier 2/3");
    }

    // Case 2: Subsequent ID change (cached reportingId 1865440142 -> 2999999999 via library).
    {
        SongIdentityHint hint;
        hint.trackId = L"1237064886";
        hint.catalogId = L"1237064886";
        hint.libraryId = L"i.LibrarySong1";
        hint.reportingId = L"1865440142";

        RecoveryCallbacks cb;
        cb.tryOpen = [](const std::wstring& id) { return id == L"2999999999"; };
        cb.fromLibrary = [](std::wstring_view, const std::set<std::wstring>& exclude) -> std::optional<std::wstring> {
            Expect(exclude.contains(L"1865440142"), "failed cached reportingId is passed in exclude");
            return L"2999999999";
        };

        const auto resolved = RecoverAfterNotFound(hint, L"1865440142", true, cb);
        Expect(resolved.has_value() && *resolved == L"2999999999",
               "second ID change recovers via tier 1");
    }

    // Case 3: Missing libraryId -> Tier 2 filter[equivalents] recovers immediately!
    {
        SongIdentityHint hint;
        hint.trackId = L"1237064886";
        hint.catalogId = L"1237064886";

        int libCalls = 0;
        RecoveryCallbacks cb;
        cb.tryOpen = [](const std::wstring& id) { return id == L"1865440142"; };
        cb.fromLibrary = [&](std::wstring_view, const std::set<std::wstring>&) -> std::optional<std::wstring> {
            ++libCalls;
            return std::nullopt;
        };
        cb.fromEquivalents = [](std::wstring_view seed, const std::set<std::wstring>&) {
            return seed == L"1237064886" ? std::vector<std::wstring>{L"1865440142"}
                                         : std::vector<std::wstring>{};
        };

        const auto resolved = RecoverAfterNotFound(hint, L"1237064886", true, cb);
        Expect(libCalls == 0, "tier 1 skipped when libraryId is empty");
        Expect(resolved.has_value() && *resolved == L"1865440142",
               "tier 2 equivalents recovers without libraryId");
    }

    // Case 4: Tier 1 & 2 miss -> Tier 3 ISRC recovers.
    {
        SongIdentityHint hint;
        hint.trackId = L"1237064886";
        hint.catalogId = L"1237064886";
        hint.isrc = L"JP1234567890";

        RecoveryCallbacks cb;
        cb.tryOpen = [](const std::wstring& id) { return id == L"3333333333"; };
        cb.fromEquivalents = [](std::wstring_view, const std::set<std::wstring>&) {
            return std::vector<std::wstring>{};
        };
        cb.fromIsrc = [](std::wstring_view isrc, const std::set<std::wstring>&) {
            return isrc == L"JP1234567890" ? std::vector<std::wstring>{L"3333333333"}
                                           : std::vector<std::wstring>{};
        };

        const auto resolved = RecoverAfterNotFound(hint, L"1237064886", true, cb);
        Expect(resolved.has_value() && *resolved == L"3333333333", "tier 3 ISRC recovers");
    }

    // Case 5: Stale reportingId falls back to original catalogId in Tier 4.
    {
        SongIdentityHint hint;
        hint.trackId = L"1237064886";
        hint.catalogId = L"1237064886";
        hint.reportingId = L"1865440142";

        RecoveryCallbacks cb;
        cb.tryOpen = [](const std::wstring& id) { return id == L"1237064886"; };
        cb.fromEquivalents = [](std::wstring_view, const std::set<std::wstring>&) {
            return std::vector<std::wstring>{};
        };

        const auto resolved = RecoverAfterNotFound(hint, L"1865440142", true, cb);
        Expect(resolved.has_value() && *resolved == L"1237064886",
               "tier 4 falls back to original catalogId");
    }

    // Case 6: Fixture mode disables catalog discovery (Tier 2 & 3 never called).
    {
        SongIdentityHint hint;
        hint.trackId = L"glasswave-test";
        hint.catalogId = L"1237064886";
        hint.libraryId = L"i.GlasswaveStaleFixture";
        hint.isrc = L"JP1234567890";

        int eqCalls = 0;
        int isrcCalls = 0;
        RecoveryCallbacks cb;
        cb.tryOpen = [](const std::wstring&) { return false; };
        cb.fromLibrary = [](std::wstring_view, const std::set<std::wstring>&) -> std::optional<std::wstring> {
            return std::nullopt;
        };
        cb.fromEquivalents = [&](std::wstring_view, const std::set<std::wstring>&) {
            ++eqCalls;
            return std::vector<std::wstring>{L"1865440142"};
        };
        cb.fromIsrc = [&](std::wstring_view, const std::set<std::wstring>&) {
            ++isrcCalls;
            return std::vector<std::wstring>{L"1865440142"};
        };

        const auto resolved = RecoverAfterNotFound(hint, L"1237064886", false, cb);
        Expect(!resolved.has_value(), "fixture mode returns nullopt when library mapping fails");
        Expect(eqCalls == 0 && isrcCalls == 0, "fixture mode never invokes catalog discovery");
    }

    // Case 7: Cycle prevention - overlapping dead ids are never retried.
    {
        SongIdentityHint hint;
        hint.trackId = L"1237064886";
        hint.catalogId = L"1237064886";
        hint.libraryId = L"i.LibrarySong1";
        hint.isrc = L"JP1234567890";

        std::vector<std::wstring> attempts;
        RecoveryCallbacks cb;
        cb.tryOpen = [&](const std::wstring& id) {
            attempts.push_back(id);
            return false;
        };
        cb.fromLibrary = [](std::wstring_view, const std::set<std::wstring>&) -> std::optional<std::wstring> {
            return L"1865440142";
        };
        cb.fromEquivalents = [](std::wstring_view, const std::set<std::wstring>&) {
            return std::vector<std::wstring>{L"1237064886", L"1865440142", L"4444444444"};
        };
        cb.fromIsrc = [](std::wstring_view, const std::set<std::wstring>&) {
            return std::vector<std::wstring>{L"4444444444", L"5555555555"};
        };

        const auto resolved = RecoverAfterNotFound(hint, L"1237064886", true, cb);
        Expect(!resolved.has_value(), "exhausted cascade returns nullopt");
        Expect(attempts.size() == 3 && attempts[0] == L"1865440142" && attempts[1] == L"4444444444" &&
                   attempts[2] == L"5555555555",
               "each distinct candidate id is attempted at most once");
    }

    // Case 8: Non-404 exception from tryOpen propagates immediately.
    {
        SongIdentityHint hint;
        hint.trackId = L"1237064886";
        hint.catalogId = L"1237064886";

        RecoveryCallbacks cb;
        cb.tryOpen = [](const std::wstring&) -> bool { throw std::runtime_error("HTTP 500"); };
        cb.fromEquivalents = [](std::wstring_view, const std::set<std::wstring>&) {
            return std::vector<std::wstring>{L"1865440142"};
        };

        bool caught = false;
        try {
            (void)RecoverAfterNotFound(hint, L"1237064886", true, cb);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        Expect(caught, "non-404 errors propagate out of recovery cascade");
    }
}

} // namespace

int main() {
    TestCatalogSongIdValidation();
    TestPreferredCatalogAssetId();
    TestSelectLibraryReplacementId();
    TestRankCatalogCandidates();
    TestRecoverAfterNotFound();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All Apple Music playback identity checks passed.\n";
    return 0;
}
