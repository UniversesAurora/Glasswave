#include "pch.h"
#include "applemusic/lyrics_protocol.h"
#include "core/text.h"

#include <algorithm>

namespace glasswave::applemusic::lyrics_protocol {
namespace {

constexpr size_t kMaxResults = 32;
constexpr size_t kMaxEnhancedLrcBytes = 4ull * 1024 * 1024;

using core::Utf8;

} // namespace

std::string BuildRequestBody(const Request& request) {
    core::json::Object object;
    object.Insert(L"mode", winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                               request.mode == Mode::Direct ? L"direct" : L"search"));
    object.Insert(L"trackId", winrt::Windows::Data::Json::JsonValue::CreateStringValue(request.trackId));
    object.Insert(L"trackType", winrt::Windows::Data::Json::JsonValue::CreateStringValue(request.trackType));
    object.Insert(L"title", winrt::Windows::Data::Json::JsonValue::CreateStringValue(request.title));
    object.Insert(L"artist", winrt::Windows::Data::Json::JsonValue::CreateStringValue(request.artist));
    object.Insert(L"album", winrt::Windows::Data::Json::JsonValue::CreateStringValue(request.album));
    object.Insert(L"locale", winrt::Windows::Data::Json::JsonValue::CreateStringValue(request.locale));
    object.Insert(L"durationMs",
                  winrt::Windows::Data::Json::JsonValue::CreateNumberValue(std::max(request.durationMs, 0)));
    return Utf8(core::json::Stringify(object));
}

std::vector<Result> ParseResponse(const core::json::Object& response) {
    const auto array = core::json::GetArray(response, L"results");
    if (!array) return {};
    auto objects = core::json::Objects(*array);
    // The bridge is asked for a handful; more than kMaxResults are cut rather than failing the
    // whole search.
    if (objects.size() > kMaxResults) objects.erase(objects.begin() + kMaxResults, objects.end());

    std::vector<Result> results;
    results.reserve(objects.size());
    for (const auto& object : objects) {
        Result result;
        result.title = core::json::GetString(object, L"title");
        result.artist = core::json::GetString(object, L"artist");
        result.album = core::json::GetString(object, L"album");
        result.enhancedLrc = core::json::GetString(object, L"lrc");
        if (result.enhancedLrc.empty() || Utf8(result.enhancedLrc).size() > kMaxEnhancedLrcBytes) continue;
        const int confidence = core::json::GetInt(object, L"confidence", -1);
        result.confidence = confidence >= 0 && confidence <= 100 ? confidence : -1;
        results.push_back(std::move(result));
    }
    return results;
}

} // namespace glasswave::applemusic::lyrics_protocol
