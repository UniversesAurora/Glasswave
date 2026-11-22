#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Plain HTTP for every source (NetEase, Apple Music, images, lyrics scripts).
namespace glasswave::net {

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

// Reserves and releases an ephemeral TCP port on 127.0.0.1 for local helper daemons; 0 on failure.
int PickLoopbackPort();

// Returns `bytes * 2` lowercase hex characters from the OS CSPRNG; empty on failure.
std::string RandomHex(size_t bytes);

}  // namespace glasswave::net
