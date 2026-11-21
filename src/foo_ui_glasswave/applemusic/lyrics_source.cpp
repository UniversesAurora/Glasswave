#include "pch.h"
#include "applemusic/lyrics_source.h"

#include "applemusic/client.h"
#include "applemusic/lyrics_protocol.h"
#include "cache/store.h"
#include "lyrics/lyrics_service.h"
#include "ui/i18n.h"

#include <algorithm>
#include <climits>
#include <memory>

namespace glasswave::applemusic::lyrics_source {
namespace {

constexpr wchar_t kUniversalKey[] = L"applemusic";

std::string Utf8(std::wstring_view value) {
    return value.empty() ? std::string()
                         : pfc::stringcvt::string_utf8_from_wide(std::wstring(value).c_str()).get_ptr();
}

std::wstring Wide(std::string_view value) {
    return value.empty() ? std::wstring()
                         : pfc::stringcvt::string_wide_from_utf8(std::string(value).c_str()).get_ptr();
}

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
        request.locale = ui::UsingChinese() ? L"zh-Hans-CN" : L"en-US";
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

std::shared_ptr<lyrics::Searcher> g_universal;

} // namespace

void Start() {
    if (!g_universal) {
        g_universal = std::make_shared<AppleMusicSearcher>();
        lyrics::RegisterSearcher(g_universal);
    }
}

void Shutdown() {
    if (g_universal) lyrics::UnregisterSearcher(g_universal.get());
    g_universal.reset();
}

} // namespace glasswave::applemusic::lyrics_source
