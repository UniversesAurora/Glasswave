#include "pch.h"
#include "netease/http.h"

#include <winhttp.h>

#include <cwchar>

namespace glasswave::netease {

namespace {

struct InternetHandle {
    HINTERNET handle = nullptr;
    ~InternetHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
    InternetHandle() = default;
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET* put() { return &handle; }
    HINTERNET get() const { return handle; }
    explicit operator bool() const { return handle != nullptr; }
};

std::wstring Win32Error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                   reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = n && buffer ? std::wstring(buffer, n) : L"WinHTTP error " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

bool QueryNumber(HINTERNET request, DWORD query, DWORD& value) {
    DWORD size = sizeof(value);
    return WinHttpQueryHeaders(request, query | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &value,
                               &size, WINHTTP_NO_HEADER_INDEX) != FALSE;
}

std::wstring QueryString(HINTERNET request, DWORD query) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (!size) return {};
    std::wstring out(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, out.data(), &size,
                             WINHTTP_NO_HEADER_INDEX))
        return {};
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

}  // namespace

HttpResponse SendHttp(std::wstring_view url, const HttpOptions& options, abort_callback& abort) {
    HttpResponse result;
    std::wstring urlCopy(url);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(urlCopy.c_str(), 0, 0, &parts)) {
        result.error = L"Invalid URL: " + Win32Error(GetLastError());
        return result;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    InternetHandle session;
    *session.put() = WinHttpOpen(L"Glasswave/0.1",
                                 options.noProxy ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                                 : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.error = L"WinHttpOpen: " + Win32Error(GetLastError());
        return result;
    }
    WinHttpSetTimeouts(session.get(), 5000, 5000, options.timeoutMs, options.timeoutMs);

    InternetHandle connection;
    *connection.put() = WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0);
    if (!connection) {
        result.error = L"WinHttpConnect: " + Win32Error(GetLastError());
        return result;
    }

    InternetHandle request;
    *request.put() = WinHttpOpenRequest(connection.get(), options.method.c_str(), path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        result.error = L"WinHttpOpenRequest: " + Win32Error(GetLastError());
        return result;
    }

    std::wstring headers;
    for (const auto& [name, value] : options.headers) {
        headers += name;
        headers += L": ";
        headers += value;
        headers += L"\r\n";
    }
    if (!headers.empty() &&
        !WinHttpAddRequestHeaders(request.get(), headers.c_str(), static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        result.error = L"WinHttpAddRequestHeaders: " + Win32Error(GetLastError());
        return result;
    }

    void* body = options.body.empty() ? nullptr : const_cast<char*>(options.body.data());
    const DWORD bodySize = static_cast<DWORD>(options.body.size());
    abort.check();
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, bodySize, bodySize, 0)) {
        result.error = L"WinHttpSendRequest: " + Win32Error(GetLastError());
        return result;
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        result.error = L"WinHttpReceiveResponse: " + Win32Error(GetLastError());
        return result;
    }

    if (!QueryNumber(request.get(), WINHTTP_QUERY_STATUS_CODE, result.status))
        result.error = L"Cannot read the HTTP status";
    result.contentType = QueryString(request.get(), WINHTTP_QUERY_CONTENT_TYPE);

    DWORD rawSize = 0;
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &rawSize, WINHTTP_NO_HEADER_INDEX);
    if (rawSize > sizeof(wchar_t)) {
        std::wstring raw(rawSize / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                raw.data(), &rawSize, WINHTTP_NO_HEADER_INDEX)) {
            size_t lineStart = 0;
            while (lineStart < raw.size()) {
                const size_t lineEnd = raw.find(L"\r\n", lineStart);
                const std::wstring line =
                    raw.substr(lineStart, lineEnd == std::wstring::npos ? std::wstring::npos : lineEnd - lineStart);
                const std::wstring prefix = L"set-cookie:";
                if (line.size() > prefix.size() &&
                    _wcsnicmp(line.c_str(), prefix.c_str(), prefix.size()) == 0) {
                    result.setCookies.push_back(line.substr(prefix.size()));
                }
                if (lineEnd == std::wstring::npos) break;
                lineStart = lineEnd + 2;
            }
        }
    }

    DWORD total = 0;
    QueryNumber(request.get(), WINHTTP_QUERY_CONTENT_LENGTH, total);
    long long received = 0;
    for (;;) {
        abort.check();
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            result.error = L"WinHttpQueryDataAvailable: " + Win32Error(GetLastError());
            break;
        }
        if (!available) break;
        const size_t old = result.body.size();
        if (old + available > options.maxBodyBytes) {
            result.error = L"HTTP response is too large";
            break;
        }
        result.body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), result.body.data() + old, available, &read)) {
            result.body.resize(old);
            result.error = L"WinHttpReadData: " + Win32Error(GetLastError());
            break;
        }
        result.body.resize(old + read);
        received += read;
        if (options.progress) options.progress(received, total ? static_cast<long long>(total) : 0);
    }
    // A connection dropped mid-body ends like a finished one; a truncated cover or helper
    // download must not be kept as if it were complete.
    if (result.error.empty() && total && received != static_cast<long long>(total))
        result.error = L"The HTTP response ended early (" + std::to_wstring(received) + L" of " +
                       std::to_wstring(total) + L" bytes).";

    return result;
}

}  // namespace glasswave::netease
