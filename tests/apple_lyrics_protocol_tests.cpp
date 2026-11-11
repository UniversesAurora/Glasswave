#include "applemusic/lyrics_protocol.h"
#include "netease/json.h"

#include <winrt/base.h>

#include <cstdlib>
#include <iostream>

namespace {

using glasswave::applemusic::lyrics_protocol::Mode;
using glasswave::applemusic::lyrics_protocol::Request;
using glasswave::netease::json::Object;

size_t checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

Object Parse(std::string_view text) {
    auto value = glasswave::netease::json::Parse(text);
    if (!value) {
        std::cerr << "JSON fixture is malformed\n";
        std::exit(1);
    }
    return *value;
}

void CheckRequests() {
    Request direct;
    direct.mode = Mode::Direct;
    direct.trackId = L"1440833098";
    direct.trackType = L"library-songs";
    direct.title = L"Song \"title\"";
    direct.artist = L"Artist";
    direct.album = L"Album";
    direct.locale = L"zh-Hans-CN";
    direct.durationMs = 245000;
    const Object request = Parse(glasswave::applemusic::lyrics_protocol::BuildRequestBody(direct));
    Check(glasswave::netease::json::GetString(request, L"mode") == L"direct",
          "Direct mode was not serialized");
    Check(glasswave::netease::json::GetString(request, L"trackId") == L"1440833098",
          "Direct track id was not serialized");
    Check(glasswave::netease::json::GetString(request, L"trackType") == L"library-songs",
          "Direct track type was not serialized");
    Check(glasswave::netease::json::GetString(request, L"locale") == L"zh-Hans-CN",
          "Request locale was not serialized");
    Check(glasswave::netease::json::GetString(request, L"title") == L"Song \"title\"",
          "Request title escaping changed");
    Check(glasswave::netease::json::GetInt(request, L"durationMs") == 245000,
          "Request duration was not serialized");

    Request search;
    search.mode = Mode::Search;
    search.title = L"Song";
    search.artist = L"Artist";
    search.locale = L"en-US";
    const Object searchJson = Parse(glasswave::applemusic::lyrics_protocol::BuildRequestBody(search));
    Check(glasswave::netease::json::GetString(searchJson, L"mode") == L"search",
          "Search mode was not serialized");
    Check(glasswave::netease::json::GetString(searchJson, L"trackId").empty(),
          "Search mode synthesized a track id");
}

void CheckResponses() {
    const Object response = Parse(R"({
        "results": [
            {
                "title": "First",
                "artist": "Artist",
                "album": "Album",
                "lrc": "[00:01.00]<00:01.00>Hello <00:01.40>world\\n[00:01.00]Translation",
                "confidence": 100
            },
            {
                "title": "Second",
                "lrc": "[00:02.00]Second"
            },
            {
                "title": "Broken"
            }
        ]
    })");
    const auto results = glasswave::applemusic::lyrics_protocol::ParseResponse(response);
    Check(results.size() == 2, "Response parsing did not preserve usable candidates");
    Check(results[0].title == L"First" && results[0].confidence == 100,
          "First response candidate metadata changed");
    Check(results[0].enhancedLrc.find(L"<00:01.40>world") != std::wstring::npos,
          "Word timing was not preserved");
    Check(results[0].enhancedLrc.find(L"[00:01.00]Translation") != std::wstring::npos,
          "Translation line was not preserved");
    Check(results[1].title == L"Second" && results[1].confidence == -1, "Optional response metadata changed");
    Check(glasswave::applemusic::lyrics_protocol::ParseResponse(Parse(R"({"results":[]})")).empty(),
          "Empty result array was not accepted");
}

} // namespace

int main() {
    winrt::init_apartment();
    CheckRequests();
    CheckResponses();
    std::cout << checks << " Apple lyrics protocol checks passed.\n";
    winrt::uninit_apartment();
}
