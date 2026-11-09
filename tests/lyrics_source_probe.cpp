#include "lyrics/script_host.h"
#include "netease/http_request.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace glasswave::lyrics;

namespace {
std::wstring Wide(std::string_view text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::string Utf8(std::wstring_view text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<size_t>(size), 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr,
                        nullptr);
    return result;
}
struct Handle {
    HINTERNET value;
    ~Handle() {
        if (value) WinHttpCloseHandle(value);
    }
};
struct Event {
    HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Event() { CloseHandle(value); }
};

ScriptResponse Fetch(const ScriptRequest& input) {
    ScriptResponse output;
    const auto url = Wide(input.url);
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) throw std::runtime_error("Invalid request URL");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    std::wstring method = Wide(input.method);
    std::transform(method.begin(), method.end(), method.begin(), towupper);
    Handle session{WinHttpOpen(L"Glasswave/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    WinHttpSetTimeouts(session.value, 5000, 5000, input.timeoutMs, input.timeoutMs);
    Handle connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    Event abort;
    glasswave::netease::detail::HttpRequest request(WinHttpOpenRequest(
        connection.value, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) throw std::runtime_error("Cannot create HTTP request");
    for (const auto& [name, value] : input.headers) {
        const auto header = Wide(name + ": " + value);
        WinHttpAddRequestHeaders(request.get(), header.c_str(), static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    const auto wait = [&](BOOL started) {
        if (!request.Wait(started, abort.value, input.timeoutMs))
            throw std::runtime_error("HTTP transport error " + std::to_string(request.Error()));
    };
    request.Begin();
    wait(WinHttpSendRequest(request.get(), nullptr, 0, const_cast<char*>(input.body.data()),
                            static_cast<DWORD>(input.body.size()), static_cast<DWORD>(input.body.size()),
                            reinterpret_cast<DWORD_PTR>(&request)));
    request.Begin();
    wait(WinHttpReceiveResponse(request.get(), nullptr));
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                        &status, &size, nullptr);
    output.status = static_cast<int>(status);
    for (;;) {
        request.Begin();
        wait(WinHttpQueryDataAvailable(request.get(), nullptr));
        const DWORD available = request.Available();
        if (!available) break;
        const auto previous = output.body.size();
        if (previous + available > 8 * 1024 * 1024) throw std::runtime_error("Response too large");
        output.body.resize(previous + available);
        request.Begin();
        wait(WinHttpReadData(request.get(), output.body.data() + previous, available, nullptr));
        output.body.resize(previous + request.Read());
    }
    std::cout << "HTTP " << input.method << ' ' << Utf8(host) << " -> " << output.status << " ("
              << output.body.size() << " bytes)\n";
    return output;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 6) {
        std::cerr << "Expected repo, script filename, title, artist, duration\n";
        return 1;
    }
    try {
        const auto profile = std::filesystem::path(argv[1]) / L".local/fb2k-dev/profile";
        ScriptEnvironment environment;
        environment.roots = {profile / L"eslyric-data/scripts", profile / L"glasswave/lyrics/scripts"};
        environment.request = [](const ScriptRequest& input) {
            try {
                return Fetch(input);
            } catch (const std::exception& error) {
                ScriptResponse response;
                response.error = error.what();
                std::cout << response.error << '\n';
                return response;
            }
        };
        environment.log = [](const std::string& text) { std::cout << "Script log: " << text << '\n'; };
        std::map<std::wstring, std::filesystem::path> parsers;
        std::filesystem::path source;
        for (const auto& root : environment.roots) {
            for (const auto& entry : std::filesystem::directory_iterator(root / L"parser"))
                if (entry.path().extension() == L".js")
                    parsers[entry.path().filename().wstring()] = entry.path();
            if (std::filesystem::exists(root / L"searcher" / argv[2])) source = root / L"searcher" / argv[2];
        }
        for (const auto& [name, path] : parsers) {
            ScriptHost host(environment);
            environment.parsers.push_back(host.Configure(path));
        }
        size_t candidates = 0, wordTimed = 0;
        environment.found = [&](const ScriptLyric& lyric) {
            ++candidates;
            if (lyric.text.find('<') != std::string::npos) ++wordTimed;
            std::cout << "Candidate " << candidates << ": " << lyric.title << " / " << lyric.artist << " ("
                      << lyric.text.size() << " bytes)\n";
        };
        ScriptHost host(environment);
        const auto config = host.Configure(source);
        std::cout << "Source " << config.name << '\n';
        const std::string title = Utf8(argv[3]), artist = Utf8(argv[4]);
        host.Search(config,
                    {{"title", title},
                     {"artist", artist},
                     {"album", ""},
                     {"rawTitle", title},
                     {"rawArtist", artist},
                     {"rawAlbum", ""}},
                    0, _wtof(argv[5]));
        std::cout << "RESULT candidates=" << candidates << " wordTimed=" << wordTimed << '\n';
        return candidates ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
