#include "pch.h"
#include "applemusic/lyrics_protocol.h"

#include <algorithm>

namespace glasswave::applemusic::lyrics_protocol {
namespace {

constexpr size_t kMaxResults = 32;
constexpr size_t kMaxEnhancedLrcBytes = 4ull * 1024 * 1024;

std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<size_t>(std::max(size, 0)), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size,
                            nullptr, nullptr);
    return result;
}

} // namespace

std::string BuildRequestBody(const Request& request) {
    netease::json::Object object;
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
    return Utf8(netease::json::Stringify(object));
}

std::vector<Result> ParseResponse(const netease::json::Object& response) {
    const auto array = netease::json::GetArray(response, L"results");
    if (!array) return {};
    const auto objects = netease::json::Objects(*array);
    if (objects.size() > kMaxResults) throw std::runtime_error("Too many Apple Music lyric results.");

    std::vector<Result> results;
    results.reserve(objects.size());
    for (const auto& object : objects) {
        Result result;
        result.title = netease::json::GetString(object, L"title");
        result.artist = netease::json::GetString(object, L"artist");
        result.album = netease::json::GetString(object, L"album");
        result.enhancedLrc = netease::json::GetString(object, L"lrc");
        if (result.enhancedLrc.empty() || Utf8(result.enhancedLrc).size() > kMaxEnhancedLrcBytes) continue;
        const int confidence = netease::json::GetInt(object, L"confidence", -1);
        result.confidence = confidence >= 0 && confidence <= 100 ? confidence : -1;
        results.push_back(std::move(result));
    }
    return results;
}

} // namespace glasswave::applemusic::lyrics_protocol
