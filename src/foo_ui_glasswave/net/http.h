#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace glasswave::netease {

struct HttpResponse {
    unsigned long status = 0;
    std::string body;
    std::wstring contentType;
    std::wstring statusMessage;
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    std::vector<std::wstring> setCookies;
    std::wstring error;

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

struct HttpOptions {
    std::wstring method = L"GET";
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    std::string body;
    bool noProxy = false;
    int timeoutMs = 30000;
    size_t maxBodyBytes = 32u * 1024u * 1024u;
    std::function<void(long long received, long long total)> progress;
};

// Blocking WinHTTP request. `abort` is checked between reads.
HttpResponse SendHttp(std::wstring_view url, const HttpOptions& options, abort_callback& abort);

}  // namespace glasswave::netease
