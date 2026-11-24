#include "pch.h"
#include "applemusic/lyrics_source.h"
#include "core/text.h"

#include "applemusic/client.h"
#include "applemusic/config.h"
#include "applemusic/display_metadata.h"
#include "applemusic/lyrics_protocol.h"
#include "cache/store.h"
#include "lyrics/lyrics_service.h"
#include "ui/i18n.h"

#include <algorithm>
#include <climits>
#include <cwctype>
#include <memory>
#include <vector>

namespace glasswave::applemusic::lyrics_source {
namespace {

constexpr wchar_t kUniversalKey[] = L"applemusic";

using core::Utf8;
using core::Wide;

bool SameLocaleIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

} // namespace

std::wstring EffectiveLyricsLocale() {
    std::vector<std::wstring> locales;
    auto pushUnique = [&](std::wstring tag) {
        if (tag.empty() || locales.size() >= 4) return;
        for (const auto& existing : locales) {
            if (SameLocaleIgnoreCase(existing, tag)) return;
        }
        locales.push_back(std::move(tag));
    };
    if (RegionalLyricsActive()) {
        if (const auto target = DisplayTargetFor(GetAppleMusicDisplayRegion())) {
            pushUnique(target->locale);
        }
    }
    for (auto& tag : ui::PreferredLocales()) pushUnique(std::move(tag));
    if (locales.empty()) locales.push_back(L"en-US");
    std::wstring joined;
    for (size_t i = 0; i < locales.size(); ++i) {
        if (i > 0) joined.push_back(L',');
        joined += locales[i];
    }
    return joined;
}

namespace {

struct CacheScope {
    std::wstring value;
    uint64_t revision = 0;

    bool Current(abort_callback& abort) const {
        if (abort.is_aborting()) return false;
        try {
            return cache::AccountRevision("apple") == revision && cache::AccountScope("apple") == Utf8(value);
        } catch (...) {
            return false;
        }
    }
};

class AppleMusicSearcher final : public lyrics::Searcher {
  public:
    std::wstring Name() const override { return L"Apple Music"; }
    std::wstring Key() const override { return kUniversalKey; }

    void Search(const lyrics::Query& query, abort_callback& abort,
                const std::function<void(lyrics::Candidate)>& found) override {
        if (!found || abort.is_aborting()) return;

        lyrics_protocol::Request request;
        request.title = query.title;
        request.artist = query.artist;
        request.album = query.album;
        request.locale = EffectiveLyricsLocale();
        request.durationMs =
            static_cast<int>(std::clamp(query.duration * 1000.0, 0.0, static_cast<double>(INT_MAX)));
        request.mode = lyrics_protocol::Mode::Search;
        request.trackType = L"songs";

        CacheScope scope;
        try {
            scope.value = Wide(cache::AccountScope("apple"));
            scope.revision = cache::AccountRevision("apple");
        } catch (const std::exception& error) {
            console::printf("Glasswave lyrics [applemusic]: %s", error.what());
            return;
        }
        if (scope.value.empty() || !scope.Current(abort)) return;

        core::json::Object response;
        try {
            response = Request(L"/api/v1/glasswave/lyrics", abort, L"POST",
                               lyrics_protocol::BuildRequestBody(request));
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& error) {
            console::printf("Glasswave lyrics [applemusic]: %s", error.what());
            return;
        }
        abort.check();
        if (!scope.Current(abort)) return;

        for (auto& result : lyrics_protocol::ParseResponse(response)) {
            abort.check();
            if (!scope.Current(abort)) return;
            lyrics::Candidate candidate;
            candidate.title = std::move(result.title);
            candidate.artist = std::move(result.artist);
            candidate.album = std::move(result.album);
            candidate.source = Name();
            candidate.lrc = std::move(result.enhancedLrc);
            candidate.confidence = result.confidence;
            candidate.cacheProvider = L"apple";
            candidate.cacheScope = scope.value;
            candidate.accountRevision = scope.revision;
            candidate.sourceKey = Key();
            found(std::move(candidate));
        }
    }
};

} // namespace

std::shared_ptr<lyrics::Searcher> MakeSearcher() {
    return std::make_shared<AppleMusicSearcher>();
}

} // namespace glasswave::applemusic::lyrics_source
