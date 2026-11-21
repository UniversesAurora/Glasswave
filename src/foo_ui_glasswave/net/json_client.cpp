#include "pch.h"
#include "net/json_client.h"

#include "core/log.h"
#include "core/text.h"

#include <chrono>

namespace glasswave::net {

TransportError::TransportError(std::wstring reason)
    : std::runtime_error(reason.empty() ? std::string("HTTP transport failed.")
                                        : "HTTP transport failed: " + core::Utf8(reason)),
      m_reason(std::move(reason)) {}

HttpStatusError::HttpStatusError(int status, std::string message, std::string body)
    : std::runtime_error(std::move(message)), m_status(status), m_body(std::move(body)) {}

namespace {

std::wstring PathOf(const std::wstring& url) {
    const size_t scheme = url.find(L"://");
    const size_t path = url.find(L'/', scheme == std::wstring::npos ? 0 : scheme + 3);
    if (path == std::wstring::npos) return L"/";
    return url.substr(path, url.find(L'?', path) - path);
}

} // namespace

JsonReply CallJson(JsonCall call, abort_callback& abort) {
    HttpOptions options;
    options.method = call.method;
    options.headers = std::move(call.headers);
    options.body = std::move(call.body);
    options.noProxy = call.noProxy;
    options.timeoutMs = call.timeoutMs;
    options.maxBodyBytes = call.maxBody;

    const auto started = std::chrono::steady_clock::now();
    struct WipeBody {
        std::string& text;
        ~WipeBody() {
            if (!text.empty()) SecureZeroMemory(text.data(), text.size());
        }
    } wipeBody{options.body};
    HttpResponse response = SendHttp(call.url, options, abort);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count();
    core::LogHttp(call.logMethod.empty() ? call.method : call.logMethod,
                  call.logPath.empty() ? PathOf(call.url) : call.logPath, response.status, elapsed);

    if (response.status == 0 || !response.error.empty()) throw TransportError(response.error);
    if (call.throwOnStatus && (response.status < 200 || response.status >= 300)) {
        throw HttpStatusError(static_cast<int>(response.status),
                              "HTTP request failed (HTTP " + std::to_string(response.status) + ").",
                              std::move(response.body));
    }

    JsonReply reply;
    reply.status = response.status;
    reply.setCookies = std::move(response.setCookies);
    reply.bodyEmpty = response.body.empty();
    if (!reply.bodyEmpty) reply.json = core::json::Parse(response.body, &reply.parseError);
    return reply;
}

} // namespace glasswave::net
