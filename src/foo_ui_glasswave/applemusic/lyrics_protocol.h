#pragma once

#include "core/json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace glasswave::applemusic::lyrics_protocol {

enum class Mode {
    Direct,
    Search,
};

struct Request {
    Mode mode = Mode::Search;
    std::wstring trackId;
    std::wstring trackType = L"songs";
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring locale;
    int durationMs = 0;
};

struct Result {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring enhancedLrc;
    int confidence = -1;
};

std::string BuildRequestBody(const Request& request);
std::vector<Result> ParseResponse(const core::json::Object& response);

} // namespace glasswave::applemusic::lyrics_protocol
